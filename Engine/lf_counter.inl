// lf_counter.inl - the FPS counter, drawn by the engine itself.
//
// A counter that is a window of its own on top of the output makes the
// compositor blend the two for every frame shown instead of handing the
// output's frames straight to the screen - more delay and uneven frames
// exactly when a generator runs, which is when a person turns the counter on.
//
// Here it is drawn into the frame just before its Present, by the thread that
// presents - no window, no other process. It is two figures and nothing else,
//     95/380
// the game's own frames a second, and all frames shown a second: white with a
// thin dark edge, straight on the picture. They are counted where the frames
// are shown:
//     shown  every Present of the output
//     base   every real frame's first Present (a picture shown again, and a
//            frame in between, are not the game's)
// over the last second, in quarter-second steps. The text is drawn with GDI
// only when a figure changes - how much of each pixel the figures cover, and
// their edge - and a small pass blends it onto the back buffer in that
// buffer's own format (8-bit, HDR10 or scRGB).
//
// Whether it is shown is the "FPS counter" switch in the app; the app reads
// the same two figures once a second for its own status line (CounterRates).

static struct CounterState
{
    std::mutex lock;
    // What the manager asked for.
    bool on = false, hold = false;
    int dx = 12, dy = 12;
    // Counting.
    unsigned shown = 0, reals = 0;          // in the running quarter second
    unsigned shown_q[4] = {}, reals_q[4] = {};
    double span_q[4] = {};
    unsigned q = 0;
    double since = 0.0, last_source = 0.0;
    int base_fps = -1, shown_fps = -1;      // what is drawn
    // The drawing.
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    uint32_t *pixels = nullptr;
    HFONT font = nullptr;
    int scale_for = 0;
    ID3D12Resource *upload[2] = {};         // the figures as GDI left them: edge and fill, a byte each
    BYTE *mapped[2] = {};
    unsigned front = 0;
    ID3D12Resource *glyphs = nullptr;       // the same on the card
    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *blend = nullptr;   // made for `format`
    ID3D12DescriptorHeap *views = nullptr, *target = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT w = 0, h = 0, pitch = 0;           // the drawn box; pitch: a row of it in the back buffer's format (CounterProof)
    bool drawn = false, failed = false;
    int drawn_base = -2, drawn_shown = -2, drawn_mode = -2;
} g_counter;

static constexpr UINT kCounterMaxW = 640, kCounterMaxH = 128;

// The switch in the app (the corner offset is the counter's own).
static void CounterLook()
{
    CounterState &c = g_counter;
    const bool on = smOpt.counter.load() != 0;
    if (on != c.on) Trace("[counter] the FPS counter is %s (drawn by the engine, in the frame)", on ? "on" : "off");
    c.on = on;
    c.hold = false;
}

// After every Present of the output (lf_vrr.inl). real: a real frame's first.
// Round 56, Adaptive MFG: a pair shown by the beat counts as a frame of the game whether or not its real frame
// falls on a beat (the counter read 15 to 33 for a game at 59 - most real frames lie between beats).
static void CounterGameFrame(double source)
{
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> hold(c.lock);
    if (source > 0.0 && source != c.last_source) { c.last_source = source; ++c.reals; }
}

static void CounterPresented(double source, bool own_capture)
{
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> hold(c.lock);
    const double now = LatencyNow();
    const double stamp = own_capture ? lfCap.stamp : source;
    const bool real = own_capture ? (stamp <= 0.0 || stamp != c.last_source) : (stamp > 0.0 && stamp != c.last_source);
    if (real) { c.last_source = stamp; ++c.reals; }
    ++c.shown;
    if (c.since == 0.0) c.since = now;
    if (now - c.since < 0.25) return;
    c.shown_q[c.q] = c.shown;
    c.reals_q[c.q] = c.reals;
    c.span_q[c.q] = now - c.since;
    c.q = (c.q + 1) % 4;
    c.shown = c.reals = 0;
    c.since = now;
    double span = 0.0;
    unsigned shown = 0, reals = 0;
    for (unsigned i = 0; i < 4; ++i) { span += c.span_q[i]; shown += c.shown_q[i]; reals += c.reals_q[i]; }
    // A gap (a still picture, a menu) is not a rate.
    if (span <= 0.0 || span > 3.0) { c.base_fps = c.shown_fps = -1; return; }
    c.base_fps = static_cast<int>(reals / span + 0.5);
    c.shown_fps = static_cast<int>(shown / span + 0.5);
}

// The two figures for the app's status line: real frames and all frames shown
// a second, -1 while there is no rate (a still picture).
static void CounterRates(int &base, int &shown)
{
    std::lock_guard<std::mutex> hold(g_counter.lock);
    base = g_counter.base_fps;
    shown = g_counter.shown_fps;
}

static constexpr UINT kCounterGlyphPitch = (kCounterMaxW * 2u + 255u) & ~255u;

static void CounterRelease()
{
    CounterState &c = g_counter;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (c.upload[i]) { c.upload[i]->Unmap(0, nullptr); c.upload[i]->Release(); c.upload[i] = nullptr; }
        c.mapped[i] = nullptr;
    }
    if (c.glyphs) { c.glyphs->Release(); c.glyphs = nullptr; }
    if (c.blend) { c.blend->Release(); c.blend = nullptr; }
    if (c.views) { c.views->Release(); c.views = nullptr; }
    if (c.target) { c.target->Release(); c.target = nullptr; }
    c.format = DXGI_FORMAT_UNKNOWN;
    c.drawn = false;
}

static UINT CounterBytes(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
}

// What GDI drew (the edge in red, the figures in green over it) into the
// upload buffer: two bytes a pixel, edge and fill.
static void CounterConvert(BYTE *to)
{
    CounterState &c = g_counter;
    for (UINT y = 0; y < c.h; ++y)
    {
        const uint32_t *from = c.pixels + static_cast<size_t>(y) * kCounterMaxW;
        BYTE *row = to + static_cast<size_t>(y) * kCounterGlyphPitch;
        for (UINT x = 0; x < c.w; ++x)
        {
            row[x * 2] = static_cast<BYTE>((from[x] >> 16) & 0xFF);
            row[x * 2 + 1] = static_cast<BYTE>((from[x] >> 8) & 0xFF);
        }
    }
}

// The text, when a figure changed: "95/380", or the one figure there is
// without frames in between.
static bool CounterDraw(int base, int shown, bool both, UINT screen_h)
{
    CounterState &c = g_counter;
    if (!c.dc)
    {
        c.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = kCounterMaxW;
        info.bmiHeader.biHeight = -static_cast<LONG>(kCounterMaxH);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        c.dib = c.dc ? CreateDIBSection(c.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        c.pixels = static_cast<uint32_t *>(bits);
        if (!c.dib || !c.pixels) return false;
        SelectObject(c.dc, c.dib);
        SetBkMode(c.dc, TRANSPARENT);
    }
    if (!c.pixels) return false;
    // 24 pixels tall on 1440 lines; other screens in proportion.
    const int scale = std::clamp(static_cast<int>(screen_h * 100u / 1440u), 70, 220);
    if (scale != c.scale_for)
    {
        if (c.font) DeleteObject(c.font);
        c.font = CreateFontW(-(24 * scale / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        c.scale_for = scale;
    }
    wchar_t text[40];
    if (base < 0) wcscpy_s(text, L"-");
    else if (both && shown >= 0) swprintf_s(text, L"%d/%d", base, shown);
    else swprintf_s(text, L"%d", base);
    const int length = static_cast<int>(wcslen(text));
    SelectObject(c.dc, c.font);
    SIZE size = {};
    GetTextExtentPoint32W(c.dc, text, length, &size);
    const int edge = std::max(1, 2 * scale / 100);
    const int w = std::min<int>((size.cx + 2 * edge + 3) & ~1, kCounterMaxW);
    const int h = std::min<int>((size.cy + 2 * edge + 1) & ~1, kCounterMaxH);
    RECT all = {0, 0, w, h};
    FillRect(c.dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    // The edge: the figures once at every place around their own.
    SetTextColor(c.dc, RGB(255, 0, 0));
    for (int dy = -edge; dy <= edge; ++dy)
        for (int dx = -edge; dx <= edge; ++dx)
            if ((dx || dy) && dx * dx + dy * dy <= edge * edge + 1) TextOutW(c.dc, edge + dx, edge + dy, text, length);
    SetTextColor(c.dc, RGB(0, 255, 0));
    TextOutW(c.dc, edge, edge, text, length);
    GdiFlush();
    c.w = static_cast<UINT>(w);
    c.h = static_cast<UINT>(h);
    return true;
}

static const char kCounterHlsl[] = R"hlsl(
Texture2D<float2> glyphs : register(t0);
cbuffer C : register(b0) { int2 origin; uint encoding; float white; };

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
}

// White figures with a dark edge, in the back buffer's own encoding:
// 0 8-bit, 1 HDR10 (white at 203 nits), 2 scRGB (`white` times 80 nits).
float4 PSMain(float4 at : SV_Position) : SV_Target
{
    float2 g = glyphs.Load(int3(int2(at.xy) - origin, 0));
    float level = 1.0;
    if (encoding == 1)
    {
        float n = pow(203.0 / 10000.0, 0.1593017578125);
        level = pow((0.8359375 + 18.8515625 * n) / (1.0 + 18.6875 * n), 78.84375);
    }
    else if (encoding == 2) level = white;
    return float4(level * g.y, level * g.y, level * g.y, saturate(0.85 * g.x + g.y));
}
)hlsl";

// The blend pass for a back buffer format, the figures' texture and its two
// upload buffers. Under g_counter.lock.
static bool CounterReady(DXGI_FORMAT format)
{
    CounterState &c = g_counter;
    if (c.failed) return false;
    if (c.format == format && c.blend && c.glyphs) return true;
    CounterRelease();
    auto fail = [&](const char *what) { c.failed = true; CounterRelease(); Trace("[counter] off: %s", what); return false; };
    if (!c.root)
    {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc = {2, params};
        winrt::com_ptr<ID3DBlob> signature, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                      IID_PPV_ARGS(&c.root)))) return fail("its root signature");
    }
    winrt::com_ptr<ID3DBlob> vs, ps, errors;
    if (FAILED(CachedCompile(kCounterHlsl, sizeof(kCounterHlsl) - 1, "sm-counter.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, vs.put(), errors.put())) ||
        FAILED(CachedCompile(kCounterHlsl, sizeof(kCounterHlsl) - 1, "sm-counter.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, ps.put(), errors.put())))
        return fail("its shaders");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = c.root;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    if (FAILED(lfCore.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&c.blend)))) return fail("its pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC views = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC target = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    if (FAILED(lfCore.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&c.views))) ||
        FAILED(lfCore.device->CreateDescriptorHeap(&target, IID_PPV_ARGS(&c.target)))) return fail("its descriptor heaps");
    D3D12_HEAP_PROPERTIES card = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC texture = {};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = kCounterMaxW; texture.Height = kCounterMaxH; texture.DepthOrArraySize = 1; texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8_UNORM; texture.SampleDesc.Count = 1;
    if (FAILED(lfCore.device->CreateCommittedResource(&card, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&c.glyphs)))) return fail("its texture");
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8_UNORM; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(c.glyphs, &view, c.views->GetCPUDescriptorHandleForHeapStart());
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64(kCounterGlyphPitch) * kCounterMaxH;
    buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (unsigned i = 0; i < 2; ++i)
    {
        D3D12_RANGE none = {0, 0};
        void *at = nullptr;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&c.upload[i]))) ||
            FAILED(c.upload[i]->Map(0, &none, &at))) return fail("its upload buffers");
        c.mapped[i] = static_cast<BYTE *>(at);
    }
    c.format = format;
    c.pitch = (kCounterMaxW * CounterBytes(format) + 255u) & ~255u;
    c.drawn = false;
    c.drawn_base = -2;
    return true;
}

// LF_COUNTER_PROOF=<file>: once, some seconds in, the counter's box is read
// back from the back buffer it was copied onto and written out raw (width,
// height, format, row pitch as four 32-bit numbers, then the rows) - the
// output is hidden from screen capture, so tests/screen_counter_native.py has
// no other way to see what reached the frame. Under g_counter.lock.
static void CounterProof(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, DXGI_FORMAT format, UINT x, UINT y)
{
    static int stage = -1;   // -1 unread, 0 off, 1 waiting, 2 copied, 3 done
    static char path[MAX_PATH] = {};
    static ID3D12Resource *readback = nullptr;
    static ULONGLONG began = 0, copied = 0;
    static UINT w = 0, h = 0, pitch = 0;
    CounterState &c = g_counter;
    if (stage == -1)
    {
        const DWORD len = GetEnvironmentVariableA("LF_COUNTER_PROOF", path, sizeof(path));
        stage = (len > 0 && len < sizeof(path)) ? 1 : 0;
        began = GetTickCount64();
    }
    if (stage == 0 || stage == 3) return;
    if (stage == 1)
    {
        static const ULONGLONG after = [] {
            char text[16] = {};
            const DWORD len = GetEnvironmentVariableA("LF_COUNTER_PROOF_AFTER", text, sizeof(text));
            return static_cast<ULONGLONG>((len > 0 && len < sizeof(text)) ? std::max(200, atoi(text)) : 6000);
        }();
        if (GetTickCount64() - began < after || c.base_fps < 0) return;
        w = c.w; h = c.h; pitch = c.pitch;
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = UINT64(pitch) * h;
        buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) { stage = 0; return; }
        auto to_source = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_source);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = back_buffer;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = format;
        to.PlacedFootprint.Footprint.Width = w;
        to.PlacedFootprint.Footprint.Height = h;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = pitch;
        const D3D12_BOX box = {x, y, 0, x + w, y + h, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        auto back = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &back);
        copied = GetTickCount64();
        stage = 2;
        return;
    }
    // Long after the frame it was copied from has been shown.
    if (GetTickCount64() - copied < 1000) return;
    stage = 3;
    void *at = nullptr;
    D3D12_RANGE all = {0, SIZE_T(pitch) * h};
    if (FAILED(readback->Map(0, &all, &at))) return;
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") == 0 && file)
    {
        const uint32_t head[4] = {w, h, static_cast<uint32_t>(format), pitch};
        fwrite(head, sizeof(head), 1, file);
        fwrite(at, 1, SIZE_T(pitch) * h, file);
        fclose(file);
        Trace("[counter] proof written: %ux%u, format %u, base %d, shown %d", w, h, (unsigned)format, c.drawn_base, c.drawn_shown);
    }
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    readback->Release();
    readback = nullptr;
}

// The counter onto a back buffer in COPY_DEST, by the thread that presents;
// the buffer is in COPY_DEST again afterwards.
static void CounterStamp(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer)
{
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> hold(c.lock);
    CounterLook();
    if (!c.on || c.hold || !list || !back_buffer || !lfCore.device) return;
    const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    if (!CounterReady(desc.Format)) return;
    // Both figures while frames in between are made; the one there is otherwise.
    const bool both = FgRequested();
    const int base = c.base_fps, shown = both ? c.shown_fps : -1;
    if (!c.drawn || base != c.drawn_base || shown != c.drawn_shown || int(both) != c.drawn_mode)
    {
        if (!CounterDraw(base, shown, both, desc.Height)) return;
        // The other buffer: the one in front may still be read by a list on its way.
        const unsigned next = c.front ^ 1u;
        CounterConvert(c.mapped[next]);
        c.front = next;
        c.drawn = true;
        c.drawn_base = base; c.drawn_shown = shown; c.drawn_mode = int(both);
        auto to_write = Barrier(c.glyphs, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_write);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = c.upload[c.front];
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8_UNORM;
        from.PlacedFootprint.Footprint.Width = c.w;
        from.PlacedFootprint.Footprint.Height = c.h;
        from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = kCounterGlyphPitch;
        to.pResource = c.glyphs;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto to_read = Barrier(c.glyphs, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_read);
    }
    if (c.w == 0 || c.h == 0 || desc.Width < c.w || desc.Height < c.h) return;
    const UINT x = static_cast<UINT>(std::clamp<LONGLONG>(c.dx, 0, static_cast<LONGLONG>(desc.Width) - c.w));
    const UINT y = static_cast<UINT>(std::clamp<LONGLONG>(c.dy, 0, static_cast<LONGLONG>(desc.Height) - c.h));
    auto to_target = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &to_target);
    const D3D12_CPU_DESCRIPTOR_HANDLE target = c.target->GetCPUDescriptorHandleForHeapStart();
    lfCore.device->CreateRenderTargetView(back_buffer, nullptr, target);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {float(x), float(y), float(c.w), float(c.h), 0.0f, 1.0f};
    const D3D12_RECT box = {LONG(x), LONG(y), LONG(x + c.w), LONG(y + c.h)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &box);
    ID3D12DescriptorHeap *heaps[] = {c.views};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(c.root);
    list->SetPipelineState(c.blend);
    // scRGB: 1.0 is 80 nits. HDR kept: white at 203 nits; the linear SDR
    // output: white at the screen's SDR white, like the picture under it.
    struct { int x, y; UINT encoding; float white; } numbers = {
        int(x), int(y),
        desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2u : 0u,
        lfHdr.keep ? 2.5375f : lfHdr.paper_white};
    list->SetGraphicsRoot32BitConstants(0, 4, &numbers, 0);
    list->SetGraphicsRootDescriptorTable(1, c.views->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    auto to_copy = Barrier(back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_copy);
    CounterProof(list, back_buffer, desc.Format, x, y);
}

// LF_SHOW_PROOF=<file>: what reached the screen, for tests. Whenever
// "<file>.go" exists (looked for twice a second) the next real frame's whole
// back buffer is read back - before the counter goes on it - and written to
// <file> raw (width, height, format and row pitch as four 32-bit numbers, then
// the rows); "<file>.go" is removed when it is there. The output is hidden
// from screen capture, and the engine's own screenshot is the network's
// result, not what the presenter put on the screen (tests/screen_color_passes.py).
// By the thread that presents; back_buffer in COPY_DEST.
static void ShowProof(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, bool real)
{
    static std::mutex lock;
    static int stage = -1;   // -1 unread, 0 off, 1 waiting for the go, 2 copied
    static char path[MAX_PATH] = {}, go[MAX_PATH + 4] = {};
    static ID3D12Resource *readback = nullptr;
    static ULONGLONG looked = 0, copied = 0;
    static UINT w = 0, h = 0, pitch = 0;
    static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (stage == 0) return;
    std::lock_guard<std::mutex> hold(lock);
    if (stage == -1)
    {
        const DWORD len = GetEnvironmentVariableA("LF_SHOW_PROOF", path, sizeof(path));
        stage = (len > 0 && len < sizeof(path)) ? 1 : 0;
        if (stage) { strcpy_s(go, path); strcat_s(go, ".go"); }
        return;
    }
    const ULONGLONG tick = GetTickCount64();
    if (stage == 1)
    {
        if (!list || !back_buffer) return;
        // "<file>.go" holding a "g" asks for a frame in between instead.
        static bool want_made = false, asked = false;
        if (!asked)
        {
            if (tick - looked < 500) return;
            looked = tick;
            if (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES) return;
            char kind = 0;
            FILE *which = nullptr;
            if (fopen_s(&which, go, "rb") == 0 && which) { fread(&kind, 1, 1, which); fclose(which); }
            want_made = kind == 'g';
            asked = true;
        }
        if (real == want_made) return;
        asked = false;
        const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
        const UINT bytes = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        const UINT want_pitch = (static_cast<UINT>(desc.Width) * bytes + 255u) & ~255u;
        if (!readback || w != desc.Width || h != desc.Height || pitch != want_pitch)
        {
            if (readback) { readback->Release(); readback = nullptr; }
            D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC buffer = {};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = UINT64(want_pitch) * desc.Height;
            buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) { readback = nullptr; return; }
        }
        w = static_cast<UINT>(desc.Width); h = desc.Height; pitch = want_pitch; format = desc.Format;
        auto to_source = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_source);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = back_buffer;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = format;
        to.PlacedFootprint.Footprint.Width = w;
        to.PlacedFootprint.Footprint.Height = h;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = pitch;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto back = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &back);
        copied = tick;
        stage = 2;
        return;
    }
    // Long after the frame it was copied from has been shown.
    if (tick - copied < 700) return;
    stage = 1;
    void *at = nullptr;
    D3D12_RANGE all = {0, SIZE_T(pitch) * h};
    if (FAILED(readback->Map(0, &all, &at))) return;
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") == 0 && file)
    {
        const uint32_t head[4] = {w, h, static_cast<uint32_t>(format), pitch};
        fwrite(head, sizeof(head), 1, file);
        fwrite(at, 1, SIZE_T(pitch) * h, file);
        fclose(file);
    }
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    DeleteFileA(go);
}
