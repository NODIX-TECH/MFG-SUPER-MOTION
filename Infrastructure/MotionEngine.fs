namespace ARMO.Infrastructure

open System
open System.Diagnostics
open System.Globalization
open System.IO
open System.Text
open System.Threading
open ARMO.Models

/// A graphics card as the engine lists it (Engine/lf_gpu_core.inl, "--cards").
type CardInfo =
    { Index: int            // the engine's number for it ("gpu=<number>")
      Dlss: bool            // DLSS MFG can run on it (an NVIDIA RTX card)
      Xess: bool            // XeSS MFG can run on it (Intel's library says so: not on older graphics of Intel's own)
      Name: string }        // as Windows names it; a second card of the same name ends in " (2)"

/// What the engine process is doing, as the page shows it.
type EnginePhase =
    | Idle
    | Starting
    | Picking
    | Running

/// One snapshot of the engine's own status lines (Engine/sm_control.inl).
type EngineStatus =
    { Phase: EnginePhase
      Countdown: int          // seconds left to click the game
      BaseFps: int            // real frames a second, -1 none yet
      OutputFps: int          // all frames a second, -1 none yet
      Width: int
      Height: int
      Api: string             // "dxgi" or "wgc"
      Placement: string       // "borderless", "scaled", "window"
      Multiplier: int         // running multiplier, 0 off
      MfgUnavailable: bool
      VrrFixed: bool          // VRR asked for and not reaching the screen
      Learning: int           // Adaptive Motion Learning: -1 not said (or off), 0..100 how far along, 101 learnt
      Error: string }         // the engine's error code, "" none

module EngineStatus =
    let idle =
        { Phase = Idle; Countdown = 0; BaseFps = -1; OutputFps = -1; Width = 0; Height = 0; Api = ""
          Placement = ""; Multiplier = 0; MfgUnavailable = false; VrrFixed = false; Learning = -1; Error = "" }

/// What Adaptive Motion Learning keeps for one game (Engine/sm_learn.inl: a few lines of text).
type LearnedGame =
    { File: string            // the file itself
      Game: string            // the game's program, by its name
      Percent: int            // how far along, 0..100
      Learned: bool
      Updated: string }       // when it was last written, as the file says it

module LearnedGames =
    let folder = AppData.path [| "Learned" |]

    /// Every game something is kept for, the last played first.
    let list () =
        try
            if not (Directory.Exists folder) then [||]
            else
                Directory.GetFiles(folder, "*.aml")
                |> Array.sortByDescending File.GetLastWriteTimeUtc
                |> Array.truncate 200
                |> Array.choose (fun file ->
                    try
                        let lines =
                            File.ReadLines file |> Seq.truncate 12
                            |> Seq.choose (fun line ->
                                match line.IndexOf '=' with
                                | -1 -> None
                                | at -> Some (line.Substring(0, at), line.Substring(at + 1).Trim()))
                            |> dict
                        let value key = match lines.TryGetValue key with | true, v -> v | _ -> ""
                        let number key = match Int32.TryParse(value key) with | true, v -> v | _ -> 0
                        Some { File = file
                               Game = (if value "game" = "" then Path.GetFileNameWithoutExtension file else value "game")
                               Percent = Math.Clamp(number "percent", 0, 100)
                               Learned = value "learned" = "1"
                               Updated = value "updated" }
                    with _ -> None)
        with _ -> [||]

    let delete (game: LearnedGame) =
        // (And what was learnt of its read-outs, kept beside it.)
        try
            if File.Exists game.File then File.Delete game.File
            let hud = Path.ChangeExtension(game.File, ".hud")
            if File.Exists hud then File.Delete hud
        with _ -> ()


/// Starts, steers and stops the native engine (Engine\nvngx.dll, an executable:
/// NGX checks its caller's file name). Everything it needs goes in as text - a
/// start file, then one "key=value" line for each control that moves.
/// The pointer as it was (round 43): the engine's Hide / Scale cursor swap Windows' pointers and Adjust cursor speed
/// sets its speed, and put both back when they end - or, ended from outside, leave a note (cursor.state) that this
/// puts back.
module CursorRestore =
    [<System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SystemParametersInfoW")>]
    extern bool SystemParametersInfo(uint32 action, uint32 param, nativeint value, uint32 winIni)

    let fromNote (folder: string) =
        let note = Path.Combine(folder, "cursor.state")
        try
            if File.Exists note then
                let values =
                    File.ReadAllLines note
                    |> Array.choose (fun line -> match line.Split('=') with | [| k; v |] -> Some (k.Trim(), v.Trim()) | _ -> None)
                    |> dict
                if values.ContainsKey "cursors" && values["cursors"] = "1" then SystemParametersInfo(0x0057u, 0u, 0n, 0u) |> ignore
                match values.TryGetValue "speed" with
                | true, v ->
                    match Int32.TryParse v with
                    | true, speed when speed >= 1 && speed <= 20 -> SystemParametersInfo(0x0071u, 0u, nativeint speed, 0u) |> ignore
                    | _ -> ()
                | _ -> ()
                File.Delete note
        with _ -> ()

type MotionEngine() =
    let gate = obj ()
    let changed = Event<EngineStatus>()
    let mutable proc: Process = null
    let mutable status = EngineStatus.idle
    let mutable sent: (string * string) list = []
    let mutable keepLog = false          // the engine keeps its log (Settings: off)
    let mutable stopping = false
    // (Console Link: a window named outright - the console's stream - taken at once, no countdown.)
    let mutable targetWindow = 0UL
    let mutable cards: CardInfo array = [||]
    // Lines for the engine that are not a profile's: what it writes on the frame, in the app's language.
    let mutable texts: (string * string) list = []
    // Round 72, App Mods: every line the engine writes, for the mods; and lines a mod puts in the start file.
    let line = Event<string>()
    let mutable extras: (string * string) list = []
    // Round 73, Linux: there is no engine program - the engine is the Vulkan layer in the game (LinuxLayer). START
    // writes it on, STOP off, an option moved is written at once (the layer reads its settings every second), and
    // what it says is read each second while it is on.
    let linux = OperatingSystem.IsLinux()
    let mutable linuxOn = false
    let mutable linuxTimer: Timer = null
    let mutable linuxOptions = Preferences.defaultOptions
    let linuxSettings (options: DisplayOptions) on : LinuxSettings =
        { Enabled = on && options.FrameGeneration
          Multiplier = (if options.FrameGeneration then options.Mode + 2 else 1)
          Flow = max 25 (Preferences.effectiveFlow options)
          Fast = options.MsmSearch = 1
          Adaptive = options.Adaptive
          Target = (if options.AdaptiveFps >= 30 then options.AdaptiveFps else LinuxLayer.screenRate.Value)
          // (Round 74: Adaptive Motion Learning on Linux too - the Windows engine's own, in the layer.)
          Aml = options.AdaptiveLearning
          AmlFlow = options.AdaptiveFlow
          Game = options.GameType
          // (Round 88n: SMSR 1.0 in play - "Upscaler" SMSR, while the layer is on; with frame generation off too.)
          Smsr = on && options.Upscaler = 5
          SmsrStrength = 100 + options.SmsrBoost
          // (Round 101: "Triangle Mesh" - the layer's SMFG frames by a mesh of triangles.)
          Tri = options.TriangleMesh
          // (Round 114: and SMT's own numbers from its gear, as the Windows engine gets them.)
          TriPitch = Preferences.triValue Preferences.triPitches 0 options.TriPitch
          TriRounds = Preferences.triValue Preferences.triRounds 1 options.TriRounds
          TriFine = Preferences.triValue Preferences.triFines 2 options.TriFine
          TriFull = Preferences.triValue Preferences.triFulls 3 options.TriFull
          TriHome = Preferences.triValue Preferences.triHomes 4 options.TriHome
          TriTop = Preferences.triValue Preferences.triRanges 5 options.TriRange
          TriExact = Preferences.triValue Preferences.triExacts 6 options.TriExact
          TriRelax = Preferences.triValue Preferences.triRelaxes 7 options.TriRelax
          Smooth = options.UltraSmooth
          EvenPacing = not options.PairPacing
          LowLatency = options.LowLatency
          Vrr = options.Vrr
          Hdr = options.Hdr
          ScreenLimit = options.LimitToScreen }
    // (An app closed without stopping last time left the layer on: it starts off, as START MOTION shows.)
    do if linux && File.Exists LinuxLayer.configFile then
        let left = LinuxLayer.readSettings ()
        if left.Enabled then LinuxLayer.writeSettings { left with Enabled = false } |> ignore

    // Where the engine is just now (EngineStore: beside the program in a build folder, downloaded otherwise).
    let enginePath () =
        match EngineStore.path () with
        | Some path -> path
        | None -> Path.Combine(AppContext.BaseDirectory, "Engine", "nvngx.dll")
    let dataFolder = AppData.path [| "Engine" |]
    // (An engine ended from outside last time may have left the pointer changed.)
    do CursorRestore.fromNote dataFolder

    let publish next =
        let fire = lock gate (fun () ->
            if status = next then false
            else
                status <- next
                true)
        if fire then changed.Trigger next

    let update change = publish (change (lock gate (fun () -> status)))

    /// The engine's options for a profile, in the engine's own words.
    let optionLines (options: DisplayOptions) =
        let flag value = if value then "1" else "0"
        // The card picked ("Preferred GPU"), when it is there and the generator can run on it; else the engine picks.
        let card =
            cards |> Array.tryFind (fun c -> c.Name = options.Gpu && (match options.Generator with | 1 -> c.Xess | 2 | 3 -> true | _ -> c.Dlss))
                  |> Option.map _.Index |> Option.defaultValue -1
        // (Round 88c: SMAI is the engine's MSM path - generator 2 - with SMFG Net making the frames: msm_ai 1.)
        [ "generator", string (if options.Generator = 3 then 2 else options.Generator)
          // (Round 85: XeSS MFG on the tensor cores - the engine takes it on an RTX 20/30 only, at its start.)
          "xess_tensor", flag options.XessTensor
          // (Round 90: SMHUD - "hud_ai" - put aside at the owner's word, 2026-11-09: not sent; the engine keeps it off.)
          "gpu", string card
          "mfg", flag options.FrameGeneration
          // (Round 54, Mode "Adaptive": the frames shown at an even beat, as many as the target asks; the most between
          // two real frames is x6's.)
          // (x2.5 and the like go by the beat as Adaptive does: the generator is made for x6's most, the beat takes what it needs.)
          "multiplier", string (if options.Adaptive || Preferences.multiplierTenths options % 10 <> 0 then 6 else Preferences.multiplierTenths options / 10)
          // (Round 85, GitHub 73: x2.5 and the like - the game's own rate times it, evenly; 0 a whole multiplier.)
          "ratio", (let tenths = Preferences.multiplierTenths options in if options.Adaptive || tenths % 10 = 0 then "0" else string tenths)
          "adaptive", (if not options.Adaptive then "0" elif options.AdaptiveFps >= 30 then string options.AdaptiveFps else "1")
          "flow", string (max 25 options.Flow)
          "game", string options.GameType
          "queue", string options.QueueTarget
          "still", flag options.StillPixels
          "fixed_beat", flag (not options.PairPacing)
          // (Round 96: "Ultra Smooth" - Even pacing and a frame's time more delay kept in hand; the engine's `smooth`.)
          "smooth", flag options.UltraSmooth
          // (Round 99: "Triangle Mesh" - SMFG's frames by msm_tri.h; the engine's `msm_tri`.)
          // (1.1.0, "SMT": with SMFG its own switch, with DLSS MFG / XeSS MFG another - on for SMFG to begin with, off for them.)
          "msm_tri", flag (match options.Generator with | 2 -> options.TriangleMesh | 0 | 1 -> options.SmtAssist | _ -> false)
          "tri_pitch", string (Preferences.triValue Preferences.triPitches 0 options.TriPitch)
          "tri_rounds", string (Preferences.triValue Preferences.triRounds 1 options.TriRounds)
          "tri_fine", string (Preferences.triValue Preferences.triFines 2 options.TriFine)
          "tri_full", string (Preferences.triValue Preferences.triFulls 3 options.TriFull)
          "tri_home", string (Preferences.triValue Preferences.triHomes 4 options.TriHome)
          "tri_top", string (Preferences.triValue Preferences.triRanges 5 options.TriRange)
          "tri_exact", string (Preferences.triValue Preferences.triExacts 6 options.TriExact)
          "tri_relax", string (Preferences.triValue Preferences.triRelaxes 7 options.TriRelax)
          "guard", "0"
          "msm_search", string options.MsmSearch
          "aml", flag options.AdaptiveLearning
          "aml_flow", flag options.AdaptiveFlow
          "aml_guide", flag (not options.AmlGuideOff)
          "dlssg_preset", string ([| 0; 3; 2; 1 |][Math.Clamp(options.DlssPreset, 0, 3)])
          "frame_limit", string options.FrameLimit
          "game_fps", string options.GameFps
          "multi_display", flag options.MultiDisplay
          "crop_l", string options.CropLeft
          "crop_t", string options.CropTop
          "crop_r", string options.CropRight
          "crop_b", string options.CropBottom
          "cursor_clip", flag options.CursorClip
          "cursor_speed", flag options.CursorSpeed
          "cursor_hide", flag options.CursorHide
          "cursor_scale", flag options.CursorScale
          "screen_limit", flag options.LimitToScreen
          "vrr", flag options.Vrr
          "lock", flag options.LockRefresh
          "sync", string options.SyncMode
          "low_latency", flag options.LowLatency
          // (Round 88c: SMAI, a generator of its own - the old switch under SMFG is gone; SMFG never asks for the network.)
          "msm_ai", flag (options.Generator = 3)
          "hdr", flag options.Hdr
          "gpu_class", (if options.GpuPriority = 0 then "1" else "0")
          // (Round 79: 2 the counter with its frame time line.)
          "counter", (if not options.FpsCounter then "0" elif options.FpsCounterAdvanced then "2" else "1")
          // (Round 85, GitHub 71: its size, percent.)
          "counter_size", string options.FpsCounterSize
          "spout", flag options.Spout
          "upscaler", string options.Upscaler
          "dlss5_dll", (if isNull options.Dlss5Dll then "" else options.Dlss5Dll.Trim())
          "dlss5_model", string options.Dlss5Model
          "dlss5_style", string options.Dlss5Style
          "dlss5_scale", string options.Dlss5Scale
          "dlss5_passes", string options.Dlss5Passes
          "dlss5_intensity", string options.Dlss5Intensity
          "dlss5_tone", string options.Dlss5Tone
          "dlss5_structure", string options.Dlss5Structure
          "dlss5_skin", string options.Dlss5Skin
          "dlss5_denoise", string options.Dlss5Denoise
          "dlss5_edge", string options.Dlss5Edge
          "dlss5_keep_colors", string options.Dlss5KeepColors
          "dlss5_low_light", string options.Dlss5LowLight
          "dlss5_bright", string options.Dlss5Brightness
          "dlss5_contrast", string options.Dlss5Contrast
          "dlss5_sat", string options.Dlss5Saturation
          "dlss5_warm", string options.Dlss5Warmth
          "smsr_strength", string (100 + options.SmsrBoost)
          "sharpness", string options.Sharpness
          // (Round 86: Bloom on the real frames, before any generator reads them - lf_bloom.inl.)
          "bloom", string options.Bloom
          "bloom_wide", string options.BloomWide
          "over_sharpness", string options.OverSharpness
          "stretch", flag options.Stretch
          "capture_api", string options.CaptureApi
          // The engine's numbers: 2 the generator's own, 0 NVIDIA Optical Flow, 1 the GPU estimate.
          // (MSM FG finds the motion itself: it is what it is.)
          "motion", (match options.Generator with
                     | 1 -> (match options.XessMotionSource with | 1 -> "1" | 2 -> "0" | _ -> "2")
                     | 2 | 3 -> "2"
                     | _ -> (match options.MotionSource with | 1 -> "0" | 2 -> "1" | _ -> "2"))
          "window_mode", string options.WindowMode
          "delay", string (Math.Clamp(options.Countdown, 0, Preferences.countdownMax)) ]

    let number (text: string) =
        match Int32.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture) with
        | true, value -> value
        | _ -> -1

    let onLine (text: string) =
        (try line.Trigger text with _ -> ())
        let line = text
        if not (String.IsNullOrEmpty line) && line[0] = '@' then
            let parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
            match parts[0] with
            | "@state" when parts.Length > 1 ->
                match parts[1] with
                | "picking" -> update (fun s -> { s with Phase = Picking; Countdown = -1 })
                | "starting" -> update (fun s -> { s with Phase = Starting })
                | "running" -> update (fun s -> { s with Phase = Running; Countdown = 0 })
                | _ -> ()
            | "@pick" when parts.Length > 1 -> update (fun s -> { s with Phase = Picking; Countdown = max 0 (number parts[1]) })
            | "@source" when parts.Length > 5 ->
                update (fun s -> { s with Width = number parts[2]; Height = number parts[3]; Api = parts[4]; Placement = parts[5] })
            | "@fps" when parts.Length > 2 -> update (fun s -> { s with BaseFps = number parts[1]; OutputFps = number parts[2] })
            | "@mfg" when parts.Length > 1 ->
                match parts[1] with
                | "off" -> update (fun s -> { s with Multiplier = 0; MfgUnavailable = false })
                | "unavailable" -> update (fun s -> { s with Multiplier = 0; MfgUnavailable = true })
                | value -> update (fun s -> { s with Multiplier = max 0 (number value); MfgUnavailable = false })
            | "@vrr" when parts.Length > 1 -> update (fun s -> { s with VrrFixed = parts[1] = "fixed" })
            // "@aml off | learning <percent> | learned <percent> <delay>"
            | "@aml" when parts.Length > 1 ->
                let learning =
                    match parts[1] with
                    | "learned" -> 101
                    | "learning" when parts.Length > 2 -> Math.Clamp(number parts[2], 0, 100)
                    | _ -> -1
                update (fun s -> { s with Learning = learning })
            | "@error" when parts.Length > 1 -> update (fun s -> if s.Error = "" then { s with Error = parts[1] } else s)
            | _ -> ()

    // Linux: what the layer said this last second (a game running with it), as the engine's lines would say it - for
    // the page, and for the mods ("@fps <game> <shown>").
    let linuxPoll () =
        match LinuxLayer.readStatus () with
        | Some said ->
            let size = said.Size.Split('x')
            let side i = if size.Length = 2 then max 0 (number size[i]) else 0
            let options = lock gate (fun () -> linuxOptions)
            // ("learning 40", "learned 100 50", "off": as the Windows engine's @aml line.)
            let aml = said.Aml.Split(' ', StringSplitOptions.RemoveEmptyEntries)
            let learning = if aml.Length >= 1 && aml[0] = "learned" then 101 elif aml.Length >= 2 && aml[0] = "learning" then Math.Clamp(number aml[1], 0, 100) else -1
            update (fun s -> { s with Phase = Running; BaseFps = said.GameFps; OutputFps = said.ShownFps; Width = side 0; Height = side 1; Api = "Vulkan"
                                      Multiplier = (if not said.Working then 0 elif options.Adaptive then 6 else options.Mode + 2); Learning = learning })
            (try line.Trigger(sprintf "@fps %d %d" said.GameFps said.ShownFps) with _ -> ())
        | None -> update (fun s -> { s with BaseFps = -1; OutputFps = -1; Width = 0; Height = 0 })

    let linuxStop () =
        let timer = lock gate (fun () ->
            let t = linuxTimer
            linuxTimer <- null
            linuxOn <- false
            t)
        if not (isNull timer) then timer.Dispose()
        LinuxLayer.writeSettings (linuxSettings (lock gate (fun () -> linuxOptions)) false) |> ignore
        publish EngineStatus.idle

    let onExit (ended: Process) =
        let mine = lock gate (fun () ->
            if obj.ReferenceEquals(ended, proc) then
                proc <- null
                sent <- []
                true
            else false)
        if mine then
            CursorRestore.fromNote dataFolder
            let code = try ended.ExitCode with _ -> 0
            ended.Dispose()
            let last = lock gate (fun () -> status)
            let error =
                if last.Error <> "" then last.Error
                elif code <> 0 && not stopping then "failed"
                else ""
            publish { EngineStatus.idle with Error = error }

    member _.Changed = changed.Publish
    member _.Status = lock gate (fun () -> status)
    member _.IsActive = lock gate (fun () -> if linux then linuxOn else not (isNull proc))
    member _.IsInstalled = if linux then LinuxLayer.isShipped () || LinuxLayer.isInstalled () else File.Exists (enginePath ())
    member _.LogPath = Path.Combine(dataFolder, "engine.log")
    /// What the engine writes on the frame (its start file's "aml_say0" .. lines): taken at the next start.
    member _.SetTexts(lines: (string * string) list) = lock gate (fun () -> texts <- lines)
    /// Whether the engine keeps its log (Settings, off): taken at the next start.
    member _.SetLog(on: bool) = lock gate (fun () -> keepLog <- on)
    /// Every line the engine writes (round 72, App Mods) - raised on the thread that reads them.
    member _.Line = line.Publish
    /// A mod's lines for the start file, after the app's own: taken at the next start.
    member _.SetExtras(lines: (string * string) list) = lock gate (fun () -> extras <- lines)
    /// One option for the running engine now, as its start file has them; nothing when it is not running.
    member _.SendLine(key: string, value: string) =
        let running = lock gate (fun () -> proc)
        if not (isNull running) then
            try
                let one (s: string) = if isNull s then "" else s.Replace('\n', ' ').Replace('\r', ' ')
                running.StandardInput.WriteLine((one key).Replace('=', ' ') + "=" + one value)
                running.StandardInput.Flush()
            with :? IOException | :? InvalidOperationException | :? ObjectDisposedException -> ()

    /// The graphics cards of this machine as the engine sees them (asked once; empty when it cannot be asked).
    member _.Cards() =
        if not linux && cards.Length = 0 && File.Exists (enginePath ()) then
            try
                let info =
                    ProcessStartInfo(
                        FileName = (enginePath ()),
                        Arguments = "--cards",
                        WorkingDirectory = Path.GetDirectoryName (enginePath ()),
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        StandardOutputEncoding = Encoding.UTF8)
                use listing = Process.Start info
                // An engine that does not know "--cards" would start for real: it is given six seconds and ended.
                let reading = listing.StandardOutput.ReadToEndAsync()
                let lines =
                    if reading.Wait 6000 then reading.Result.Split('\n')
                    else
                        (try listing.Kill() with _ -> ())
                        [||]
                listing.WaitForExit 1000 |> ignore
                let seen = Collections.Generic.Dictionary<string, int>()
                // "@xess <number> <1|0>": what Intel's library says to that card (an engine that does not say: yes).
                let refused =
                    lines |> Array.choose (fun line ->
                        let parts = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries)
                        if parts.Length = 3 && parts[0] = "@xess" && parts[2] = "0" then Some (number parts[1]) else None)
                cards <-
                    lines |> Array.choose (fun line ->
                        // "@card <number> <DLSS MFG: 1|0> <MB> <name>"
                        let parts = line.Trim().Split(' ', 5, StringSplitOptions.RemoveEmptyEntries)
                        if parts.Length = 5 && parts[0] = "@card" && number parts[1] >= 0 then
                            let name = parts[4].Trim()
                            let count = (match seen.TryGetValue name with | true, n -> n | _ -> 0) + 1
                            seen[name] <- count
                            Some { Index = number parts[1]; Dlss = parts[2] = "1"; Xess = not (Array.contains (number parts[1]) refused)
                                   Name = (if count > 1 then sprintf "%s (%d)" name count else name) }
                        else None)
            with _ -> ()
        cards

    /// Is VRR on for a window on this PC (Engine/lf_vrr.inl, "--vrr"): Some true / Some false, None when it
    /// cannot be told (or the engine does not know the question).
    member _.SystemVrr() =
        if linux || not (File.Exists (enginePath ())) then None
        else
            try
                let info =
                    ProcessStartInfo(
                        FileName = (enginePath ()),
                        Arguments = "--vrr",
                        WorkingDirectory = Path.GetDirectoryName (enginePath ()),
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true,
                        StandardOutputEncoding = Encoding.UTF8)
                use asking = Process.Start info
                // An engine that does not know "--vrr" would start for real: it is given six seconds and ended.
                let reading = asking.StandardOutput.ReadToEndAsync()
                let lines =
                    if reading.Wait 6000 then reading.Result.Split('\n')
                    else
                        (try asking.Kill() with _ -> ())
                        [||]
                asking.WaitForExit 1000 |> ignore
                // "@vrr-system <1|0|-1> <why>"
                lines |> Array.tryPick (fun line ->
                    let parts = line.Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries)
                    if parts.Length >= 2 && parts[0] = "@vrr-system" then
                        match parts[1] with
                        | "1" -> Some (Some true)
                        | "0" -> Some (Some false)
                        | _ -> Some None
                    else None)
                |> Option.flatten
            with _ -> None

    /// Start the engine for a profile. False when it cannot be started at all.
    /// now: the window in front is taken at once, no countdown (started by the shortcut in a game).
    member this.Start(options: DisplayOptions, now: bool) =
        if linux then
            if this.IsActive then true
            else
                // (Installed, or brought up to the app's own, at the first START.)
                let ready = if LinuxLayer.isInstalled () && not (LinuxLayer.isOutdated ()) then Ok () else LinuxLayer.install ()
                match ready with
                | Error _ ->
                    publish { EngineStatus.idle with Error = "missing" }
                    false
                | Ok () ->
                    lock gate (fun () -> linuxOptions <- options)
                    LinuxLayer.writeSettings (linuxSettings options true) |> ignore
                    publish { EngineStatus.idle with Phase = Running; Api = "Vulkan" }
                    lock gate (fun () ->
                        linuxOn <- true
                        linuxTimer <- new Timer((fun _ -> linuxPoll ()), null, 0, 1000))
                    true
        elif this.IsActive then true
        elif not (File.Exists (enginePath ())) then
            publish { EngineStatus.idle with Error = "missing" }
            false
        else
            try
                Directory.CreateDirectory dataFolder |> ignore
                let lines = optionLines options |> List.map (fun (key, value) -> if now && key = "delay" then key, "0" else key, value)
                let startFile = Path.Combine(dataFolder, "start.cfg")
                let text = StringBuilder()
                for key, value in lines do text.Append(key).Append('=').AppendLine(value) |> ignore
                // (One line each, and no '#': the engine takes what follows one for a remark.)
                // (Round 72: a mod's own lines after the app's - the engine takes the last of one key.)
                for key, value in lock gate (fun () -> texts @ extras) do
                    text.Append(key).Append('=').AppendLine(value.Replace('\n', ' ').Replace('\r', ' ').Replace('#', ' ')) |> ignore
                text.Append("log=").AppendLine(if lock gate (fun () -> keepLog) then "1" else "0") |> ignore
#if DEBUG
                // The development build (Debug, run from Visual Studio) only: frames.log beside the engine's log - every
                // frame shown, its gap, the stutters marked (round 45). A released app never writes it.
                text.AppendLine("frame_log=1") |> ignore
#endif
                text.Append("owner=").AppendLine(string Environment.ProcessId) |> ignore
                if targetWindow <> 0UL then
                    text.Append("hwnd=").AppendLine(string targetWindow) |> ignore
                    text.AppendLine("stream_dedup=1") |> ignore
                File.WriteAllText(startFile, text.ToString(), UTF8Encoding(false))
                let info =
                    ProcessStartInfo(
                        FileName = (enginePath ()),
                        Arguments = sprintf "--config \"%s\"" startFile,
                        WorkingDirectory = Path.GetDirectoryName (enginePath ()),
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardInput = true,
                        RedirectStandardOutput = true,
                        StandardOutputEncoding = Encoding.UTF8)
                let started = new Process(StartInfo = info, EnableRaisingEvents = true)
                started.OutputDataReceived.Add(fun args -> if not (isNull args.Data) then onLine args.Data)
                started.Exited.Add(fun _ -> onExit started)
                stopping <- false
                publish { EngineStatus.idle with Phase = Starting }
                lock gate (fun () ->
                    proc <- started
                    sent <- lines)
                if not (started.Start()) then failwith "not started"
                WindowsPerformance.applyToProcess started ProcessPriorityClass.High
                started.BeginOutputReadLine()
                true
            with _ ->
                lock gate (fun () -> proc <- null; sent <- [])
                publish { EngineStatus.idle with Error = "failed" }
                false

    /// Console Link: frame generation on the console's stream window, given outright (no countdown).
    member this.StartOnWindow(options: DisplayOptions, window: uint64) =
        targetWindow <- window
        let started = this.Start(options, true)
        targetWindow <- 0UL
        started

    /// Send the controls that moved since the last time.
    member _.Send(options: DisplayOptions) =
        if linux then
            if lock gate (fun () -> linuxOn) then
                lock gate (fun () -> linuxOptions <- options)
                LinuxLayer.writeSettings (linuxSettings options true) |> ignore
        else
            let running, changes =
                lock gate (fun () ->
                    if isNull proc then null, []
                    else
                        let lines = optionLines options
                        let before = dict sent
                        let changes = lines |> List.filter (fun (key, value) -> not (before.ContainsKey key) || before[key] <> value)
                        sent <- lines
                        proc, changes)
            if not (isNull running) && not changes.IsEmpty then
                try
                    for key, value in changes do running.StandardInput.WriteLine(key + "=" + value)
                    running.StandardInput.Flush()
                with :? IOException | :? InvalidOperationException | :? ObjectDisposedException -> ()

    /// What was learnt for a game is deleted: an engine that is playing that game starts it from nothing.
    member _.Forget(game: string) =
        let running = lock gate (fun () -> proc)
        if not (isNull running) && not (String.IsNullOrWhiteSpace game) then
            try
                running.StandardInput.WriteLine("aml_forget=" + game)
                running.StandardInput.Flush()
            with :? IOException | :? InvalidOperationException | :? ObjectDisposedException -> ()


    /// RivaTuner Statistics Server installed: the engine's program gets RTSS's overlay switched off ("--rtss-quiet"),
    /// so it is seen once - in the game - and not again on the engine's window. Waited for (five seconds at most).
    member _.QuietRtss() =
        let installed =
            try
                use key = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Unwinder\RTSS")
                not (isNull key)
            with _ -> false
        if installed && File.Exists (enginePath ()) then
            try
                let info =
                    ProcessStartInfo(
                        FileName = (enginePath ()),
                        Arguments = "--rtss-quiet",
                        WorkingDirectory = Path.GetDirectoryName (enginePath ()),
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardOutput = true)
                use asking = Process.Start info
                // (An engine that does not know the question would start for real: it is ended.)
                if not (asking.WaitForExit 5000) then (try asking.Kill() with _ -> ())
            with _ -> ()

    /// Round 84: the engine's shaders made for this PC once after each update ("--warm-shaders"), so the first
    /// SMFG start does not stand some 13 s compiling them. True when it is still to be done for this engine.
    member _.ShadersNeedWarm() =
        not linux && File.Exists (enginePath ()) &&
        (let info = FileInfo (enginePath ())
         not (File.Exists (Path.Combine(dataFolder, sprintf "shaders-%d-%d.ready" info.Length info.LastWriteTimeUtc.Ticks))))

    /// Round 84: the shaders made (two minutes at most); the mark written when it went through.
    member _.WarmShaders() =
        try
            let info = FileInfo (enginePath ())
            let start =
                ProcessStartInfo(
                    FileName = (enginePath ()),
                    Arguments = "--warm-shaders",
                    WorkingDirectory = Path.GetDirectoryName (enginePath ()),
                    UseShellExecute = false,
                    CreateNoWindow = true,
                    RedirectStandardOutput = true)
            use warming = Process.Start start
            let said = warming.StandardOutput.ReadToEndAsync()
            // (An engine that does not know the question would start for real: it is ended.)
            if not (warming.WaitForExit 120000) then (try warming.Kill() with _ -> ())
            elif warming.ExitCode = 0 && said.Result.StartsWith "warm " then
                Directory.CreateDirectory dataFolder |> ignore
                File.WriteAllText(Path.Combine(dataFolder, sprintf "shaders-%d-%d.ready" info.Length info.LastWriteTimeUtc.Ticks), said.Result)
        with _ -> ()

    /// Ask the engine to stop; it puts the game's window back as it was.
    member _.Stop() =
        let running = lock gate (fun () -> proc)
        if linux then (if lock gate (fun () -> linuxOn) then linuxStop ())
        elif not (isNull running) then
            stopping <- true
            try
                running.StandardInput.WriteLine "stop"
                running.StandardInput.Flush()
            with :? IOException | :? InvalidOperationException | :? ObjectDisposedException -> ()
            // An engine that does not answer its line is told through its event,
            // and ended when it will not listen to that either.
            ThreadPool.QueueUserWorkItem(fun _ ->
                try
                    if not (running.WaitForExit 4000) then
                        try
                            use stop = EventWaitHandle.OpenExisting "Local\\SuperMotionEngineStop"
                            stop.Set() |> ignore
                        with _ -> ()
                        if not (running.WaitForExit 3000) then running.Kill()
                with _ -> ()) |> ignore

    interface IDisposable with
        member this.Dispose() =
            let running = lock gate (fun () -> proc)
            if linux then (if lock gate (fun () -> linuxOn) then linuxStop ())
            elif not (isNull running) then
                stopping <- true
                try
                    running.StandardInput.WriteLine "stop"
                    running.StandardInput.Flush()
                    if not (running.WaitForExit 3000) then running.Kill()
                with _ -> ()

// Round 55: the main screen's refresh rate, what Adaptive aims at while no target is set.
module ScreenRate =
    [<System.Runtime.InteropServices.DllImport("user32.dll")>]
    extern nativeint GetDC(nativeint window)
    [<System.Runtime.InteropServices.DllImport("user32.dll")>]
    extern int ReleaseDC(nativeint window, nativeint dc)
    [<System.Runtime.InteropServices.DllImport("gdi32.dll")>]
    extern int GetDeviceCaps(nativeint dc, int index)

    let primary () =
        if OperatingSystem.IsLinux() then LinuxLayer.screenRate.Value
        else
            try
                let dc = GetDC 0n
                let rate = GetDeviceCaps(dc, 116)       // VREFRESH
                ReleaseDC(0n, dc) |> ignore
                if rate >= 30 && rate <= 1000 then rate else 60
            with _ -> 60
