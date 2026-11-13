namespace ARMO.Models

open System

[<CLIMutable>]
type DisplayOptions =
    { FrameGeneration: bool
      Generator: int        // 0 DLSS MFG, 1 XeSS MFG, 2 MSM FG (SMFG), 3 SMAI (round 88c); each runs in its own engine process
      Mode: int
      MultiplierCustom: int  // round 85, GitHub 73: "Custom" - any multiplier typed, in tenths (21 = x2.1, 11 .. 60); 0: Mode's whole one
      Flow: int              // used when GameType is "custom"; the other kinds of game bring their own
      Upscaler: int          // 0 off, 1 sharpening only, 2 integer, 3 nearest neighbour, 4 Anime4K, 5 SMSR 1.0 (the engine's own numbers)
      Sharpness: int         // 0..100, used unless Upscaler is off
      OverSharpness: int     // 0..100, on top of it
      // Advanced. Schema says which of these a saved profile already holds:
      // an older file reads them as zero, and normalizeOptions fills them in.
      Schema: int
      CaptureApi: int        // 0 DXGI, 1 WGC
      WindowMode: int        // 0 borderless, 1 scaled, 2 windowed
      Countdown: int         // index into Preferences.countdowns
      QueueTarget: int       // 0, 1, 2
      FrameLimit: int        // frames a second shown, 0 off
      GameFps: int           // the game's frame rate as the person gives it (20..500); 0 the engine reads it from the frames
      Adaptive: bool         // round 54, Mode "Adaptive" (Lossless Scaling's): the frames shown at an even beat of AdaptiveFps; off: "Fixed", the multiplier (Mode)
      AdaptiveFps: int       // Adaptive's target, frames a second (30..1000); 0 the screen's refresh
      // Round 43, as Lossless Scaling has them: the picture stays while another screen's window is in front; pixels left
      // out of the captured window on each side; and the pointer (kept inside, its speed, hidden, scaled).
      MultiDisplay: bool
      CropLeft: int
      CropTop: int
      CropRight: int
      CropBottom: int
      CursorClip: bool
      CursorSpeed: bool
      CursorHide: bool
      CursorScale: bool
      LimitToScreen: bool    // only as many frames in between as fill the screen (off: the multiplier's own count)
      Vrr: bool
      SyncMode: int          // 0 off (tearing allowed), 1 default, 2 VSync, 3 and 4 VSync on every second, every third refresh (the engine's own numbers)
      LowLatency: bool
      Hdr: bool
      GpuPriority: int       // 0 realtime, 1 normal
      Gpu: string            // the card the engine runs on, by its name ("Preferred GPU"); "": the engine picks
      MotionSource: int      // 0 DLSS MFG's own, 1 NVIDIA Optical Flow, 2 GPU estimate
      XessMotionSource: int  // XeSS MFG: 0 it finds the motion itself, 1 the engine's GPU estimate, 2 NVIDIA Optical Flow; kept apart from DLSS MFG's choice
      MsmSearch: int         // MSM FG: 0 its search in full, 1 the lighter one; kept apart too
      XessTensor: bool       // round 85: XeSS MFG on an RTX 20/30's tensor cores (the compatibility package's XeSS-FG engine); false: Intel's library
      HudAi: bool            // round 90: SMHUD - a network finds the game's HUD; frames in between show it as the real frame has it, every generator (a file without it: off)
      Bloom: int             // round 86: Bloom's strength, 0..100 (0 off) - the engine's lf_bloom.inl
      BloomWide: int         // ... drawn out sideways (anamorphic), 0..100
      MsmTensor: bool        // round 86: SMFG on the card's matrix units (SMFG AI, msm_ai.h: RIFE through DirectML) - off unless asked for
      MsmAi: bool            // round 52: SMFG AI - its frames made by the neural network on the card's AI units (off unless asked for)
      AdaptiveLearning: bool // Adaptive Motion Learning: what the hand does to the picture is learnt for each game, and the generator told
      DlssPreset: int        // DLSS MFG's preset: 0 the driver's own, 1 Performance (C), 2 Quality (B), 3 Balanced (A); Quality unless picked
      AdaptiveFlow: bool     // Adaptive Flow Scale (needs Adaptive Motion Learning): the size frames in between are made at follows how fast the game's picture moves
      AmlGuideOff: bool      // round 61: Adaptive Motion Learning's lines at the top of the game left out - its study goes on unseen (a file without it: shown)
      VrrChosen: bool        // round 61: VRR set in the app by the person - kept as they left it, no longer taken from the PC at each start (GitHub 58)

      Spout: bool            // every frame shown is shared as a Spout sender (OBS: Spout2 Capture)
      StillPixels: bool
      // (Round 99, the Triangle Mesh's own settings, for trying: each the choice's place in its list + 1; 0 - a file without it - the default.)
      TriPitch: int; TriRounds: int; TriFine: int; TriFull: int; TriHome: int; TriRange: int; TriExact: int; TriRelax: int
      TriangleMesh: bool     // round 99: "SMT" (was "Triangle Mesh") - SMFG's frames made by a mesh of triangles (3DRS + the rasteriser); 1.1.0: on to begin with
      SmtAssist: bool        // 1.1.0: SMT with DLSS MFG and XeSS MFG - the triangles read the motion and hand it to them (off unless asked for)
      UltraSmooth: bool      // round 96: "Ultra Smooth" - an even beat and a frame's time more delay kept in hand, for the smoothest motion (a file without it: off)
      PairPacing: bool       // round 58: a fixed multiplier's frames laid over each pair (as before) - off: at an even beat of their own ("Even pacing")
      MotionGuard: bool      // no frames in between while the picture moves too fast for them (off unless asked for)
      FpsCounter: bool
      // Round 74: a picture of another shape than the screen (a 4:3 game on a 16:9 screen) stretched over all of it -
      // off: fitted, aspect kept, bars (a file without it: off).
      Stretch: bool
      // Round 79: SMFG's frames on the screen's own refreshes, VSync, each made for its refresh - VRR not used for the
      // output (a file without it: off - on in schema 20, off again from schema 21 at the owner's word). PresentMon on a user's GTA V: 25% of the gaps on the screen a
      // quarter off with VRR (each frame shown when its turn on a full card came), 0.1% locked. FIX PACING.md.
      LockRefresh: bool
      // Round 79: the FPS counter with its frame time line and figures (the counter's own switch still turns it on).
      FpsCounterAdvanced: bool
      // Round 85 (GitHub 71): the counter's size, percent of the usual (a file without it: 0, read as 100).
      FpsCounterSize: int
      // Round 88o: SMSR's strength, percent from 100 (a file without it: 0 - as trained); -100..100 = 0..200%.
      SmsrBoost: int
      GameType: int }        // index into Preferences.gameFlows; the last one is "custom"

[<CLIMutable>]
type Profile =
    { Id: string
      Name: string
      Options: DisplayOptions }

[<CLIMutable>]
type Preferences =
    { Version: int
      Language: int
      Theme: int
      Transparency: bool
      AdvancedOpen: bool
      StartWithWindows: bool
      StartMinimized: bool
      MinimizeToTray: bool
      EngineLog: bool        // the engine keeps a log (engine.log) - for when something is to be looked into; off
      // The START / STOP MOTION shortcut: modifiers * 65536 + virtual key
      // (Windows' RegisterHotKey values); 0 the default, -1 none.
      Hotkey: int
      SelectedProfile: string
      Profiles: Profile array }

module Preferences =
    let schema = 24
    // The sync modes the engine has (see SyncMode).
    let syncModes = 5
    // The upscalers the engine has: off, sharpening only, integer, nearest neighbour, Anime4K, and (round 88k) SMSR 1.0 -
    // our own network, twice the size (Engine\lf_smsr.inl).
    let upscalers = 6
    // Round 99, the Triangle Mesh's settings (the engine's tri_pitch, tri_rounds, tri_fine, tri_full, tri_home, tri_top): what
    // each list offers, and which place is the engine's own. A setting is kept as its place + 1 (0: the default).
    let triPitches = [| 8; 12; 16; 24; 32 |]
    let triRounds = [| 1; 2; 3; 4 |]
    let triFines = [| 0; 1; 2; 3 |]
    let triFulls = [| 0; 1; 2; 3 |]
    let triHomes = [| 0; 2; 4; 8 |]
    let triRanges = [| 2; 3; 4 |]
    let triExacts = [| 0; 1; 2; 3 |]
    let triRelaxes = [| 0; 1; 2; 3 |]
    let triDefaults = [| 2; 1; 2; 2; 2; 1; 2; 2 |]  // pitch 16, 2 rounds, 2, 2, 4 pixels, size 3, 2 exact, 2 relax
    let triPlace (list: int[]) (which: int) (kept: int) = if kept >= 1 && kept <= list.Length then kept - 1 else triDefaults[which]
    let triValue (list: int[]) (which: int) (kept: int) = list[triPlace list which kept]
    // The generators: DLSS MFG, XeSS MFG, MSM FG (SMFG), SMAI (round 88c: SMFG Net makes the frames - the engine's MSM
    // path with msm_ai 1).
    let generators = 4
    let countdowns = [| 3; 5; 10 |]
    // The size frames in between are made at for each kind of game, percent of
    // the picture: auto, 2D, 2.5D, first person, third person, racing, open
    // world (the engine's own table, Engine/sm_options.h, kGameKinds - the same
    // numbers, shown beside the slider). One more kind, "custom", goes by the
    // Flow slider.
    let gameFlows = [| 35; 75; 50; 35; 35; 35; 35 |]
    let gameCustom = gameFlows.Length
    let frameLimitMax = 360
    let cropMax = 4000
    // Game FPS (round 43): "Auto" (0) and the rates games run at.
    let gameFpsRates = [| 0; 30; 40; 45; 48; 50; 60; 72; 75; 90; 100; 120; 144; 165; 180; 200; 240; 280; 300; 360 |]
    // Shift + Alt + M (Windows counts Alt 1, Ctrl 2, Shift 4, Win 8).
    let defaultHotkey = (0x0005 <<< 16) ||| 0x4D

    let defaultOptions =
        { FrameGeneration = true; Generator = 0; Mode = 2; MultiplierCustom = 0; Flow = 35; Upscaler = 0; Sharpness = 50; OverSharpness = 0
          Schema = schema; CaptureApi = 1; WindowMode = 0; Countdown = 1
          QueueTarget = 1; FrameLimit = 0; GameFps = 0; Adaptive = false; AdaptiveFps = 0; MultiDisplay = false; CropLeft = 0; CropTop = 0; CropRight = 0; CropBottom = 0
          CursorClip = false; CursorSpeed = false; CursorHide = false; CursorScale = false; LimitToScreen = false; Vrr = false; SyncMode = 1; LowLatency = true; Hdr = false
          GpuPriority = 0; Gpu = ""; MotionSource = 0; XessMotionSource = 0; MsmSearch = 1; XessTensor = false; HudAi = false; Bloom = 0; BloomWide = 0; MsmTensor = false; MsmAi = false; AdaptiveLearning = false; AdaptiveFlow = true; AmlGuideOff = false; VrrChosen = false; DlssPreset = 2
          Spout = false; StillPixels = true; UltraSmooth = false; TriangleMesh = false; SmtAssist = false; TriPitch = 0; TriRounds = 0; TriFine = 0; TriFull = 0; TriHome = 0; TriRange = 0; TriExact = 0; TriRelax = 0; PairPacing = false; MotionGuard = false; FpsCounter = false; Stretch = false; LockRefresh = false; FpsCounterAdvanced = false; FpsCounterSize = 100; SmsrBoost = 0; GameType = 0 }

    let defaultProfile = { Id = "default"; Name = "Default"; Options = defaultOptions }

    let defaults =
        { Version = 1; Language = 0; Theme = 2; Transparency = false; AdvancedOpen = false
          StartWithWindows = false; StartMinimized = false; MinimizeToTray = false; EngineLog = false; Hotkey = 0
          SelectedProfile = defaultProfile.Id; Profiles = [| defaultProfile |] }

    /// The size frames in between are made at, percent of the picture - the same for DLSS MFG and XeSS MFG.
    let effectiveFlow (options: DisplayOptions) =
        if options.GameType >= 0 && options.GameType < gameCustom then
            // (Round 88j: XeSS MFG is made bigger than the kind of game says - the engine's FlowShare, sm_options.h:
            // 75% on the tensor cores, 60% through Intel's library - unless the share is set by hand.)
            if options.Generator = 1 then max gameFlows[options.GameType] (if options.XessTensor then 75 else 60)
            else gameFlows[options.GameType]
        else options.Flow

    /// Round 85 (GitHub 73): the Fixed multiplier in tenths - Mode's whole one, or the one typed under "Custom" (21 for x2.1).
    let multiplierTenths (options: DisplayOptions) =
        if options.MultiplierCustom > 0 then options.MultiplierCustom else (options.Mode + 2) * 10
    /// The most a custom multiplier may be: what the engine's generators make a pair today (x6).
    let multiplierCustomMost = 60

    let normalizeFrameLimit value =
        if value < 20 then 0 else min frameLimitMax value

    let normalizeOptions options =
        if isNull (box options) then defaultOptions
        else
            // A profile saved before the advanced options existed keeps its own
            // values of the first card and takes the defaults for the rest. One
            // saved before frames in between were rebuilt at the picture's size
            // (schema 4) starts from the Flow that costs the game least - it no
            // longer softens them - and within the screen's refresh rate; one
            // older still (schema 2) also from the generator finding the motion
            // itself. Every one saved before schema 5 has the fast motion guard
            // off - frames in between going missing in every fast turn was worse
            // than the frames it kept from the screen - and the multiplier's own
            // count of frames whatever the screen shows: x6 that came out as 190
            // on a 200 Hz screen read as "less than asked".
            // And every one saved before schema 6 goes by the kind of game,
            // "auto" (an older file reads it as that already): its Flow is kept
            // for "custom", but no longer decides by itself - at 100% it was
            // both the dearest setting and the one that broke first in fast
            // movement, and looked like the best one.
            // Before schema 10 "Upscaler" was a list nothing in the engine listened to (DLSS, FSR, XeSS,
            // Native): whatever it stood at, it is "off" now.
            let options = if options.Schema < 10 then { options with Upscaler = 0; OverSharpness = 0 } else options
            // Before schema 11 there was no sync mode (a file reads it as 0, which is "off" now): "default" is
            // what the engine did then.
            let options = if options.Schema < 11 then { options with SyncMode = 1 } else options
            // Before schema 14 MSM FG started on its search in full: it starts on "Fast" now.
            let options = if options.Schema < 14 then { options with MsmSearch = 1 } else options
            // Before schema 15 there was no Adaptive Flow Scale (a file reads it as off): it starts on.
            let options = if options.Schema < 15 then { options with AdaptiveFlow = true } else options
            // Before schema 18 DLSS MFG's preset was the driver's own: Quality (preset B) is where it starts now, the best looking one.
            let options = if options.Schema < 18 then { options with DlssPreset = 2 } else options
            // Before schema 19 the capture was DXGI (Desktop Duplication) to begin with: measured in a user's GTA V locked at
            // 30 (round 71), every frame reached the screen 21 ms later than by Windows Graphics Capture (53 against 32 ms)
            // and its delay swung twice as much - the "drops" and the lag. WGC is where every profile starts now.
            let options = if options.Schema < 19 then { options with CaptureApi = 1 } else options
            // Before schema 20 there was no "Lock to screen refresh" (a file reads it as off): it starts on - the frames
            // even on the screen (round 79, FIX PACING.md).
            let options = if options.Schema < 20 then { options with LockRefresh = true } else options
            // Before schema 21 "Lock to screen refresh" started on: it is off now - the owner's word (round 81), "pure, like
            // LSFG": SMFG on an even beat of its own, VRR as set.
            let options = if options.Schema < 21 then { options with LockRefresh = false } else options
            // Before schema 22 (1.1.0) "Super Smooth" (was "Ultra Smooth"), Even pacing and SMT with SMFG started off: they
            // start on now - the owner's word. SMT with DLSS MFG / XeSS MFG is a switch of its own and starts off.
            let options = if options.Schema < 22 then { options with UltraSmooth = true; PairPacing = false; TriangleMesh = true; SmtAssist = false } else options
            // Before schema 23 SMT started on with SMFG (1.1.0's first build, the testers' copy): off for every generator now -
            // the owner's word after the testers' round. On only where the person turns it on.
            let options = if options.Schema < 23 then { options with TriangleMesh = false; SmtAssist = false } else options
            // Before schema 24 Super Smooth started on (1.1.0): off now - the owner's word (2026-11-13). Once: on again only
            // where the person turns it on, and kept so.
            let options = if options.Schema < 24 then { options with UltraSmooth = false } else options
            let options =
                if options.Schema >= schema then options
                elif options.Schema >= 8 then options
                elif options.Schema >= 6 then { options with XessMotionSource = 0 }
                elif options.Schema = 5 then
                    { options with GameType = 0 }
                elif options.Schema = 4 then
                    { options with MotionGuard = false; LimitToScreen = false; GameType = 0 }
                elif options.Schema = 3 then
                    { options with Flow = defaultOptions.Flow; LimitToScreen = false; MotionGuard = false; GameType = 0 }
                elif options.Schema = 2 then
                    { options with Flow = defaultOptions.Flow; LimitToScreen = false; MotionSource = 0; MotionGuard = false; GameType = 0 }
                else
                    { defaultOptions with
                        FrameGeneration = options.FrameGeneration; Mode = options.Mode
                        Upscaler = options.Upscaler; Sharpness = options.Sharpness }
            // (A development build kept "Adaptive" as a sixth multiplier, Mode 5: it is the mode of its own now.)
            let options = if options.Mode >= 5 then { options with Mode = 4; Adaptive = true } else options
            // (Round 88g: XeSS MFG on the tensor cores - the RTX compatibility package's XeSS-FG - makes three frames a pair
            // at most (its MaxGeneratedFrames 3): asked for x5 or x6 the engine ran x4 while the app said more - x4 at most.)
            let options =
                if options.Generator = 1 && options.XessTensor then
                    { options with Mode = min options.Mode 2; MultiplierCustom = (if options.MultiplierCustom > 40 then 40 else options.MultiplierCustom) }
                else options
            { options with Mode = Math.Clamp(options.Mode, 0, 4)
                           Bloom = Math.Clamp(options.Bloom, 0, 100)
                           BloomWide = Math.Clamp(options.BloomWide, 0, 100)
                           MultiplierCustom = (if options.MultiplierCustom >= 11 then min options.MultiplierCustom multiplierCustomMost else 0)
                           Generator = Math.Clamp(options.Generator, 0, generators - 1)
                           Flow = Math.Clamp(options.Flow, 25, 100)
                           Upscaler = (if options.Upscaler >= 0 && options.Upscaler < upscalers then options.Upscaler else 0)
                           Sharpness = Math.Clamp(options.Sharpness, 0, 100)
                           OverSharpness = Math.Clamp(options.OverSharpness, 0, 100)
                           Schema = schema
                           CaptureApi = Math.Clamp(options.CaptureApi, 0, 1)
                           WindowMode = Math.Clamp(options.WindowMode, 0, 2)
                           Countdown = Math.Clamp(options.Countdown, 0, countdowns.Length - 1)
                           QueueTarget = Math.Clamp(options.QueueTarget, 0, 2)
                           FrameLimit = normalizeFrameLimit options.FrameLimit
                           GameFps = (if options.GameFps >= 20 && options.GameFps <= 500 then options.GameFps else 0)
                           AdaptiveFps = (if options.AdaptiveFps >= 30 && options.AdaptiveFps <= 1000 then options.AdaptiveFps else 0)
                           CropLeft = Math.Clamp(options.CropLeft, 0, cropMax)
                           CropTop = Math.Clamp(options.CropTop, 0, cropMax)
                           CropRight = Math.Clamp(options.CropRight, 0, cropMax)
                           CropBottom = Math.Clamp(options.CropBottom, 0, cropMax)
                           SyncMode = (if options.SyncMode >= 0 && options.SyncMode < syncModes then options.SyncMode else 1)
                           GpuPriority = Math.Clamp(options.GpuPriority, 0, 1)

                           // (A file saved before schema 9 has no card: read as null.)
                           Gpu = (if isNull options.Gpu then "" else options.Gpu.Trim())
                           MotionSource = Math.Clamp(options.MotionSource, 0, 2)
                           XessMotionSource = Math.Clamp(options.XessMotionSource, 0, 2)
                           MsmSearch = Math.Clamp(options.MsmSearch, 0, 1)
                           TriPitch = Math.Clamp(options.TriPitch, 0, triPitches.Length); TriRounds = Math.Clamp(options.TriRounds, 0, triRounds.Length)
                           TriFine = Math.Clamp(options.TriFine, 0, triFines.Length); TriFull = Math.Clamp(options.TriFull, 0, triFulls.Length)
                           TriHome = Math.Clamp(options.TriHome, 0, triHomes.Length); TriRange = Math.Clamp(options.TriRange, 0, triRanges.Length)
                           TriExact = Math.Clamp(options.TriExact, 0, triExacts.Length); TriRelax = Math.Clamp(options.TriRelax, 0, triRelaxes.Length)
                           DlssPreset = Math.Clamp(options.DlssPreset, 0, 3)
                           FpsCounterSize = (if options.FpsCounterSize >= 50 && options.FpsCounterSize <= 200 then options.FpsCounterSize else 100)
                           SmsrBoost = Math.Clamp(options.SmsrBoost, -100, 100)
                           GameType = Math.Clamp(options.GameType, 0, gameCustom) }

    let normalize state =
        if isNull (box state) || state.Version <> 1 then defaults
        else
            let profiles =
                (if isNull state.Profiles then [||] else state.Profiles)
                |> Array.filter (fun p -> not (isNull (box p)) && not (String.IsNullOrWhiteSpace p.Id) && not (String.IsNullOrWhiteSpace p.Name))
                |> Array.distinctBy _.Id
                |> Array.truncate 100
                |> Array.map (fun p -> { p with Name = p.Name.Trim().Substring(0, min 40 (p.Name.Trim().Length)); Options = normalizeOptions p.Options })
            let profiles =
                if profiles |> Array.exists (fun p -> p.Id = "default") then profiles
                else Array.append [| defaultProfile |] (profiles |> Array.truncate 99)
            { state with Language = Math.Clamp(state.Language, 0, ARMO.Localization.Languages.count - 1)
                         Theme = Math.Clamp(state.Theme, 0, 2)
                         Profiles = profiles
                         SelectedProfile =
                            if profiles |> Array.exists (fun p -> p.Id = state.SelectedProfile) then state.SelectedProfile
                            else "default" }
