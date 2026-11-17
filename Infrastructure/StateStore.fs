namespace ARMO.Infrastructure

open System
open System.IO
open System.Text.Json
open System.Threading
open ARMO.Models

/// Keeps one in-memory snapshot. A one-shot timer merges rapid slider changes into one write.
type StateStore(path: string) =
    let gate = obj ()
    let jsonOptions = JsonSerializerOptions(WriteIndented = true)
    let mutable pending: Preferences option = None
    let mutable lastSaved: Preferences option = None
    let mutable disposed = false
    let mutable saveError = ""
    let statusChanged = Event<unit>()

    let writePending () =
        let changed = lock gate (fun () ->
            let previousError = saveError
            match pending with
            | Some state ->
                try
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath path)) |> ignore
                    let temporary = path + ".tmp"
                    File.WriteAllText(temporary, JsonSerializer.Serialize(state, jsonOptions))
                    File.Move(temporary, path, true)
                    lastSaved <- Some state
                    pending <- None
                    saveError <- ""
                with
                | :? IOException as error -> saveError <- error.Message
                | :? UnauthorizedAccessException as error -> saveError <- error.Message
            | None -> ()
            previousError <> saveError)
        if changed then statusChanged.Trigger()

    let timer = new Timer((fun _ -> writePending ()), null, Timeout.Infinite, Timeout.Infinite)

    member _.Load() =
        lock gate (fun () ->
            let state =
                try
                    if File.Exists path then
                        // (A file saved while the app's own generator was called OSM FG has its search under the
                        // old name: read as the new one, so that what was picked is kept.)
                        File.ReadAllText path
                        |> fun json -> json.Replace("\"OsmSearch\"", "\"MsmSearch\"")
                        |> fun json -> JsonSerializer.Deserialize<Preferences>(json) |> Preferences.normalize
                    else Preferences.defaults
                with
                | :? IOException | :? UnauthorizedAccessException | :? JsonException -> Preferences.defaults
            lastSaved <- Some state
            state)

    member _.Schedule(state) =
        let changed = lock gate (fun () ->
            let previousError = saveError
            if not disposed then
                if lastSaved <> Some state then
                    pending <- Some state
                    timer.Change(400, Timeout.Infinite) |> ignore
                else
                    pending <- None
                    saveError <- ""
                    timer.Change(Timeout.Infinite, Timeout.Infinite) |> ignore
            previousError <> saveError)
        if changed then statusChanged.Trigger()

    member _.SaveError = lock gate (fun () -> saveError)
    member _.StatusChanged = statusChanged.Publish
    member _.Flush() = writePending ()
    member _.Cancel() =
        lock gate (fun () ->
            disposed <- true
            pending <- None
            timer.Change(Timeout.Infinite, Timeout.Infinite) |> ignore)

    interface IDisposable with
        member _.Dispose() =
            lock gate (fun () -> disposed <- true; timer.Dispose())
            writePending ()

    static member DefaultPath =
        AppData.path [| "preferences.json" |]
