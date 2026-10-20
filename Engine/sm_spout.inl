// sm_spout.inl - what the engine shows, shared with other programs as a Spout
// sender ("Spout output" in the app, the option spout).
//
// The output window is the one thing on the PC a recording program cannot
// take in the usual way: it lies over the game, and unless the game's own
// window is what is captured it is kept out of screen captures on purpose
// (lf_show.inl) - so a recording of the screen shows the game's own frames
// and none of the frames in between. Spout is how programs on one PC hand
// pictures to each other without a copy through memory: the sender puts each
// frame into a texture the card shares, and says in a few named pieces of
// shared memory what it is called and where the texture is. OBS Studio takes
// it with the Spout2 plugin ("Spout2 Capture", sender "MFG SUPER MOTION"),
// and records every frame the screen is shown - real and in between.
//
// A Spout texture is a Direct3D 11 one shared the old way (a handle that is a
// number, not an object), which Direct3D 12 can neither make nor open. So:
//   - a device of Direct3D 11 on the engine's own card, used for nothing else;
//   - `relay`: a texture of that device shared the new way, which the engine's
//     Direct3D 12 device opens - each frame shared is copied into it in the
//     list that puts it on the back buffer (SpoutCopy, from ShowBlit);
//   - `shared`: the Spout texture itself. Once that list is sent (SpoutAfter)
//     the Direct3D 11 device copies relay into it - after the list's work, by
//     a fence the two devices share: nothing waits on the processor.
// At most 125 frames a second are shared (a recording takes 60 or 120); each
// costs the card two copies of the picture.
//
// What a receiver looks for (Spout 2, SpoutSenderNames.cpp of its SDK):
//   "SpoutSenderNames"   the names of all senders, 256 bytes each
//   "ActiveSenderName"   the one a receiver takes when it is not told which
//   "<the sender's name>" its texture: handle, width, height, format
// each a piece of shared memory with a mutex "<its name>_mutex" beside it.
#include <d3d11_4.h>

// Not while DLSS 5 MANAGER hands its frames over: OBS records LIVE FLOW's
// sender then (sm_dlss5_link.inl).
static bool SpoutWanted()
{
    return smOpt.spout.load() != 0 && !Dlss5LinkOn();
}

static const char kSpoutName[] = "MFG SUPER MOTION";
static const DWORD kSpoutNameBytes = 256;

struct SpoutTextureInfo
{
    uint32_t handle, width, height;
    DWORD format, usage;
    wchar_t description[128];
    uint32_t partner;
};

// A named piece of shared memory with its mutex.
struct SpoutMemory
{
    HANDLE map = nullptr, mutex = nullptr;
    BYTE *at = nullptr;
    DWORD bytes = 0;
    bool Open(const char *name, DWORD size)
    {
        map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name);
        if (map == nullptr) return false;
        at = static_cast<BYTE *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (at == nullptr) { CloseHandle(map); map = nullptr; return false; }
        // One that was there already has the size its maker gave it.
        MEMORY_BASIC_INFORMATION region = {};
        bytes = VirtualQuery(at, &region, sizeof(region)) ? static_cast<DWORD>(std::min<SIZE_T>(region.RegionSize, 1u << 20)) : size;
        const std::string beside = std::string(name) + "_mutex";
        mutex = CreateMutexA(nullptr, FALSE, beside.c_str());
        return true;
    }
    bool Lock() const { return mutex == nullptr || WaitForSingleObject(mutex, 67) != WAIT_TIMEOUT; }
    void Unlock() const { if (mutex) ReleaseMutex(mutex); }
    void Close()
    {
        if (at) UnmapViewOfFile(at);
        if (map) CloseHandle(map);
        if (mutex) CloseHandle(mutex);
        at = nullptr; map = nullptr; mutex = nullptr; bytes = 0;
    }
};

static struct SpoutState
{
    std::mutex lock;                // the Direct3D 11 context is one thread's at a time
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::com_ptr<ID3D11DeviceContext4> context4;
    winrt::com_ptr<ID3D11Texture2D> relay11, shared;
    winrt::com_ptr<ID3D12Resource> relay;
    winrt::com_ptr<ID3D12Fence> fence;
    winrt::com_ptr<ID3D11Fence> fence11;
    UINT64 value = 0;
    // What a frame still on its way may hold: let go when the next size comes.
    winrt::com_ptr<ID3D11Texture2D> old11[2];
    winrt::com_ptr<ID3D12Resource> old12;
    SpoutMemory names, active, info;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    double shared_at = 0.0;
    bool pending = false, failed = false, said = false;
} g_spout;

// The sender's name into the list of all senders (kept in order, as Spout keeps it), or out of it.
static void SpoutList(bool in)
{
    SpoutState &s = g_spout;
    if (!s.names.at || !s.names.Lock()) return;
    const DWORD slots = s.names.bytes / kSpoutNameBytes;
    std::vector<std::string> all;
    for (DWORD i = 0; i < slots; ++i)
    {
        const char *name = reinterpret_cast<const char *>(s.names.at + i * kSpoutNameBytes);
        if (name[0] == 0) break;
        const std::string one(name, strnlen(name, kSpoutNameBytes - 1));
        if (one != kSpoutName) all.push_back(one);
    }
    if (in) all.push_back(kSpoutName);
    std::sort(all.begin(), all.end());
    for (DWORD i = 0; i < slots; ++i)
    {
        char *name = reinterpret_cast<char *>(s.names.at + i * kSpoutNameBytes);
        memset(name, 0, kSpoutNameBytes);
        if (i < all.size()) memcpy(name, all[i].c_str(), std::min<size_t>(all[i].size(), kSpoutNameBytes - 1));
    }
    s.names.Unlock();
    // The one a receiver takes when it is not told which: this one, when none is - or when the one named is gone.
    if (s.active.at && s.active.Lock())
    {
        char *name = reinterpret_cast<char *>(s.active.at);
        const std::string now(name, strnlen(name, kSpoutNameBytes - 1));
        const bool ours = now == kSpoutName;
        bool there = false;
        if (!now.empty() && !ours)
        {
            const HANDLE its = OpenFileMappingA(FILE_MAP_READ, FALSE, now.c_str());
            there = its != nullptr;
            if (its) CloseHandle(its);
        }
        if (in ? !there : ours)
        {
            memset(name, 0, kSpoutNameBytes);
            if (in) memcpy(name, kSpoutName, sizeof(kSpoutName) - 1);
        }
        s.active.Unlock();
    }
}

// Everything let go: no sender of this name any more.
static void SpoutClose()
{
    SpoutState &s = g_spout;
    std::lock_guard<std::mutex> hold(s.lock);
    if (s.info.at) SpoutList(false);
    s.info.Close(); s.active.Close(); s.names.Close();
    if (s.context) { s.context->ClearState(); s.context->Flush(); }
    // (The Direct3D 12 side of the relay may still be in a list on its way: kept until the next one is made.)
    if (s.relay) s.old12 = s.relay;
    s.relay = nullptr; s.relay11 = nullptr; s.shared = nullptr; s.old11[0] = nullptr; s.old11[1] = nullptr;
    s.fence = nullptr; s.fence11 = nullptr;
    s.context4 = nullptr; s.context = nullptr; s.device = nullptr;
    s.w = s.h = 0; s.format = DXGI_FORMAT_UNKNOWN; s.pending = false;
    if (s.said) Trace("[spout] the sender is closed");
    s.said = false;
}

// The device, the two textures and the sender's entry, for frames of this size and format. Under the lock.
static bool SpoutReady(UINT w, UINT h, DXGI_FORMAT format)
{
    SpoutState &s = g_spout;
    if (s.failed) return false;
    if (s.shared && s.w == w && s.h == h && s.format == format) return true;
    auto fail = [&](const char *what) {
        s.failed = true;
        Trace("[spout] unavailable: %s", what);
        return false;
    };
    if (!s.device)
    {
        winrt::com_ptr<IDXGIAdapter> card;
        if (lfCard.adapter) lfCard.adapter->QueryInterface(__uuidof(IDXGIAdapter), card.put_void());
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (!card || FAILED(D3D11CreateDevice(card.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
                                              D3D11_SDK_VERSION, s.device.put(), nullptr, s.context.put())))
            return fail("no Direct3D 11 device on the engine's card");
        s.context4 = s.context.try_as<ID3D11DeviceContext4>();
        const auto device5 = s.device.try_as<ID3D11Device5>();
        if (!s.context4 || !device5) return fail("this Windows has no fences for Direct3D 11");
        HANDLE nt = nullptr;
        if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(s.fence.put()))) ||
            FAILED(lfCore.device->CreateSharedHandle(s.fence.get(), nullptr, GENERIC_ALL, nullptr, &nt)))
            return fail("no shared fence");
        device5->OpenSharedFence(nt, IID_PPV_ARGS(s.fence11.put()));
        CloseHandle(nt);
        if (!s.fence11) return fail("the Direct3D 11 side has no fence");
        // (HKCU\Software\Leading Edge\Spout, MaxSenders: how many names the list holds; 64 unless set.)
        DWORD senders = 64, size = sizeof(senders);
        RegGetValueA(HKEY_CURRENT_USER, "Software\\Leading Edge\\Spout", "MaxSenders", RRF_RT_REG_DWORD, nullptr, &senders, &size);
        senders = std::clamp<DWORD>(senders, 10, 1024);
        if (!s.names.Open("SpoutSenderNames", senders * kSpoutNameBytes) || !s.active.Open("ActiveSenderName", kSpoutNameBytes))
            return fail("no shared memory for the senders' names");
    }
    // The textures of the size before may still be read by a list on its way.
    s.old11[0] = s.relay11; s.old11[1] = s.shared; s.old12 = s.relay;
    s.relay11 = nullptr; s.shared = nullptr; s.relay = nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HANDLE nt = nullptr;
    if (FAILED(s.device->CreateTexture2D(&desc, nullptr, s.relay11.put()))) return fail("no texture to hand frames over in");
    const auto relay_shared = s.relay11.try_as<IDXGIResource1>();
    if (!relay_shared || FAILED(relay_shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt)))
        return fail("that texture cannot be shared");
    const HRESULT opened = lfCore.device->OpenSharedHandle(nt, IID_PPV_ARGS(s.relay.put()));
    CloseHandle(nt);
    if (FAILED(opened)) return fail("Direct3D 12 could not open that texture");
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HANDLE number = nullptr;
    if (FAILED(s.device->CreateTexture2D(&desc, nullptr, s.shared.put()))) return fail("no Spout texture");
    const auto its = s.shared.try_as<IDXGIResource>();
    if (!its || FAILED(its->GetSharedHandle(&number)) || number == nullptr) return fail("the Spout texture has no handle");
    // The sender's own entry, then its name among the others.
    if (!s.info.at && !s.info.Open(kSpoutName, sizeof(SpoutTextureInfo))) return fail("no shared memory for the sender");
    if (s.info.Lock())
    {
        SpoutTextureInfo info = {};
        info.handle = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(number));
        info.width = w; info.height = h; info.format = static_cast<DWORD>(format);
        memcpy(s.info.at, &info, std::min<size_t>(sizeof(info), s.info.bytes));
        s.info.Unlock();
    }
    SpoutList(true);
    s.w = w; s.h = h; s.format = format;
    Trace("[spout] sender \"%s\": %ux%u, format %u - every frame shown is shared (OBS: Spout2 Capture)", kSpoutName, w, h, static_cast<unsigned>(format));
    s.said = true;
    return true;
}

// The frame on the back buffer (COPY_DEST as it comes, and left so), into the texture the Direct3D 11 device
// hands on: recorded into the list that draws the frame. Called from ShowBlit.
static void SpoutCopy(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer)
{
    SpoutState &s = g_spout;
    if (!SpoutWanted()) return;
    const double now = LatencyNow();
    // About sixty a second - what OBS records at (round 47: every frame shown, x6 at 1440p, cost the game it runs
    // beside 12% of its frames; 125 a second before that).
    if (now - s.shared_at < 0.0155) return;
    std::lock_guard<std::mutex> hold(s.lock);
    const D3D12_RESOURCE_DESC bd = back_buffer->GetDesc();
    if (s.pending || !SpoutReady(static_cast<UINT>(bd.Width), bd.Height, bd.Format)) return;
    D3D12_RESOURCE_BARRIER open[] = { Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                      Barrier(s.relay.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
    list->ResourceBarrier(2, open);
    list->CopyResource(s.relay.get(), back_buffer);
    D3D12_RESOURCE_BARRIER shut[] = { Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                      Barrier(s.relay.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON) };
    list->ResourceBarrier(2, shut);
    s.pending = true;
    s.shared_at = now;
}

// The list with a frame for Spout in it has been sent on this queue: once its work is done, the Direct3D 11
// device copies the frame into the Spout texture. Nothing waits here.
static void SpoutAfter(ID3D12CommandQueue *queue)
{
    SpoutState &s = g_spout;
    if (!s.pending) return;
    std::lock_guard<std::mutex> hold(s.lock);
    if (!s.pending || !s.shared || !s.relay11) { s.pending = false; return; }
    s.pending = false;
    if (FAILED(queue->Signal(s.fence.get(), ++s.value))) return;
    if (FAILED(s.context4->Wait(s.fence11.get(), s.value))) return;
    s.context->CopyResource(s.shared.get(), s.relay11.get());
    s.context->Flush();
}

// The option went off: the sender goes (from the loop's thread, between two frames).
static void SpoutFollow()
{
    if (!SpoutWanted() && (g_spout.said || g_spout.failed))
    {
        const bool was = g_spout.said;
        SpoutClose();
        g_spout.failed = false;
        if (was) Trace("[option] Spout output off");
    }
}
