// xe_api.h - Intel's two libraries as XeSS MFG uses them (xe_mfg.inl):
//   libxess_fg.dll   XeSS Frame Generation: a swap chain of its own that makes
//                    the frames in between and shows them
//   libxell.dll      Xe Low Latency: frame generation refuses to start without it
// Only what the engine calls is declared here, in the libraries' own names
// and layouts (Intel's XeSS SDK, inc\xess_fg and inc\xell, version 1.3), so a
// line here can be looked up there. Nothing is linked: both are loaded while
// running (XeApiLoad), from the engine's "xess" folder.
#pragma once

#pragma pack(push, 8)

// ---- XeSS Frame Generation ------------------------------------------------

typedef struct _xefg_swapchain_handle_t *xefg_swapchain_handle_t;

enum xefg_swapchain_result_t : int32_t
{
    XEFG_SWAPCHAIN_RESULT_SUCCESS = 0,
    XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER = 2,
    XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES = 3,
    XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH = 4,
    XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS = 5,
    XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH = 6,
    XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED = -3,
    XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT = -4,
    XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE = -6,
    XEFG_SWAPCHAIN_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT = -8,
    XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS = -9,
    XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED = -10,
    XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY = -11,
    XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES = -12,
    XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES = -13,
    XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES = -14,
    XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED = -15,
    XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING = -16,
    XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE = -17,
    XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL = -18,
    XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE = -19,
    XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_DESCRIPTOR_HEAP = -20,
    XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER = -21,
    XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN = -1000,
};

enum xefg_swapchain_init_flags_t : uint32_t
{
    XEFG_SWAPCHAIN_INIT_FLAG_NONE = 0,
    XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH = 1u << 0,
    XEFG_SWAPCHAIN_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1u << 1,
    XEFG_SWAPCHAIN_INIT_FLAG_HIGH_RES_MV = 1u << 2,
    XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY = 1u << 3,
    XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV = 1u << 4,
    XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA = 1u << 5,
};

// What a picture handed over is.
enum xefg_swapchain_resource_type_t : int32_t
{
    XEFG_SWAPCHAIN_RES_HUDLESS_COLOR = 0,
    XEFG_SWAPCHAIN_RES_DEPTH = 1,
    XEFG_SWAPCHAIN_RES_MOTION_VECTOR = 2,
    XEFG_SWAPCHAIN_RES_UI = 3,
    XEFG_SWAPCHAIN_RES_BACKBUFFER = 4,
};

// How long it stays as it is: until the next Present, or only now (then the
// library copies it with the list it is given).
enum xefg_swapchain_resource_validity_t : int32_t
{
    XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT = 0,
    XEFG_SWAPCHAIN_RV_ONLY_NOW = 1,
};

enum xefg_swapchain_logging_level_t : int32_t
{
    XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG = 0,
    XEFG_SWAPCHAIN_LOGGING_LEVEL_INFO = 1,
    XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING = 2,
    XEFG_SWAPCHAIN_LOGGING_LEVEL_ERROR = 3,
};

enum xefg_swapchain_ui_mode_t : int32_t
{
    XEFG_SWAPCHAIN_UI_MODE_AUTO = 0,
    XEFG_SWAPCHAIN_UI_MODE_NONE = 1,
    XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_UITEXTURE = 2,
    XEFG_SWAPCHAIN_UI_MODE_HUDLESS_UITEXTURE = 3,
    XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS = 4,
    XEFG_SWAPCHAIN_UI_MODE_BACKBUFFER_HUDLESS_UITEXTURE = 5,
};

enum xefg_swapchain_debug_feature_t : int32_t
{
    XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION = 0,
    XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES = 1,
    XEFG_SWAPCHAIN_DEBUG_FEATURE_PRESENT_FAILED_INTERPOLATION = 2,
};

static const uint32_t XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES = 0xFFFFFFFFu;

struct xefg_swapchain_version_t { uint16_t major, minor, patch, reserved; };
struct xefg_swapchain_2d_t { uint32_t x, y; };

struct xefg_swapchain_properties_t
{
    uint32_t requiredDescriptorCount;
    uint64_t tempBufferHeapSize;
    uint64_t tempTextureHeapSize;
    uint64_t constantBufferSize;
    uint32_t maxSupportedInterpolations;    // frames in between a pair this card is allowed
};

// What goes with a frame besides its pictures.
struct xefg_swapchain_frame_constant_data_t
{
    float viewMatrix[16];
    float projectionMatrix[16];
    float jitterOffsetX, jitterOffsetY;
    float motionVectorScaleX, motionVectorScaleY;
    uint32_t resetHistory;                  // 1: nothing is made between this frame and the one before
    float frameRenderTime;                  // milliseconds, 0: not known
};

struct xefg_swapchain_present_status_t
{
    uint32_t framesPresented;               // what the last Present put on the screen, the real frame among them
    xefg_swapchain_result_t frameGenResult;
    uint32_t isFrameGenEnabled;
};

struct xefg_swapchain_d3d12_init_params_t
{
    IDXGISwapChain *pApplicationSwapChain;
    uint32_t initFlags;
    uint32_t maxInterpolatedFrames;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    ID3D12Heap *pTempBufferHeap;
    uint64_t bufferHeapOffset;
    ID3D12Heap *pTempTextureHeap;
    uint64_t textureHeapOffset;
    ID3D12PipelineLibrary *pPipelineLibrary;
    xefg_swapchain_ui_mode_t uiMode;
};

struct xefg_swapchain_d3d12_resource_data_t
{
    xefg_swapchain_resource_type_t type;
    xefg_swapchain_resource_validity_t validity;
    xefg_swapchain_2d_t resourceBase;
    xefg_swapchain_2d_t resourceSize;
    ID3D12Resource *pResource;
    D3D12_RESOURCE_STATES incomingState;
};

typedef void (*xefg_swapchain_app_log_callback_t)(const char *message, xefg_swapchain_logging_level_t level, void *user);

// ---- Xe Low Latency -------------------------------------------------------

typedef struct _xell_context_handle_t *xell_context_handle_t;
typedef int32_t xell_result_t;      // 0 is success, the rest are below zero

enum xell_latency_marker_type_t : int32_t
{
    XELL_SIMULATION_START = 0,
    XELL_SIMULATION_END = 1,
    XELL_RENDERSUBMIT_START = 2,
    XELL_RENDERSUBMIT_END = 3,
    XELL_PRESENT_START = 4,
    XELL_PRESENT_END = 5,
    XELL_INPUT_SAMPLE = 6,
};

struct xell_sleep_params_t
{
    uint32_t minimumIntervalUs;
    uint32_t bLowLatencyMode : 1;
    uint32_t bLowLatencyBoost : 1;
    uint32_t reserved : 30;
};

struct xell_version_t { uint16_t major, minor, patch, reserved; };

#pragma pack(pop)

// ---- The calls, found by name ----------------------------------------------

struct XeApi
{
    HMODULE fg = nullptr, ll = nullptr;

    // libxess_fg.dll
    xefg_swapchain_result_t (*GetVersion)(xefg_swapchain_version_t *) = nullptr;
    xefg_swapchain_result_t (*CreateContext)(ID3D12Device *, xefg_swapchain_handle_t *) = nullptr;
    xefg_swapchain_result_t (*SetLoggingCallback)(xefg_swapchain_handle_t, xefg_swapchain_logging_level_t, xefg_swapchain_app_log_callback_t, void *) = nullptr;
    xefg_swapchain_result_t (*SetLatencyReduction)(xefg_swapchain_handle_t, void *) = nullptr;
    xefg_swapchain_result_t (*GetD3D12Properties)(xefg_swapchain_handle_t, const xefg_swapchain_d3d12_init_params_t *, uint32_t, uint32_t, DXGI_FORMAT, xefg_swapchain_properties_t *) = nullptr;
    xefg_swapchain_result_t (*GetProperties)(xefg_swapchain_handle_t, xefg_swapchain_properties_t *) = nullptr;
    xefg_swapchain_result_t (*InitFromSwapChainDesc)(xefg_swapchain_handle_t, HWND, const DXGI_SWAP_CHAIN_DESC1 *, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, ID3D12CommandQueue *, IDXGIFactory2 *, const xefg_swapchain_d3d12_init_params_t *) = nullptr;
    xefg_swapchain_result_t (*GetInitializationParameters)(xefg_swapchain_handle_t, xefg_swapchain_d3d12_init_params_t *) = nullptr;
    xefg_swapchain_result_t (*GetSwapChainPtr)(xefg_swapchain_handle_t, REFIID, void **) = nullptr;
    xefg_swapchain_result_t (*SetEnabled)(xefg_swapchain_handle_t, uint32_t) = nullptr;
    xefg_swapchain_result_t (*SetNumInterpolatedFrames)(xefg_swapchain_handle_t, uint32_t) = nullptr;
    xefg_swapchain_result_t (*SetSceneChangeThreshold)(xefg_swapchain_handle_t, float) = nullptr;
    xefg_swapchain_result_t (*TagFrameResource)(xefg_swapchain_handle_t, ID3D12CommandList *, uint32_t, const xefg_swapchain_d3d12_resource_data_t *) = nullptr;
    xefg_swapchain_result_t (*TagFrameConstants)(xefg_swapchain_handle_t, uint32_t, const xefg_swapchain_frame_constant_data_t *) = nullptr;
    xefg_swapchain_result_t (*SetPresentId)(xefg_swapchain_handle_t, uint32_t) = nullptr;
    xefg_swapchain_result_t (*GetLastPresentStatus)(xefg_swapchain_handle_t, xefg_swapchain_present_status_t *) = nullptr;
    xefg_swapchain_result_t (*EnableDebugFeature)(xefg_swapchain_handle_t, xefg_swapchain_debug_feature_t, uint32_t, void *) = nullptr;
    xefg_swapchain_result_t (*Destroy)(xefg_swapchain_handle_t) = nullptr;

    // libxell.dll
    xell_result_t (*LlGetVersion)(xell_version_t *) = nullptr;
    xell_result_t (*LlCreateContext)(ID3D12Device *, xell_context_handle_t *) = nullptr;
    xell_result_t (*LlDestroyContext)(xell_context_handle_t) = nullptr;
    xell_result_t (*LlSetSleepMode)(xell_context_handle_t, const xell_sleep_params_t *) = nullptr;
    xell_result_t (*LlSleep)(xell_context_handle_t, uint32_t) = nullptr;
    xell_result_t (*LlAddMarkerData)(xell_context_handle_t, uint32_t, xell_latency_marker_type_t) = nullptr;
};

// Both libraries from one folder (its path ends in a backslash). What is
// missing is named in `missing`; nothing is half loaded afterwards.
static bool XeApiLoad(XeApi &api, const wchar_t *folder, std::string &missing)
{
    auto open = [&](const wchar_t *name) {
        std::wstring path = std::wstring(folder) + name;
        // Its own folder first: the library brings others of its own along.
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
        {
            char text[96];
            _snprintf_s(text, sizeof(text), _TRUNCATE, "%ls did not load (%lu)", name, GetLastError());
            missing = text;
        }
        return module;
    };
    bool whole = true;
    auto find = [&](HMODULE module, const char *name, void *slot) {
        FARPROC call = GetProcAddress(module, name);
        if (!call) { if (whole) missing = std::string(name) + " is not in the library"; whole = false; }
        memcpy(slot, &call, sizeof(call));
    };
    api.ll = open(L"libxell.dll");
    if (!api.ll) return false;
    api.fg = open(L"libxess_fg.dll");
    if (!api.fg) { FreeLibrary(api.ll); api.ll = nullptr; return false; }

    find(api.fg, "xefgSwapChainGetVersion", &api.GetVersion);
    find(api.fg, "xefgSwapChainD3D12CreateContext", &api.CreateContext);
    find(api.fg, "xefgSwapChainSetLoggingCallback", &api.SetLoggingCallback);
    find(api.fg, "xefgSwapChainSetLatencyReduction", &api.SetLatencyReduction);
    find(api.fg, "xefgSwapChainD3D12GetProperties", &api.GetD3D12Properties);
    find(api.fg, "xefgSwapChainGetProperties", &api.GetProperties);
    find(api.fg, "xefgSwapChainD3D12InitFromSwapChainDesc", &api.InitFromSwapChainDesc);
    find(api.fg, "xefgSwapChainD3D12GetInitializationParameters", &api.GetInitializationParameters);
    find(api.fg, "xefgSwapChainD3D12GetSwapChainPtr", &api.GetSwapChainPtr);
    find(api.fg, "xefgSwapChainSetEnabled", &api.SetEnabled);
    find(api.fg, "xefgSwapChainSetNumInterpolatedFrames", &api.SetNumInterpolatedFrames);
    find(api.fg, "xefgSwapChainSetSceneChangeThreshold", &api.SetSceneChangeThreshold);
    find(api.fg, "xefgSwapChainD3D12TagFrameResource", &api.TagFrameResource);
    find(api.fg, "xefgSwapChainTagFrameConstants", &api.TagFrameConstants);
    find(api.fg, "xefgSwapChainSetPresentId", &api.SetPresentId);
    find(api.fg, "xefgSwapChainGetLastPresentStatus", &api.GetLastPresentStatus);
    find(api.fg, "xefgSwapChainEnableDebugFeature", &api.EnableDebugFeature);
    find(api.fg, "xefgSwapChainDestroy", &api.Destroy);
    find(api.ll, "xellGetVersion", &api.LlGetVersion);
    find(api.ll, "xellD3D12CreateContext", &api.LlCreateContext);
    find(api.ll, "xellDestroyContext", &api.LlDestroyContext);
    find(api.ll, "xellSetSleepMode", &api.LlSetSleepMode);
    find(api.ll, "xellSleep", &api.LlSleep);
    find(api.ll, "xellAddMarkerData", &api.LlAddMarkerData);
    if (!whole)
    {
        FreeLibrary(api.fg);
        FreeLibrary(api.ll);
        api = XeApi{};
    }
    return whole;
}

// A result in words for the log.
static const char *XeResultText(xefg_swapchain_result_t result)
{
    switch (result)
    {
    case XEFG_SWAPCHAIN_RESULT_SUCCESS: return "success";
    case XEFG_SWAPCHAIN_RESULT_WARNING_OLD_DRIVER: return "old driver";
    case XEFG_SWAPCHAIN_RESULT_WARNING_TOO_FEW_FRAMES: return "too few frames yet";
    case XEFG_SWAPCHAIN_RESULT_WARNING_FRAMES_ID_MISMATCH: return "frame ids do not match";
    case XEFG_SWAPCHAIN_RESULT_WARNING_MISSING_PRESENT_STATUS: return "no present status yet";
    case XEFG_SWAPCHAIN_RESULT_WARNING_RESOURCE_SIZES_MISMATCH: return "picture sizes do not match";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE: return "unsupported card";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DRIVER: return "unsupported driver";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED: return "not initialised";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT: return "invalid argument";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE_OUT_OF_MEMORY: return "out of video memory";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DEVICE: return "device error";
    case XEFG_SWAPCHAIN_RESULT_ERROR_NOT_IMPLEMENTED: return "not implemented";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_CONTEXT: return "invalid context";
    case XEFG_SWAPCHAIN_RESULT_ERROR_OPERATION_IN_PROGRESS: return "operation in progress";
    case XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED: return "unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_CANT_LOAD_LIBRARY: return "a library did not load";
    case XEFG_SWAPCHAIN_RESULT_ERROR_MISMATCH_INPUT_RESOURCES: return "input pictures do not match";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_OUTPUT_RESOURCES: return "wrong output pictures";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INCORRECT_INPUT_RESOURCES: return "wrong input pictures";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_UNSUPPORTED: return "low latency unsupported";
    case XEFG_SWAPCHAIN_RESULT_ERROR_LATENCY_REDUCTION_FUNCTION_MISSING: return "a low latency call is missing";
    case XEFG_SWAPCHAIN_RESULT_ERROR_HRESULT_FAILURE: return "a Windows call failed";
    case XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL: return "invalid DXGI call";
    case XEFG_SWAPCHAIN_RESULT_ERROR_POINTER_STILL_IN_USE: return "the swap chain is still held";
    case XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_DESCRIPTOR_HEAP: return "invalid descriptor heap";
    case XEFG_SWAPCHAIN_RESULT_ERROR_WRONG_CALL_ORDER: return "calls in the wrong order";
    default: return "unknown error";
    }
}
