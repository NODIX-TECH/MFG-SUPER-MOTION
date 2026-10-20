namespace ARMO.Localization

open System
open System.Collections.Concurrent
open System.Text.Json
open Microsoft.FSharp.Reflection

[<CLIMutable>]
type UiText =
    { Display: string; Support: string; Profiles: string; Settings: string; Help: string
      FrameGeneration: string; Generator: string; XessUnavailable: string; Mode: string; Flow: string; Upscaling: string
      Upscaler: string; Sharpness: string; OverSharpness: string; SharpnessOnly: string; Advanced: string; Language: string
      // The headings the advanced rows stand under ("Display" is the page's own word).
      GroupPacing: string; GroupMotion: string; GroupCapture: string; GroupGpu: string
      Appearance: string; Light: string; Dark: string; System: string
      Transparency: string; TransparencyDescription: string
      SettingsDescription: string
      // The page that asks for support: its button, and the thanks under it.
      SupportButton: string; SupportThanks: string; SupportText1: string; SupportText2: string
      LocalOnly: string; Reset: string; Add: string; Rename: string; Delete: string
      ProfileName: string; Save: string; Cancel: string; DeleteQuestion: string
      InvalidName: string; HelpTitle: string; HelpDescription: string
      ProfileHelp: string; SliderHelp: string; KeyboardHelp: string; Default: string
      Minimize: string; Maximize: string; Close: string; SaveFailed: string
      StartWithWindows: string; StartMinimized: string; MinimizeToTray: string
      StartupDescription: string; StartupFailed: string; Restore: string; Exit: string
      // The engine: what the page says while it runs, and the advanced rows.
      StatusReady: string; StatusPicking: string; StatusStarting: string; StatusRunning: string
      StatusNoWindow: string; StatusClosed: string; StatusNoGpu: string; StatusUnavailable: string
      StatusFailed: string; StatusMissing: string; VrrFixed: string; MotionHelp: string
      CaptureApi: string; WindowMode: string; Borderless: string; Scaled: string; Windowed: string
      Countdown: string; Seconds: string; MotionGuard: string; QueueTarget: string
      GameType: string; GameAuto: string; GameFirstPerson: string; GameThirdPerson: string
      GameRacing: string; GameOpenWorld: string; GameCustom: string
      FrameLimit: string; ScreenLimit: string; Off: string; LowLatency: string; GpuPriority: string; PreferredGpu: string; Realtime: string
      SyncMode: string; SyncOff: string; SpoutOutput: string
      Normal: string; MotionSource: string; GpuEstimate: string; StillPixels: string
      FpsCounter: string; Shortcut: string; PressKey: string; ShortcutTaken: string
      // Adaptive Motion Learning (its name is the same in every language): the switch's own word of how far
      // along it is, and the list of what is kept in Settings.
      AdaptiveLearning: string; AdaptiveFlow: string; LearningProgress: string; LearnedDone: string
      // What Adaptive Flow Scale is (Help); the "?" beside the app's own techniques: each in a few words; and the
      // switch that keeps the engine's log (Settings).
      HelpAfs: string; WhatIsAml: string; WhatIsAfs: string; EngineLog: string
      LearningCache: string; CacheEmpty: string; DeleteCache: string; Fast: string
      // DLSS MFG's preset row (NVIDIA's preset letter kept in each name).
      DlssPreset: string; PresetPerformance: string; PresetQuality: string; PresetBalanced: string; PresetDriver: string
      // Shown when Performance (preset C) is picked.
      PresetCWarning: string
      // Game FPS (the game's own frame rate, typed; empty: Auto) and what it is for.
      GameFps: string; GameFpsHelp: string
      // A message to the developer (Help): its title, the box's hint, the log's tick and why it cannot be ticked, the
      // button, what is said while and after it is sent ({0}: what went wrong), and what is sent.
      FeedbackTitle: string; FeedbackHint: string; FeedbackAttach: string; FeedbackNoLog: string; FeedbackSend: string
      FeedbackSending: string; FeedbackSent: string; FeedbackFailed: string; FeedbackWait: string; FeedbackWhat: string
      // Super Video (round 48): the page, its lists, what each video is doing ({0}: percent; in VideoFailed: why), and
      // that only SMFG makes a video's frames.
      VideoTitle: string; VideoIntro: string; VideoAdd: string; VideoEmpty: string; VideoOutput: string; VideoNextToEach: string
      VideoChooseFolder: string; VideoFrames: string; VideoSize: string; VideoSameSize: string; VideoCodec: string
      VideoStart: string; VideoStop: string; VideoRemove: string; VideoShow: string; VideoWaiting: string; VideoWorking: string
      VideoDone: string; VideoStopped: string; VideoFailed: string; VideoStopMotionFirst: string; VideoNote: string
      // Round 49: the sidebar's "More" (Super Video, App Mods) and the Help page's button that opens the guide.
      NavMore: string; HelpGuide: string
      // Round 43: multi-display mode, Crop input (each side's box) and the pointer's options; what they are for.
      MultiDisplay: string; MultiDisplayHelp: string; CropInput: string; CropHelp: string; CropLeft: string; CropTop: string
      CropRight: string; CropBottom: string; Cursor: string; CursorClip: string; CursorSpeed: string; CursorHide: string
      CursorScale: string; CursorScaledOnly: string
      // What the engine writes at the top of the frame while it studies a game: that it is studying ({0} how
      // far along, percent; {1} about how many seconds are left), the step it asks for just now - which ones
      // goes by the kind of game - and that it is through.
      StudyLine: string; StudyTurn: string; StudyLook: string; StudyWalk: string; StudyDrive: string
      StudyStraight: string; StudyTurns: string; StudyScroll: string; StudyDone: string
      StudyNotice: string; StudyBack: string; StudyHud: string; StudyRun: string; StudyAim: string; StudyFree: string
      // The Help page: getting started, the generators, each option (under the option's own name), recording
      // with OBS, what to try when something is wrong, and the links ({0} in LinkDrivers: the card's maker).
      HelpOfficial: string; HelpMaker: string
      // The engine's download when the app is opened ({0}: its version; in EngineDownloading: percent), and the
      // word that a newer app is out ({0}: its version).
      EngineTitle: string; EngineChecking: string; EngineRequired: string; EngineUpdate: string; EngineDownloading: string
      EngineFailed: string; Download: string; UpdateNow: string; Skip: string; Retry: string; AppUpdate: string
      OpenLogs: string
      HelpStartTitle: string; HelpStart: string; HelpGeneratorsTitle: string; HelpDlss: string; HelpXess: string; HelpMsm: string
      HelpOptionsTitle: string; HelpMode: string; HelpGameType: string; HelpAml: string; HelpUpscaling: string; HelpStill: string
      HelpPacing: string; HelpScreen: string; HelpMotion: string; HelpCapture: string; HelpGpu: string; HelpCounter: string
      HelpObsTitle: string; HelpObs: string; HelpTroubleTitle: string; HelpTrouble: string
      HelpKeysTitle: string; HelpLinksTitle: string; LinkDrivers: string }

module UiText =
    // Only requested languages are loaded; the fixed catalog bounds this cache.
    let private cache = ConcurrentDictionary<int, UiText>()
    let private fields = FSharpType.GetRecordFields typeof<UiText>

    let private read index =
        let resource = sprintf "ARMO.Localization.%s.json" (Languages.at index).Code
        use stream = typeof<UiText>.Assembly.GetManifestResourceStream(resource)
        if isNull stream then failwithf "Missing translation: %s" resource
        JsonSerializer.Deserialize<UiText>(stream)

    // A string a translation does not have yet is shown in English.
    let private complete (english: UiText) (text: UiText) =
        let values =
            fields |> Array.map (fun field ->
                match field.GetValue text with
                | :? string as value when not (String.IsNullOrWhiteSpace value) -> box value
                | _ -> field.GetValue english)
        FSharpValue.MakeRecord(typeof<UiText>, values) :?> UiText

    let rec forLanguage index =
        let index = if index >= 0 && index < Languages.count then index else 0
        cache.GetOrAdd(index, fun index ->
            let text = read index
            if index = 0 then text else complete (forLanguage 0) text)

    let defaultNames = lazy (Array.init Languages.count (fun index -> (forLanguage index).Default))
