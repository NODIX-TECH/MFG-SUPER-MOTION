namespace ARMO.ViewModels

open System
open System.Collections.ObjectModel
open System.Diagnostics
open System.IO
open CommunityToolkit.Mvvm.Input
open ARMO.Infrastructure
open ARMO.Localization

/// A choice in one of Super Video's lists, its label in the app's language.
type VideoChoice(label: string) =
    inherit ViewModelBase()
    let mutable label = label
    member this.Label
        with get () = label
        and set value = this.SetProperty(&label, value) |> ignore
    override _.ToString() = label

/// One video on the Super Video page: its own settings, and how far it is.
type VideoItem(path: string) =
    inherit ViewModelBase()
    let mutable frames = 1          // index: Off, x2 .. x6
    let mutable size = 0            // index: the video's own, 720p .. 4K
    let mutable upscaler = 0        // index into SuperVideoModel.upscalerKinds
    let mutable codec = 0
    let mutable technique = 0       // SMFG, SMFG fast
    let mutable status = ""
    let mutable progress = 0.0
    let mutable state = 0           // 0 waiting, 1 working, 2 done, 3 failed or stopped
    let mutable output = ""
    member _.Path = path
    member _.Name = System.IO.Path.GetFileName path
    member this.FramesIndex with get () = frames and set v = if v >= 0 then this.SetProperty(&frames, v) |> ignore
    member this.SizeIndex with get () = size and set v = if v >= 0 then this.SetProperty(&size, v) |> ignore
    member this.UpscalerIndex with get () = upscaler and set v = if v >= 0 then this.SetProperty(&upscaler, v) |> ignore
    member this.CodecIndex with get () = codec and set v = if v >= 0 then this.SetProperty(&codec, v) |> ignore
    member this.TechniqueIndex with get () = technique and set v = if v >= 0 then this.SetProperty(&technique, v) |> ignore
    member this.Status with get () = status and set v = this.SetProperty(&status, v) |> ignore
    member this.Progress with get () = progress and set v = this.SetProperty(&progress, v) |> ignore
    member this.State
        with get () = state
        and set v =
            if this.SetProperty(&state, v) then
                this.Notify "IsEditable"
                this.Notify "IsDone"
                this.Notify "IsWorking"
    member _.IsEditable = state <> 1
    member _.IsWorking = state = 1
    member _.IsDone = state = 2
    member this.Output with get () = output and set v = this.SetProperty(&output, v) |> ignore

/// The Super Video page (round 47): videos made smoother (SMFG's frames in between) and, if asked, larger - one after
/// the other, each with its own settings, into a folder chosen or next to each video. Nothing starts but by Start.
type SuperVideoModel(text: unit -> UiText, post: (unit -> unit) -> unit, liveActive: unit -> bool) as this =
    inherit ViewModelBase()
    static let heights = [| 0; 720; 1080; 1440; 2160 |]
    static let upscalerKinds = [| 0; 1; 4; 2; 3 |]
    static let videoTypes = set [ ".mp4"; ".mkv"; ".mov"; ".avi"; ".wmv"; ".webm"; ".m4v"; ".ts"; ".mts"; ".m2ts"; ".flv" ]
    let items = ObservableCollection<VideoItem>()
    let frames = [| for _ in 1 .. 6 -> VideoChoice("") |]
    let sizes = [| for _ in heights -> VideoChoice("") |]
    let upscalers = [| VideoChoice("Lanczos"); VideoChoice(""); VideoChoice("Anime4K"); VideoChoice("Integer"); VideoChoice("Nearest Neighbor") |]
    let codecs = [| VideoChoice("H.264"); VideoChoice("HEVC (H.265)") |]
    let techniques = [| VideoChoice("SMFG"); VideoChoice("") |]
    let mutable folder = ""
    let mutable running: VideoRun option = None
    let mutable stopping = false
    let mutable note = ""
    let relabel () =
        let t = text ()
        frames[0].Label <- t.Off
        for i in 1 .. 5 do frames[i].Label <- sprintf "×%d" (i + 1)
        sizes[0].Label <- t.VideoSameSize
        sizes[1].Label <- "720p"; sizes[2].Label <- "1080p"; sizes[3].Label <- "1440p"; sizes[4].Label <- "4K (2160p)"
        upscalers[1].Label <- sprintf "Lanczos · %s" t.Sharpness
        techniques[1].Label <- sprintf "SMFG · %s" t.Fast
    do relabel ()

    // The new video's name: the old one's with " - Super Video", in the folder chosen or beside it, never over a file.
    let outputFor (input: string) =
        let where = if folder <> "" && Directory.Exists folder then folder else Path.GetDirectoryName input
        let stem = Path.GetFileNameWithoutExtension input + " - Super Video"
        let mutable candidate = Path.Combine(where, stem + ".mp4")
        let mutable n = 2
        while File.Exists candidate do
            candidate <- Path.Combine(where, sprintf "%s (%d).mp4" stem n)
            n <- n + 1
        candidate
    let startCommand = RelayCommand(fun () -> this.Start())
    let stopCommand = RelayCommand(fun () -> this.Stop())
    let removeCommand = RelayCommand<VideoItem>(fun item -> this.Remove item)
    let revealCommand = RelayCommand<VideoItem>(fun item -> this.Reveal item)
    let clearFolderCommand = RelayCommand(fun () -> this.SetFolder "")

    member this.Items = items
    member _.Frames = frames
    member _.Sizes = sizes
    member _.Upscalers = upscalers
    member _.Codecs = codecs
    member _.Techniques = techniques
    member _.IsRunning = running.IsSome
    member _.HasItems = items.Count > 0
    member _.IsEmpty = items.Count = 0
    member this.CanStart = running.IsNone && items |> Seq.exists (fun i -> i.State = 0)
    member _.Note = note
    member _.HasNote = note <> ""
    member _.Folder = folder
    member _.FolderLabel = if folder = "" then (text ()).VideoNextToEach else folder

    member this.Relabel() =
        relabel ()
        this.Notify "FolderLabel"

    member this.SetFolder(path: string) =
        folder <- (if isNull path then "" else path)
        this.Notify "Folder"
        this.Notify "FolderLabel"

    member this.AddPaths(paths: string seq) =
        for p in paths do
            if File.Exists p && videoTypes.Contains((Path.GetExtension p).ToLowerInvariant()) && not (items |> Seq.exists (fun i -> String.Equals(i.Path, p, StringComparison.OrdinalIgnoreCase))) then
                let item = VideoItem(p)
                // (A new video starts from the settings of the one above it: several alike are set once.)
                if items.Count > 0 then
                    let last = items[items.Count - 1]
                    item.FramesIndex <- last.FramesIndex
                    item.SizeIndex <- last.SizeIndex
                    item.UpscalerIndex <- last.UpscalerIndex
                    item.CodecIndex <- last.CodecIndex
                    item.TechniqueIndex <- last.TechniqueIndex
                item.Status <- (text ()).VideoWaiting
                items.Add item
        this.Refresh()

    member this.Remove(item: VideoItem) =
        if not (isNull (box item)) && item.State <> 1 then
            items.Remove item |> ignore
            this.Refresh()

    member this.Refresh() =
        for name in [ "IsRunning"; "HasItems"; "IsEmpty"; "CanStart"; "Note"; "HasNote" ] do this.Notify name

    member private this.Say(text': string) =
        note <- text'
        this.Refresh()

    /// The next waiting video, or the end.
    member private this.Next() =
        let t = text ()
        match items |> Seq.tryFind (fun i -> i.State = 0) with
        | None ->
            running <- None
            stopping <- false
            this.Say ""
        | Some _ when stopping ->
            running <- None
            stopping <- false
            this.Say ""
        | Some item ->
            let job =
                { Input = item.Path
                  Output = outputFor item.Path
                  Multiplier = item.FramesIndex + 1
                  Height = heights[max 0 (min (heights.Length - 1) item.SizeIndex)]
                  Codec = item.CodecIndex
                  Upscaler = upscalerKinds[max 0 (min (upscalerKinds.Length - 1) item.UpscalerIndex)]
                  Search = item.TechniqueIndex }
            item.State <- 1
            item.Progress <- 0.0
            item.Status <- String.Format(t.VideoWorking, 0)
            item.Output <- job.Output
            let report event =
                post (fun () ->
                    match event with
                    | Progress(d, total) ->
                        let share = if total > 0L then 100.0 * float d / float total else 0.0
                        item.Progress <- share
                        item.Status <- String.Format(t.VideoWorking, int share)
                    | Finished ->
                        item.Progress <- 100.0
                        item.State <- 2
                        item.Status <- t.VideoDone
                        this.Next()
                    | Stopped ->
                        item.State <- 3
                        item.Status <- t.VideoStopped
                        this.Next()
                    | Failed why ->
                        item.State <- 3
                        item.Status <- String.Format(t.VideoFailed, why)
                        this.Next())
            let run = new VideoRun(job, report)
            running <- Some run
            this.Refresh()
            run.Start() |> ignore

    member this.Start() =
        if running.IsNone then
            if liveActive () then this.Say (text ()).VideoStopMotionFirst
            else
                stopping <- false
                this.Say ""
                this.Next()

    member this.Stop() =
        match running with
        | Some run ->
            stopping <- true
            run.Stop()
        | None -> ()

    member _.Reveal(item: VideoItem) =
        if not (isNull (box item)) && item.Output <> "" && File.Exists item.Output then
            try Process.Start(ProcessStartInfo("explorer.exe", sprintf "/select,\"%s\"" item.Output, UseShellExecute = true)) |> ignore with _ -> ()

    member _.StartCommand = startCommand
    member _.StopCommand = stopCommand
    member _.RemoveCommand = removeCommand
    member _.RevealCommand = revealCommand
    member _.ClearFolderCommand = clearFolderCommand

    interface IDisposable with
        member _.Dispose() = running |> Option.iter (fun r -> (r :> IDisposable).Dispose())
