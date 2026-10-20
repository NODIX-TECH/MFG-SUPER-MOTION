// xe_tap.h - a tap on the swap chain Intel's library shows its frames in.
//
// XeSS Frame Generation has no call that hands a frame over. It makes its
// frames and shows them itself, in a swap chain it creates with the DXGI
// factory it is given (xefgSwapChainD3D12InitFromSwapChainDesc). So the
// factory it is given is this one: it has the real factory make the real swap
// chain - for a window that is never shown - and hands the library a swap
// chain that passes every call on to it. Every call but one: at Present,
// whoever listens (XeTapSink) is first told that a picture is about to be
// shown, and may take it from the back buffer.
//
// That is how the engine has XeSS make frames small and shows them itself
// (xe_mfg.inl) - at the picture's own size, when and how its own presenter
// decides - instead of leaving the screen to the library.
//
// The real swap chain's window is never shown - and since round 14 nothing is
// shown in it at all. The library is handed pictures of the tap's own to draw
// into (as many as the swap chain has buffers, taken in turn as a swap
// chain's are), and its Present ends here: whoever listens is told which of
// them holds the picture, and that is all. Before, the picture went into the
// real back buffer, was copied out of it - one more piece of work sent to the
// card for every frame, and each piece sent by itself costs a game that fills
// the card a third of a millisecond - and the real Present was called for a
// window nobody sees: three of them a pair. The real swap chain is still
// made - small, it only answers what the library asks about its window and
// its screen; what size and how many buffers it has, the tap answers. (`own`
// false: the old way still, to compare - the Presents then with sync interval
// 0 and as tearing ones, so that none waits for a screen.)
//
// Nothing here knows the engine: Windows' own interfaces only, so the test
// tools use the same file (tests\xeprobe.cpp).
#pragma once
#include <dxgi1_6.h>
#include <d3d12.h>
#include <atomic>
#include <vector>

// Who is told of each picture the library shows.
struct XeTapSink
{
    // On the thread that shows it. `picture` holds it once the work sent to
    // `queue` so far has run: the tap's own picture number `index` (at rest,
    // as a back buffer is: D3D12_RESOURCE_STATE_PRESENT), or - `own` false -
    // the real swap chain's back buffer, about to be shown and reused soon.
    virtual void Shown(ID3D12Resource *picture, UINT index, ID3D12CommandQueue *queue) = 0;
};

// The swap chain the library is handed: the real one, with the tap at Present.
class XeTapChain final : public IDXGISwapChain4
{
    std::atomic<ULONG> held{1};
    IDXGISwapChain4 *real;
    ID3D12CommandQueue *queue;
    XeTapSink *sink;
    bool tearing;           // the real swap chain was made for tearing Presents
    // The pictures the library draws into instead of the real back buffers, whose turn it is, and what the
    // swap chain says it is (the size and the count the library asked for - the real one is small).
    std::vector<ID3D12Resource *> own;
    std::atomic<UINT> turn{0};
    DXGI_SWAP_CHAIN_DESC1 shape = {};
    // The library makes its swap chain with a "frame latency waitable object" and waits on it before every
    // Present - a second at most, each time, when nothing is ever shown. With pictures of the tap's own it is
    // handed this instead: always free.
    HANDLE free_to_show = nullptr;

    UINT Never(UINT flags) const { return tearing ? (flags | DXGI_PRESENT_ALLOW_TEARING) : flags; }
    UINT Kept(UINT flags) const { return tearing ? (flags | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) : flags; }

    void Drop()
    {
        for (ID3D12Resource *picture : own) picture->Release();
        own.clear();
        turn = 0;
    }
    // One for each buffer the swap chain says it has, as large and of its kind.
    bool Make()
    {
        Drop();
        const DXGI_SWAP_CHAIN_DESC1 desc = shape;
        ID3D12Device *device = nullptr;
        if (!queue || desc.Width == 0 || desc.Height == 0 || desc.BufferCount == 0 ||
            FAILED(queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&device))) || !device)
            return false;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = desc.Width; d.Height = desc.Height; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = desc.Format; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (UINT i = 0; i < desc.BufferCount; ++i)
        {
            ID3D12Resource *picture = nullptr;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                       __uuidof(ID3D12Resource), reinterpret_cast<void **>(&picture))) || !picture)
            {
                Drop();
                break;
            }
            own.push_back(picture);
        }
        device->Release();
        return !own.empty();
    }
    // A Present of the library's. True: it ended here.
    bool Tap()
    {
        if (!own.empty())
        {
            const UINT index = turn.load();
            if (sink) sink->Shown(own[index], index, queue);
            turn = (index + 1) % static_cast<UINT>(own.size());
            return true;
        }
        ID3D12Resource *picture = nullptr;
        const UINT index = real->GetCurrentBackBufferIndex();
        if (sink && SUCCEEDED(real->GetBuffer(index, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&picture))) && picture)
        {
            sink->Shown(picture, index, queue);
            picture->Release();
        }
        return false;
    }

public:
    // says: with pictures of its own, what the swap chain is to say it is (null: none of its own).
    XeTapChain(IDXGISwapChain4 *chain, ID3D12CommandQueue *made_on, XeTapSink *listener, bool tears, const DXGI_SWAP_CHAIN_DESC1 *says)
        : real(chain), queue(made_on), sink(listener), tearing(tears)
    {
        if (queue) queue->AddRef();
        if (says) shape = *says;
        if (says && Make()) free_to_show = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    }
    bool Own() const { return !own.empty(); }
    // How many pictures of its own there are, and one of them (they are taken in turn: the one after `index`
    // is index + 1, and after the last comes the first).
    UINT Pictures() const { return static_cast<UINT>(own.size()); }
    ID3D12Resource *Picture(UINT index) const { return own.empty() ? nullptr : own[index % own.size()]; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIDeviceSubObject) ||
            id == __uuidof(IDXGISwapChain) || id == __uuidof(IDXGISwapChain1) || id == __uuidof(IDXGISwapChain2) ||
            id == __uuidof(IDXGISwapChain3) || id == __uuidof(IDXGISwapChain4))
        {
            *out = static_cast<IDXGISwapChain4 *>(this);
            AddRef();
            return S_OK;
        }
        return real->QueryInterface(id, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++held; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --held;
        if (left == 0)
        {
            Drop();
            if (free_to_show) CloseHandle(free_to_show);
            real->Release();
            if (queue) queue->Release();
            delete this;
        }
        return left;
    }

    // The two that show a picture.
    HRESULT STDMETHODCALLTYPE Present(UINT, UINT flags) override
    {
        return Tap() ? S_OK : real->Present(0, Never(flags));
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT, UINT flags, const DXGI_PRESENT_PARAMETERS *parameters) override
    {
        return Tap() ? S_OK : real->Present1(0, Never(flags), parameters);
    }

    // Everything else goes on as it came.
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void *data) override { return real->SetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown *unknown) override { return real->SetPrivateDataInterface(name, unknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT *size, void *data) override { return real->GetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void **parent) override { return real->GetParent(id, parent); }
    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void **device) override { return real->GetDevice(id, device); }
    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer, REFIID id, void **surface) override
    {
        if (own.empty()) return real->GetBuffer(buffer, id, surface);
        return buffer < own.size() ? own[buffer]->QueryInterface(id, surface) : DXGI_ERROR_INVALID_CALL;
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen, IDXGIOutput *target) override { return real->SetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen, IDXGIOutput **target) override { return real->GetFullscreenState(fullscreen, target); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override
    {
        const HRESULT result = real->GetDesc(desc);
        if (SUCCEEDED(result) && desc && !own.empty())
        {
            desc->BufferDesc.Width = shape.Width; desc->BufferDesc.Height = shape.Height; desc->BufferDesc.Format = shape.Format;
            desc->BufferCount = shape.BufferCount;
        }
        return result;
    }
    // With pictures of its own the real swap chain stays as it is: the pictures are made anew (0: as before).
    HRESULT Reshape(UINT count, UINT width, UINT height, DXGI_FORMAT format)
    {
        if (count) shape.BufferCount = count;
        if (width) shape.Width = width;
        if (height) shape.Height = height;
        if (format != DXGI_FORMAT_UNKNOWN) shape.Format = format;
        return Make() ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) override
    {
        return own.empty() ? real->ResizeBuffers(count, width, height, format, Kept(flags)) : Reshape(count, width, height, format);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *mode) override { return real->ResizeTarget(mode); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override { return real->GetContainingOutput(output); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS *statistics) override { return real->GetFrameStatistics(statistics); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *count) override { return real->GetLastPresentCount(count); }
    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) override
    {
        if (own.empty()) return real->GetDesc1(desc);
        if (!desc) return E_POINTER;
        *desc = shape;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) override { return real->GetFullscreenDesc(desc); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *window) override { return real->GetHwnd(window); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID id, void **window) override { return real->GetCoreWindow(id, window); }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return real->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput **output) override { return real->GetRestrictToOutput(output); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *colour) override { return real->SetBackgroundColor(colour); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *colour) override { return real->GetBackgroundColor(colour); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override { return real->SetRotation(rotation); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) override { return real->GetRotation(rotation); }
    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override { return real->SetSourceSize(width, height); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override
    {
        if (own.empty()) return real->GetSourceSize(width, height);
        if (width) *width = shape.Width;
        if (height) *height = shape.Height;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT latency) override { return real->SetMaximumFrameLatency(latency); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *latency) override { return real->GetMaximumFrameLatency(latency); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override
    {
        if (own.empty() || !free_to_show) return real->GetFrameLatencyWaitableObject();
        // (A handle of the caller's own, as the real one is: closed by whoever asked.)
        HANDLE copy = nullptr;
        DuplicateHandle(GetCurrentProcess(), free_to_show, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return copy;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override { return real->SetMatrixTransform(matrix); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override { return real->GetMatrixTransform(matrix); }
    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return own.empty() ? real->GetCurrentBackBufferIndex() : turn.load(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE space, UINT *support) override { return real->CheckColorSpaceSupport(space, support); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE space) override { return real->SetColorSpace1(space); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT *masks, IUnknown *const *queues) override
    {
        return own.empty() ? real->ResizeBuffers1(count, width, height, format, Kept(flags), masks, queues) : Reshape(count, width, height, format);
    }
    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type, UINT size, void *data) override { return real->SetHDRMetaData(type, size, data); }
};

// The factory the library is handed: the real one, whose swap chains for a
// window come back with the tap on them.
class XeTapFactory final : public IDXGIFactory2
{
    std::atomic<ULONG> held{1};
    IDXGIFactory2 *real;
    XeTapSink *sink;
    bool own;               // its swap chains hand out pictures of their own (see the top of this file)

public:
    DXGI_SWAP_CHAIN_DESC1 asked = {};       // what the library last asked a swap chain for
    // So many buffers instead of what it asks for (three): it goes by what the swap chain says it has, and
    // draws into each in turn (tests\xetruth.cpp tap=16: the same frames) - so a picture stays as it was
    // drawn for that many Presents.
    UINT ring = 0;
    XeTapChain *made = nullptr;             // the swap chain last made (the library's to hold)
    XeTapFactory(IDXGIFactory2 *factory, XeTapSink *listener, bool pictures_of_its_own = true)
        : real(factory), sink(listener), own(pictures_of_its_own) { real->AddRef(); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override
    {
        if (!out) return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIFactory) ||
            id == __uuidof(IDXGIFactory1) || id == __uuidof(IDXGIFactory2))
        {
            *out = static_cast<IDXGIFactory2 *>(this);
            AddRef();
            return S_OK;
        }
        // A later factory interface is the real one's: nothing it adds makes a swap chain.
        return real->QueryInterface(id, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++held; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --held;
        if (left == 0) { real->Release(); delete this; }
        return left;
    }

    // The one that matters.
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen, IDXGIOutput *restrict_to,
                                                     IDXGISwapChain1 **chain) override
    {
        if (!chain) return E_POINTER;
        *chain = nullptr;
        if (desc) asked = *desc;
        // For tearing Presents where Windows has them (see the top of this file).
        DXGI_SWAP_CHAIN_DESC1 wanted = *desc;
        if (ring) wanted.BufferCount = ring;
        BOOL tears = FALSE;
        IDXGIFactory5 *newer = nullptr;
        if (SUCCEEDED(real->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&newer))) && newer)
        {
            if (FAILED(newer->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tears, sizeof(tears)))) tears = FALSE;
            newer->Release();
        }
        if (tears) wanted.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        // With pictures of the tap's own the real one only stands for its window: two small buffers will do.
        const DXGI_SWAP_CHAIN_DESC1 says = wanted;
        if (own) { wanted.Width = 64; wanted.Height = 64; wanted.BufferCount = 2; }
        IDXGISwapChain1 *made = nullptr;
        HRESULT result = real->CreateSwapChainForHwnd(device, window, &wanted, fullscreen, restrict_to, &made);
        if ((FAILED(result) || !made) && tears)
        {
            tears = FALSE;
            wanted.Flags = desc->Flags;
            result = real->CreateSwapChainForHwnd(device, window, &wanted, fullscreen, restrict_to, &made);
        }
        if (FAILED(result) || !made) return result;
        IDXGISwapChain4 *whole = nullptr;
        made->QueryInterface(__uuidof(IDXGISwapChain4), reinterpret_cast<void **>(&whole));
        made->Release();
        if (!whole) return E_NOINTERFACE;
        ID3D12CommandQueue *queue = nullptr;
        if (device) device->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue));
        this->made = new XeTapChain(whole, queue, sink, tears != FALSE, own ? &says : nullptr);     // takes `whole` over
        *chain = this->made;
        if (queue) queue->Release();
        return result;
    }

    // Everything else goes on as it came.
    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID name, UINT size, const void *data) override { return real->SetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID name, const IUnknown *unknown) override { return real->SetPrivateDataInterface(name, unknown); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID name, UINT *size, void *data) override { return real->GetPrivateData(name, size, data); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void **parent) override { return real->GetParent(id, parent); }
    // IDXGIFactory
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT index, IDXGIAdapter **adapter) override { return real->EnumAdapters(index, adapter); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND window, UINT flags) override { return real->MakeWindowAssociation(window, flags); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *window) override { return real->GetWindowAssociation(window); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain) override { return real->CreateSwapChain(device, desc, chain); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE module, IDXGIAdapter **adapter) override { return real->CreateSoftwareAdapter(module, adapter); }
    // IDXGIFactory1
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT index, IDXGIAdapter1 **adapter) override { return real->EnumAdapters1(index, adapter); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return real->IsCurrent(); }
    // IDXGIFactory2
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return real->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to, IDXGISwapChain1 **chain) override
    { return real->CreateSwapChainForCoreWindow(device, window, desc, restrict_to, chain); }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE resource, LUID *luid) override { return real->GetSharedResourceAdapterLuid(resource, luid); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND window, UINT message, DWORD *cookie) override { return real->RegisterStereoStatusWindow(window, message, cookie); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE event, DWORD *cookie) override { return real->RegisterStereoStatusEvent(event, cookie); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD cookie) override { real->UnregisterStereoStatus(cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND window, UINT message, DWORD *cookie) override { return real->RegisterOcclusionStatusWindow(window, message, cookie); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE event, DWORD *cookie) override { return real->RegisterOcclusionStatusEvent(event, cookie); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD cookie) override { real->UnregisterOcclusionStatus(cookie); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc, IDXGIOutput *restrict_to, IDXGISwapChain1 **chain) override
    { return real->CreateSwapChainForComposition(device, desc, restrict_to, chain); }
};
