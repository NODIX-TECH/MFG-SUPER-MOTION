namespace ARMO.ViewModels

open System
open System.Collections.ObjectModel
open System.Diagnostics
open CommunityToolkit.Mvvm.Input
open ARMO.Models
open ARMO.Infrastructure
open ARMO.Localization

/// One console found on the network, as the Console Link page lists it: linked with this PC or not, and its buttons.
type ConsoleEntry(info: ConsoleInfo, stateText: string, linked: LinkedConsole option, link: ConsoleEntry -> unit, play: ConsoleEntry -> unit) as this =
    inherit ViewModelBase()
    let mutable pin = ""
    let mutable note = ""
    let mutable busy = false
    let mutable linkedNow = linked
    let linkCommand = RelayCommand(fun () -> link this)
    let playCommand = RelayCommand(fun () -> play this)
    member _.Info = info
    member _.Name = match linkedNow with Some l when l.Name <> "" -> l.Name | _ -> info.Name
    member _.Kind = match info.Kind with PS5 -> "PS5" | PS4 -> "PS4"
    member _.Address = info.Address
    member _.StateText = stateText
    member _.IsReady = info.State = Ready
    member _.Linked = linkedNow
    member _.IsLinked = linkedNow.IsSome
    member _.IsNotLinked = linkedNow.IsNone
    member _.Pin
        with get () = pin
        and set (value: string) =
            pin <- (if isNull value then "" else value.Trim())
            this.Notify "Pin"
    member _.Note = note
    member _.HasNote = note <> ""
    member _.IsBusy = busy
    member _.IsFree = not busy
    member _.LinkCommand = linkCommand
    member _.PlayCommand = playCommand
    member _.SetLinked(l: LinkedConsole) = linkedNow <- Some l; this.Notify "IsLinked"; this.Notify "IsNotLinked"; this.Notify "Name"
    member _.Say(text: string, working: bool) =
        note <- text
        busy <- working
        for n in [ "Note"; "HasNote"; "IsBusy"; "IsFree" ] do this.Notify n

/// One Xbox of the signed-in account, as the page lists it.
type XboxEntry(console: XboxConsole, stateText: string, play: XboxEntry -> unit) as this =
    inherit ViewModelBase()
    let mutable note = ""
    let mutable busy = false
    let playCommand = RelayCommand(fun () -> play this)
    member _.Console = console
    member _.Name = console.Name
    member _.Kind =
        match console.ConsoleType with
        | "XboxSeriesX" -> "Series X"
        | "XboxSeriesS" -> "Series S"
        | "XboxOneX" -> "One X"
        | "XboxOneS" -> "One S"
        | "XboxOne" -> "One"
        | t -> t.Replace("Xbox", "")
    member _.StateText = stateText
    member _.IsReady = console.Power = "On"
    member _.Note = note
    member _.HasNote = note <> ""
    member _.IsBusy = busy
    member _.IsFree = not busy
    member _.PlayCommand = playCommand
    member _.Say(text: string, working: bool) =
        note <- text
        busy <- working
        for n in [ "Note"; "HasNote"; "IsBusy"; "IsFree" ] do this.Notify n

/// The stream playing now, whichever console: lines to its player, its window to the front, and its end asked.
type StreamHandle = { Tell: string -> unit; Front: unit -> unit; Stop: unit -> unit; Xbox: bool }

/// Console Link (2026-11-16, the owner): the page that finds a PlayStation (and, next, an Xbox) on the home network,
/// links this PC with it once (the PSN account, the console's PIN), and plays it - the stream in Console\mfgsm-console.exe's
/// window, frame generation on that window. Its settings are the app's (Preferences.Console), saved at once.
/// Frame generation on the stream is Super Motion itself, every technique of the profile (the owner: "we only shorten
/// the way for the user - the overlay is Super Motion's interface"): startFg starts the engine on the stream's window
/// with the profile as it is; the page shows Super Motion's own cards under the console's; F1 / Guide + Start in the
/// stream brings this window over it (MenuRequested), "Back to the game" hands the stream the front again.
type ConsoleLinkModel(text: unit -> UiText, post: (unit -> unit) -> unit, initial: ConsoleOptions, save: ConsoleOptions -> unit,
                      startFg: uint64 -> bool, stopFg: unit -> unit) as this =
    inherit ViewModelBase()
    let mutable options = Preferences.normalizeConsole initial
    let mutable searching = false
    let mutable searched = false
    let mutable link = ConsoleLink.pcLink ()
    let mutable pasted = ""
    let mutable accountNote = ""
    let mutable playing: StreamHandle option = None
    let mutable playingText = ""
    let consoles = ObservableCollection<ConsoleEntry>()
    let xboxes = ObservableCollection<XboxEntry>()
    let mutable xboxFound: XboxConsole list = []
    let mutable xboxSearching = false
    let mutable xboxSearched = false
    let mutable xboxSigningIn = false
    let mutable xboxNote = ""
    let mutable xboxCancel: Threading.CancellationTokenSource = null
    let menuRequested = Event<unit>()
    let backToGame = Event<unit>()
    let mutable streamQuality = (0, 0, 0, 0)      // (what the running stream was asked at)
    let notify (names: string list) = for n in names do this.Notify n
    let update f =
        let next = Preferences.normalizeConsole (f options)
        if next <> options then
            options <- next
            save options
            notify [ "PlatformIndex"; "IsPlayStation"; "IsXbox"; "MenuHelp"; "Resolutions"; "ResolutionIndex"; "FpsIndex"; "BitrateIndex"; "CodecIndex"
                     "ManualAddress"; "BitrateLabel"; "IsSignedIn"; "IsSignedOut"; "AccountText"; "Passcode"; "QualityChanged" ]
    let linkedFor (c: ConsoleInfo) =
        options.Linked |> Array.tryFind (fun l -> (c.HostId <> "" && l.HostId = c.HostId) || l.Address = c.Address)
    /// A stream's lines to the app, whichever console: its window (frame generation on it), the menu, its state.
    let onStreamLine (name: string) (say: string -> bool -> unit) (line: string) =
        let t = text ()
        if line.StartsWith "@window " then
            match UInt64.TryParse(line.Substring 8) with
            | true, hwnd -> startFg hwnd |> ignore
            | _ -> ()
        elif line = "@menu" then menuRequested.Trigger()
        elif line = "@connected" then
            say t.ConsoleStreaming false
            playingText <- name + " · " + t.ConsoleStreaming
            notify [ "IsPlaying"; "PlayingText" ]
        elif line = "@passcode" then say t.ConsolePasscodeAsked false
        elif line.StartsWith "@error " then say (line.Substring 7) false
        elif line.StartsWith "@quit " then say (line.Substring 6) false
    let streamEnded () =
        stopFg ()
        playing <- None
        playingText <- ""
        notify [ "IsPlaying"; "PlayingText"; "QualityChanged" ]
    let rec fill (found: ConsoleInfo list) =
        consoles.Clear()
        let t = text ()
        for c in found do
            let state = match c.State with Ready -> t.ConsoleReady | Standby -> t.ConsoleStandby | Unknown -> "?"
            consoles.Add(ConsoleEntry(c, state, linkedFor c, linkConsole, playConsole))
    /// Linked by the PIN the console shows (Settings > System > Remote Play > Link Device).
    and linkConsole (entry: ConsoleEntry) =
        let t = text ()
        let pin = entry.Pin
        if options.AccountId = "" then entry.Say(t.ConsoleNeedsAccount, false)
        elif pin.Length <> 8 || not (pin |> Seq.forall Char.IsDigit) then entry.Say(t.ConsolePinHelp, false)
        else
            entry.Say(t.ConsoleLinking, true)
            task {
                let! result = ConsolePlayer.register entry.Info options.AccountId pin
                post (fun () ->
                    match result with
                    | Ok linked ->
                        update (fun o -> { o with Linked = Array.append (o.Linked |> Array.filter (fun l -> l.HostId <> linked.HostId)) [| linked |] })
                        entry.SetLinked linked
                        entry.Pin <- ""
                        entry.Say(t.ConsoleLinked, false)
                    | Error why -> entry.Say(t.ConsoleLinkFailed + " (" + why + ")", false))
            } |> ignore
    /// Played: woken first when it rests, then its stream; frame generation on the stream's window once it is there.
    and playConsole (entry: ConsoleEntry) =
        let t = text ()
        match entry.Linked with
        | None -> ()
        | Some _ when playing.IsSome -> entry.Say(t.ConsoleStreaming, false)
        | Some linked ->
            let linked = { linked with Address = entry.Address }      // (the address it answers at today)
            task {
                if entry.Info.State = Standby then
                    post (fun () -> entry.Say(t.ConsoleWaking, true))
                    let! _ = ConsolePlayer.wake linked
                    // (Up to half a minute for it to come up and answer as "on".)
                    let mutable up = false
                    let mutable tries = 0
                    while not up && tries < 15 do
                        let! found = ConsoleLink.discover (TimeSpan.FromSeconds 2.0) [ linked.Address ]
                        up <- found |> List.exists (fun c -> c.Address = linked.Address && c.State = Ready)
                        tries <- tries + 1
                post (fun () ->
                    entry.Say(t.ConsoleConnecting, true)
                    let started =
                        ConsolePlayer.stream linked options (fun line ->
                            post (fun () -> onStreamLine entry.Name (fun s w -> entry.Say(s, w)) line))
                    started.Exited.Add(fun _ ->
                        post (fun () ->
                            if entry.IsBusy then entry.Say("", false)
                            streamEnded ()))
                    playing <- Some { Tell = ConsolePlayer.tell started; Front = (fun () -> ConsolePlayer.front started)
                                      Stop = (fun () -> ConsolePlayer.tell started "quit"); Xbox = false }
                    streamQuality <- (options.Resolution, options.Fps, options.Bitrate, options.Codec)
                    notify [ "IsPlaying"; "PlayingText"; "QualityChanged" ])
            } |> ignore
    let searchCommand =
        AsyncRelayCommand(fun () ->
            (task {
                searching <- true
                link <- ConsoleLink.pcLink ()
                notify [ "IsSearching"; "CanSearch"; "LinkText"; "LinkShort"; "IsCable" ]
                let direct = (if options.Address <> "" then [ options.Address ] else []) @ (options.Linked |> Array.map _.Address |> Array.toList)
                let! found = ConsoleLink.discover (TimeSpan.FromSeconds 2.5) (List.distinct direct)
                post (fun () ->
                    fill found
                    searching <- false
                    searched <- true
                    notify [ "IsSearching"; "CanSearch"; "HasConsoles"; "NoneFound" ])
            } :> System.Threading.Tasks.Task))
    let signInCommand = RelayCommand(fun () -> try Process.Start(ProcessStartInfo(Psn.loginUrl, UseShellExecute = true)) |> ignore with _ -> ())
    let finishSignInCommand =
        AsyncRelayCommand(fun () ->
            (task {
                let t = text ()
                if Psn.isAccountId pasted then
                    update (fun o -> { o with AccountId = pasted.Trim() })
                    accountNote <- t.ConsoleSignedIn
                else
                    accountNote <- t.ConsoleSigningIn
                    notify [ "AccountNote" ]
                    let! result = Psn.accountId pasted
                    match result with
                    | Ok id -> update (fun o -> { o with AccountId = id }); accountNote <- t.ConsoleSignedIn
                    | Error why -> accountNote <- t.ConsoleSignInFailed + " (" + why + ")"
                pasted <- ""
                post (fun () -> notify [ "Pasted"; "AccountNote"; "HasAccountNote" ])
            } :> System.Threading.Tasks.Task))
    let signOutCommand = RelayCommand(fun () -> update (fun o -> { o with AccountId = "" }); accountNote <- ""; notify [ "AccountNote"; "HasAccountNote" ])
    let stopCommand = RelayCommand(fun () -> playing |> Option.iter (fun p -> p.Stop()))
    let backCommand = RelayCommand(fun () -> backToGame.Trigger(); playing |> Option.iter (fun p -> p.Front()))
    // (The console takes a stream's size, rate, bitrate and codec only when a session starts: the stream reconnects.)
    let applyQualityCommand =
        RelayCommand(fun () ->
            match playing with
            | Some p when p.Xbox -> p.Tell(sprintf "quality %d %d" (min 1 options.Resolution) options.Bitrate)
            | Some p -> p.Tell(sprintf "quality %d %d %d %d" options.Resolution (if options.Fps = 0 then 30 else 60) options.Bitrate options.Codec)
            | None -> ()
            streamQuality <- (options.Resolution, options.Fps, options.Bitrate, options.Codec)
            notify [ "QualityChanged" ])
    // Xbox: the account (signed in once, in Edge's window), its consoles, and play.
    let xboxStateText (c: XboxConsole) =
        let t = text ()
        let power = match c.Power with "On" -> t.ConsoleReady | "ConnectedStandby" | "Standby" -> t.ConsoleStandby | "Off" -> t.ConsoleXboxOff | p -> p
        if c.OutOfHome then power + " · " + t.ConsoleXboxAway else power
    let sayXbox (note: string) = xboxNote <- note; notify [ "XboxNote"; "HasXboxNote" ]
    let rec fillXbox () =
        xboxes.Clear()
        for c in xboxFound do xboxes.Add(XboxEntry(c, xboxStateText c, playXbox))
        notify [ "HasXboxes"; "XboxNoneFound" ]
    and playXbox (entry: XboxEntry) =
        let t = text ()
        if playing.IsSome then entry.Say(t.ConsoleStreaming, false)
        elif not (Edge.isThere ()) then entry.Say(t.ConsoleXboxEdgeMissing, false)
        else
            entry.Say(t.ConsoleConnecting, true)
            let session =
                XboxPlayer.play entry.Console (min 1 options.Resolution) options.Bitrate (fun line ->
                    post (fun () -> onStreamLine entry.Name (fun s w -> entry.Say(s, w)) line))
            XboxPlayer.watchPad session
            session.Ended.ContinueWith(fun (_: Threading.Tasks.Task) ->
                post (fun () ->
                    if entry.IsBusy then entry.Say("", false)
                    streamEnded ())) |> ignore
            playing <- Some { Tell = session.Tell; Front = session.Front; Stop = session.Close; Xbox = true }
            streamQuality <- (options.Resolution, options.Fps, options.Bitrate, options.Codec)
            notify [ "IsPlaying"; "PlayingText"; "QualityChanged" ]
    let xboxSearchCommand =
        AsyncRelayCommand(fun () ->
            (task {
                let t = text ()
                xboxSearching <- true
                sayXbox ""
                notify [ "IsXboxSearching"; "CanXboxSearch" ]
                let! result = task { try let! found = XboxAuth.consoles () in return Ok found with e -> return Error e.Message }
                post (fun () ->
                    match result with
                    | Ok found -> xboxFound <- found
                    | Error why -> xboxFound <- []; sayXbox why
                    xboxSearching <- false
                    xboxSearched <- true
                    fillXbox ()
                    notify [ "IsXboxSearching"; "CanXboxSearch"; "XboxSignedInText" ])
            } :> System.Threading.Tasks.Task))
    let xboxSignInCommand =
        AsyncRelayCommand(fun () ->
            (task {
                let t = text ()
                if not (Edge.isThere ()) then sayXbox t.ConsoleXboxEdgeMissing
                else
                    xboxSigningIn <- true
                    sayXbox t.ConsoleXboxSigningIn
                    notify [ "IsXboxSigningIn"; "CanXboxSignIn" ]
                    xboxCancel <- new Threading.CancellationTokenSource()
                    let! result =
                        task {
                            try
                                let! begun = XboxAuth.start ()
                                let! answer = XboxAuth.signInWindow begun xboxCancel.Token
                                match answer with
                                | Ok url ->
                                    let! tag = XboxAuth.finish begun url
                                    return Ok tag
                                | Error why -> return Error why
                            with e -> return Error e.Message
                        }
                    post (fun () ->
                        xboxSigningIn <- false
                        match result with
                        | Ok _ -> sayXbox ""
                        | Error why -> sayXbox (t.ConsoleXboxSignInFailed + " (" + why + ")")
                        notify [ "IsXboxSigningIn"; "CanXboxSignIn"; "IsXboxSignedIn"; "IsXboxSignedOut"; "XboxSignedInText" ]
                        if XboxAuth.isSignedIn () then xboxSearchCommand.Execute null)
            } :> System.Threading.Tasks.Task))
    let xboxSignOutCommand =
        RelayCommand(fun () ->
            XboxAuth.signOut ()
            xboxFound <- []
            xboxSearched <- false
            fillXbox ()
            sayXbox ""
            notify [ "IsXboxSignedIn"; "IsXboxSignedOut"; "XboxSignedInText" ])

    member _.Options = options
    // PlayStation | Xbox
    member _.Platforms = [| "PlayStation"; "Xbox" |]
    member _.PlatformIndex
        with get () = options.Platform
        and set value = if value >= 0 then update (fun o -> { o with Platform = value })
    member _.IsPlayStation = options.Platform = 0
    member _.IsXbox = options.Platform = 1
    // This PC's way to the network: a cable is best for streaming.
    member _.IsCable = link.Kind = Cable
    member _.LinkText =
        let t = text ()
        match link.Kind with
        | Cable -> String.Format(t.ConsoleCable, link.Adapter, link.SpeedMbps)
        | WiFi -> String.Format(t.ConsoleWifi, link.Adapter, link.SpeedMbps)
        | NoLink -> t.ConsoleNoLink
    member this.LinkShort =
        let s = this.LinkText
        match s.IndexOf " - " with
        | i when i > 0 -> s.Substring(0, i)
        | _ -> s
    // The PSN account (a PS5's link needs its id).
    member _.IsSignedIn = options.AccountId <> ""
    member _.IsSignedOut = options.AccountId = ""
    member _.SignInCommand = signInCommand
    member _.FinishSignInCommand = finishSignInCommand
    member _.SignOutCommand = signOutCommand
    member _.Pasted
        with get () = pasted
        and set (value: string) = pasted <- (if isNull value then "" else value)
    member _.AccountNote = accountNote
    member _.HasAccountNote = accountNote <> ""
    member _.Passcode
        with get () = options.Passcode
        and set (value: string) = update (fun o -> { o with Passcode = (if isNull value then "" else value) })
    // The search.
    member _.SearchCommand = searchCommand
    member _.IsSearching = searching
    member _.CanSearch = not searching
    member _.Consoles = consoles
    member _.HasConsoles = consoles.Count > 0
    member _.NoneFound = searched && consoles.Count = 0 && not searching
    member _.ManualAddress
        with get () = options.Address
        and set (value: string) = update (fun o -> { o with Address = (if isNull value then "" else value) })
    // Whether the player is beside the app.
    member _.IsStreamReady = ConsolePlayer.isThere ()
    member _.IsStreamMissing = not (ConsolePlayer.isThere ())
    // Playing now.
    member _.IsPlaying = playing.IsSome
    member _.PlayingText = playingText
    member _.StopCommand = stopCommand
    member _.MenuHelp = let t = text () in if options.Platform = 1 then t.ConsoleXboxMenuHelp else t.ConsoleMenuHelp
    // Xbox.
    member _.IsXboxSignedIn = XboxAuth.isSignedIn ()
    member _.IsXboxSignedOut = not (XboxAuth.isSignedIn ())
    member _.XboxSignedInText = String.Format((text ()).ConsoleXboxSignedIn, XboxAuth.gamertag ())
    member _.XboxSignInCommand = xboxSignInCommand
    member _.XboxSignOutCommand = xboxSignOutCommand
    member _.IsXboxSigningIn = xboxSigningIn
    member _.CanXboxSignIn = not xboxSigningIn
    member _.XboxSearchCommand = xboxSearchCommand
    member _.IsXboxSearching = xboxSearching
    member _.CanXboxSearch = not xboxSearching
    member _.Xboxes = xboxes
    member _.HasXboxes = xboxes.Count > 0
    member _.XboxNoneFound = xboxSearched && xboxes.Count = 0 && not xboxSearching && xboxNote = ""
    member _.XboxNote = xboxNote
    member _.HasXboxNote = xboxNote <> ""
    member _.IsXboxReady = XboxPlayer.isThere ()
    member _.IsEdgeMissing = not (Edge.isThere ())
    member _.BackCommand = backCommand
    member _.ApplyQualityCommand = applyQualityCommand
    /// While playing, the stream's settings moved since it started: Apply asks it anew.
    member _.QualityChanged = playing.IsSome && streamQuality <> (options.Resolution, options.Fps, options.Bitrate, options.Codec)
    [<CLIEvent>] member _.MenuRequested = menuRequested.Publish
    [<CLIEvent>] member _.BackToGame = backToGame.Publish
    // The stream's settings.
    // (An Xbox streams 720p or 1080p.)
    member _.Resolutions = if options.Platform = 1 then [| "720p"; "1080p" |] else [| "720p"; "1080p"; "1440p"; "4K" |]
    member _.ResolutionIndex
        with get () = if options.Platform = 1 then min 1 options.Resolution else options.Resolution
        and set value = if value >= 0 then update (fun o -> { o with Resolution = value })
    member _.FpsChoices = [| "30 FPS"; "60 FPS" |]
    member _.FpsIndex
        with get () = options.Fps
        and set value = if value >= 0 then update (fun o -> { o with Fps = value })
    member this.Bitrates = [| (text ()).ConsoleAuto; "5 Mbit/s"; "10 Mbit/s"; "15 Mbit/s"; "20 Mbit/s"; "30 Mbit/s"; "40 Mbit/s"; "50 Mbit/s" |]
    member _.BitrateIndex
        with get () = match options.Bitrate with 0 -> 0 | b -> (match [| 5000; 10000; 15000; 20000; 30000; 40000; 50000 |] |> Array.tryFindIndex ((=) b) with Some i -> i + 1 | None -> 0)
        and set value = if value >= 0 then update (fun o -> { o with Bitrate = (if value = 0 then 0 else [| 5000; 10000; 15000; 20000; 30000; 40000; 50000 |][min 6 (value - 1)]) })
    member _.BitrateLabel = sprintf "%d Mbit/s" (Preferences.consoleBitrate options / 1000)
    member _.Codecs = [| "H.264"; "H.265"; "H.265 HDR" |]
    member _.CodecIndex
        with get () = options.Codec
        and set value = if value >= 0 then update (fun o -> { o with Codec = value })
    /// The language changed: its words again.
    member _.Retext() =
        let found = consoles |> Seq.map _.Info |> Seq.toList
        fill found
        fillXbox ()
        notify [ "LinkText"; "LinkShort"; "Bitrates"; "BitrateIndex"; "MenuHelp"; "XboxSignedInText" ]
