// sm_front.inl - the graphics driver told that the output window is the one in front (round 51).
//
// VRR "not reaching the screen" on most users' PCs (GitHub #7, #27, #28, #30 - G-SYNC on for windowed programs, the
// driver's own indicator saying "VRR Requested: No"): NVIDIA's driver lets the screen follow a window's frames only
// when that window is the one in front - and the output window never is: it lets clicks through to the game, which
// keeps the keyboard and the focus. A user found it (Discord, "Evil Quaint") and showed it working by having the
// driver's question - GetForegroundWindow - answered with the output window. The same answer ends the driver's
// "Background Application Max Frame Rate" holding the engine at 20 FPS (#8).
//
// Done here in the engine's own process only, and only for the driver: the import tables of the modules Windows
// loaded from the driver store (the graphics driver's own - NVIDIA's, AMD's, Intel's user-mode drivers, all of which
// import GetForegroundWindow from user32) are pointed at DriverForeground. Nothing of the game's is touched, and the
// engine's own code - which asks the same question to know whether the game is in front - still gets the true answer.
// While the output window is not shown the true answer is given to the driver too.
//   SM_FRONT=0 leaves the driver's question alone (to compare).

#include <psapi.h>

namespace front
{
    static std::atomic<HWND> g_ours{nullptr};
    static std::atomic<unsigned> g_asked{0}, g_told{0};
    using ForegroundFn = HWND(WINAPI *)();
    static ForegroundFn g_real = nullptr;

    static HWND WINAPI DriverForeground()
    {
        const HWND real = g_real ? g_real() : nullptr;
        ++g_asked;
        const HWND ours = g_ours.load();
        if (ours != nullptr && real != ours && IsWindowVisible(ours)) { ++g_told; return ours; }
        return real;
    }

    static bool Wanted()
    {
        char v[8] = {};
        return !(GetEnvironmentVariableA("SM_FRONT", v, sizeof(v)) > 0 && v[0] == '0');
    }

    // The module's import entries that hold user32's GetForegroundWindow, pointed at DriverForeground. How many.
    static int PatchModule(HMODULE module)
    {
        auto *base = reinterpret_cast<unsigned char *>(module);
        auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0 || dir.Size == 0) return 0;
        int patched = 0;
        for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
        {
            if (imp->FirstThunk == 0) continue;
            for (auto *slot = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk); slot->u1.Function != 0; ++slot)
            {
                // (By the address it holds - the import may name user32 or one of Windows' API sets.)
                if (slot->u1.Function != reinterpret_cast<ULONG_PTR>(g_real)) continue;
                DWORD was = 0;
                if (!VirtualProtect(&slot->u1.Function, sizeof(ULONG_PTR), PAGE_READWRITE, &was)) continue;
                slot->u1.Function = reinterpret_cast<ULONG_PTR>(&DriverForeground);
                VirtualProtect(&slot->u1.Function, sizeof(ULONG_PTR), was, &was);
                ++patched;
            }
        }
        return patched;
    }

    // The driver's modules loaded so far (called once the output window's swap chain is made - the user-mode drivers
    // are in by then; again for a later one: a module already pointed here is passed over).
    static void Hook(HWND output)
    {
        g_ours = output;
        if (!Wanted()) return;
        if (!g_real) g_real = reinterpret_cast<ForegroundFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetForegroundWindow"));
        if (!g_real) return;
        static std::set<std::wstring> done;
        HMODULE modules[1024];
        DWORD bytes = 0;
        if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return;
        const HMODULE self = GetModuleHandleW(nullptr);
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i)
        {
            if (modules[i] == self) continue;
            wchar_t path[MAX_PATH] = {};
            if (!GetModuleFileNameW(modules[i], path, MAX_PATH)) continue;
            std::wstring lower(path);
            for (auto &c : lower) c = static_cast<wchar_t>(towlower(c));
            if (lower.find(L"\\driverstore\\") == std::wstring::npos || done.count(lower)) continue;
            done.insert(lower);
            const int n = PatchModule(modules[i]);
            if (n > 0)
            {
                const wchar_t *name = wcsrchr(path, L'\\');
                Trace("[front] %ls: the graphics driver is told the output window is in front (VRR, its background frame limit)",
                      name ? name + 1 : path);
            }
        }
    }

    // The output window gone: the driver gets the true answer again.
    static void Release() { g_ours = nullptr; }

    // For the log, once in a while: how often the driver asked, and was told ours.
    static void Tell()
    {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (!g_real || now - last < 30000) return;
        last = now;
        const unsigned asked = g_asked.exchange(0), told = g_told.exchange(0);
        if (asked) Trace("[front] the graphics driver asked which window is in front %u times - the output window %u of them", asked, told);
    }
}
