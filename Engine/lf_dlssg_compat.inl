// Desktop FG compatibility routing. Loaded only on the selected NVIDIA adapter,
// only when DLSS FG is requested; never installed in the game or manager process.
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static int FgComputeCapability()
{
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    DXGI_ADAPTER_DESC1 desc = {};
    if (!lfCard.adapter || FAILED(lfCard.adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de) return 0;
    const HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return 0;
    auto init = reinterpret_cast<int(__stdcall *)(unsigned)>(GetProcAddress(cuda, "cuInit"));
    auto count = reinterpret_cast<int(__stdcall *)(int *)>(GetProcAddress(cuda, "cuDeviceGetCount"));
    auto device = reinterpret_cast<int(__stdcall *)(int *, int)>(GetProcAddress(cuda, "cuDeviceGet"));
    auto luid = reinterpret_cast<int(__stdcall *)(char *, unsigned *, int)>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    auto attribute = reinterpret_cast<int(__stdcall *)(int *, int, int)>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
    int n = 0;
    const LUID selected = lfCore.device->GetAdapterLuid();
    if (init && count && device && luid && attribute && init(0) == 0 && count(&n) == 0) {
        for (int i = 0; i < n; ++i) {
            int dev = 0, major = 0, minor = 0; unsigned mask = 0; LUID found = {};
            if (device(&dev, i) == 0 && luid(reinterpret_cast<char *>(&found), &mask, dev) == 0 &&
                memcmp(&found, &selected, sizeof(LUID)) == 0 &&
                attribute(&major, 75, dev) == 0 && attribute(&minor, 76, dev) == 0) {
                cached = major * 10 + minor; break;
            }
        }
    }
    // CUDA may have installed process state; retain its module for the worker lifetime.
    Trace("[dlssg] selected adapter %ls, CUDA SM%d", desc.Description, cached);
    return cached;
}

static bool FgFileHash(const std::wstring &path, const char *expected)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    BYTE buffer[65536], digest[32] = {}; DWORD got = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        ok = BCryptHashData(hash, buffer, got, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    char hex[65] = {};
    for (unsigned i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02X", digest[i]);
    return ok && _stricmp(hex, expected) == 0;
}

static bool FgWriteUtf8(const std::wstring &path, const std::wstring &text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return false;
    std::vector<char> bytes(length);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), length, nullptr, nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file); return ok;
}

// A folder of the engine's own cache and all in it (no links followed: one is taken away, not gone into).
static void FgRemoveOwnFolder(const std::wstring &folder)
{
    WIN32_FIND_DATAW found = {};
    HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            const std::wstring path = folder + L"\\" + found.cFileName;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                FgRemoveOwnFolder(path);
            else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(path.c_str());
            else
            {
                if (found.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(path.c_str());
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(folder.c_str());
}

static bool FgPrepareCompatibility(const wchar_t *workerDirectory, bool startup = false)
{
    const int sm = FgComputeCapability();
    if (sm != 75 && sm != 86) return true; // Ada has its own retarget; Blackwell stays native.
    static HMODULE proxy = nullptr;
    if (proxy) return true;
    if (!startup) {
        Trace("[dlssg] unavailable: RTX 20/30 compatibility requires a worker restart before NGX initialization"); return false;
    }
    // Round 42: the reviewed upstream 0.4.0 (dlssg-release-0.4.0-x64). One package for RTX 30 (SM86) and RTX 20
    // (SM75, its kernel family picked from the card itself), on NVIDIA's DLSS-G 310.9.16 - the engine's own
    // nvngx_dlssg.dll, the same file - and preset C (FG Lite). Its files are no longer inside the proxy: the proxy
    // finds its INI and its dlssg_sm86\ folder (backend, kernel pack, runtime) beside itself, so all of them are laid
    // out side by side in the cache, each one checked by its hash. (Its super-resolution and XeSS packs are not used.)
    struct Part { const wchar_t *from; const wchar_t *to; const char *sha; };
    const std::wstring here = std::wstring(workerDirectory);
    const Part parts[] = {
        { L"dlssg_compat\\version.dll", L"version.dll", "7B3F90435BE4F94348BF8D128C84871C7F904C704F7690601A263277C327A7B2" },
        { L"dlssg_compat\\sm86_backend.dll", L"dlssg_sm86\\backend\\sm86_backend.dll", "30FFC6E09A347CCDE87ECC6FA54FD7818F5ED8A9353F5C31C2A90835C7F0CA9B" },
        { L"dlssg_compat\\fg_kernels.dll", L"dlssg_sm86\\kernels\\fg_kernels.dll", "1394C2D5D9CC8938BA3E52B043048D20D6D4C13A019C161B11A3DFD16343EC24" },
        { L"nvngx_dlssg.dll", L"dlssg_sm86\\runtime\\nvngx_dlssg.dll", "CADEE1AA6DB3907BABDC3285DA2AEBC4553D92B89C38BA3EAE8ABD8BBA1BEA72" },
    };
    wchar_t root[MAX_PATH] = {}; EngineDataPath(root);
    if (!root[0]) { Trace("[dlssg] unavailable: no folder for the compatibility package"); return false; }
    const std::wstring parent = std::wstring(root) + L"\\dlssg-compat";
    const std::wstring cache = parent + L"\\0.4.0";
    if (cache.size() + 60 >= MAX_PATH) { Trace("[dlssg] unavailable: compatibility cache path is too long"); return false; }
    for (const std::wstring &folder : { parent, cache, cache + L"\\dlssg_sm86", cache + L"\\dlssg_sm86\\backend", cache + L"\\dlssg_sm86\\kernels",
                                        cache + L"\\dlssg_sm86\\runtime", cache + L"\\logs" })
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const std::wstring local = cache + L"\\version.dll";
    // The 0.3.5 package's own folders (its proxy, its runtime and backend unpacked, its logs: some 60 MB) are no
    // longer used. (A proxy of it still loaded by another engine running keeps its file; the next start takes it.)
    for (const wchar_t *old : { L"\\0.3.5-sm86", L"\\0.3.5-sm75" })
        if (GetFileAttributesW((parent + old).c_str()) != INVALID_FILE_ATTRIBUTES) FgRemoveOwnFolder(parent + old);
    for (const Part &part : parts)
    {
        const std::wstring placed = cache + L"\\" + part.to;
        if (FgFileHash(placed, part.sha)) continue;
        const std::wstring shipped = here + part.from;
        const std::wstring temporary = placed + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        if (!FgFileHash(shipped, part.sha))
        {
            Trace("[dlssg] unavailable: the RTX 20/30 compatibility package is missing a file or has a wrong one (%ls)", part.from);
            return false;
        }
        if (!CopyFileW(shipped.c_str(), temporary.c_str(), FALSE) || !FgFileHash(temporary, part.sha) ||
            !MoveFileExW(temporary.c_str(), placed.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temporary.c_str());
            Trace("[dlssg] unavailable: the RTX 20/30 compatibility package could not be laid out (%ls, Windows %lu)", part.to, GetLastError());
            return false;
        }
    }
    // The package's consistency tiers ([DLSSG] Optimized: 1 exact, as NVIDIA's own; 2 and 3 with its faster, not
    // bit-exact image kernels). Measured with 0.3.5 on an RTX 3060 with frames in between made at 896x504, x6: 5.2 to
    // 5.4 ms a pair at tier 1, 5.2 to 5.3 at tier 2, 5.15 to 5.2 at tier 3 - nothing that can be told from noise (the
    // faster kernels are the ones that work at the picture's size, and that is small here). So tier 1 stays.
    // SM_FG_TIER sets it, to measure on another card. Its preset ([DLSSG] Preset): the one picked in the app
    // (sm_dlssg_preset.inl writes the same into the driver's profile), Auto when the driver's own is wanted.
    auto number = [](const char *name, int otherwise) { char v[8] = {}; return GetEnvironmentVariableA(name, v, sizeof(v)) > 0 ? atoi(v) : otherwise; };
    const int tier = std::clamp(number("SM_FG_TIER", 1), 0, 3);
    static const wchar_t *const presets[] = { L"Auto", L"A", L"B", L"C" };
    const int preset = std::clamp(smOpt.dlssg_preset.load(), 0, 3);
    const std::wstring ini = L"[General]\nEnabled=1\n[FrameGeneration]\nEngine=DLSS\nMaxGeneratedFrames=5\n"
        L"[DLSSG]\nOptimized=" + std::to_wstring(tier) + L"\nPreset=" + presets[preset] + L"\n"
        L"[Logging]\nLevel=2\nDirectory=" + cache + L"\\logs\n[Runtime]\nMode=Bundled\n[SuperResolution]\nMode=Off\n";
    if (!FgWriteUtf8(cache + L"\\dlssg_sm86.ini", ini)) return false;
    {
        // The package writes two small logs for every start and never takes
        // one away (1,400 files after two days of tests): all but the forty
        // newest go.
        std::vector<std::pair<ULONGLONG, std::wstring>> logs;
        WIN32_FIND_DATAW found = {};
        HANDLE find = FindFirstFileW((cache + L"\\logs\\*.jsonl").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE)
        {
            do logs.emplace_back((ULONGLONG(found.ftLastWriteTime.dwHighDateTime) << 32) | found.ftLastWriteTime.dwLowDateTime, found.cFileName);
            while (FindNextFileW(find, &found));
            FindClose(find);
        }
        std::sort(logs.begin(), logs.end());
        for (size_t i = 0; i + 40 < logs.size(); ++i) DeleteFileW((cache + L"\\logs\\" + logs[i].second).c_str());
    }
    proxy = LoadLibraryW(local.c_str());
    if (!proxy) { Trace("[dlssg] unavailable: compatibility loader Windows error %lu", GetLastError()); return false; }
    Trace("[dlssg] RTX compatibility 0.4.0 loaded for SM%d (runtime 310.9.16); exact kernels, up to 6x, preset %ls", sm, presets[preset]);
    return true;
}
