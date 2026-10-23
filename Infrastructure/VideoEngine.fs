namespace ARMO.Infrastructure

open System
open System.Diagnostics
open System.Globalization
open System.IO
open System.Text

/// One video for Super Video (round 47): what the engine is told (Engine/sm_video.inl).
type VideoJob =
    { Input: string
      Output: string
      Multiplier: int     // 1 none, 2..6
      Height: int         // 0 the video's own
      Codec: int          // 0 H.264, 1 HEVC
      Upscaler: int       // the engine's upscaler kinds (0 Lanczos, 1 sharpened, 2 integer, 3 nearest, 4 Anime4K)
      Search: int         // SMFG's search: 0 in full, 1 the lighter one
      Ai: bool }          // round 52: SMFG AI - the frames made by the neural network (msm_ai.h)

type VideoEvent =
    | Progress of made: int64 * total: int64
    | Finished
    | Stopped
    | Failed of string

/// The engine making one video (nvngx.dll --video <job file>): its lines turned into events, "stop" on its stdin.
type VideoRun(job: VideoJob, report: VideoEvent -> unit) =
    let gate = obj ()
    let mutable proc: Process = null
    let mutable ended = false
    let mutable error = ""
    let finish event =
        let first = lock gate (fun () -> if ended then false else (ended <- true; true))
        if first then report event

    let enginePath () =
        match EngineStore.path () with
        | Some path -> path
        | None -> Path.Combine(AppContext.BaseDirectory, "Engine", "nvngx.dll")

    let onLine (line: string) =
        let parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
        if parts.Length >= 2 && parts[0] = "@video" then
            match parts[1] with
            | "done" -> finish Finished
            | "stopped" -> finish Stopped
            | _ when parts.Length >= 3 ->
                match Int64.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture),
                      Int64.TryParse(parts[2], NumberStyles.Integer, CultureInfo.InvariantCulture) with
                | (true, a), (true, b) -> report (Progress(a, b))
                | _ -> ()
            | _ -> ()
        elif line.StartsWith("@error video ", StringComparison.Ordinal) then
            lock gate (fun () -> error <- line.Substring("@error video ".Length))

    /// False when it could not be started (the engine missing, the file not written).
    member _.Start() =
        let engine = enginePath ()
        if not (File.Exists engine) then
            finish (Failed "the engine is missing")
            false
        else
            try
                let folder = AppData.path [| "Engine" |]
                Directory.CreateDirectory folder |> ignore
                let file = Path.Combine(folder, "video-job.cfg")
                let lines =
                    [ "video_input=" + job.Input
                      "video_output=" + job.Output
                      sprintf "video_multiplier=%d" job.Multiplier
                      sprintf "video_height=%d" job.Height
                      sprintf "video_codec=%d" job.Codec
                      sprintf "upscaler=%d" job.Upscaler
                      sprintf "msm_search=%d" job.Search
                      sprintf "msm_ai=%d" (if job.Ai then 1 else 0)
                      "log=1" ]
                File.WriteAllLines(file, lines, UTF8Encoding(false))
                let info =
                    ProcessStartInfo(
                        FileName = engine,
                        Arguments = sprintf "--video \"%s\"" file,
                        WorkingDirectory = Path.GetDirectoryName engine,
                        UseShellExecute = false,
                        CreateNoWindow = true,
                        RedirectStandardInput = true,
                        RedirectStandardOutput = true,
                        StandardOutputEncoding = Encoding.UTF8)
                let p = new Process(StartInfo = info, EnableRaisingEvents = true)
                p.OutputDataReceived.Add(fun args -> if not (isNull args.Data) then onLine args.Data)
                p.Exited.Add(fun _ ->
                    let code = try p.ExitCode with _ -> -1
                    let why = lock gate (fun () -> error)
                    if why <> "" then finish (Failed why)
                    elif code <> 0 then finish (Failed(sprintf "the engine stopped (%d)" code))
                    else finish Finished)
                lock gate (fun () -> proc <- p)
                if not (p.Start()) then failwith "not started"
                p.BeginOutputReadLine()
                true
            with e ->
                finish (Failed e.Message)
                false

    /// The video ends where it is (the file is closed as a whole one).
    member _.Stop() =
        let p = lock gate (fun () -> proc)
        if not (isNull p) then
            try
                p.StandardInput.WriteLine "stop"
                p.StandardInput.Flush()
            with _ -> ()

    interface IDisposable with
        member _.Dispose() =
            let p = lock gate (fun () -> proc)
            if not (isNull p) then
                // (The app closing mid-video: the engine is asked to close the file whole first.)
                try
                    if not p.HasExited then
                        p.StandardInput.WriteLine "stop"
                        p.StandardInput.Flush()
                        if not (p.WaitForExit 5000) then p.Kill()
                with _ -> ()
                p.Dispose()
