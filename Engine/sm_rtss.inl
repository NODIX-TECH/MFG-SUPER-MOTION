// sm_rtss.inl - RivaTuner Statistics Server's overlay seen once (round 38, kept when DFAL was taken out in round 39).
//
// RTSS draws its overlay into the game - which the engine takes and shows - and drew it once more on the engine's
// own window, so it was seen twice. "--rtss-quiet" (the app at its start, where RTSS is installed): the engine's
// program gets a profile of its own in RTSS - RTSS's global one, copied - with the overlay off. RTSS still counts
// its frames; the overlay is seen once, in the game, as with Lossless Scaling. Nothing of a game's is touched; done
// once. RTSS's own library (RTSSHooks64.dll: LoadProfile, SaveProfile, UpdateProfiles) is loaded only in that short
// process, never in the one that draws the frames.

namespace rtss
{
    // Where RTSS is installed (its own word in the registry); false: it is not.
    static bool Installed(wchar_t (&dir)[MAX_PATH])
    {
        dir[0] = 0;
        DWORD size = sizeof(dir);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Unwinder\\RTSS", L"InstallDir", RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS)
        {
            size = sizeof(dir);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Unwinder\\RTSS", L"InstallDir", RRF_RT_REG_SZ, nullptr, dir, &size) != ERROR_SUCCESS) dir[0] = 0;
        }
        if (!dir[0]) return false;
        wchar_t hooks[MAX_PATH];
        swprintf_s(hooks, L"%s\\RTSSHooks64.dll", dir);
        return GetFileAttributesW(hooks) != INVALID_FILE_ATTRIBUTES;
    }

    static void Quiet()
    {
        wchar_t dir[MAX_PATH];
        if (!Installed(dir)) return;
        wchar_t hooks[MAX_PATH];
        swprintf_s(hooks, L"%s\\RTSSHooks64.dll", dir);
        HMODULE library = LoadLibraryExW(hooks, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!library) return;
        auto load = reinterpret_cast<void (__cdecl *)(const char *)>(GetProcAddress(library, "LoadProfile"));
        auto save = reinterpret_cast<void (__cdecl *)(const char *)>(GetProcAddress(library, "SaveProfile"));
        auto update = reinterpret_cast<void (__cdecl *)()>(GetProcAddress(library, "UpdateProfiles"));
        if (!load || !save || !update) return;
        // The engine's own program, by its file's name (RTSS knows a program's profile by that).
        wchar_t self[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
        const wchar_t *base = wcsrchr(self, L'\\');
        const wchar_t *program = base ? base + 1 : self;
        char name[260] = {};
        WideCharToMultiByte(CP_ACP, 0, program, -1, name, sizeof(name) - 1, nullptr, nullptr);
        wchar_t file[MAX_PATH];
        swprintf_s(file, L"%s\\Profiles\\%s.cfg", dir, program);
        if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES && GetPrivateProfileIntW(L"OSD", L"EnableOSD", 1, file) == 0) return;
        if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) { load(""); save(name); }
        if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) return;
        WritePrivateProfileStringW(L"OSD", L"EnableOSD", L"0", file);
        update();
    }
}
