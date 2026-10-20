// Small resampler owned by DLSS FG. Real frames never pass through it.
// Separate producer/presenter descriptor pairs prevent concurrent rewrites.
static struct FgScaleState {
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> pso;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    winrt::com_ptr<ID3D12Resource> display;
} g_fg_scale;

static bool FgScaleReady(UINT w, UINT h, DXGI_FORMAT format)
{
    if (!g_fg_scale.pso || !g_fg_scale.root || !g_fg_scale.heap) {
        g_fg_scale.pso=nullptr; g_fg_scale.root=nullptr; g_fg_scale.heap=nullptr;
        static const char shader[] = R"hlsl(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { uint outW; uint outH; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outW || id.y >= outH) return;
    uint w,h; src.GetDimensions(w,h);
    float2 ratio = float2(w,h)/float2(outW,outH), inv = 1.0/float2(w,h);
    float2 p = (float2(id.xy)+0.5)*ratio;
    if (ratio.x > 1 || ratio.y > 1) {
        float2 q = ratio*0.25;
        float3 c = src.SampleLevel(lin,(p+float2(-q.x,-q.y))*inv,0).rgb;
        c += src.SampleLevel(lin,(p+float2(q.x,-q.y))*inv,0).rgb;
        c += src.SampleLevel(lin,(p+float2(-q.x,q.y))*inv,0).rgb;
        c += src.SampleLevel(lin,(p+q)*inv,0).rgb;
        dst[id.xy]=float4(c*0.25,1); return;
    }
    // Catmull-Rom: combine the two positive inner weights into one bilinear
    // tap. Nine filtered taps, no trigonometry or secondary sharpening pass.
    p -= 0.5;
    float2 b=floor(p), f=p-b, f2=f*f, f3=f2*f;
    float2 a0=-0.5*f+f2-0.5*f3;
    float2 a1=1-2.5*f2+1.5*f3;
    float2 a2=0.5*f+2*f2-1.5*f3;
    float2 a3=-0.5*f2+0.5*f3, a12=a1+a2;
    float2 p0=b-0.5, p12=b+0.5+a2/a12, p3=b+2.5;
    float xs[3]={p0.x,p12.x,p3.x}, ys[3]={p0.y,p12.y,p3.y};
    float wx[3]={a0.x,a12.x,a3.x}, wy[3]={a0.y,a12.y,a3.y};
    float3 c=0;
    [unroll] for(int y=0;y<3;++y) [unroll] for(int x=0;x<3;++x)
        c+=src.SampleLevel(lin,float2(xs[x],ys[y])*inv,0).rgb*wx[x]*wy[y];
    int2 low=int2(b), limit=int2(w,h)-1;
    float3 n0=src.Load(int3(clamp(low,int2(0,0),limit),0)).rgb;
    float3 n1=src.Load(int3(clamp(low+int2(1,0),int2(0,0),limit),0)).rgb;
    float3 n2=src.Load(int3(clamp(low+int2(0,1),int2(0,0),limit),0)).rgb;
    float3 n3=src.Load(int3(clamp(low+1,int2(0,0),limit),0)).rgb;
    dst[id.xy]=float4(clamp(c,min(min(n0,n1),min(n2,n3)),max(max(n0,n1),max(n2,n3))),1);
})hlsl";
        winrt::com_ptr<ID3DBlob> code, errors, signature;
        if (FAILED(CachedCompile(shader, sizeof(shader)-1, "lf-fg-scale.hlsl", nullptr, nullptr,
                                "main", "cs_5_0", 0, 0, code.put(), errors.put()))) {
            Trace("[dlssg] scale shader: %s", errors ? (char *)errors->GetBufferPointer() : "compile failed"); return false;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0}};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues=2;
        for (UINT i=0;i<2;++i) {
            params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i+1].DescriptorTable={1,&ranges[i]};
        }
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy=1; sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc = {3,params,1,&sampler};
        if (FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,signature.put(),errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(g_fg_scale.root.put())))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature=g_fg_scale.root.get(); pso.CS={code->GetBufferPointer(),code->GetBufferSize()};
        if (FAILED(lfCore.device->CreateComputePipelineState(&pso,IID_PPV_ARGS(g_fg_scale.pso.put())))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors=4;
        heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(g_fg_scale.heap.put())))) return false;
    }
    g_fg_scale.display.attach(NewTexture(w,h,format,true));
    return bool(g_fg_scale.display);
}

static void FgScaleRecord(ID3D12GraphicsCommandList *list, ID3D12Resource *source,
                          D3D12_RESOURCE_STATES source_state, ID3D12Resource *destination, bool presenter)
{
    const auto sd=source->GetDesc(), dd=destination->GetDesc();
    const UINT stride=lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu=g_fg_scale.heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu=g_fg_scale.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr+=(presenter?2:0)*stride; gpu.ptr+=(presenter?2:0)*stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format=sd.Format; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
    lfCore.device->CreateShaderResourceView(source,&srv,cpu); cpu.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format=dd.Format; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(destination,nullptr,&uav,cpu);
    if (source_state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        auto b=Barrier(source,source_state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); list->ResourceBarrier(1,&b);
    }
    ID3D12DescriptorHeap *heaps[]={g_fg_scale.heap.get()}; list->SetDescriptorHeaps(1,heaps);
    list->SetComputeRootSignature(g_fg_scale.root.get()); list->SetPipelineState(g_fg_scale.pso.get());
    const UINT dims[]={UINT(dd.Width),dd.Height}; list->SetComputeRoot32BitConstants(0,2,dims,0);
    list->SetComputeRootDescriptorTable(1,gpu); gpu.ptr+=stride; list->SetComputeRootDescriptorTable(2,gpu);
    list->Dispatch((UINT(dd.Width)+7)/8,(dd.Height+7)/8,1);
    if (source_state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        auto b=Barrier(source,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,source_state); list->ResourceBarrier(1,&b);
    }
}
