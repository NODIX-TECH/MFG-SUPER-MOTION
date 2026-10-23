// sm_dlssg_preset.inl - DLSS MFG's preset (round 42): Performance (preset C), Quality (preset B), Balanced (preset A),
// or the driver's own choice.
//
// DLSS frame generation takes no preset from the program that runs it (nvsdk_ngx_defs_dlssg.h has none); NVIDIA's NGX
// hands it the one the driver's profile of that program says - "Enable DLSS-FG override" (0x10E41E03) and "Override
// DLSS-FG preset" (0x10E41DF1: A 1, B 2, C 3), the two NVIDIA Profile Inspector sets. The library applies what it is
// given ("ApplyRenderPreset" in nvngx_dlssg.dll 310.9). So the engine writes them into the profile of its own program
// (nvngx.dll - never a game's), at its very start, before anything of it is on the card: the driver reads a
// program's profile when the program begins to use the card. "Default" takes the engine's profile away again, and the
// driver's own choice holds. Nothing is done on another maker's card.
//   LF_NGX_LOG=1: NGX's own lines about presets go into the engine's log (lf_ngx.inl) - to see which one it applied.

namespace dlssgpreset
{
    typedef void *(__cdecl *QueryFn)(unsigned);
    struct Setting { unsigned char bytes[12320]; };     // NVDRS_SETTING v1: id at 4100, type 4104, the value at 8220
    struct App { unsigned char bytes[12296]; };         // NVDRS_APPLICATION v1: the program's name at 8
    struct Profile { unsigned char bytes[4116]; };      // NVDRS_PROFILE v1: its name at 4
    template <typename T> static void Version(T &s) { memset(&s, 0, sizeof(s)); const unsigned v = static_cast<unsigned>(sizeof(s)) | (1u << 16); memcpy(s.bytes, &v, 4); }
    static constexpr unsigned kOverride = 0x10E41E03, kPreset = 0x10E41DF1;
    static const wchar_t kProfileName[] = L"MFG SUPER MOTION engine";

    // preset: 0 the driver's own, 1 A (Balanced), 2 B (Quality), 3 C (Performance).
    static void Apply(int preset)
    {
        HMODULE library = LoadLibraryW(L"nvapi64.dll");
        auto query = library ? reinterpret_cast<QueryFn>(GetProcAddress(library, "nvapi_QueryInterface")) : nullptr;
        if (!query) return;
        auto init = reinterpret_cast<int (__cdecl *)()>(query(0x0150E828));
        auto create = reinterpret_cast<int (__cdecl *)(void **)>(query(0x0694D52E));
        auto destroy = reinterpret_cast<int (__cdecl *)(void *)>(query(0xDAD9CFF8));
        auto load = reinterpret_cast<int (__cdecl *)(void *)>(query(0x375DBD6B));
        auto save = reinterpret_cast<int (__cdecl *)(void *)>(query(0xFCBC7E14));
        auto find_app = reinterpret_cast<int (__cdecl *)(void *, const wchar_t *, void **, void *)>(query(0xEEE566B2));
        auto find_profile = reinterpret_cast<int (__cdecl *)(void *, const wchar_t *, void **)>(query(0x7E4A9A0B));
        auto create_profile = reinterpret_cast<int (__cdecl *)(void *, void *, void **)>(query(0xCC176068));
        auto create_app = reinterpret_cast<int (__cdecl *)(void *, void *, void *)>(query(0x4347A9DE));
        auto delete_profile = reinterpret_cast<int (__cdecl *)(void *, void *)>(query(0x17093206));
        auto get = reinterpret_cast<int (__cdecl *)(void *, void *, unsigned, void *)>(query(0x73BF8338));
        auto set = reinterpret_cast<int (__cdecl *)(void *, void *, void *)>(query(0x577DD202));
        if (!init || !create || !destroy || !load || !save || !find_app || !find_profile || !create_profile || !create_app ||
            !delete_profile || !get || !set || init() != 0) return;
        // The engine's own program, by its file's name.
        wchar_t self[MAX_PATH];
        if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return;
        const wchar_t *base = wcsrchr(self, L'\\');
        const wchar_t *program = base ? base + 1 : self;
        void *session = nullptr;
        if (create(&session) != 0 || !session) return;
        if (load(session) == 0)
        {
            void *profile = nullptr;
            const bool ours = find_profile(session, kProfileName, &profile) == 0 && profile;
            // Round 51 (a user's word, GitHub #11, seen with the driver's own Flip Indicator): the driver takes the program
            // the profile is for as "nvngx.dll.exe" - the engine's file has no .exe of its own - so a profile that names
            // "nvngx.dll" alone never applied. Both names are in it; true when one was added here.
            auto also_exe = [&](void *into) -> bool {
                wchar_t name[MAX_PATH + 8];
                swprintf_s(name, L"%ls.exe", program);
                if (_wcsicmp(program + wcslen(program) - (wcslen(program) >= 4 ? 4 : 0), L".exe") == 0) return false;
                void *where = nullptr;
                static App found, a;
                Version(found);
                if (find_app(session, name, &where, &found) == 0 && where) return false;
                Version(a);
                wcsncpy_s(reinterpret_cast<wchar_t *>(a.bytes + 8), 2048, name, _TRUNCATE);
                const bool added = create_app(session, into, &a) == 0;
                if (added) Trace("[dlssg] the driver's profile of the engine names %ls too (the name the driver goes by)", name);
                return added;
            };
            auto value_of = [&](unsigned id) -> long long {
                static Setting s;
                Version(s);
                if (!ours || get(session, profile, id, &s) != 0) return -1;
                unsigned v = 0;
                memcpy(&v, s.bytes + 8220, 4);
                return v;
            };
            if (preset <= 0)
            {
                // The driver's own choice: the engine's profile taken away, if there is one.
                if (ours && delete_profile(session, profile) == 0 && save(session) == 0)
                    Trace("[dlssg] preset: the driver's own (the engine's profile with an override taken away)");
            }
            else if (value_of(kOverride) == 1 && value_of(kPreset) == preset)
            {
                Trace("[dlssg] preset %c (%s) - already in the driver's profile of the engine", 'A' + preset - 1,
                      preset == 3 ? "Performance" : preset == 2 ? "Quality" : "Balanced");
                // (A profile made by an engine before round 51 has the program's name alone: the second one is added.)
                if (also_exe(profile)) save(session);
            }
            else
            {
                if (!ours)
                {
                    static Profile p;
                    Version(p);
                    wcsncpy_s(reinterpret_cast<wchar_t *>(p.bytes + 4), 2048, kProfileName, _TRUNCATE);
                    static App a;
                    Version(a);
                    wcsncpy_s(reinterpret_cast<wchar_t *>(a.bytes + 8), 2048, program, _TRUNCATE);
                    // (The program in another profile already - its name is not ours to take - is left alone.)
                    void *elsewhere = nullptr;
                    static App found;
                    Version(found);
                    if (find_app(session, program, &elsewhere, &found) == 0 && elsewhere) profile = nullptr;
                    else if (create_profile(session, &p, &profile) != 0 || !profile || create_app(session, profile, &a) != 0) profile = nullptr;
                }
                if (profile) also_exe(profile);
                bool done = false;
                if (profile)
                {
                    static Setting s;
                    const unsigned values[2][2] = { { kOverride, 1u }, { kPreset, static_cast<unsigned>(preset) } };
                    done = true;
                    for (const auto &pair : values)
                    {
                        Version(s);
                        const unsigned type = 0;
                        memcpy(s.bytes + 4100, &pair[0], 4); memcpy(s.bytes + 4104, &type, 4); memcpy(s.bytes + 8220, &pair[1], 4);
                        done = done && set(session, profile, &s) == 0;
                    }
                    done = done && save(session) == 0;
                }
                Trace(done ? "[dlssg] preset %c (%s) set in the driver's profile of the engine (%ls)" :
                             "[dlssg] preset %c (%s) could not be set in the driver's profile of the engine (%ls)",
                      'A' + preset - 1, preset == 3 ? "Performance" : preset == 2 ? "Quality" : "Balanced", program);
            }
        }
        destroy(session);
    }
}
