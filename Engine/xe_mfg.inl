// xe_mfg.inl - XeSS MFG: Intel's XeSS Frame Generation as the engine's second
// maker of frames in between, for every card that runs it (NVIDIA, AMD,
// Intel). Picked in the app ("Generator"), read once at the start
// (sm_options.h, XeSelected): the two generators never share a process.
//
// What is XeSS's own is here and in the two files it brings in:
//   xe_api.h    Intel's two libraries, as far as the engine calls them
//   xe_tap.h    the tap on the swap chain the library shows its frames in
//   xe_guise.h  a card of Intel's own that the libraries turn away, shown to them as another maker's
// Everything around it is the engine's and the same for both generators: the
// source, the real frames, the presenter with its pacing, VRR, HDR and Low
// Latency, the rebuilding of small frames at the picture's size, what stays
// still (lf_dlssg.inl, sm_rebuild.inl, lf_fg_still.inl). lf_dlssg.inl asks
// for a pair's frames in between at one place - of NGX, or here:
//   XeStart    the libraries, while the person clicks the game
//   XeReady    the library's swap chain at the size frames in between are made at
//   XeFeed     the real frame into its back buffer (in the pair's own list)
//   XeMake     the frames in between of that frame and the one before: which pictures hold them
//   XeAfter    a queue waits until they are drawn
//   XeClose    all of it away
//
// The library has no call that makes a frame and hands it over. It is built
// to sit between a game and the screen: the game draws into a swap chain of
// the library's and calls Present, and the library shows the frames in
// between and then the frame itself, in the real swap chain. Used like that
// (the first version here, 7 to 9 October) it made every frame at the
// picture's full size - 3.5 ms each on an RTX 3060 at 2560x1440, where one at
// 896x504 is 1 ms - and showed them itself, so nothing of the engine's own
// presenter reached them: a game that kept 48 frames a second under DLSS MFG
// x6 fell to 27, and there was no HDR.
//
// So the library is given a small swap chain and a window nobody sees, and a
// tap is put on that swap chain (xe_tap.h). Until round 13 every picture the
// library was about to show there was copied to where the pair wanted it and
// then shown in that window all the same, and the loop waited until the last
// of them had come. Since round 14 the library draws into sixteen pictures
// of the tap's own, in turn, and nothing is shown anywhere: a pair is handed
// the pictures its frames are (being) drawn into - they stay untouched for
// five pairs - and the number the tap's fence reaches when the last of them
// is whole. No copy, no Present, no waiting: under a game that fills the
// card each of those was a piece of work sent by itself, a third of a
// millisecond of the game's time each, six a pair; and the pair reached the
// presenter one to several milliseconds after its frame was taken, never the
// same twice ("XeSS MFG feels cut up, DLSS MFG does not").
// What the library shows after one Present of ours, measured
// (tests\xeprobe.cpp tap=1, without the unlock plugin's own pacing):
//   - the first frame in between at once, on our own thread, inside the call
//     (0.6 ms after it began);
//   - the second and third from a thread of the library's, 1.1 and 1.3 ms
//     after it; then the real frame, later (it is passed over: the engine has
//     the real frame at its full size);
//   - a fourth and fifth only 7 ms later and with the next Present. So three
//     are asked for at most (XeMost), and x5 and x6 go by two, as with DLSS
//     MFG (lf_dlssg.inl, FgMadeCount).
//   - nothing in between after a new start (the first frame, a scene cut):
//     the real frame only.
// The library says how many pictures it decided to show as soon as the call
// returns (xefgSwapChainGetLastPresentStatus); which is which follows from
// the order they come in.
//
// The motion (round 13). The library is made for a game that tells it how
// every pixel moved; told nothing it finds slow movement itself and leaves
// what moves fast half where it was and half where it goes - a lamp post
// thin and doubled, the road by the car not moved at all ("some places get
// no frames in between"). Measured on the street of tests\world.h, frames of
// 896x504 with the picture tilting 16 pixels a frame (tests\xetruth.cpp):
//     told nothing                          5.2 to 5.8% of the pixels off
//     told the picture's true movement      2.9 to 3.0%
//     told a wrong one, either way          4.2 to 5.1%
// What it wants, found the same way: for every pixel of the motion picture
// the way from where the thing is now to where it was in the frame before,
// counted in pixels of that motion picture - whatever its size (a quarter and
// an eighth of the frame's give the same frames as its own size).
//
// So it is told the picture's own movement, which the engine has for every
// pair before its frames are asked for (sm_guard.inl: twenty-four blocks,
// found on the processor from the small luma, at no cost on the card - what
// a rebuilt frame starts from too): spread smoothly over a motion picture an
// eighth of the frame's size (XeField). "GPU estimate" in the app hands it
// the engine's finer field instead. The depth is flat, the camera stands
// still. SM_XE_NO_FIELD=1 (to compare): told nothing.
//
// The unlock plugin (XeFGUnlock.asi, the user's own, beside the libraries)
// lets the library make more than one frame in between on cards that are not
// Intel's. It patches the library about a second after it is loaded, so both
// are loaded when the engine starts, and XeReady waits for it only when that
// second is not over yet. Without it (or with a build of the library it does
// not know) one frame is made a pair and the others go by that one.
#include "xe_api.h"
#include "xe_tap.h"
#include "xe_guise.h"
#include <deque>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>


static struct XeState : XeTapSink
{
    // For the whole run.
    XeApi api;
    HMODULE plugin = nullptr;
    bool loaded = false;
    ULONGLONG loaded_at = 0;
    // From XeReady to XeClose.
    xell_context_handle_t ll = nullptr;
    xefg_swapchain_handle_t fg = nullptr;
    XeTapFactory *factory = nullptr;
    XeTapChain *tap = nullptr;              // the swap chain the library shows its frames in: they stay in the tap's own pictures
    IDXGISwapChain3 *chain = nullptr;       // the library's: its back buffer takes the real frame
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    // The motion picture and the flat depth beside it (XeField), fw x fh.
    winrt::com_ptr<ID3D12Resource> field, flat;
    static constexpr int kFields = 4;       // its uploads in turn: one may still be on its way when the next is written
    winrt::com_ptr<ID3D12Resource> field_upload[kFields];
    BYTE *field_mapped[kFields] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT field_layout = {};
    UINT fw = 0, fh = 0;
    int field_at = 0;
    unsigned most = 1;                      // frames in between a pair the library makes here, in time
    unsigned set = 0;                       // how many it is set to make
    ULONGLONG many_at = 0;                  // when a pair last wanted two or more (XeCount)
    uint32_t number = 0;                    // the frame's number, for the library
    // The window its real swap chain is made for - never shown - and the thread that owns it.
    HWND window = nullptr;
    HANDLE window_thread = nullptr;
    // The tap: each picture the library shows gets a number, and the fence
    // reaches that number on the queue it is shown on - once the picture is
    // whole.
    std::mutex lock;
    DWORD asking = 0;                       // the thread inside the library's Present, 0: none
    bool first_came = false;                // that Present's first picture was shown
    UINT first_index = 0;                   // in which of the tap's pictures
    UINT64 first_mark = 0;                  // with which number
    winrt::com_ptr<ID3D12Fence> fence;
    UINT64 value = 0;                       // the newest number given
    UINT64 owed = 0;                        // the number the pair before was told its last frame would bring
    HANDLE passed = nullptr;

    void Shown(ID3D12Resource *picture, UINT index, ID3D12CommandQueue *queue) override;
} g_xe;

static bool XeLive() { return g_xe.chain != nullptr; }
static unsigned XeMost() { return g_xe.most; }

// What the library has to say, into the engine's log (warnings and errors).
static void XeSaid(const char *message, xefg_swapchain_logging_level_t level, void *)
{
    Trace("[xess] the library (%s): %s", level >= XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR ? "error" : "warning", message);
}

// A call that failed is said once, with what it was.
static bool XeFine(xefg_swapchain_result_t result, const char *what)
{
    if (result >= 0) return true;
    Trace("[xess] %s: %d (%s)", what, static_cast<int>(result), XeResultText(result));
    return false;
}

// The motion picture (an eighth of the frame's size, all zeros to begin
// with) and the flat depth of the same size, at rest as shader resources.
static bool XeFieldReady()
{
    g_xe.fw = std::max(16u, (g_xe.w + 4) / 8);
    g_xe.fh = std::max(9u, (g_xe.h + 4) / 8);
    g_xe.field.attach(NewTexture(g_xe.fw, g_xe.fh, DXGI_FORMAT_R16G16_FLOAT, false));
    g_xe.flat.attach(NewTexture(g_xe.fw, g_xe.fh, DXGI_FORMAT_R32_FLOAT, false));
    if (!g_xe.field || !g_xe.flat) return false;
    D3D12_RESOURCE_DESC desc = g_xe.field->GetDesc();
    UINT64 bytes = 0;
    lfCore.device->GetCopyableFootprints(&desc, 0, 1, 0, &g_xe.field_layout, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = bytes; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (int i = 0; i < XeState::kFields; ++i)
    {
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(g_xe.field_upload[i].put()))) ||
            FAILED(g_xe.field_upload[i]->Map(0, nullptr, reinterpret_cast<void **>(&g_xe.field_mapped[i]))))
            return false;
        memset(g_xe.field_mapped[i], 0, static_cast<size_t>(bytes));
    }
    // The depth: one value everywhere, written once.
    StagedTexture depth;
    if (!StagedCreate(depth, g_xe.fw, g_xe.fh, DXGI_FORMAT_R32_FLOAT, g_xe.fw * 4)) return false;
    const std::vector<float> half(static_cast<size_t>(g_xe.fw) * g_xe.fh, 0.5f);
    bool done = StagedFill(depth, reinterpret_cast<const BYTE *>(half.data()), g_xe.fw * 4, g_xe.fh) && CmdOpen();
    if (done)
    {
        D3D12_RESOURCE_BARRIER to_write[] = {
            Barrier(g_xe.field.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST),
            Barrier(g_xe.flat.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST) };
        lfCore.cmd->ResourceBarrier(2, to_write);
        D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = g_xe.field_upload[0].get(); from.PlacedFootprint = g_xe.field_layout; into.pResource = g_xe.field.get();
        lfCore.cmd->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
        from.pResource = depth.staging; from.PlacedFootprint = depth.layout; into.pResource = g_xe.flat.get();
        lfCore.cmd->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
        D3D12_RESOURCE_BARRIER to_rest[] = {
            Barrier(g_xe.field.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(g_xe.flat.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
        lfCore.cmd->ResourceBarrier(2, to_rest);
        const UINT64 mark = CmdSubmit();
        done = mark != 0 && AwaitMark(lfCore.timeline, mark, 5000);
    }
    StagedRelease(depth);
    return done;
}

// The picture's own movement from the frame before to this one - 6 x 4
// blocks, each a share of the picture's width (lf_dlssg.inl, RebuildPrior) -
// written into the motion picture as the library wants it: for every pixel
// the way back to where it was, in pixels of the motion picture; between the
// blocks' middles it changes smoothly, as the picture's movement does (a
// camera driving ahead moves the road fast and the horizon hardly). Recorded
// in the pair's own list.
static void XeField(ID3D12GraphicsCommandList *list, const float (*moved)[2])
{
    static const bool none = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_XE_NO_FIELD", v, sizeof(v)) > 0 && v[0] == '1'; }();
    if (!g_xe.field || moved == nullptr || none) return;
    g_xe.field_at = (g_xe.field_at + 1) % XeState::kFields;
    BYTE *into = g_xe.field_mapped[g_xe.field_at];
    if (into == nullptr) return;
    const float scale = -static_cast<float>(g_xe.fw);
    for (UINT y = 0; y < g_xe.fh; ++y)
    {
        const float gy = std::clamp((y + 0.5f) / g_xe.fh * 4.0f - 0.5f, 0.0f, 3.0f);
        const int y0 = static_cast<int>(gy), y1 = std::min(y0 + 1, 3);
        const float ty = gy - y0;
        uint16_t *row = reinterpret_cast<uint16_t *>(into + g_xe.field_layout.Offset + static_cast<size_t>(y) * g_xe.field_layout.Footprint.RowPitch);
        for (UINT x = 0; x < g_xe.fw; ++x)
        {
            const float gx = std::clamp((x + 0.5f) / g_xe.fw * 6.0f - 0.5f, 0.0f, 5.0f);
            const int x0 = static_cast<int>(gx), x1 = std::min(x0 + 1, 5);
            const float tx = gx - x0;
            for (int c = 0; c < 2; ++c)
            {
                const float top = moved[y0 * 6 + x0][c] + (moved[y0 * 6 + x1][c] - moved[y0 * 6 + x0][c]) * tx;
                const float low = moved[y1 * 6 + x0][c] + (moved[y1 * 6 + x1][c] - moved[y1 * 6 + x0][c]) * tx;
                row[x * 2 + c] = DirectX::PackedVector::XMConvertFloatToHalf((top + (low - top) * ty) * scale);
            }
        }
    }
    auto to_write = Barrier(g_xe.field.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_write);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = g_xe.field_upload[g_xe.field_at].get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = g_xe.field_layout;
    to.pResource = g_xe.field.get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto to_rest = Barrier(g_xe.field.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_rest);
}

// Do Intel's two libraries start on this device? (Asked of them: a context of each, made and let go.)
static bool XeTakes(XeApi &api, ID3D12Device *device)
{
    xell_context_handle_t low = nullptr;
    xefg_swapchain_handle_t fg = nullptr;
    const bool takes = api.LlCreateContext(device, &low) >= 0 && low && api.CreateContext(device, &fg) >= 0 && fg;
    if (fg) api.Destroy(fg);
    if (low) api.LlDestroyContext(low);
    return takes;
}

// The same for a card of Intel's own they turn away - an integrated UHD or
// Iris Xe: "unsupported card", "unsupported driver" - once it is shown to them
// as another maker's (xe_guise.h). Its shaders run there all the same: on a
// UHD 730 a frame in between of 896x504 is whole 10.7 ms after it was asked
// for, one of 448x252 after 4.6 (tests\xeprobe.exe card=1 guise=1 tap=3).
// True: it runs so, and stays shown so. False: the card is what it was.
static bool XeTakesInGuise(XeApi &api, ID3D12Device *device, IDXGIAdapter1 *card)
{
    DXGI_ADAPTER_DESC1 desc = {};
    if (card == nullptr || FAILED(xe_guise::True(card, &desc)) || desc.VendorId != 0x8086 || !xe_guise::Put(card)) return false;
    if (XeTakes(api, device)) return true;
    xe_guise::Off();
    return false;
}

// Does Intel's library make frames on this card? Asked of the library itself
// (--cards, for the app's "Preferred GPU"): it takes NVIDIA's and AMD's cards
// and Intel's Arc as they are, and older graphics of Intel's own in the guise
// of another maker's.
static bool XeRunsOn(IDXGIAdapter1 *card)
{
    static XeApi api;
    static int loaded = -1;
    if (loaded < 0)
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (auto slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
        std::string missing;
        loaded = XeApiLoad(api, (std::wstring(path) + L"xess\\").c_str(), missing) ? 1 : 0;
    }
    if (loaded != 1 || card == nullptr) return false;
    winrt::com_ptr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(card, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(device.put()))) || !device) return false;
    if (XeTakes(api, device.get())) return true;
    const bool in_guise = XeTakesInGuise(api, device.get(), card);
    xe_guise::Off();        // only asked: the next card is asked as it is
    return in_guise;
}

// The libraries and the plugin, once. False: XeSS MFG cannot run here.
static bool XeStart()
{
    if (g_xe.loaded) return true;
    D3D12_FEATURE_DATA_SHADER_MODEL model = { D3D_SHADER_MODEL_6_4 };
    if (FAILED(lfCore.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_4)
    { Trace("[dlssg] unavailable: XeSS MFG needs a card with Shader Model 6.4"); return false; }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (auto slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
    const std::wstring folder = std::wstring(path) + L"xess\\";
    // The plugin first: it watches for the library being loaded.
    g_xe.plugin = LoadLibraryExW((folder + L"XeFGUnlock.asi").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (g_xe.plugin)
        if (auto begin = reinterpret_cast<void (*)()>(GetProcAddress(g_xe.plugin, "InitializeASI"))) begin();
    std::string missing;
    if (!XeApiLoad(g_xe.api, folder.c_str(), missing))
    { Trace("[dlssg] unavailable: XeSS MFG's files - %s", missing.c_str()); return false; }
    // A card of Intel's own that the libraries turn away as it is (an Arc they take: nothing is done then).
    {
        winrt::com_ptr<IDXGIAdapter1> card;
        if (lfCard.adapter) lfCard.adapter->QueryInterface(__uuidof(IDXGIAdapter1), card.put_void());
        DXGI_ADAPTER_DESC1 desc = {};
        if (card && SUCCEEDED(xe_guise::True(card.get(), &desc)) && desc.VendorId == 0x8086 && !XeTakes(g_xe.api, lfCore.device) &&
            XeTakesInGuise(g_xe.api, lfCore.device, card.get()))
            Trace("[xess] Intel's libraries turn this card away as it is: it is shown to them as another maker's, and they run on it");
    }
    g_xe.loaded = true;
    g_xe.loaded_at = GetTickCount64();
    xefg_swapchain_version_t fg_version = {};
    xell_version_t ll_version = {};
    g_xe.api.GetVersion(&fg_version);
    g_xe.api.LlGetVersion(&ll_version);
    Trace("[xess] XeSS Frame Generation %u.%u.%u, Xe Low Latency %u.%u.%u; the unlock plugin is %s",
          fg_version.major, fg_version.minor, fg_version.patch, ll_version.major, ll_version.minor, ll_version.patch,
          g_xe.plugin ? "loaded" : "not there (one frame in between a pair from the library)");
    return true;
}

// The window the library's real swap chain is made for. It is never shown;
// it has a thread of its own because a window's thread has to answer for it.
static DWORD WINAPI XeWindowThread(LPVOID made)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SuperMotionXeSS";
    RegisterClassExW(&wc);
    g_xe.window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0,
                                  static_cast<int>(g_xe.w), static_cast<int>(g_xe.h), nullptr, nullptr, wc.hInstance, nullptr);
    SetEvent(static_cast<HANDLE>(made));
    if (!g_xe.window) return 0;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (msg.message == WM_APP) { DestroyWindow(g_xe.window); break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_xe.window = nullptr;
    return 0;
}

static void XeWindowClose()
{
    if (!g_xe.window_thread) return;
    PostThreadMessageW(GetThreadId(g_xe.window_thread), WM_APP, 0, 0);
    WaitForSingleObject(g_xe.window_thread, 2000);
    CloseHandle(g_xe.window_thread);
    g_xe.window_thread = nullptr;
    g_xe.window = nullptr;
}

// A picture the library shows (xe_tap.h), on whichever thread shows it: it
// stays where the library drew it, and gets its number.
void XeState::Shown(ID3D12Resource *, UINT index, ID3D12CommandQueue *queue)
{
    std::lock_guard<std::mutex> hold(lock);
    if (!fence || !queue) return;
    const UINT64 mark = ++value;
    queue->Signal(fence.get(), mark);
    // The first one of the Present being called: a frame in between, or -
    // after a new start - the real frame.
    if (asking == GetCurrentThreadId() && !first_came)
    {
        first_came = true;
        first_index = index;
        first_mark = mark;
    }
}

// Before the pairs' pictures are let go (lf_dlssg.inl, StopFgPresentation):
// what the library was drawing has run on the card.
static void XeForget()
{
    UINT64 sent = 0;
    {
        std::lock_guard<std::mutex> hold(g_xe.lock);
        sent = g_xe.value;
    }
    if (g_xe.fence && sent && g_xe.fence->GetCompletedValue() < sent && g_xe.passed &&
        SUCCEEDED(g_xe.fence->SetEventOnCompletion(sent, g_xe.passed)))
        WaitForSingleObject(g_xe.passed, 2000);
}

// Away with the library's swap chain and everything made for it. The
// presenter has ended and the card is through with the pair's work before
// this is called (lf_dlssg.inl, CloseFgResources).
static void XeClose()
{
    if (!g_xe.fg && !g_xe.chain && !g_xe.ll && !g_xe.window_thread) return;
    XeForget();
    if (g_xe.fg) g_xe.api.SetEnabled(g_xe.fg, 0);
    if (g_xe.chain) { g_xe.chain->Release(); g_xe.chain = nullptr; }
    if (g_xe.fg)
    {
        // Its own threads end in here, and with them the last pictures shown.
        const xefg_swapchain_result_t result = g_xe.api.Destroy(g_xe.fg);
        if (result < 0) Trace("[xess] the library's swap chain did not go: %d (%s)", static_cast<int>(result), XeResultText(result));
        g_xe.fg = nullptr;
    }
    {
        std::lock_guard<std::mutex> hold(g_xe.lock);
        g_xe.asking = 0;
        g_xe.owed = 0;
    }
    if (g_xe.tap) { g_xe.tap->Release(); g_xe.tap = nullptr; }
    if (g_xe.factory) { g_xe.factory->Release(); g_xe.factory = nullptr; }
    // Xe Low Latency after the frame generation that holds it.
    if (g_xe.ll) { g_xe.api.LlDestroyContext(g_xe.ll); g_xe.ll = nullptr; }
    XeWindowClose();
    for (int i = 0; i < XeState::kFields; ++i)
    {
        if (g_xe.field_upload[i] && g_xe.field_mapped[i]) g_xe.field_upload[i]->Unmap(0, nullptr);
        g_xe.field_upload[i] = nullptr;
        g_xe.field_mapped[i] = nullptr;
    }
    g_xe.field = nullptr;
    g_xe.flat = nullptr;
    g_xe.fw = g_xe.fh = 0;
    g_xe.w = g_xe.h = 0;
    g_xe.set = 0;
}

// The library's swap chain, w x h in `format` - the size and kind of the
// pictures frames in between are made as. False: XeSS MFG is not to be had
// (said in the log as "[dlssg] unavailable", which the app is told).
static bool XeReady(UINT w, UINT h, DXGI_FORMAT format)
{
    if (g_xe.chain && g_xe.w == w && g_xe.h == h && g_xe.format == format) return true;
    XeClose();
    if (!XeStart()) return false;
    g_xe.w = w; g_xe.h = h; g_xe.format = format;
    auto fail = [](const char *why) { Trace("[dlssg] unavailable: XeSS MFG - %s", why); XeClose(); return false; };

    // Xe Low Latency: frame generation asks for it, in its low latency mode;
    // its sleep and its markers it does not need (measured), and the engine
    // has its own Low Latency around the same presenter as DLSS MFG.
    if (g_xe.api.LlCreateContext(lfCore.device, &g_xe.ll) < 0 || !g_xe.ll) return fail("Xe Low Latency does not start on this card");
    xell_sleep_params_t sleep = {};
    sleep.bLowLatencyMode = 1;
    if (g_xe.api.LlSetSleepMode(g_xe.ll, &sleep) < 0) return fail("Xe Low Latency refused its mode");

    // How many frames in between the library allows here. The plugin raises
    // it about a second after the library was loaded, and a context keeps the
    // answer it was made with: until that second (three, to be sure) is over,
    // a context that still says one is made again.
    xefg_swapchain_properties_t properties = {};
    for (;;)
    {
        if (!XeFine(g_xe.api.CreateContext(lfCore.device, &g_xe.fg), "no context") || !g_xe.fg) return fail("the library does not start on this card");
        xefg_swapchain_d3d12_init_params_t ask = {};
        ask.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
        ask.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
        if (!XeFine(g_xe.api.GetD3D12Properties(g_xe.fg, &ask, w, h, format, &properties), "no properties")) return fail("the library does not answer");
        if (!g_xe.plugin || properties.maxSupportedInterpolations > 1 || GetTickCount64() - g_xe.loaded_at > 3000 || StopAsked()) break;
        g_xe.api.Destroy(g_xe.fg);
        g_xe.fg = nullptr;
        Sleep(50);
    }
    g_xe.most = std::clamp(properties.maxSupportedInterpolations, 1u, 3u);
    g_xe.api.SetLoggingCallback(g_xe.fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, XeSaid, nullptr);
    if (!XeFine(g_xe.api.SetLatencyReduction(g_xe.fg, g_xe.ll), "Xe Low Latency not taken")) return fail("the library refused Xe Low Latency");

    // Its window, and the factory with the tap.
    const HANDLE made = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_xe.window_thread = made ? CreateThread(nullptr, 0, XeWindowThread, made, 0, nullptr) : nullptr;
    if (g_xe.window_thread) WaitForSingleObject(made, 3000);
    if (made) CloseHandle(made);
    if (!g_xe.window) return fail("no window for the library's swap chain");
    IDXGIFactory2 *dxgi_factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&dxgi_factory))) || !dxgi_factory) return fail("no DXGI factory");
    g_xe.factory = new XeTapFactory(dxgi_factory, &g_xe);
    g_xe.factory->ring = 16;
    dxgi_factory->Release();

    // What numbers the pictures shown.
    if (!g_xe.fence && FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g_xe.fence.put())))) return fail("no fence");
    if (!g_xe.passed) g_xe.passed = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The swap chain: the pair's own queue draws into it (XeFeed).
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = w; desc.Height = h;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    xefg_swapchain_d3d12_init_params_t init = {};
    init.maxInterpolatedFrames = g_xe.most;
    init.uiMode = XEFG_SWAPCHAIN_UI_MODE_NONE;
    if (!XeFine(g_xe.api.InitFromSwapChainDesc(g_xe.fg, g_xe.window, &desc, nullptr, lfCore.direct_queue, g_xe.factory, &init), "no swap chain"))
        return fail("the library's swap chain did not come up");
    if (!XeFine(g_xe.api.GetSwapChainPtr(g_xe.fg, __uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&g_xe.chain)), "no swap chain pointer") || !g_xe.chain)
        return fail("the library did not hand its swap chain over");
    // The pictures it will draw its frames into: the tap's own, sixteen of them.
    g_xe.tap = g_xe.factory->made;
    if (g_xe.tap) g_xe.tap->AddRef();
    if (!g_xe.tap || !g_xe.tap->Own() || g_xe.tap->Pictures() < 8) return fail("the library's frames cannot be taken from where it draws them");
    // HDR: the pictures are HDR10 then (lf_hdr_show.inl), and the library is told so.
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        const HRESULT told = g_xe.chain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        if (FAILED(told)) Trace("[xess] the library's swap chain did not take the HDR10 colour space (0x%08X)", static_cast<unsigned>(told));
    }
    if (!XeFieldReady()) return fail("no motion picture");
    g_xe.set = g_xe.most;
    g_xe.api.SetNumInterpolatedFrames(g_xe.fg, g_xe.set);
    if (!XeFine(g_xe.api.SetEnabled(g_xe.fg, 1), "not enabled")) return fail("the library refused to make frames");
    g_xe.number = 0;
    Trace("[xess] XeSS MFG makes the frames in between at %ux%u%s, %u a pair at most; the engine's own presenter shows them "
          "(its lines in this log say [dlssg])", w, h, format == DXGI_FORMAT_R10G10B10A2_UNORM ? " in HDR10" : "", g_xe.most);
    return true;
}

// How many frames in between the library is asked for, for a pair that wants
// `wanted` of them. The same number - but not across the step between one
// and two: there the library changes its whole way of working, and a change
// stands still for a tenth of a second (measured, tests\xeprobe.cpp
// alternate=1: 112 ms each, where two to three and back costs nothing). Under
// a frame limit or VRR's range the pairs take turns - one frame, one, two,
// one - and every turn was that long a halt. So while pairs wanted two within
// the last three seconds, a pair that wants one gets two made as well and is
// shown by the nearer of them (lf_dlssg.inl, FgMadeFor). exact: the frames
// are shown as made, not rebuilt - then a pair must get its own count.
static unsigned XeCount(unsigned wanted, bool exact)
{
    wanted = std::clamp(wanted, 1u, g_xe.most);
    const ULONGLONG now = GetTickCount64();
    if (wanted >= 2) { g_xe.many_at = now; return wanted; }
    if (!exact && g_xe.most >= 2 && g_xe.set >= 2 && g_xe.many_at != 0 && now - g_xe.many_at < 3000) return 2;
    return 1;
}

// The real frame (at the size frames in between are made at) into the
// library's back buffer, and how the picture moved since the frame before
// into its motion picture (moved: see XeField; null when the engine's own
// field is handed over instead) - both recorded in the pair's own list.
static winrt::com_ptr<ID3D12Resource> g_xe_back;
static void XeFeed(ID3D12GraphicsCommandList *list, ID3D12Resource *picture, D3D12_RESOURCE_STATES state, const float (*moved)[2])
{
    g_xe_back = nullptr;
    if (!g_xe.chain || FAILED(g_xe.chain->GetBuffer(g_xe.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(g_xe_back.put())))) return;
    XeField(list, moved);
    D3D12_RESOURCE_BARRIER before[] = {
        Barrier(g_xe_back.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(picture, state, D3D12_RESOURCE_STATE_COPY_SOURCE) };
    list->ResourceBarrier(state == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 2, before);
    list->CopyResource(g_xe_back.get(), picture);
    D3D12_RESOURCE_BARRIER after[] = {
        Barrier(g_xe_back.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        Barrier(picture, D3D12_RESOURCE_STATE_COPY_SOURCE, state) };
    list->ResourceBarrier(state == D3D12_RESOURCE_STATE_COPY_SOURCE ? 1 : 2, after);
}

// How long the library's own call takes (the first frame in between is drawn
// inside it), said every ten seconds.
static void XeTook(double call_ms)
{
    static std::vector<float> calls;
    static ULONGLONG told_at = 0;
    calls.push_back(static_cast<float>(call_ms));
    const ULONGLONG now = GetTickCount64();
    if (told_at == 0) told_at = now;
    if (now - told_at < 10000 || calls.size() < 20) return;
    told_at = now;
    std::sort(calls.begin(), calls.end());
    Trace("[xess] the library's call for a pair: %.1f ms in the middle, %.1f at the 95th of 100, %.1f at most (%zu pairs)",
          calls[calls.size() / 2], calls[calls.size() * 95 / 100], calls.back(), calls.size());
    calls.clear();
}

// The frames in between of the frame just fed and the one fed before it:
// `frames` of them into `into` (each at rest as a copy source), once the list
// with XeFeed in it was sent. into: where the pictures that hold them are
// written (the tap's own, at rest as a back buffer is - PRESENT - and left
// alone by the library for five pairs); null: they are wanted by nobody (the
// frame found no place), the library still sees it. restart: nothing is made
// across a cut or a pause - the frame only becomes the one "before".
// motion, depth: the engine's own motion field and the depth of its size
// (field_w x field_h), or null: the motion picture XeFeed wrote.
// True: they are being drawn, and whole once the fence reaches *taken. False:
// this pair has none.
static bool XeMake(unsigned frames, ID3D12Resource **into, ID3D12Resource *motion, ID3D12Resource *depth,
                   UINT field_w, UINT field_h, bool restart, double interval, UINT64 *taken)
{
    *taken = 0;
    if (!g_xe.chain || !g_xe_back) return false;
    g_xe_back = nullptr;
    if (motion == nullptr || depth == nullptr)
    {
        motion = g_xe.field.get(); depth = g_xe.flat.get();
        field_w = g_xe.fw; field_h = g_xe.fh;
    }
    frames = std::clamp(frames, 1u, g_xe.most);
    const uint32_t number = ++g_xe.number;
    if (g_xe.set != frames)
    {
        if (XeFine(g_xe.api.SetNumInterpolatedFrames(g_xe.fg, frames), "the count of frames in between was refused")) g_xe.set = frames;
        else frames = g_xe.set;
    }
    // What goes with the frame: the motion field (zeros unless the engine's
    // estimate was asked for) and a flat depth, both as they lie until the
    // next Present, and a camera that stands still.
    xefg_swapchain_d3d12_resource_data_t with = {};
    with.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
    with.resourceSize = { field_w, field_h };
    with.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    with.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR; with.pResource = motion;
    bool told = XeFine(g_xe.api.TagFrameResource(g_xe.fg, nullptr, number, &with), "the motion field was refused");
    with.type = XEFG_SWAPCHAIN_RES_DEPTH; with.pResource = depth;
    told = XeFine(g_xe.api.TagFrameResource(g_xe.fg, nullptr, number, &with), "the depth was refused") && told;
    xefg_swapchain_frame_constant_data_t constants = {};
    for (int i = 0; i < 4; ++i) constants.viewMatrix[i * 5] = 1.0f;
    {
        const float zn = 0.1f, zf = 1000.0f, ys = 1.0f / tanf(0.5f);
        constants.projectionMatrix[0] = ys * static_cast<float>(g_xe.h) / static_cast<float>(g_xe.w);
        constants.projectionMatrix[5] = ys;
        constants.projectionMatrix[10] = zf / (zf - zn);
        constants.projectionMatrix[11] = 1.0f;
        constants.projectionMatrix[14] = -zn * zf / (zf - zn);
    }
    constants.motionVectorScaleX = constants.motionVectorScaleY = 1.0f;
    constants.resetHistory = restart ? 1u : 0u;
    constants.frameRenderTime = interval > 0.0 && interval < 1.0 ? static_cast<float>(interval * 1000.0) : 0.0f;
    told = XeFine(g_xe.api.TagFrameConstants(g_xe.fg, number, &constants), "the frame's constants were refused") && told;
    told = XeFine(g_xe.api.SetPresentId(g_xe.fg, number), "the frame's number was refused") && told;

    // The library shows the first frame in between inside this call, on this
    // thread, the others from a thread of its own a millisecond later, then
    // the real frame (after a new start the real frame only, inside the
    // call). Each lands in the tap's next picture and gets the fence's next
    // number: so once the call is back and the library has said how many
    // pictures it decided on, the pair knows where all its frames are and
    // which number the last of them brings - nobody waits for them here.
    const bool wanted = !restart && into != nullptr;
    {
        std::lock_guard<std::mutex> hold(g_xe.lock);
        // The pair before was told a number. A frame the library never showed after all must not keep the
        // presenter's queue waiting for it for good: by now - a whole frame of the game later - it is given.
        if (g_xe.owed > g_xe.value)
        {
            static ULONGLONG told_at = 0;
            if (GetTickCount64() - told_at >= 5000)
            {
                told_at = GetTickCount64();
                Trace("[xess] the library showed %llu frames fewer than it said for the pair before", g_xe.owed - g_xe.value);
            }
            g_xe.value = g_xe.owed;
            lfCore.direct_queue->Signal(g_xe.fence.get(), g_xe.value);
        }
        g_xe.owed = 0;
        g_xe.first_came = false;
        g_xe.asking = GetCurrentThreadId();
    }
    const double asked_at = LatencyNow();
    const HRESULT shown = g_xe.chain->Present(0, 0);
    XeTook((LatencyNow() - asked_at) * 1000.0);
    xefg_swapchain_present_status_t status = {};
    const bool said = g_xe.api.GetLastPresentStatus(g_xe.fg, &status) >= 0;
    const unsigned made = SUCCEEDED(shown) && said && told && status.framesPresented > 1 ? status.framesPresented - 1 : 0;
    std::lock_guard<std::mutex> hold(g_xe.lock);
    g_xe.asking = 0;
    if (FAILED(shown))
    {
        static ULONGLONG told_at = 0;
        if (GetTickCount64() - told_at >= 5000) { told_at = GetTickCount64(); Trace("[xess] the library's Present failed (0x%08X)", static_cast<unsigned>(shown)); }
    }
    if (!wanted || made == 0 || !g_xe.first_came || !g_xe.tap) return false;
    if (made < frames)
    {
        static ULONGLONG told_at = 0;
        if (GetTickCount64() - told_at >= 5000)
        {
            told_at = GetTickCount64();
            Trace("[xess] the library makes %u of %u frames in between for this pair - it goes without", made, frames);
        }
        return false;
    }
    for (unsigned k = 0; k < frames; ++k) into[k] = g_xe.tap->Picture(g_xe.first_index + k);
    *taken = g_xe.owed = g_xe.first_mark + frames - 1;
    return true;
}

// A queue waits until the pair's frames in between are drawn.
static void XeAfter(ID3D12CommandQueue *queue, UINT64 taken)
{
    if (taken && g_xe.fence && queue) queue->Wait(g_xe.fence.get(), taken);
}

// And an event is set when they are.
static void XeTell(UINT64 taken, HANDLE event)
{
    if (taken && g_xe.fence && event) g_xe.fence->SetEventOnCompletion(taken, event);
}
