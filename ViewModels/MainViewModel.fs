namespace ARMO.ViewModels

open System
open System.Collections.ObjectModel
open System.Diagnostics
open System.Threading
open CommunityToolkit.Mvvm.Input
open ARMO.Models
open ARMO.Infrastructure
open ARMO.Localization

type ProfileItem(profile: Profile) =
    inherit ViewModelBase()
    let mutable name = profile.Name
    let mutable options = profile.Options
    member _.Id = profile.Id
    member this.Name
        with get () = name
        and set value = this.SetProperty(&name, value) |> ignore
    member _.Options with get () = options and set value = options <- value
    member _.Snapshot = { Id = profile.Id; Name = (if profile.Id = "default" then "Default" else name); Options = options }
    override _.ToString() = name

type ChoiceItem(label: string) =
    inherit ViewModelBase()
    let mutable label = label
    member this.Label
        with get () = label
        and set value = this.SetProperty(&label, value) |> ignore
    override _.ToString() = label

/// One game Adaptive Motion Learning keeps something for, as Settings lists it.
type LearnedItem(game: LearnedGame, status: string, delete: LearnedGame -> unit) =
    let deleteCommand = RelayCommand(fun () -> delete game)
    member _.Game = game.Game
    member _.Status = status
    member _.DeleteCommand = deleteCommand


/// The START / STOP MOTION shortcut as Windows counts it: modifiers * 65536 + virtual key.
module Shortcut =
    let private modifierNames = [ 2, "Ctrl"; 1, "Alt"; 4, "Shift"; 8, "Win" ]

    let private keyName vk =
        if (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) then string (char vk)
        elif vk >= 0x70 && vk <= 0x87 then sprintf "F%d" (vk - 0x6F)
        elif vk >= 0x60 && vk <= 0x69 then sprintf "Num %d" (vk - 0x60)
        else
            match vk with
            | 0x20 -> "Space" | 0x21 -> "Page Up" | 0x22 -> "Page Down" | 0x23 -> "End" | 0x24 -> "Home"
            | 0x25 -> "Left" | 0x26 -> "Up" | 0x27 -> "Right" | 0x28 -> "Down" | 0x2D -> "Insert"
            | 0x13 -> "Pause" | 0x91 -> "Scroll Lock" | 0xC0 -> "`"
            | _ -> sprintf "0x%02X" vk

    let describe value =
        let modifiers = value >>> 16
        let names = modifierNames |> List.filter (fun (bit, _) -> modifiers &&& bit <> 0) |> List.map snd
        String.Join(" + ", names @ [ keyName (value &&& 0xFFFF) ])

    /// A key by the name Avalonia gives it; 0 when it cannot be a shortcut.
    let virtualKey (name: string) =
        if name.Length = 1 && Char.IsAsciiLetterUpper name[0] then int name[0]
        elif name.Length = 2 && name[0] = 'D' && Char.IsAsciiDigit name[1] then int name[1]
        elif name.StartsWith "NumPad" && name.Length = 7 && Char.IsAsciiDigit name[6] then 0x60 + int name[6] - int '0'
        elif name.Length >= 2 && name[0] = 'F' && name.Substring(1) |> Seq.forall Char.IsAsciiDigit then
            let number = int (name.Substring 1)
            if number >= 1 && number <= 24 then 0x6F + number else 0
        else
            match name with
            | "Space" -> 0x20 | "PageUp" -> 0x21 | "PageDown" -> 0x22 | "End" -> 0x23 | "Home" -> 0x24
            | "Left" -> 0x25 | "Up" -> 0x26 | "Right" -> 0x27 | "Down" -> 0x28 | "Insert" -> 0x2D
            | "Pause" -> 0x13 | "Scroll" -> 0x91 | "OemTilde" -> 0xC0
            | _ -> 0

type MainViewModel(store: StateStore, startupRegistration: IStartupRegistration) as this =
    inherit ViewModelBase()

    let initial = store.Load()
    let profiles = ObservableCollection<ProfileItem>(initial.Profiles |> Array.map ProfileItem)
    let mutable selected = profiles |> Seq.find (fun p -> p.Id = initial.SelectedProfile)
    let mutable language = initial.Language
    let mutable theme = initial.Theme
    let mutable transparency = initial.Transparency
    let mutable startWithWindows = startupRegistration.IsEnabled
    let mutable startMinimized = initial.StartMinimized
    let mutable engineLog = initial.EngineLog
    let mutable minimizeToTray = initial.MinimizeToTray
    let mutable startupError = false
    let mutable hotkeyValue = initial.Hotkey
    let mutable capturingHotkey = false
    // A message to the developer (Help, round 47): sent only by its button.
    let mutable feedbackText = ""
    let mutable feedbackAttach = false
    let mutable feedbackStatus = ""
    let mutable feedbackBusy = false
    let mutable feedbackSentAt = DateTime.MinValue
    let text () = UiText.forLanguage language
    let themes =
        let text = text ()
        [| ChoiceItem(text.Light); ChoiceItem(text.Dark); ChoiceItem(text.System) |]
    let upscalers = Array.init Preferences.upscalers (fun _ -> ChoiceItem(""))
    let windowModes = [| ChoiceItem(""); ChoiceItem(""); ChoiceItem("") |]
    let countdowns = Preferences.countdowns |> Array.map (fun _ -> ChoiceItem(""))
    let gpuPriorities = [| ChoiceItem(""); ChoiceItem("") |]

    // The engine's sync modes, by their numbers: off, default, VSync on every, every second, every third refresh.
    let syncModes = [| ChoiceItem(""); ChoiceItem(""); ChoiceItem("VSync"); ChoiceItem("VSync ½"); ChoiceItem("VSync ⅓") |]
    let motionSources = [| ChoiceItem("DLSS MFG"); ChoiceItem("NVIDIA Optical Flow"); ChoiceItem("") |]
    let xessMotionSources = [| ChoiceItem("XeSS MFG"); ChoiceItem(""); ChoiceItem("NVIDIA Optical Flow") |]
    let msmSearches = [| ChoiceItem("SMFG"); ChoiceItem("") |]
    let gameFpsChoices = Preferences.gameFpsRates |> Array.map (fun r -> ChoiceItem(if r = 0 then "" else string r))
    let dlssPresets = [| ChoiceItem(""); ChoiceItem(""); ChoiceItem(""); ChoiceItem("") |]
    // Performance (preset C) picked: the page shows why it is not advised.
    let presetCPicked = Event<unit>()
    let generatorNames = [| "DLSS MFG"; "XeSS MFG"; "SMFG 1.1" |]
    let gameTypes = [| ChoiceItem(""); ChoiceItem("2D"); ChoiceItem("2.5D"); ChoiceItem(""); ChoiceItem(""); ChoiceItem(""); ChoiceItem(""); ChoiceItem("") |]
    let labelChoices () =
        let text = text ()
        upscalers[0].Label <- text.Off; upscalers[1].Label <- text.SharpnessOnly
        upscalers[2].Label <- "Integer"; upscalers[3].Label <- "Nearest Neighbor"; upscalers[4].Label <- "Anime4K"
        windowModes[0].Label <- text.Borderless; windowModes[1].Label <- text.Scaled; windowModes[2].Label <- text.Windowed
        countdowns |> Array.iteri (fun i item -> item.Label <- String.Format(text.Seconds, Preferences.countdowns[i]))
        gpuPriorities[0].Label <- text.Realtime; gpuPriorities[1].Label <- text.Normal

        syncModes[0].Label <- text.SyncOff; syncModes[1].Label <- text.Default
        motionSources[2].Label <- text.GpuEstimate
        xessMotionSources[1].Label <- text.GpuEstimate
        msmSearches[1].Label <- sprintf "SMFG · %s" text.Fast
        gameFpsChoices[0].Label <- text.GameAuto
        dlssPresets[0].Label <- text.PresetDriver; dlssPresets[1].Label <- text.PresetPerformance
        dlssPresets[2].Label <- text.PresetQuality; dlssPresets[3].Label <- text.PresetBalanced
        gameTypes[0].Label <- text.GameAuto; gameTypes[3].Label <- text.GameFirstPerson; gameTypes[4].Label <- text.GameThirdPerson
        gameTypes[5].Label <- text.GameRacing; gameTypes[6].Label <- text.GameOpenWorld; gameTypes[7].Label <- text.GameCustom
    let mutable page = "display"
    let mutable advanced = initial.AdvancedOpen
    let mutable editorOpen = false
    let mutable deleteOpen = false
    let mutable adding = false
    let mutable draftName = ""

    // The engine. Its lines arrive on another thread; the page hears of them here.
    let ui = SynchronizationContext.Current
    let post (action: unit -> unit) = if isNull ui then action () else ui.Post((fun _ -> action ()), null)
    let engine = new MotionEngine()
    let hotkey = new GlobalHotkey()
    let mutable engineStatus = EngineStatus.idle
    let mutable disposed = false
    // Super Video (round 48): its own page; it does not start while Super Motion runs.
    let video = new SuperVideoModel(text, post, (fun () -> engineStatus.Phase <> Idle))
    // "Preferred GPU" and "Generator" go by each other: the cards offered are the ones the picked generator can
    // run on (DLSS MFG: NVIDIA RTX cards only; XeSS MFG: the ones Intel's library takes; MSM FG: every card), by
    // name - and the generators offered are the ones that run on the picked card (any, while the card is "Auto").
    let mutable cards: CardInfo array = [||]
    /// What the PC said of VRR at this start (engine "--vrr"); Reset takes it too.
    let mutable systemVrr: bool option = None
    let runsOn generator (card: CardInfo) = match generator with | 0 -> card.Dlss | 1 -> card.Xess | _ -> true
    let cardNames generator = cards |> Array.filter (runsOn generator) |> Array.map _.Name
    let generatorIds (options: DisplayOptions) =
        let all = [| 0 .. Preferences.generators - 1 |]
        // (A card saved with the profile that the generator saved with it cannot run on counts as "Auto".)
        match cards |> Array.tryFind (fun c -> c.Name = options.Gpu && runsOn options.Generator c) with
        | Some card -> all |> Array.filter (fun g -> runsOn g card)
        // "Auto": the generators some card of this PC can run (round 43: an old GTX was offered DLSS MFG); all of
        // them while the engine has not told the cards yet.
        | None when cards.Length > 0 -> all |> Array.filter (fun g -> cards |> Array.exists (runsOn g))
        | None -> all

    // While a list is handed over anew (the cards the engine found, the generators that run on the card picked)
    // its box puts back whatever it stood at - "Auto", the first entry - as if the person had picked that: a card
    // saved with the profile was "Auto" again at every start. What a box says while the page is being told is
    // not a choice.
    let mutable telling = false
    let notify names =
        let was = telling
        telling <- true
        try names |> List.iter this.Notify
        finally telling <- was
    // The two lists that go by each other, as their boxes have them. A box handed its list anew drops what it
    // shows - and one handed the same entries in a new list, while the person's pick in it is still being taken
    // in, stood at the entry before ("XeSS MFG" in the box, MSM FG running). So a list is handed over only when
    // its entries changed, and its box is told its place once more when it has taken the list in.
    let mutable generatorsShown: string array = [||]
    let mutable gpusShown: string array = [||]
    let refreshLists () =
        let generators = generatorIds selected.Options |> Array.map (fun g -> generatorNames[g])
        let gpus = Array.append [| (text ()).GameAuto |] (cardNames selected.Options.Generator)
        let mutable changed = []
        if gpus <> gpusShown then
            gpusShown <- gpus
            changed <- "GpuChoices" :: changed
        if generators <> generatorsShown then
            generatorsShown <- generators
            changed <- "Generators" :: changed
        changed
    let notifyLists () =
        let lists = refreshLists ()
        notify (lists @ [ "GeneratorIndex"; "GpuIndex" ])
        if not lists.IsEmpty then post (fun () -> if not disposed then notify [ "GeneratorIndex"; "GpuIndex" ])
    let notifyOptions () =
        notifyLists ()
        notify [ "GeneratorName"; "IsDlss"; "IsXess"; "IsMsm"; "SpoutEnabled"; "IsOverSharpnessOpen"; "FrameGenerationEnabled"; "ModeIndex"; "GameTypeIndex"; "IsFlowOpen"; "Flow"; "FlowLabel"; "UpscalerIndex"; "IsSharpnessOpen"; "Sharpness"; "SharpnessLabel"; "OverSharpness"; "OverSharpnessLabel"
                 "CaptureApiIndex"; "WindowModeIndex"; "CountdownIndex"; "QueueTargetIndex"
                 "FrameLimit"; "FrameLimitLabel"; "GameFpsIndex"; "MultiDisplayEnabled"; "CropLeftText"; "CropTopText"; "CropRightText"; "CropBottomText"; "CursorClipEnabled"; "CursorSpeedEnabled"; "CursorHideEnabled"; "CursorScaleEnabled"; "LimitToScreenEnabled"; "VrrEnabled"; "SyncModeIndex"; "LowLatencyEnabled"; "HdrEnabled"; "GpuPriorityIndex"
                 "MotionSourceIndex"; "XessMotionSourceIndex"; "MsmSearchIndex"; "DlssPresetIndex"; "AdaptiveLearningEnabled"; "AdaptiveFlowEnabled"; "IsAdaptiveFlowOpen"; "LearningStatus"
                 "IsMotionPrecisionOpen"; "StillPixelsEnabled"; "FpsCounterEnabled"; "StatusNote"; "HasStatusNote" ]
    let notifyEngine () = notify [ "IsMotionActive"; "IsMotionWaiting"; "IsMotionRunning"; "MotionButtonText"; "StatusText"; "HasStatus"; "StatusNote"; "HasStatusNote"; "LearningStatus" ]
    // What Adaptive Motion Learning keeps, game by game (Settings): read when the page is opened, and again when
    // one is deleted or the engine says a game got further.
    let learned = ObservableCollection<LearnedItem>()

    let rec refreshLearned () =
        let text = text ()
        learned.Clear()
        for game in LearnedGames.list () do
            let state = if game.Learned then text.LearnedDone else String.Format(text.LearningProgress, game.Percent)
            learned.Add(LearnedItem(game, (if game.Updated = "" then state else sprintf "%s · %s" state game.Updated), forget))
        notify [ "HasLearnedGames"; "NoLearnedGames" ]
    and forget (game: LearnedGame) =
        engine.Forget game.Game
        LearnedGames.delete game
        refreshLearned ()

    let persist () =
        store.Schedule { Version = 1; Language = language; Theme = theme
                         Transparency = transparency; AdvancedOpen = advanced
                         StartWithWindows = startWithWindows; StartMinimized = startMinimized; MinimizeToTray = minimizeToTray; EngineLog = engineLog
                         Hotkey = hotkeyValue
                         SelectedProfile = selected.Id; Profiles = profiles |> Seq.map _.Snapshot |> Seq.toArray }
    // A slider dragged across its range is one change for the engine, not fifty.
    let sendTimer = new Timer((fun _ -> post (fun () -> if not disposed then engine.Send selected.Options)), null, Timeout.Infinite, Timeout.Infinite)
    let sendOptions () = if engine.IsActive then sendTimer.Change(200, Timeout.Infinite) |> ignore
    let updateOptions update =
        let next = update selected.Options |> Preferences.normalizeOptions
        if selected.Options <> next then
            if engine.IsActive && next.Generator <> selected.Options.Generator then engine.Stop()
            selected.Options <- next
            notifyOptions ()
            persist ()
            sendOptions ()
    // The lines the engine writes on the frame while Adaptive Motion Learning studies a game.
    let studyTexts () =
        let text = text ()
        [ "aml_say0", text.StudyLine; "aml_say1", text.StudyTurn; "aml_say2", text.StudyLook; "aml_say3", text.StudyWalk; "aml_say4", text.StudyDone
          "aml_say5", text.StudyDrive; "aml_say6", text.StudyStraight; "aml_say7", text.StudyTurns; "aml_say8", text.StudyScroll
          "aml_say9", text.StudyNotice; "aml_say10", text.StudyBack; "aml_say11", text.StudyHud
          "aml_say12", text.StudyRun; "aml_say13", text.StudyAim; "aml_say14", text.StudyFree ]
    // now: started by the shortcut with the game in front - that window is taken at once, no countdown (round 42).
    let startOrStop now =
        if engine.IsActive then engine.Stop()
        else
            engine.SetTexts(studyTexts ())
            engine.Start(selected.Options, now) |> ignore
    let toggleMotion () = startOrStop false
    // A Crop input box: empty is 0, a number is taken (up to the most), anything else leaves it as it was.
    let cropSet (value: string) (put: DisplayOptions -> int -> DisplayOptions) =
        let text = if isNull value then "" else value.Trim()
        let number = if text = "" then Some 0 else match Int32.TryParse text with | true, n when n >= 0 -> Some (min n Preferences.cropMax) | _ -> None
        match number with
        | Some n -> updateOptions (fun o -> put o n)
        | None -> ()
    let effectiveHotkey () =
        if hotkeyValue = 0 then Preferences.defaultHotkey elif hotkeyValue < 0 then 0 else hotkeyValue
    let validName () =
        let name = draftName.Trim()
        name.Length > 0 && name.Length <= 40 &&
        not (UiText.defaultNames.Value |> Array.exists (fun reserved -> String.Equals(reserved, name, StringComparison.OrdinalIgnoreCase))) &&
        (profiles |> Seq.forall (fun p -> (not adding && p.Id = selected.Id) || not (String.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))))
    // Round 49: the sidebar shows Super Motion and Support; "More" opens the rest (Super Video, App Mods) - and stays
    // open while one of them is the page. The Help page's guide is behind a button of its own.
    let mutable moreOpen = false
    let mutable helpGuideOpen = false
    let navigate target =
        if page <> target then
            page <- target
            if target = "settings" then refreshLearned ()
            if target = "video" || target = "mods" then moreOpen <- true
            notify [ "IsDisplay"; "IsVideo"; "IsMods"; "IsMoreOpen"; "IsSupport"; "IsSettings"; "IsHelp"; "CanAttachLog"; "FeedbackLogNote" ]
    let displayCommand = RelayCommand(fun () -> navigate "display")
    let videoCommand = RelayCommand(fun () -> navigate "video")
    let modsCommand = RelayCommand(fun () -> navigate "mods")
    let moreCommand = RelayCommand(fun () ->
        moreOpen <- not moreOpen
        notify [ "IsMoreOpen" ])
    // The engine's logs that are there to send (engine.log with "Keep a log"; frames.log in a development build).
    let feedbackLogs () =
        let folder = System.IO.Path.GetDirectoryName engine.LogPath
        [ engine.LogPath; System.IO.Path.Combine(folder, "frames.log") ] |> List.filter System.IO.File.Exists
    let sendFeedback () =
        let text = text ()
        let words = if isNull feedbackText then "" else feedbackText.Trim()
        if feedbackBusy || (words = "" && not feedbackAttach) then ()
        elif DateTime.Now - feedbackSentAt < TimeSpan.FromSeconds 30.0 then
            feedbackStatus <- text.FeedbackWait
            notify [ "FeedbackStatus" ]
        else
            feedbackBusy <- true
            feedbackStatus <- text.FeedbackSending
            notify [ "FeedbackStatus"; "CanSendFeedback" ]
            let logs = if feedbackAttach then feedbackLogs () else []
            async {
                let! answer = Feedback.send words logs
                post (fun () ->
                    feedbackBusy <- false
                    match answer with
                    | Ok () ->
                        feedbackSentAt <- DateTime.Now
                        feedbackText <- ""
                        feedbackStatus <- text.FeedbackSent
                    | Error why -> feedbackStatus <- String.Format(text.FeedbackFailed, why)
                    notify [ "FeedbackText"; "FeedbackStatus"; "CanSendFeedback" ])
            } |> Async.Start
    let sendFeedbackCommand = RelayCommand(fun () -> sendFeedback ())
    let supportCommand = RelayCommand(fun () -> navigate "support")
    // ---- The engine's download, and what is new (Updates.fs) --------------------
    // The published app has no engine in it: it is downloaded the first time the app is opened, and the app
    // cannot be used before it is there. Every time it is opened it asks whether there is a newer one - that one
    // may be passed over - and whether there is a newer app, which is only said.
    //   gate: "" the app is open | checking | required | update | downloading | failed
    let ui = SynchronizationContext.Current
    let onUi (action: unit -> unit) = if isNull ui then action () else ui.Post((fun _ -> action ()), null)
    let mutable gate = ""
    let mutable gateRelease : Release option = None
    let mutable gateProgress = 0.0
    let mutable appUpdate = ""
    let notifyGate () =
        notify [ "IsGateOpen"; "IsDialogOpen"; "GateText"; "GateProgress"; "IsGateDownloading"; "CanGateDownload"; "CanGateSkip"
                 "GateActionText"; "HasAppUpdate"; "AppUpdateText" ]
    let setGate state =
        gate <- state
        notifyGate ()
    let checkEngine () =
        if not (EngineStore.isLocal ()) then
            let has = (EngineStore.path ()).IsSome
            if not has then setGate "checking"
            task {
                let! release = Releases.latest Releases.engineRepository CancellationToken.None
                onUi (fun () ->
                    gateRelease <- release
                    match release with
                    | Some found when not (String.IsNullOrEmpty found.Asset) ->
                        if not has then setGate "required"
                        else
                            match EngineStore.installed () with
                            | Some version when version >= found.Version -> setGate ""
                            | _ -> setGate "update"
                    | _ -> setGate (if has then "" else "failed"))
            } |> ignore
    let checkApp () =
        task {
            let! release = Releases.latest Releases.appRepository CancellationToken.None
            onUi (fun () ->
                match release with
                | Some found when found.Version > Releases.appVersion ->
                    appUpdate <- found.Version.ToString()
                    notifyGate ()
                | _ -> ())
        } |> ignore
    let downloadEngine () =
        match gateRelease with
        | Some release when gate = "required" || gate = "update" ->
            let wasActive = engine.IsActive
            if wasActive then engine.Stop()
            gateProgress <- 0.0
            setGate "downloading"
            task {
                if wasActive then do! System.Threading.Tasks.Task.Delay 1500
                let! result =
                    EngineStore.install release (fun part -> onUi (fun () -> gateProgress <- part; notify [ "GateProgress"; "GateText" ])) CancellationToken.None
                onUi (fun () ->
                    match result with
                    | Ok () -> setGate ""
                    | Error _ -> setGate "failed")
            } |> ignore
        | _ -> checkEngine ()
    // Help: the engine's log folder opened in Explorer, for a user to send what went wrong.
    let openLogsCommand = RelayCommand(fun () ->
        try
            let folder = AppData.path [| "Engine" |]
            System.IO.Directory.CreateDirectory folder |> ignore
            Process.Start(ProcessStartInfo("explorer.exe", "\"" + folder + "\"", UseShellExecute = true)) |> ignore
        with _ -> ())
    let gateDownloadCommand = RelayCommand(fun () -> downloadEngine ())
    let gateSkipCommand = RelayCommand(fun () -> if (EngineStore.path ()).IsSome then setGate "")
    do onUi (fun () -> checkEngine (); checkApp ())
    // The pages' links, opened in the user's own browser: these and no others.
    let links =
        dict [ "patreon", "https://www.patreon.com/cw/NODIXTECH/shop"
               "github", "https://github.com/NODIX-TECH/MFG-SUPER-MOTION"
               "site", "https://www.mfgsm.com/"
               "youtube", "https://www.youtube.com/@Nodix-Tech"
               "app-releases", Releases.appPage
               "obs", "https://obsproject.com/download"
               "obs-spout", "https://github.com/Off-World-Live/obs-spout2-plugin/releases"
               "spout", "https://spout.zeal.co/"
               "nvidia", "https://www.nvidia.com/en-us/drivers/"
               "amd", "https://www.amd.com/en/support/download/drivers.html"
               "intel", "https://www.intel.com/content/www/us/en/download-center/home.html" ]
    let openLinkCommand = RelayCommand<string>(fun name ->
        match links.TryGetValue(if isNull name then "" else name) with
        | true, url ->
            try Process.Start(ProcessStartInfo(url, UseShellExecute = true)) |> ignore
            with _ -> ()
        | _ -> ())
    let settingsCommand = RelayCommand(fun () -> navigate "settings")
    let helpCommand = RelayCommand(fun () -> navigate "help")
    let cancelCommand = RelayCommand(fun () -> this.CancelDialog())
    let addCommand = RelayCommand((fun () ->
        adding <- true
        this.DraftName <- ""
        this.IsEditorOpen <- true
        notify [ "DialogTitle" ]), (fun () -> profiles.Count < 100))
    let renameCommand = RelayCommand((fun () ->
        adding <- false
        this.DraftName <- selected.Name
        this.IsEditorOpen <- true
        notify [ "DialogTitle" ]), (fun () -> selected.Id <> "default"))
    let deleteCommand = RelayCommand((fun () -> this.IsDeleteOpen <- true), (fun () -> selected.Id <> "default"))
    let saveCommand = RelayCommand((fun () ->
        if adding then
            let item = ProfileItem({ Id = Guid.NewGuid().ToString("N"); Name = draftName.Trim(); Options = selected.Options })
            profiles.Add item
            this.SelectedProfile <- item
        else selected.Name <- draftName.Trim()
        this.CancelDialog()
        persist ()
        addCommand.NotifyCanExecuteChanged()), validName)
    let confirmDeleteCommand = RelayCommand(fun () ->
        if selected.Id <> "default" then
            let removed = selected
            this.SelectedProfile <- profiles |> Seq.find (fun p -> p.Id = "default")
            profiles.Remove removed |> ignore
            persist ()
            addCommand.NotifyCanExecuteChanged()
        this.CancelDialog())
    let resetCommand = RelayCommand(fun () -> updateOptions (fun _ -> match systemVrr with Some on -> { Preferences.defaultOptions with Vrr = on } | None -> Preferences.defaultOptions))
    let retrySaveCommand = RelayCommand(fun () -> store.Flush())
    let motionCommand = RelayCommand(toggleMotion)
    let shortcutCommand = RelayCommand(fun () -> this.BeginShortcutCapture())

    do
        profiles |> Seq.filter (fun p -> p.Id = "default") |> Seq.iter (fun p -> p.Name <- this.Text.Default)
        labelChoices ()
        refreshLists () |> ignore
        engine.Changed.Add(fun status ->
            post (fun () ->
                if not disposed then
                    let further = status.Learning <> engineStatus.Learning
                    engineStatus <- status
                    notifyEngine ()
                    if further && page = "settings" then refreshLearned ()))
        // (This app's own window in front: there is no game to take at once - the countdown, as the button has it.)
        hotkey.Pressed.Add(fun oursInFront -> post (fun () -> if not disposed && not capturingHotkey then startOrStop (not oursInFront)))
        hotkey.Set(effectiveHotkey ())
        engine.SetLog engineLog
        // The engine lists the cards; asked off the window's thread, the page is told when the answer is in.
        ThreadPool.QueueUserWorkItem(fun _ ->
            let listed = engine.Cards()
            post (fun () ->
                if not disposed then
                    cards <- listed
                    // A profile on a generator no card of this PC runs (DLSS MFG, the first, with no RTX card): SMFG,
                    // which runs on any card.
                    if listed.Length > 0 then
                        let mutable changed = false
                        for p in profiles do
                            if not (listed |> Array.exists (runsOn p.Options.Generator)) then
                                p.Options <- { p.Options with Generator = 2 }
                                changed <- true
                        if changed then
                            notifyOptions ()
                            persist ()
                    notifyLists ())
            // VRR follows the PC at each start: on when it is on for a window here (G-SYNC for windowed mode, or
            // Windows' dynamic refresh rate), off when it is not; left as it was when the engine cannot tell.

            engine.QuietRtss()
            let vrr = engine.SystemVrr()
            post (fun () ->
                match vrr with
                | Some on when not disposed ->
                    systemVrr <- Some on
                    let mutable changed = false
                    for p in profiles do
                        if p.Options.Vrr <> on then
                            p.Options <- { p.Options with Vrr = on }
                            changed <- true
                    if changed then
                        notifyOptions ()
                        persist ()
                        sendOptions ()
                | _ -> ())) |> ignore

    new(store) = new MainViewModel(store, StartupRegistration.forCurrentProcess())
    new() = new MainViewModel(new StateStore(StateStore.DefaultPath))

    member _.Text = UiText.forLanguage language
    member _.Languages = Languages.names
    member _.Themes = themes
    member _.Modes = [| "×2"; "×3"; "×4"; "×5"; "×6" |]
    member _.Upscalers = upscalers
    member _.CaptureApis = [| "DXGI"; "WGC" |]
    member _.WindowModes = windowModes
    member _.Countdowns = countdowns
    member _.QueueTargets = [| "0"; "1"; "2" |]
    member _.GpuPriorities = gpuPriorities
    member _.SyncModes = syncModes
    member _.MotionSources = motionSources
    member _.XessMotionSources = xessMotionSources
    member _.GameTypes = gameTypes
    member _.Profiles = profiles
    member this.SelectedProfile
        with get () = selected
        and set value =
            if not (isNull (box value)) && not (obj.ReferenceEquals(value, selected)) then
                if engine.IsActive && value.Options.Generator <> selected.Options.Generator then engine.Stop()

                selected <- value

                notify [ "SelectedProfile" ]
                notifyOptions ()
                renameCommand.NotifyCanExecuteChanged()
                deleteCommand.NotifyCanExecuteChanged()
                persist ()
                sendOptions ()
    member _.LanguageIndex
        with get () = language
        and set value =
            if value >= 0 && value < Languages.count && language <> value then
                language <- value
                let labels = [| this.Text.Light; this.Text.Dark; this.Text.System |]
                themes |> Array.iteri (fun i item -> item.Label <- labels[i])
                labelChoices ()
                video.Relabel()
                profiles |> Seq.filter (fun p -> p.Id = "default") |> Seq.iter (fun p -> p.Name <- this.Text.Default)
                notify [ "LanguageIndex"; "Text"; "DialogTitle"; "IsArabic"; "IsRightToLeft"; "UiFontFamily"; "NvidiaDrivers"; "AmdDrivers"; "IntelDrivers"; "GateText"; "GateActionText"; "AppUpdateText"
                         "FrameLimitLabel"; "ShortcutLabel" ]
                notifyLists ()
                notifyEngine ()
                if page = "settings" then refreshLearned ()
                persist ()
    member _.IsArabic = language = 1
    member _.IsRightToLeft = (Languages.at language).RightToLeft
    member _.UiFontFamily = (Languages.at language).FontFamily
    member _.StartWithWindows
        with get () = startWithWindows
        and set value =
            if startWithWindows <> value then
                match startupRegistration.SetEnabled value with
                | Ok () ->
                    startWithWindows <- value
                    startupError <- false
                    notify [ "StartWithWindows"; "HasStartupError" ]
                    persist ()
                | Error _ ->
                    startupError <- true
                    notify [ "StartWithWindows"; "HasStartupError" ]
    member _.StartMinimized
        with get () = startMinimized
        and set value = if this.SetProperty(&startMinimized, value) then persist ()
    member _.MinimizeToTray
        with get () = minimizeToTray
        and set value = if this.SetProperty(&minimizeToTray, value) then persist ()
    // The engine's log (off): for when something is to be looked into.
    member _.EngineLog
        with get () = engineLog
        and set value =
            if this.SetProperty(&engineLog, value) then
                engine.SetLog value
                persist ()
    member _.HasStartupError = startupError
    member _.ThemeIndex
        with get () = theme
        and set value =
            if value >= 0 && value <= 2 && theme <> value then
                theme <- value
                notify [ "ThemeIndex" ]
                persist ()
    member _.FrameGenerationEnabled
        with get () = selected.Options.FrameGeneration
        and set value = updateOptions (fun o -> { o with FrameGeneration = value })
    member _.Generators = generatorsShown
    member _.GeneratorName = generatorNames[selected.Options.Generator]
    member _.IsDlss = selected.Options.Generator = 0
    member _.IsXess = selected.Options.Generator = 1
    member _.IsMsm = selected.Options.Generator = 2
    member _.GeneratorIndex
        with get () = generatorIds selected.Options |> Array.tryFindIndex ((=) selected.Options.Generator) |> Option.defaultValue 0
        and set value =
            let ids = generatorIds selected.Options
            if not telling && value >= 0 && value < ids.Length && not engine.IsActive then
                updateOptions (fun o -> { o with Generator = ids[value] })
    member _.TransparencyEnabled
        with get () = transparency
        and set value =
            if this.SetProperty(&transparency, value) then persist ()
    member _.ModeIndex
        with get () = selected.Options.Mode
        and set value = if value >= 0 then updateOptions (fun o -> { o with Mode = value })
    // The size frames in between are made at, for both generators: the kind of
    // game's own value, locked, or the profile's own for "custom".
    member _.Flow
        with get () = float (Preferences.effectiveFlow selected.Options)
        and set value =
            if Double.IsFinite value && selected.Options.GameType = Preferences.gameCustom then
                updateOptions (fun o -> { o with Flow = int (Math.Round(Math.Clamp(value, 25., 100.) / 5.)) * 5 })
    member _.FlowLabel = sprintf "%d%%" (Preferences.effectiveFlow selected.Options)
    // Only "custom" leaves precision to the slider.
    member _.GameTypeIndex
        with get () = selected.Options.GameType
        and set value = if value >= 0 then updateOptions (fun o -> { o with GameType = value })
    member _.IsMotionPrecisionOpen = selected.Options.FrameGeneration
    member _.IsFlowOpen = this.IsMotionPrecisionOpen && selected.Options.GameType = Preferences.gameCustom
    member _.UpscalerIndex
        with get () = selected.Options.Upscaler
        and set value = if value >= 0 then updateOptions (fun o -> { o with Upscaler = value })
    member _.Sharpness
        with get () = float selected.Options.Sharpness
        and set value = if Double.IsFinite value then updateOptions (fun o -> { o with Sharpness = int (Math.Round(Math.Clamp(value, 0., 100.))) })
    member _.SharpnessLabel = sprintf "%d%%" selected.Options.Sharpness
    member _.OverSharpness
        with get () = float selected.Options.OverSharpness
        and set value = if Double.IsFinite value then updateOptions (fun o -> { o with OverSharpness = int (Math.Round(Math.Clamp(value, 0., 100.))) })
    member _.OverSharpnessLabel = sprintf "%d%%" selected.Options.OverSharpness
    member _.IsOverSharpnessOpen = selected.Options.Upscaler = 1
    member _.SpoutEnabled
        with get () = selected.Options.Spout
        and set value = updateOptions (fun o -> { o with Spout = value })
    /// The two sliders do something once an upscaler is picked.
    member _.IsSharpnessOpen = selected.Options.Upscaler = 1 || selected.Options.Upscaler = 4

    // Advanced: each row is one option of the engine (Models/Preferences.fs).
    member _.CaptureApiIndex
        with get () = selected.Options.CaptureApi
        and set value = if value >= 0 then updateOptions (fun o -> { o with CaptureApi = value })
    member _.WindowModeIndex
        with get () = selected.Options.WindowMode
        and set value = if value >= 0 then updateOptions (fun o -> { o with WindowMode = value })
    member _.CountdownIndex
        with get () = selected.Options.Countdown
        and set value = if value >= 0 then updateOptions (fun o -> { o with Countdown = value })
    member _.QueueTargetIndex
        with get () = selected.Options.QueueTarget
        and set value = if value >= 0 then updateOptions (fun o -> { o with QueueTarget = value })
    member _.FrameLimit
        with get () = float selected.Options.FrameLimit
        and set value =
            if Double.IsFinite value then
                // Steps of five; under 20 is "off".
                let stepped = int (Math.Round(Math.Clamp(value, 0., float Preferences.frameLimitMax) / 5.)) * 5
                updateOptions (fun o -> { o with FrameLimit = Preferences.normalizeFrameLimit stepped })
    member _.FrameLimitLabel =
        if selected.Options.FrameLimit <= 0 then this.Text.Off else string selected.Options.FrameLimit
    // Game FPS (round 43): picked from the rates games run at; "Auto" the engine reads the game's pace from its frames.
    // (A rate saved that is not in the list shows as the nearest one.)
    member _.GameFpsChoices = gameFpsChoices
    member _.GameFpsIndex
        with get () =
            let value = selected.Options.GameFps
            if value <= 0 then 0
            else Preferences.gameFpsRates |> Array.mapi (fun i r -> i, abs (r - value)) |> Array.filter (fun (i, _) -> i > 0) |> Array.minBy snd |> fst
        and set value =
            if value >= 0 && value < Preferences.gameFpsRates.Length then updateOptions (fun o -> { o with GameFps = Preferences.gameFpsRates[value] })
    // Round 43, as Lossless Scaling has them: multi-display mode, Crop input (pixels left out on each side; a box
    // that holds anything but a number is left as it was) and the pointer.
    member _.MultiDisplayEnabled
        with get () = selected.Options.MultiDisplay
        and set value = updateOptions (fun o -> { o with MultiDisplay = value })
    member _.CropLeftText with get () = string selected.Options.CropLeft and set (value: string) = cropSet value (fun o n -> { o with CropLeft = n })
    member _.CropTopText with get () = string selected.Options.CropTop and set (value: string) = cropSet value (fun o n -> { o with CropTop = n })
    member _.CropRightText with get () = string selected.Options.CropRight and set (value: string) = cropSet value (fun o n -> { o with CropRight = n })
    member _.CropBottomText with get () = string selected.Options.CropBottom and set (value: string) = cropSet value (fun o n -> { o with CropBottom = n })
    member _.CursorClipEnabled
        with get () = selected.Options.CursorClip
        and set value = updateOptions (fun o -> { o with CursorClip = value })
    member _.CursorSpeedEnabled
        with get () = selected.Options.CursorSpeed
        and set value = updateOptions (fun o -> { o with CursorSpeed = value })
    member _.CursorHideEnabled
        with get () = selected.Options.CursorHide
        and set value = updateOptions (fun o -> { o with CursorHide = value })
    member _.CursorScaleEnabled
        with get () = selected.Options.CursorScale
        and set value = updateOptions (fun o -> { o with CursorScale = value })
    member _.LimitToScreenEnabled
        with get () = selected.Options.LimitToScreen
        and set value = updateOptions (fun o -> { o with LimitToScreen = value })
    member _.VrrEnabled
        with get () = selected.Options.Vrr
        and set value = updateOptions (fun o -> { o with Vrr = value })
    member _.SyncModeIndex
        with get () = selected.Options.SyncMode
        and set value = if value >= 0 then updateOptions (fun o -> { o with SyncMode = value })
    member _.LowLatencyEnabled
        with get () = selected.Options.LowLatency
        and set value = updateOptions (fun o -> { o with LowLatency = value })
    member _.HdrEnabled
        with get () = selected.Options.Hdr
        and set value = updateOptions (fun o -> { o with Hdr = value })
    member _.GpuPriorityIndex
        with get () = selected.Options.GpuPriority
        and set value = if value >= 0 then updateOptions (fun o -> { o with GpuPriority = value })
    // The card the engine runs on: "Auto" (it picks: the first RTX card for DLSS MFG, the strongest card for
    // XeSS MFG), or one of this machine's by name. Read at the start, like the generator.
    member _.GpuChoices = gpusShown
    member _.GpuIndex
        with get () =
            match cardNames selected.Options.Generator |> Array.tryFindIndex ((=) selected.Options.Gpu) with
            | Some index -> index + 1
            | None -> 0
        and set value =
            let names = cardNames selected.Options.Generator
            if not telling && value >= 0 && value <= names.Length && not engine.IsActive then
                updateOptions (fun o -> { o with Gpu = (if value = 0 then "" else names[value - 1]) })
    member _.MotionSourceIndex
        with get () = selected.Options.MotionSource
        and set value = if value >= 0 then updateOptions (fun o -> { o with MotionSource = value })

    member _.XessMotionSourceIndex
        with get () = selected.Options.XessMotionSource
        and set value = if value >= 0 then updateOptions (fun o -> { o with XessMotionSource = value })
    // MSM FG finds the motion itself: its row says how hard it looks.
    member _.MsmSearches = msmSearches
    member _.MsmSearchIndex
        with get () = selected.Options.MsmSearch
        and set value = if value >= 0 then updateOptions (fun o -> { o with MsmSearch = value })
    // DLSS MFG's preset (NVIDIA's own C, B, A): written into the driver's profile of the engine when it starts.
    member _.DlssPresets = dlssPresets
    member _.DlssPresetIndex
        with get () = selected.Options.DlssPreset
        and set value =
            if value >= 0 then
                let warn = value = 1 && selected.Options.DlssPreset <> 1
                updateOptions (fun o -> { o with DlssPreset = value })
                if warn then presetCPicked.Trigger()
    member _.PresetCPicked = presetCPicked.Publish
    // Adaptive Motion Learning: the switch, what the engine says of how far along it is, and what is kept.
    member _.AdaptiveLearningEnabled
        with get () = selected.Options.AdaptiveLearning
        and set value = updateOptions (fun o -> { o with AdaptiveLearning = value })
    // Adaptive Flow Scale: part of it - open only while it is on.
    member _.AdaptiveFlowEnabled
        with get () = selected.Options.AdaptiveFlow
        and set value = updateOptions (fun o -> { o with AdaptiveFlow = value })
    member _.IsAdaptiveFlowOpen = selected.Options.AdaptiveLearning
    member _.LearningStatus =
        if not selected.Options.AdaptiveLearning || engineStatus.Phase <> Running || engineStatus.Learning < 0 then ""
        elif engineStatus.Learning > 100 then this.Text.LearnedDone
        else String.Format(this.Text.LearningProgress, engineStatus.Learning)
    member _.LearnedGames = learned
    member _.HasLearnedGames = learned.Count > 0
    member _.NoLearnedGames = learned.Count = 0
    member _.StillPixelsEnabled
        with get () = selected.Options.StillPixels
        and set value = updateOptions (fun o -> { o with StillPixels = value })

    member _.FpsCounterEnabled
        with get () = selected.Options.FpsCounter
        and set value = updateOptions (fun o -> { o with FpsCounter = value })

    // START MOTION and what the engine says back.
    member _.IsMotionActive = engineStatus.Phase <> Idle
    member _.IsMotionRunning = engineStatus.Phase = Running
    member _.IsMotionWaiting = engineStatus.Phase <> Idle && engineStatus.Phase <> Running
    member _.MotionButtonText = if engineStatus.Phase = Idle then "START MOTION" else "STOP MOTION"
    member _.StatusText =
        let text = this.Text
        match engineStatus.Phase with
        | Picking -> String.Format(text.StatusPicking, max 0 engineStatus.Countdown)
        | Starting -> text.StatusStarting
        | Running ->
            if engineStatus.MfgUnavailable then text.StatusUnavailable.Replace("DLSS MFG", this.GeneratorName)
            elif engineStatus.BaseFps >= 0 && engineStatus.OutputFps >= 0 && engineStatus.Width > 0 then
                let rate =
                    if engineStatus.Multiplier > 1 then sprintf "%d/%d FPS · ×%d" engineStatus.BaseFps engineStatus.OutputFps engineStatus.Multiplier
                    else sprintf "%d FPS" engineStatus.OutputFps
                sprintf "%s · %d×%d · %s" rate engineStatus.Width engineStatus.Height (engineStatus.Api.ToUpperInvariant())
            elif engineStatus.Width > 0 then
                sprintf "%s · %d×%d · %s" text.StatusRunning engineStatus.Width engineStatus.Height (engineStatus.Api.ToUpperInvariant())
            else text.StatusRunning
        | Idle ->
            match engineStatus.Error with
            | "" -> ""
            | "nowindow" -> text.StatusNoWindow
            | "closed" -> text.StatusClosed
            | "nogpu" -> text.StatusNoGpu
            | "xess" -> text.XessUnavailable
            | "missing" -> text.StatusMissing
            | _ -> text.StatusFailed
    member _.HasStatus = this.StatusText <> ""
    member _.StatusNote =
        if capturingHotkey then ""
        elif hotkey.IsTaken then this.Text.ShortcutTaken
        elif engineStatus.Phase = Running && engineStatus.VrrFixed && selected.Options.Vrr then this.Text.VrrFixed
        else ""
    member _.HasStatusNote = this.StatusNote <> ""
    member _.MotionCommand = motionCommand

    // The shortcut: pressed in the window while it is being picked, system-wide afterwards.
    member _.ShortcutLabel =
        if capturingHotkey then this.Text.PressKey
        elif effectiveHotkey () = 0 then this.Text.Off
        else Shortcut.describe (effectiveHotkey ())
    member _.IsCapturingShortcut = capturingHotkey
    member _.ShortcutCommand = shortcutCommand
    member _.BeginShortcutCapture() =
        if not capturingHotkey then
            capturingHotkey <- true
            hotkey.Set 0
            notify [ "ShortcutLabel"; "IsCapturingShortcut"; "StatusNote"; "HasStatusNote" ]
    /// value: modifiers * 65536 + virtual key; -1 none; 0 leaves it as it was.
    member _.EndShortcutCapture(value: int) =
        if capturingHotkey then
            capturingHotkey <- false
            if value <> 0 then
                hotkeyValue <- (if value = Preferences.defaultHotkey then 0 else value)
                persist ()
            hotkey.Set(effectiveHotkey ())
            notify [ "ShortcutLabel"; "IsCapturingShortcut"; "StatusNote"; "HasStatusNote" ]

    member _.IsDisplay = page = "display"
    member _.IsVideo = page = "video"
    member _.IsMods = page = "mods"
    member _.IsMoreOpen = moreOpen
    member _.IsHelpGuideOpen
        with get () = helpGuideOpen
        and set value =
            helpGuideOpen <- value
            notify [ "IsHelpGuideOpen" ]
    member _.IsSupport = page = "support"
    member _.Video = video
    member _.IsSettings = page = "settings"
    member _.IsHelp = page = "help"
    member _.FeedbackText
        with get () = feedbackText
        and set (value: string) =
            feedbackText <- (if isNull value then "" else value)
            notify [ "CanSendFeedback" ]
    member _.FeedbackAttach
        with get () = feedbackAttach
        and set value =
            feedbackAttach <- value
            notify [ "CanSendFeedback" ]
    member _.CanAttachLog = not (List.isEmpty (feedbackLogs ()))
    member this.FeedbackLogNote = if this.CanAttachLog then "" else this.Text.FeedbackNoLog
    member _.CanSendFeedback = not feedbackBusy && (feedbackText.Trim() <> "" || feedbackAttach)
    member _.FeedbackStatus = feedbackStatus
    member _.FeedbackAbout = Feedback.about ()
    member _.SendFeedbackCommand = sendFeedbackCommand
    member _.IsAdvancedOpen
        with get () = advanced
        and set value =
            if this.SetProperty(&advanced, value) then persist ()
    member _.IsEditorOpen
        with get () = editorOpen
        and set value =
            if this.SetProperty(&editorOpen, value) then notify [ "IsDialogOpen" ]
    member _.IsDeleteOpen
        with get () = deleteOpen
        and set value =
            if this.SetProperty(&deleteOpen, value) then notify [ "IsDialogOpen" ]
    member _.IsDialogOpen = editorOpen || deleteOpen || gate <> ""
    member _.IsGateOpen = gate <> ""
    member _.GateText =
        let version = match gateRelease with | Some release -> release.Version.ToString() | None -> ""
        match gate with
        | "checking" -> this.Text.EngineChecking
        | "required" -> String.Format(this.Text.EngineRequired, version)
        | "update" -> String.Format(this.Text.EngineUpdate, version)
        | "downloading" ->
            let megabytes (bytes: int64) = float bytes / 1048576.0
            String.Format(this.Text.EngineDownloading, int (gateProgress * 100.0)) +
            (if Releases.downloadSize > 0L then sprintf "   (%.1f / %.1f MB)" (megabytes Releases.downloaded) (megabytes Releases.downloadSize) else "")
        | "failed" -> this.Text.EngineFailed
        | _ -> ""
    member _.GateProgress = gateProgress * 100.0
    member _.IsGateDownloading = gate = "downloading"
    member _.CanGateDownload = gate = "required" || gate = "update" || gate = "failed"
    member _.CanGateSkip = (gate = "update" || gate = "failed") && (EngineStore.path ()).IsSome
    member _.GateActionText = match gate with | "update" -> this.Text.UpdateNow | "failed" -> this.Text.Retry | _ -> this.Text.Download
    member _.GateDownloadCommand = gateDownloadCommand
    member _.OpenLogsCommand = openLogsCommand
    member _.GateSkipCommand = gateSkipCommand
    member _.HasAppUpdate = appUpdate <> ""
    member _.AppUpdateText = String.Format(this.Text.AppUpdate, appUpdate)
    member _.DialogTitle = if adding then this.Text.Add else this.Text.Rename
    member _.DraftName
        with get () = draftName
        and set value =
            let value = if isNull value then "" else value
            if this.SetProperty(&draftName, value) then
                saveCommand.NotifyCanExecuteChanged()
                notify [ "IsNameInvalid" ]
    member _.IsNameInvalid = draftName.Length > 0 && not (validName ())
    member _.CancelDialog() = this.IsEditorOpen <- false; this.IsDeleteOpen <- false
    member _.DisplayCommand = displayCommand
    member _.SupportCommand = supportCommand
    member _.VideoCommand = videoCommand
    member _.ModsCommand = modsCommand
    member _.MoreCommand = moreCommand
    member _.OpenLinkCommand = openLinkCommand
    member _.NvidiaDrivers = String.Format(this.Text.LinkDrivers, "NVIDIA")
    member _.AmdDrivers = String.Format(this.Text.LinkDrivers, "AMD")
    member _.IntelDrivers = String.Format(this.Text.LinkDrivers, "Intel")
    member _.SettingsCommand = settingsCommand
    member _.HelpCommand = helpCommand
    member _.AddCommand = addCommand
    member _.RenameCommand = renameCommand
    member _.DeleteCommand = deleteCommand
    member _.SaveCommand = saveCommand
    member _.CancelCommand = cancelCommand
    member _.ConfirmDeleteCommand = confirmDeleteCommand
    member _.ResetCommand = resetCommand
    member _.RetrySaveCommand = retrySaveCommand
    member _.Flush() = store.Flush()
    member _.SaveError = store.SaveError
    member _.HasSaveError = store.SaveError <> ""
    member _.PersistenceChanged = store.StatusChanged
    interface IDisposable with
        member _.Dispose() =
            disposed <- true
            sendTimer.Dispose()
            (hotkey :> IDisposable).Dispose()
            (engine :> IDisposable).Dispose()
            (video :> IDisposable).Dispose()

            (store :> IDisposable).Dispose()
