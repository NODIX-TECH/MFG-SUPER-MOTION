namespace ARMO.Infrastructure

// MFG SUPER MOTION for Linux (round 73): the engine is a Vulkan layer that goes into the game itself (Linux\engine,
// libmfgsm_vk.so - SMFG made Vulkan). This is what the app does with it:
//   - installs it for the user: the library beside the app copied to ~/.local/share/mfg-super-motion/layer, and its
//     manifest put where the Vulkan loader looks for implicit layers (~/.local/share/vulkan/implicit_layer.d). It is
//     then on for any game started with MFGSM=1 (Steam: launch options "MFGSM=1 %command%"); with "every Vulkan game"
//     the app also writes MFGSM=1 for the whole session (~/.config/environment.d, after logging in again);
//   - writes its settings (~/.config/mfgsm/mfgsm.conf - the layer reads them every second, so a change is live);
//   - reads what it says (~/.cache/mfgsm/status, each second while a game runs: the game, its FPS, the FPS shown).
// Nothing of this is used on Windows.

open System
open System.IO
open System.Text

/// Adaptive: frames shown on an even beat, Target a second, whatever the game's pace; else Multiplier.
/// Round 74: Aml - Adaptive Motion Learning (and AmlFlow, Adaptive Flow Scale with it); Game - the kind of game picked.
/// (Round 88n: Smsr - SMSR 1.0 in play: the game is told its window is half the size and the layer makes each frame whole.)
/// (Round 101: Tri - "Triangle Mesh": SMFG's frames by a mesh of triangles, Linux\engine\tri_vk.inl.)
/// (Round 114: TriPitch .. TriRelax - SMT's own numbers from its gear, as the Windows engine's tri_* keys.)
type LinuxSettings = { Enabled: bool; Multiplier: int; Flow: int; Fast: bool; Adaptive: bool; Target: int; Aml: bool; AmlFlow: bool; Game: int; Smsr: bool; SmsrStrength: int; Tri: bool
                       TriPitch: int; TriRounds: int; TriFine: int; TriFull: int; TriHome: int; TriTop: int; TriExact: int; TriRelax: int }

/// Aml: what Adaptive Motion Learning says, as the Windows engine's line does - off | learning <percent> | learned <percent> <delay ms>.
type LinuxStatus = { Program: string; GameFps: int; ShownFps: int; Working: bool; Size: string; Aml: string }

module LinuxLayer =
    let private home = Environment.GetFolderPath Environment.SpecialFolder.UserProfile
    let private xdg (variable: string) (fallback: string) =
        match Environment.GetEnvironmentVariable variable with
        | null | "" -> Path.Combine(home, fallback)
        | value -> value
    let configFile = Path.Combine(xdg "XDG_CONFIG_HOME" ".config", "mfgsm", "mfgsm.conf")
    let statusFile = Path.Combine(xdg "XDG_CACHE_HOME" ".cache", "mfgsm", "status")
    let logFile = Path.Combine(xdg "XDG_CACHE_HOME" ".cache", "mfgsm", "layer.log")
    let private dataHome = xdg "XDG_DATA_HOME" (Path.Combine(".local", "share"))
    let installedLibrary = Path.Combine(dataHome, "mfg-super-motion", "layer", "libmfgsm_vk.so")
    let manifestFile = Path.Combine(dataHome, "vulkan", "implicit_layer.d", "VkLayer_mfgsm.json")
    let sessionFile = Path.Combine(xdg "XDG_CONFIG_HOME" ".config", "environment.d", "90-mfgsm.conf")
    /// The layer that comes with the app.
    let shippedLibrary = Path.Combine(AppContext.BaseDirectory, "layer", "libmfgsm_vk.so")
    /// What Steam's launch options take for one game.
    let launchOption = "MFGSM=1 %command%"

    let defaults = { Enabled = false; Multiplier = 2; Flow = 35; Fast = true; Adaptive = false; Target = 120; Aml = false; AmlFlow = true; Game = 0; Smsr = false; SmsrStrength = 100; Tri = false
                     TriPitch = 16; TriRounds = 2; TriFine = 2; TriFull = 2; TriHome = 4; TriTop = 3; TriExact = 2; TriRelax = 2 }

    let readSettings () =
        try
            if not (File.Exists configFile) then defaults
            else
                let values =
                    File.ReadAllLines configFile
                    |> Array.choose (fun line -> match line.Split('=', 2) with | [| k; v |] when not (k.StartsWith "#") -> Some (k.Trim(), v.Trim()) | _ -> None)
                    |> dict
                let number key fallback = match values.TryGetValue key with | true, v -> (match Int32.TryParse v with | true, n -> n | _ -> fallback) | _ -> fallback
                { Enabled = number "enabled" 1 <> 0
                  Multiplier = Math.Clamp(number "multiplier" 2, 1, 6)
                  Flow = Math.Clamp(number "flow" 35, 25, 100)
                  Fast = number "search" 1 <> 0
                  Adaptive = number "mode" 0 = 1
                  Target = Math.Clamp(number "target" 120, 30, 1000)
                  Aml = number "aml" 0 <> 0
                  AmlFlow = number "aml_flow" 1 <> 0
                  Game = Math.Clamp(number "game" 0, 0, 7)
                  Smsr = number "smsr" 0 <> 0
                  SmsrStrength = Math.Clamp(number "smsr_strength" 100, 0, 200)
                  Tri = number "tri" 0 <> 0
                  TriPitch = number "tri_pitch" 16; TriRounds = number "tri_rounds" 2; TriFine = number "tri_fine" 2; TriFull = number "tri_full" 2
                  TriHome = number "tri_home" 4; TriTop = number "tri_top" 3; TriExact = number "tri_exact" 2; TriRelax = number "tri_relax" 2 }
        with _ -> defaults

    let writeSettings (s: LinuxSettings) =
        try
            Directory.CreateDirectory(Path.GetDirectoryName configFile) |> ignore
            let text = StringBuilder()
            text.AppendLine("# MFG SUPER MOTION for Linux - written by the app; the layer reads it every second.") |> ignore
            text.Append("enabled=").AppendLine(if s.Enabled then "1" else "0") |> ignore
            text.Append("multiplier=").AppendLine(string s.Multiplier) |> ignore
            text.Append("flow=").AppendLine(string s.Flow) |> ignore
            text.Append("search=").AppendLine(if s.Fast then "1" else "0") |> ignore
            text.Append("mode=").AppendLine(if s.Adaptive then "1" else "0") |> ignore
            text.Append("target=").AppendLine(string s.Target) |> ignore
            text.Append("aml=").AppendLine(if s.Aml then "1" else "0") |> ignore
            text.Append("aml_flow=").AppendLine(if s.AmlFlow then "1" else "0") |> ignore
            text.Append("game=").AppendLine(string s.Game) |> ignore
            text.Append("smsr=").AppendLine(if s.Smsr then "1" else "0") |> ignore
            text.Append("smsr_strength=").AppendLine(string s.SmsrStrength) |> ignore
            text.Append("tri=").AppendLine(if s.Tri then "1" else "0") |> ignore
            for key, value in [ "tri_pitch", s.TriPitch; "tri_rounds", s.TriRounds; "tri_fine", s.TriFine; "tri_full", s.TriFull
                                "tri_home", s.TriHome; "tri_top", s.TriTop; "tri_exact", s.TriExact; "tri_relax", s.TriRelax ] do
                text.Append(key).Append('=').AppendLine(string value) |> ignore
            let temporary = configFile + ".new"
            File.WriteAllText(temporary, text.ToString())
            File.Move(temporary, configFile, true)
            true
        with _ -> false

    /// What the layer said last, when it said it within the last three seconds (a game is running with it).
    let readStatus () =
        try
            let info = FileInfo statusFile
            if not info.Exists || DateTime.UtcNow - info.LastWriteTimeUtc > TimeSpan.FromSeconds 3.0 then None
            else
                let values =
                    File.ReadAllLines statusFile
                    |> Array.choose (fun line -> match line.Split('=', 2) with | [| k; v |] -> Some (k.Trim(), v.Trim()) | _ -> None)
                    |> dict
                let number key = match values.TryGetValue key with | true, v -> (match Int32.TryParse v with | true, n -> n | _ -> 0) | _ -> 0
                let text key = match values.TryGetValue key with | true, v -> v | _ -> ""
                Some { Program = text "program"; GameFps = number "game_fps"; ShownFps = number "shown_fps"; Working = number "working" = 1; Size = text "size"; Aml = text "aml" }
        with _ -> None

    let isShipped () = File.Exists shippedLibrary
    let isInstalled () = File.Exists manifestFile && File.Exists installedLibrary
    /// The installed library is not the one that comes with this app (an update of the app brings a newer one).
    let isOutdated () =
        try isInstalled () && isShipped () && FileInfo(installedLibrary).Length <> FileInfo(shippedLibrary).Length with _ -> false

    let private manifest () =
        let path = installedLibrary.Replace("\\", "\\\\").Replace("\"", "\\\"")
        "{\n" +
        "    \"file_format_version\": \"1.2.0\",\n" +
        "    \"layer\": {\n" +
        "        \"name\": \"VK_LAYER_MFGSM_frame_generation\",\n" +
        "        \"type\": \"GLOBAL\",\n" +
        "        \"library_path\": \"" + path + "\",\n" +
        "        \"api_version\": \"1.3.0\",\n" +
        "        \"implementation_version\": \"1\",\n" +
        "        \"description\": \"MFG SUPER MOTION - SMFG frame generation\",\n" +
        "        \"enable_environment\": { \"MFGSM\": \"1\" },\n" +
        "        \"disable_environment\": { \"DISABLE_MFGSM\": \"1\" }\n" +
        "    }\n" +
        "}\n"

    /// Put the layer where games find it. Error: why not.
    let install () =
        try
            if not (isShipped ()) then Error "the layer is not beside the app (layer/libmfgsm_vk.so)"
            else
                Directory.CreateDirectory(Path.GetDirectoryName installedLibrary) |> ignore
                // (Copied beside, then put in place: a game running with the old one keeps its own until it closes.)
                let temporary = installedLibrary + ".new"
                File.Copy(shippedLibrary, temporary, true)
                File.Move(temporary, installedLibrary, true)
                Directory.CreateDirectory(Path.GetDirectoryName manifestFile) |> ignore
                File.WriteAllText(manifestFile, manifest ())
                if not (File.Exists configFile) then writeSettings defaults |> ignore
                Ok ()
        with ex -> Error ex.Message

    let uninstall () =
        try
            if File.Exists manifestFile then File.Delete manifestFile
            if File.Exists installedLibrary then File.Delete installedLibrary
            if File.Exists sessionFile then File.Delete sessionFile
            Ok ()
        with ex -> Error ex.Message

    /// The screen's refresh rate (what Adaptive aims at while no target is typed): xrandr's current mode - X11, and
    /// XWayland under Wayland - else 60.
    let screenRate =
        lazy
            (try
                let info = Diagnostics.ProcessStartInfo("xrandr", "--current", UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true)
                use p = Diagnostics.Process.Start info
                let output = p.StandardOutput.ReadToEnd()
                p.WaitForExit 2000 |> ignore
                output.Split('\n')
                |> Array.tryPick (fun line ->
                    line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                    |> Array.tryPick (fun word ->
                        if word.Contains "*" then
                            match Double.TryParse(word.Replace("*", "").Replace("+", ""), Globalization.NumberStyles.Float, Globalization.CultureInfo.InvariantCulture) with
                            | true, rate when rate >= 30.0 && rate <= 1000.0 -> Some (int (Math.Round rate))
                            | _ -> None
                        else None))
                |> Option.defaultValue 60
             with _ -> 60)

    /// MFGSM=1 for the whole session (every Vulkan program started after logging in again).
    let isEverywhere () = File.Exists sessionFile
    let setEverywhere (on: bool) =
        try
            if on then
                Directory.CreateDirectory(Path.GetDirectoryName sessionFile) |> ignore
                File.WriteAllText(sessionFile, "# MFG SUPER MOTION: frame generation in every Vulkan game (written by the app).\nMFGSM=1\n")
            elif File.Exists sessionFile then File.Delete sessionFile
            Ok ()
        with ex -> Error ex.Message
