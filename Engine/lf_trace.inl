// lf_trace.inl - how the engine talks: one timestamped line per event in its
// log (engine.log in its data folder; on stderr too when started with
// --verbose), readable names for NGX answers, and what is known when the
// device is lost.

static FILE *g_trace_file = nullptr;
static bool g_trace_echo = false;
static std::mutex g_trace_lock;

// The time of day a log line begins with, in seconds; under 0 when it has none.
static double TraceLineTime(const char *line)
{
    unsigned h = 0, m = 0, s = 0, ms = 0;
    return sscanf_s(line, "%2u:%2u:%2u.%3u", &h, &m, &s, &ms) == 4 ? h * 3600.0 + m * 60.0 + s + ms / 1000.0 : -1.0;
}

// Open the log: the run before is kept once as engine.prev.log - and, when it
// lasted more than a minute, in history\ under the time it ended, the twelve
// newest of them. (A game session's log was gone as soon as the engine had
// been started twice more, by a test or by the next try; what interrupted the
// frames in between an hour ago could no longer be read.)
// name: the log's own name (round 47: "video" for Super Video - a video made never takes the game's log away).
static void TraceOpen(const wchar_t *name = L"engine")
{
    wchar_t folder[MAX_PATH] = {};
    EngineDataPath(folder);
    const std::wstring log = std::wstring(folder) + L"\\" + name + L".log", before = std::wstring(folder) + L"\\" + name + L".prev.log";
    {
        double first = -1.0, last = -1.0;
        FILE *old = _wfsopen(log.c_str(), L"rb", _SH_DENYNO);
        if (old)
        {
            char line[4096];
            while (fgets(line, sizeof(line), old))
            {
                const double at = TraceLineTime(line);
                if (at < 0.0) continue;
                if (first < 0.0) first = at;
                last = at;
            }
            fclose(old);
        }
        const double lasted = first < 0.0 ? 0.0 : (last >= first ? last - first : last + 86400.0 - first);
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (lasted > 60.0 && GetFileAttributesExW(log.c_str(), GetFileExInfoStandard, &data))
        {
            const std::wstring history = std::wstring(folder) + L"\\history";
            CreateDirectoryW(history.c_str(), nullptr);
            FILETIME local = {};
            SYSTEMTIME ended = {};
            FileTimeToLocalFileTime(&data.ftLastWriteTime, &local);
            FileTimeToSystemTime(&local, &ended);
            wchar_t name[64];
            swprintf_s(name, L"\\engine-%04u%02u%02u-%02u%02u%02u.log", ended.wYear, ended.wMonth, ended.wDay, ended.wHour,
                       ended.wMinute, ended.wSecond);
            CopyFileW(log.c_str(), (history + name).c_str(), FALSE);
            // The names sort by time: all but the twelve newest go.
            std::vector<std::wstring> kept;
            WIN32_FIND_DATAW found = {};
            HANDLE find = FindFirstFileW((history + L"\\engine-*.log").c_str(), &found);
            if (find != INVALID_HANDLE_VALUE)
            {
                do kept.push_back(found.cFileName); while (FindNextFileW(find, &found));
                FindClose(find);
            }
            std::sort(kept.begin(), kept.end());
            for (size_t i = 0; i + 12 < kept.size(); ++i) DeleteFileW((history + L"\\" + kept[i]).c_str());
        }
    }
    MoveFileExW(log.c_str(), before.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_trace_file = _wfsopen(log.c_str(), L"w", _SH_DENYWR);
}

// A few lines are news for the app as well as for the log (sm_control.inl).
static void TraceNews(const char *text);

// One line with the local time in front.
static void Trace(const char *pixel_format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, pixel_format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, pixel_format, args);
    va_end(args);
    SYSTEMTIME now;
    GetLocalTime(&now);
    {
        std::lock_guard<std::mutex> hold(g_trace_lock);
        if (g_trace_file)
        {
            fprintf(g_trace_file, "%02u:%02u:%02u.%03u  %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text);
            fflush(g_trace_file);
        }
        if (g_trace_echo)
        {
            fprintf(stderr, "%02u:%02u:%02u.%03u  %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text);
            fflush(stderr);
        }
    }
    if (g_frames_file) FramesLine("%10.4f   # %s", FramesNow(), text);
    TraceNews(text);
}

// NGX answers by name, for the log.
static const char *NgxCodeText(NVSDK_NGX_Result r)
{
    struct Named { unsigned code; const char *text; };
    static const Named kKnown[] = {
        {0x00000001u, "Success"}, {0xBAD00001u, "FeatureNotSupported"}, {0xBAD00002u, "PlatformError"},
        {0xBAD00003u, "FeatureAlreadyExists"}, {0xBAD00004u, "FeatureNotFound"}, {0xBAD00005u, "InvalidParameter"},
        {0xBAD00007u, "NotInitialized"}, {0xBAD00008u, "UnsupportedInputFormat"}, {0xBAD0000Au, "MissingInput"},
        {0xBAD0000Bu, "UnableToInitializeFeature"}, {0xBAD0000Du, "OutOfGPUMemory"}, {0xBAD0000Eu, "UnsupportedFormat"},
    };
    for (const Named &known : kKnown)
        if (known.code == static_cast<unsigned>(r)) return known.text;
    return "?";
}

// The device is gone: say why, and what DRED recorded last.
static void ReportDeviceLoss(const char *where)
{
    if (lfCore.device == nullptr) return;
    const HRESULT reason = lfCore.device->GetDeviceRemovedReason();
    Trace("[card] the device was lost in %s: 0x%08X", where, (unsigned)reason);
    ID3D12DeviceRemovedExtendedData1 *dred = nullptr;
    if (FAILED(lfCore.device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1),
                                     reinterpret_cast<void **>(&dred))))
        return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)) && crumbs.pHeadAutoBreadcrumbNode)
    {
        const D3D12_AUTO_BREADCRUMB_NODE *n = crumbs.pHeadAutoBreadcrumbNode;
        Trace("[card] breadcrumbs: %u, the last %u, list %p",
            n->BreadcrumbCount,
            n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u,
            (void *)n->pCommandList);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA != 0)
        Trace("[card] breadcrumbs: page fault at 0x%llX", (unsigned long long)pf.PageFaultVA);
    dred->Release();
}

// How much video memory this process holds right now, and the budget.
static void ReportVram(const char *where)
{
    if (lfCard.adapter == nullptr) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (SUCCEEDED(lfCard.adapter->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        Trace("[card] video memory after %s: %llu of %llu MB", where,
            (unsigned long long)(info.CurrentUsage >> 20),
            (unsigned long long)(info.Budget >> 20));
}
