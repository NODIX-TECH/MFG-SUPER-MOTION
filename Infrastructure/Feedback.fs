namespace ARMO.Infrastructure

open System
open System.IO
open System.Net.Http
open System.Net.Http.Headers
open System.Reflection
open System.Text
open System.Text.Json

/// A message to the developer (round 47, Help): the person's own words, the app's and Windows' versions, and - only
/// when they tick it - the engine's log, posted once to the developer's Discord channel. Nothing is ever sent but by
/// the Send button.
module Feedback =
    // The developer's Discord webhook. (MFGSM_FEEDBACK_URL: another address, for tests.)
    let private webhook =
        match Environment.GetEnvironmentVariable "MFGSM_FEEDBACK_URL" with
        | null | "" -> "https://discord.com/api/webhooks/1561693378679279657/1NSvdiOszJgmNry_P4_W7QDQobL1zrMMdC2fJ2a2TFIl06naZrUqXLwyaaLMjyIMINjj"
        | other -> other

    // Discord takes 2000 characters of text and files of a few megabytes: the message is cut to fit, and of a long
    // log its last part is sent (what happened last is what was wrong).
    let private mostText = 1900
    let private mostLog = 4L * 1024L * 1024L     // each; two of them stay under the 10 MB Discord takes

    let private version () =
        let v = Assembly.GetEntryAssembly() |> Option.ofObj |> Option.map (fun a -> a.GetName().Version)
        match v with
        | Some v when not (isNull v) -> sprintf "%d.%d.%d" v.Major v.Minor v.Build
        | _ -> "?"

    /// What goes with the message besides the person's words (shown on the page as it is).
    let about () = sprintf "MFG SUPER MOTION %s · Windows %s" (version ()) (Environment.OSVersion.Version.ToString())

    let private tail (path: string) =
        use file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite ||| FileShare.Delete)
        let take = min file.Length mostLog
        file.Seek(file.Length - take, SeekOrigin.Begin) |> ignore
        let bytes = Array.zeroCreate<byte> (int take)
        let mutable read = 0
        while read < bytes.Length do
            let n = file.Read(bytes, read, bytes.Length - read)
            read <- if n <= 0 then bytes.Length else read + n
        bytes

    /// Posted once. Ok, or Error with what went wrong (no connection, Discord said no).
    let send (message: string) (logs: string list) : Async<Result<unit, string>> =
        async {
            try
                use client = new HttpClient(Timeout = TimeSpan.FromSeconds 30.0)
                use form = new MultipartFormDataContent()
                let words = if isNull message then "" else message.Trim()
                let text =
                    let all = sprintf "%s\n%s" (about ()) words
                    if all.Length > mostText then all.Substring(0, mostText) + "…" else all
                let payload = JsonSerializer.Serialize({| username = "MFG SUPER MOTION"; content = text |})
                form.Add(new StringContent(payload, Encoding.UTF8, "application/json"), "payload_json")
                logs
                |> List.filter File.Exists
                |> List.iteri (fun i path ->
                    let file = new ByteArrayContent(tail path)
                    file.Headers.ContentType <- MediaTypeHeaderValue("text/plain")
                    form.Add(file, sprintf "files[%d]" i, Path.GetFileName path))
                let! answer = client.PostAsync(webhook, form) |> Async.AwaitTask
                if answer.IsSuccessStatusCode then return Ok ()
                else return Error(sprintf "%d %s" (int answer.StatusCode) answer.ReasonPhrase)
            with e -> return Error e.Message
        }
