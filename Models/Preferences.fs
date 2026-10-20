namespace ARMO.Models

open System

[<CLIMutable>]
type DisplayOptions =
    { FrameGeneration: bool
      Generator: int        // 0 DLSS MFG, 1 XeSS MFG, 2 MSM FG; each runs in its own engine process
      Mode: int
      Flow: int              // used when GameType is "custom"; the other kinds of game bring their own
      Upscaler: int          // 0 off, 1 sharpening only, 2 integer, 3 nearest neighbour, 4 Anime4K (the engine's own numbers)
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
      AdaptiveLearning: bool // Adaptive Motion Learning: what the hand does to the picture is learnt for each game, and the generator told
      DlssPreset: int        // DLSS MFG's preset: 0 the driver's own, 1 Performance (C), 2 Quality (B), 3 Balanced (A); Quality unless picked
      AdaptiveFlow: bool     // Adaptive Flow Scale (needs Adaptive Motion Learning): the size frames in between are made at follows how fast the game's picture moves

      Spout: bool            // every frame shown is shared as a Spout sender (OBS: Spout2 Capture)
      StillPixels: bool
      MotionGuard: bool      // no frames in between while the picture moves too fast for them (off unless asked for)
      FpsCounter: bool
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
    let schema = 18
    // The sync modes the engine has (see SyncMode).
    let syncModes = 5
    // The upscalers the engine has: off, sharpening only, integer, nearest neighbour, Anime4K.
    let upscalers = 5
    // The generators: DLSS MFG, XeSS MFG, MSM FG.
    let generators = 3
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
        { FrameGeneration = true; Generator = 0; Mode = 2; Flow = 35; Upscaler = 0; Sharpness = 50; OverSharpness = 0
          Schema = schema; CaptureApi = 0; WindowMode = 0; Countdown = 1
          QueueTarget = 1; FrameLimit = 0; GameFps = 0; MultiDisplay = false; CropLeft = 0; CropTop = 0; CropRight = 0; CropBottom = 0
          CursorClip = false; CursorSpeed = false; CursorHide = false; CursorScale = false; LimitToScreen = false; Vrr = false; SyncMode = 1; LowLatency = true; Hdr = false
          GpuPriority = 0; Gpu = ""; MotionSource = 0; XessMotionSource = 0; MsmSearch = 1; AdaptiveLearning = false; AdaptiveFlow = true; DlssPreset = 2
          Spout = false; StillPixels = true; MotionGuard = false; FpsCounter = false; GameType = 0 }

    let defaultProfile = { Id = "default"; Name = "Default"; Options = defaultOptions }

    let defaults =
        { Version = 1; Language = 0; Theme = 2; Transparency = false; AdvancedOpen = false
          StartWithWindows = false; StartMinimized = false; MinimizeToTray = false; EngineLog = false; Hotkey = 0
          SelectedProfile = defaultProfile.Id; Profiles = [| defaultProfile |] }

    /// The size frames in between are made at, percent of the picture - the same for DLSS MFG and XeSS MFG.
    let effectiveFlow (options: DisplayOptions) =
        if options.GameType >= 0 && options.GameType < gameCustom then gameFlows[options.GameType] else options.Flow

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
            { options with Mode = Math.Clamp(options.Mode, 0, 4)
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
                           DlssPreset = Math.Clamp(options.DlssPreset, 0, 3)
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
