// sm_nooverlay.inl - NVIDIA's overlay kept out of the engine (round 83).
//
// The owner: the NVIDIA overlay's notice at the top right comes up each time the engine starts, and it is in the
// engine's frames - "block it, I think it has an effect". What brings it in is nvspcap64.dll (NVIDIA's capture, the
// overlay's and Instant Replay's), which NVIDIA's D3D12 user-mode driver (nvwgf2umx.dll) loads itself when the card's
// device is made - five milliseconds after the driver comes in (SM_DLL_PROBE=1 says who loads what). It then hooks
// every Present of the output. Lossless Scaling, on the user's PC, is never seen by it: it runs on the Intel card.
//
// Done in the engine's own process only: when NVIDIA's driver modules come in (Windows' DLL notification), their
// import entries for the LoadLibrary calls point at NoOverlayLoad, which refuses nvspcap64.dll and loads all else as
// asked. No file of NVIDIA's is touched; no other program changes.
//   SM_NVOVERLAY=1: leave it to come in (to compare).

namespace nooverlay
{
    using LoadExW = HMODULE(WINAPI *)(LPCWSTR, HANDLE, DWORD);
    using LoadExA = HMODULE(WINAPI *)(LPCSTR, HANDLE, DWORD);
    using LoadW = HMODULE(WINAPI *)(LPCWSTR);
    using LoadA = HMODULE(WINAPI *)(LPCSTR);
    static LoadExW g_ex_w = nullptr; static LoadExA g_ex_a = nullptr; static LoadW g_w = nullptr; static LoadA g_a = nullptr;
    static std::atomic<unsigned> g_refused{0};

    static bool Overlay(const wchar_t *name)
    {
        if (!name) return false;
        std::wstring lower(name);
        for (auto &c : lower) c = static_cast<wchar_t>(towlower(c));
        return lower.find(L"nvspcap") != std::wstring::npos;
    }
    static bool Overlay(const char *name)
    {
        if (!name) return false;
        std::string lower(name);
        for (auto &c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return lower.find("nvspcap") != std::string::npos;
    }
    static HMODULE Refuse() { ++g_refused; SetLastError(ERROR_MOD_NOT_FOUND); return nullptr; }

    static HMODULE WINAPI ExW(LPCWSTR n, HANDLE f, DWORD flags) { return Overlay(n) ? Refuse() : g_ex_w(n, f, flags); }
    static HMODULE WINAPI ExA(LPCSTR n, HANDLE f, DWORD flags) { return Overlay(n) ? Refuse() : g_ex_a(n, f, flags); }
    static HMODULE WINAPI W(LPCWSTR n) { return Overlay(n) ? Refuse() : g_w(n); }
    static HMODULE WINAPI A(LPCSTR n) { return Overlay(n) ? Refuse() : g_a(n); }

    // The module's import entries that hold one of the four, pointed at ours.
    static int Patch(void *module_base)
    {
        auto *base = static_cast<unsigned char *>(module_base);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;
        // (The import may name kernel32 or one of Windows' API sets - kernelbase's: both addresses are looked for.)
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        const struct { ULONG_PTR real; ULONG_PTR ours; } map[] = {
            {reinterpret_cast<ULONG_PTR>(g_ex_w), reinterpret_cast<ULONG_PTR>(&ExW)}, {reinterpret_cast<ULONG_PTR>(g_ex_a), reinterpret_cast<ULONG_PTR>(&ExA)},
            {reinterpret_cast<ULONG_PTR>(g_w), reinterpret_cast<ULONG_PTR>(&W)}, {reinterpret_cast<ULONG_PTR>(g_a), reinterpret_cast<ULONG_PTR>(&A)},
            {reinterpret_cast<ULONG_PTR>(GetProcAddress(k32, "LoadLibraryExW")), reinterpret_cast<ULONG_PTR>(&ExW)},
            {reinterpret_cast<ULONG_PTR>(GetProcAddress(k32, "LoadLibraryExA")), reinterpret_cast<ULONG_PTR>(&ExA)},
            {reinterpret_cast<ULONG_PTR>(GetProcAddress(k32, "LoadLibraryW")), reinterpret_cast<ULONG_PTR>(&W)},
            {reinterpret_cast<ULONG_PTR>(GetProcAddress(k32, "LoadLibraryA")), reinterpret_cast<ULONG_PTR>(&A)}};
        int patched = 0;
        for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
        {
            if (imp->FirstThunk == 0) continue;
            for (auto *slot = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk); slot->u1.Function != 0; ++slot)
                for (const auto &m : map)
                {
                    if (!m.real || slot->u1.Function != m.real) continue;
                    DWORD was = 0;
                    if (!VirtualProtect(&slot->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &was)) continue;
                    slot->u1.Function = m.ours;
                    VirtualProtect(&slot->u1.Function, sizeof(ULONG_PTR), was, &was);
                    ++patched;
                }
        }
        return patched;
    }

    typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } Name;
    typedef struct { ULONG Flags; const Name *FullDllName; const Name *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } Loaded;
    static int g_patched = 0;
    static VOID CALLBACK OnLoad(ULONG reason, const Loaded *data, PVOID)
    {
        if (reason != 1 || !data || !data->BaseDllName || !data->BaseDllName->Buffer) return;
        const std::wstring name(data->BaseDllName->Buffer, data->BaseDllName->Length / 2);
        // NVIDIA's user-mode drivers (D3D12, D3D11, the loader) - the ones that bring the overlay in.
        if (_wcsicmp(name.c_str(), L"nvwgf2umx.dll") == 0 || _wcsicmp(name.c_str(), L"nvldumdx.dll") == 0 ||
            _wcsicmp(name.c_str(), L"nvd3dumx.dll") == 0)
            g_patched += Patch(data->DllBase);
    }

    // At the start, before any card's device is made.
    static void Start()
    {
        char v[4] = {};
        if (GetEnvironmentVariableA("SM_NVOVERLAY", v, sizeof(v)) > 0 && v[0] == '1') return;
        // (The originals: kernelbase's own, where kernel32's lead.)
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) return;
        g_ex_w = reinterpret_cast<LoadExW>(GetProcAddress(kb, "LoadLibraryExW"));
        g_ex_a = reinterpret_cast<LoadExA>(GetProcAddress(kb, "LoadLibraryExA"));
        g_w = reinterpret_cast<LoadW>(GetProcAddress(kb, "LoadLibraryW"));
        g_a = reinterpret_cast<LoadA>(GetProcAddress(kb, "LoadLibraryA"));
        if (!g_ex_w || !g_ex_a || !g_w || !g_a) return;
        typedef LONG(NTAPI * RegisterFn)(ULONG, PVOID, PVOID, PVOID *);
        static PVOID cookie = nullptr;
        if (auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")))
            reg(0, reinterpret_cast<PVOID>(&OnLoad), nullptr, &cookie);
    }

    // For the log once the card is up: whether the overlay was kept out.
    static void Tell()
    {
        static bool told = false;
        if (told) return;
        told = true;
        Trace("[overlay] NVIDIA's overlay (nvspcap64.dll) %s in the engine (%d import entries of NVIDIA's driver watched)",
              GetModuleHandleW(L"nvspcap64.dll") ? "IS" : "is not", g_patched);
    }
}
