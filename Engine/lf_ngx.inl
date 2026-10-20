// lf_ngx.inl - NGX itself: started on the device, its parameter block, and
// the answer to the runtime's architecture question so every RTX card is
// accepted. The frame generator (lf_dlssg.inl) loads its own runtime on top
// of this.

// Is the architecture answer wanted (on unless LF_ARCH_SPOOF=0).
static bool ArchWanted()
{
    char text[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_ARCH_SPOOF", text, sizeof(text));
    if (len > 0 && len < sizeof(text) && text[0] == '0') return false;
    return true;
}

// Ask NvAPI for every card's architecture and keep the answers.
static int ArchPrepare()
{
    if (!ArchWanted()) return 0;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    { Trace("[gpuid] nvapi64.dll did not load (%lu)", GetLastError()); return -1; }
    auto qi = reinterpret_cast<NvQueryFn>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (qi == nullptr) { Trace("[gpuid] nvapi_QueryInterface is missing"); return -1; }

    auto init = reinterpret_cast<int (__cdecl *)()>(qi(kNvapiInit));
    auto enum_gpus = reinterpret_cast<int (__cdecl *)(void **, unsigned *)>(
        qi(kNvapiEnumGpus));
    auto get_arch = reinterpret_cast<NvArchFn>(qi(kNvapiArchInfo));
    if (init == nullptr || enum_gpus == nullptr || get_arch == nullptr)
    { Trace("[gpuid] the NvAPI calls we need did not resolve"); return -1; }
    if (init() != 0) { Trace("[gpuid] NvAPI did not start"); return -1; }

    void *handles[kArchSlots] = {};
    unsigned count = 0;
    if (enum_gpus(handles, &count) != 0 || count == 0)
    { Trace("[gpuid] no GPU list from NvAPI"); return -1; }
    if (count > static_cast<unsigned>(kArchSlots)) count = kArchSlots;

    lfArch.count = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        ArchRecord one = {};
        one.version = static_cast<unsigned>(sizeof(ArchRecord)) | (2u << 16);
        if (get_arch(handles[i], &one) != 0)
        {
            one.version = static_cast<unsigned>(sizeof(ArchRecord)) | (1u << 16);
            if (get_arch(handles[i], &one) != 0) continue;
        }
        lfArch.handles[lfArch.count] = handles[i];
        lfArch.cache[lfArch.count] = one;
        ++lfArch.count;
    }
    if (lfArch.count == 0)
    { Trace("[gpuid] no card answered the architecture question"); return -1; }

    const ArchRecord info = lfArch.cache[0];
    lfArch.real_group = info.architecture & 0xFFFFFFF0u;

    char force[8] = {};
    const DWORD force_got = GetEnvironmentVariableA("LF_ARCH_FORCE", force,
                                                    sizeof(force));
    const bool forced = force_got > 0 && force_got < sizeof(force)
                        && force[0] == '1';
    if (lfArch.real_group >= kArchBlackwell && !forced)
    {
        Trace("[gpuid] architecture 0x%X is accepted as it is - nothing to change",
            info.architecture);
        return 0;
    }

    lfArch.hook_site = reinterpret_cast<void *>(get_arch);
    if (!ArchHookInstall())
    { Trace("[gpuid] the answer could not be put in place (%lu)", GetLastError()); return -1; }
    Trace("[gpuid] answer in place for %d card(s), architecture 0x%X%s",
        lfArch.count, info.architecture,
        forced ? " (LF_ARCH_FORCE=1, reported as it is)"
               : " (reported as one the runtime accepts)");
    return 1;
}

// Put our architecture answer in place of the runtime's question.
static bool ArchHookInstall()
{
    BYTE blob[12] = { 0x48, 0xB8 };
    void *into = reinterpret_cast<void *>(&ArchAnswer);
    memcpy(blob + 2, &into, sizeof(into));
    blob[10] = 0xFF; blob[11] = 0xE0;
    if (!PatchBytes(lfArch.hook_site, blob, sizeof(blob)))
        return false;
    lfArch.patched = true;
    return true;
}

// Write a few bytes of code in place.
static bool PatchBytes(void *at, const void *from, size_t bytes)
{
    DWORD old = 0;
    if (!VirtualProtect(at, bytes, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, from, bytes);
    DWORD tmp = 0;
    VirtualProtect(at, bytes, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, bytes);
    return true;
}

// Answer the runtime: a card it would refuse is reported as one it accepts.
static int __cdecl ArchAnswer(void *gpu_at, ArchRecord *info)
{
    if (info == nullptr) return -1;
    const unsigned wanted = info->version;

    int idx = -1;
    for (int i = 0; i < lfArch.count; ++i)
        if (lfArch.handles[i] == gpu_at) { idx = i; break; }
    if (idx < 0) idx = 0;

    *info = lfArch.cache[idx];
    info->version = wanted;

    const unsigned group = info->architecture & 0xFFFFFFF0u;
    if (idx == 0 && (group == kArchTuring || group == kArchAmpere
        || group == kArchAda))
    {
        info->architecture   = kArchBlackwell;
        info->implementation = 0x3u;
        info->revision       = 0xA1u;
    }
    return 0;
}

// NGX's own log lines are not wanted.
static void NVSDK_CONV NgxQuietLog(const char *, NVSDK_NGX_Logging_Level,
                                          NVSDK_NGX_Feature) {}

// Where NGX looks for its runtimes.
static struct NgxSearchState
{
    NVSDK_NGX_FeatureCommonInfo info = {};
    wchar_t dirs[2][MAX_PATH];
    const wchar_t *list[2] = { dirs[0], dirs[1] };
} lfNgxSearch;

static NVSDK_NGX_FeatureCommonInfo *NgxSearchInfo(const wchar_t *data_dir)
{
    lfNgxSearch.info = {};
    lfNgxSearch.info.LoggingInfo.LoggingCallback = NgxQuietLog;
    lfNgxSearch.info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    lfNgxSearch.info.LoggingInfo.DisableOtherLoggingSinks = true;
    if (NgxLibraryLogWanted() > 0)      // LF_NGX_LOG (lf_dlssg.inl): NGX's lines about presets into the engine's log
    {
        lfNgxSearch.info.LoggingInfo.LoggingCallback = NgxLibraryLog;
        lfNgxSearch.info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    }

    char v[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_NGX_PATHLIST", v, sizeof(v));
    if (len > 0 && len < sizeof(v) && v[0] == '1')
    {
        wcscpy_s(lfNgxSearch.dirs[0], MAX_PATH, data_dir);
        wchar_t exe_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        if (wchar_t *s = wcsrchr(exe_dir, L'\\')) *(s + 1) = L'\0';
        wcscpy_s(lfNgxSearch.dirs[1], MAX_PATH, exe_dir);
        lfNgxSearch.info.PathListInfo.Path = lfNgxSearch.list;
        lfNgxSearch.info.PathListInfo.Length = 2;
        Trace("[card] LF_NGX_PATHLIST=1: NGX searches the exe and the engine folders");
    }
    return &lfNgxSearch.info;
}

// Start NGX on the device.
static bool NgxStart()
{
    wchar_t data_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, data_dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_dir, L'\\')) *(s + 1) = L'\0';

    char iv[8] = {};
    if (GetEnvironmentVariableA("LF_NGX_INDICATOR", iv, sizeof(iv)) > 0 && iv[0] == '1')
    {
        SetEnvironmentVariableA("__NGX_SHOW_INDICATOR", "1024");
        Trace("[card] LF_NGX_INDICATOR=1: NGX draws its debug overlay");
    }
    char cv[8] = {};
    if (GetEnvironmentVariableA("LF_NGX_NO_CUBIN_CACHE", cv, sizeof(cv)) > 0 && cv[0] == '1')
    {
        SetEnvironmentVariableA("__NGX_CUBIN_DISABLE_RESOURCE_CACHE", "1");
        Trace("[card] LF_NGX_NO_CUBIN_CACHE=1: no kernel cache");
    }

    NVSDK_NGX_FeatureCommonInfo *common = NgxSearchInfo(data_dir);
    EngineDataPath(data_dir);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_dir, lfCore.device, common, NVSDK_NGX_Version_API);
    Trace("[card] NGX start answered 0x%08X (%s)", r, NgxCodeText(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_dir, lfCore.device, common, NVSDK_NGX_Version_API);
        Trace("[card] NGX start by project answered 0x%08X (%s)", r, NgxCodeText(r));
    }
    if (NVSDK_NGX_FAILED(r)) return false;
    lfCore.ngx_ready = true;

    r = NVSDK_NGX_D3D12_AllocateParameters(&lfCore.ngx_params);
    if (NVSDK_NGX_FAILED(r) || lfCore.ngx_params == nullptr) { Trace("[card] no NGX parameter block (0x%08X)", r); return false; }
    // The runtime asks NvAPI which architecture the card is; the answer is in
    // place before the generator's runtime is loaded and started.
    ArchPrepare();
    return true;
}

// Free the generator and shut NGX down.
static void NgxShutdown()
{
    ShutdownFg();
    OfaStop();
    if (lfCore.ngx_params != nullptr)
    {
        NVSDK_NGX_D3D12_DestroyParameters(lfCore.ngx_params);
        lfCore.ngx_params = nullptr;
    }
    if (lfCore.ngx_ready)
    {
        NVSDK_NGX_D3D12_Shutdown1(lfCore.device);
        lfCore.ngx_ready = false;
        Trace("[card] NGX shut down");
    }
}
