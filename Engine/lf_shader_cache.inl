// lf_shader_cache.inl - every compute shader the worker compiles, compiled
// once (round 24).
//
// Measured on the first start of LIVE FLOW (tests/screen_startup_profile.py,
// tests/worker_hlsl_compile.py): of the 1.7 s between switching LIVE FLOW on
// and its first picture, 0.70 s was the Denoiser's shader being compiled - on
// every start, inside the first frame. FLOW MFG's kernels take 3.5 s and its
// motion estimate's 4.1 s of compiler time (they have kept their own file cache
// since round 18; the rest never had one).
//
// So CachedCompile stands in for D3DCompile everywhere, with the same
// signature:
//   - by memory: a shader compiled once in this process is handed out again;
//   - by file: the code is kept under %TEMP%\DLSS5Manager-shaders (or
//     LF_SHADER_CACHE), named by a hash of the source, entry point, profile and
//     flags - a changed shader has another name. Each file carries a hash of
//     its own bytes; one that does not match is compiled afresh. Written under
//     a temporary name and moved in whole, so two workers never read half a
//     file;
//   - one compile per shader: a second caller asking for a shader another
//     thread is compiling waits for it instead of compiling it again.
// ShaderWarmStart (lf_shader_warm.inl) compiles what this run will need on
// background threads from the first moment the worker runs, while the card
// and the network start - so even the very first start after an update finds
// most of it done.

struct ShaderCacheState
{
    std::mutex lock;
    std::condition_variable done;
    std::set<UINT64> busy;
    std::map<UINT64, ID3DBlob *> ready;   // one reference each, kept for the process's life
    CompileFn compile = nullptr;
    HRESULT(WINAPI *make_blob)(SIZE_T, ID3DBlob **) = nullptr;
    std::wstring folder;
    bool folder_ok = false;
    bool loaded = false;
    unsigned hits_memory = 0, hits_file = 0, compiled = 0;
    double compile_seconds = 0.0;
};

// Never destroyed: the warm-up threads may still be finishing a shader while
// the process exits, and nothing they touch may be torn down under them.
static ShaderCacheState &ShaderCache()
{
    static ShaderCacheState *state = new ShaderCacheState();
    return *state;
}

static UINT64 ShaderHash(UINT64 hash, const void *data, size_t size)
{
    const UINT8 *bytes = static_cast<const UINT8 *>(data);
    for (size_t k = 0; k < size; ++k) { hash ^= bytes[k]; hash *= 1099511628211ull; }
    return hash;
}

// The compiler and the folder, once. Under the lock.
static void ShaderCacheLoad(ShaderCacheState &sc)
{
    if (sc.loaded) return;
    sc.loaded = true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler != nullptr)
    {
        sc.compile = reinterpret_cast<CompileFn>(GetProcAddress(compiler, "D3DCompile"));
        sc.make_blob = reinterpret_cast<HRESULT(WINAPI *)(SIZE_T, ID3DBlob **)>(GetProcAddress(compiler, "D3DCreateBlob"));
    }
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableW(L"LF_SHADER_CACHE", path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 64)
    {
        len = GetTempPathW(MAX_PATH, path);
        if (len > 0 && len < MAX_PATH - 64) wcscat_s(path, L"SuperMotion-shaders");
        else path[0] = L'\0';
    }
    if (path[0] != L'\0')
    {
        CreateDirectoryW(path, nullptr);
        const DWORD attributes = GetFileAttributesW(path);
        sc.folder_ok = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        sc.folder = path;
    }
}

static bool ShaderFileRead(ShaderCacheState &sc, const std::wstring &path, ID3DBlob **code)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = false;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 8 + 32 && size.QuadPart < (16 << 20))
    {
        const DWORD payload = static_cast<DWORD>(size.QuadPart - 8);
        UINT64 stored = 0;
        DWORD got = 0;
        ID3DBlob *blob = nullptr;
        if (ReadFile(f, &stored, 8, &got, nullptr) && got == 8 && SUCCEEDED(sc.make_blob(payload, &blob)) &&
            ReadFile(f, blob->GetBufferPointer(), payload, &got, nullptr) && got == payload &&
            memcmp(blob->GetBufferPointer(), "DXBC", 4) == 0 &&
            ShaderHash(1469598103934665603ull, blob->GetBufferPointer(), payload) == stored)
        {
            *code = blob;
            ok = true;
        }
        else if (blob) blob->Release();
    }
    CloseHandle(f);
    return ok;
}

static void ShaderFileWrite(const std::wstring &path, ID3DBlob *code)
{
    wchar_t part[MAX_PATH + 32];
    swprintf_s(part, L"%s.%lu.part", path.c_str(), GetCurrentThreadId());
    HANDLE f = CreateFileW(part, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const UINT64 hash = ShaderHash(1469598103934665603ull, code->GetBufferPointer(), code->GetBufferSize());
    DWORD put = 0, put2 = 0;
    const BOOL ok = WriteFile(f, &hash, 8, &put, nullptr) &&
                    WriteFile(f, code->GetBufferPointer(), DWORD(code->GetBufferSize()), &put2, nullptr);
    CloseHandle(f);
    if (!ok || !MoveFileExW(part, path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(part);
}

// D3DCompile's own signature. Only the code is cached: a failed compile is
// tried again by the next caller and hands out its message as it always did.
static HRESULT WINAPI CachedCompile(LPCVOID source, SIZE_T size, LPCSTR name, const void *defines, void *include,
                                    LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                                    ID3DBlob **code, ID3DBlob **errors)
{
    if (code == nullptr) return E_INVALIDARG;
    *code = nullptr;
    if (errors) *errors = nullptr;
    ShaderCacheState &sc = ShaderCache();
    CompileFn compile = nullptr;
    {
        std::lock_guard<std::mutex> hold(sc.lock);
        ShaderCacheLoad(sc);
        compile = sc.compile;
    }
    if (compile == nullptr) return E_NOINTERFACE;
    // Defines or includes would be part of what the code is; none of the
    // worker's shaders has them, and one that does is simply not cached.
    if (defines != nullptr || include != nullptr || sc.make_blob == nullptr)
        return compile(source, size, name, defines, include, entry, target, flags1, flags2, code, errors);
    UINT64 key = ShaderHash(1469598103934665603ull, source, size);
    key = ShaderHash(key, entry ? entry : "", entry ? strlen(entry) : 0);
    key = ShaderHash(key, "|", 1);
    key = ShaderHash(key, target ? target : "", target ? strlen(target) : 0);
    key = ShaderHash(key, &flags1, sizeof(flags1));
    key = ShaderHash(key, &flags2, sizeof(flags2));
    {
        std::unique_lock<std::mutex> hold(sc.lock);
        sc.done.wait(hold, [&] { return sc.busy.count(key) == 0; });
        const auto found = sc.ready.find(key);
        if (found != sc.ready.end())
        {
            found->second->AddRef();
            *code = found->second;
            ++sc.hits_memory;
            return S_OK;
        }
        sc.busy.insert(key);
    }
    wchar_t file[MAX_PATH + 64] = {};
    if (sc.folder_ok) swprintf_s(file, L"%s\\%016llX-%S.cso", sc.folder.c_str(), key, entry ? entry : "main");
    HRESULT hr = S_OK;
    bool from_file = sc.folder_ok && ShaderFileRead(sc, file, code);
    double seconds = 0.0;
    if (!from_file)
    {
        const auto start = std::chrono::steady_clock::now();
        hr = compile(source, size, name, defines, include, entry, target, flags1, flags2, code, errors);
        seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (SUCCEEDED(hr) && *code != nullptr && sc.folder_ok) ShaderFileWrite(file, *code);
    }
    {
        std::lock_guard<std::mutex> hold(sc.lock);
        sc.busy.erase(key);
        if (SUCCEEDED(hr) && *code != nullptr)
        {
            (*code)->AddRef();
            sc.ready[key] = *code;
            if (from_file) ++sc.hits_file;
            else { ++sc.compiled; sc.compile_seconds += seconds; }
        }
    }
    sc.done.notify_all();
    return hr;
}
