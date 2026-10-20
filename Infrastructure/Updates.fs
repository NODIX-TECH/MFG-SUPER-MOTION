namespace ARMO.Infrastructure

open System
open System.IO
open System.IO.Compression
open System.Net.Http
open System.Reflection
open System.Security.Cryptography
open System.Text.Json
open System.Threading

/// A release on GitHub: its version (the tag, "1.0.0" or "v1.0.0"), its page, and - for the engine - the zip
/// file attached to it.
type Release =
    { Version: Version
      Page: string
      Asset: string        // where the zip is downloaded from ("" when the release has none)
      Size: int64
      Sha256: string }     // of the zip, when GitHub says it ("" otherwise)

/// What is published where. The app: https://github.com/NODIX-TECH/MFG-SUPER-MOTION/releases - only looked at,
/// the user is sent to the app's site (https://www.mfgsm.com/) to download it. The engine:
/// https://github.com/olivo-software/SUPER-MOTION/releases - the newest release's zip (Engine\pack: nvngx.dll and what lies beside it) is downloaded and unpacked.
module Releases =
    let appRepository = "NODIX-TECH/MFG-SUPER-MOTION"
    let engineRepository = "olivo-software/SUPER-MOTION"
    let appPage = "https://www.mfgsm.com/"

    // (MSM_RELEASES_API: another address to ask instead of GitHub's - the tests' own server.)
    let private testApi =
        match Environment.GetEnvironmentVariable "MSM_RELEASES_API" with
        | null | "" -> None
        | other -> Some(other.TrimEnd('/'))

    /// How much of the file being downloaded is here, and how large it is (bytes; 0: not known).
    let mutable downloaded = 0L
    let mutable downloadSize = 0L

    // (Half a minute for an answer's beginning: a weak connection is slow, not gone.)
    let private client =
        lazy (
            // (A connection that is not made in eight seconds is tried again rather than waited for: on a weak
            // line one address of several answers and another does not.)
            let handler = new SocketsHttpHandler(ConnectTimeout = TimeSpan.FromSeconds 8., AutomaticDecompression = Net.DecompressionMethods.All)
            let client = new HttpClient(handler, Timeout = TimeSpan.FromSeconds 30.)
            client.DefaultRequestHeaders.UserAgent.ParseAdd "MFG-SUPER-MOTION"
            client.DefaultRequestHeaders.Accept.ParseAdd "application/vnd.github+json"
            client)

    /// "1.0.0", "v1.2", "engine-1.0.3": the numbers with dots in it, as a version.
    let parseVersion (tag: string) =
        if isNull tag then None
        else
            let digits = tag |> Seq.skipWhile (fun c -> not (Char.IsDigit c)) |> Seq.takeWhile (fun c -> Char.IsDigit c || c = '.') |> Seq.toArray |> String
            let parts = digits.Trim('.').Split('.', StringSplitOptions.RemoveEmptyEntries)
            if parts.Length = 0 || parts.Length > 4 then None
            else
                let padded = Array.append parts (Array.create (max 0 (3 - parts.Length)) "0")
                match Version.TryParse(String.Join(".", padded)) with
                | true, version -> Some version
                | _ -> None

    /// This program's own version (ARMO.fsproj, Version).
    let appVersion =
        let version = Assembly.GetExecutingAssembly().GetName().Version
        if isNull version then Version(0, 0, 0) else Version(version.Major, version.Minor, max 0 version.Build)

    // A page asked for, up to five times: a try that fails or says nothing for fifteen seconds is followed by
    // the next one.
    let private ask (address: string) (cancel: CancellationToken) =
        task {
            let mutable answer : HttpResponseMessage = null
            let mutable tries = 0
            while isNull answer && tries < 5 do
                tries <- tries + 1
                let! got =
                    task {
                        try
                            use limit = CancellationTokenSource.CreateLinkedTokenSource cancel
                            limit.CancelAfter(TimeSpan.FromSeconds 15.)
                            let! response = client.Value.GetAsync(address, limit.Token)
                            // (The answer's text is read here, within the same limit.)
                            do! response.Content.LoadIntoBufferAsync()
                            return response
                        with _ -> return null
                    }
                answer <- got
                if isNull answer && tries < 5 then do! Threading.Tasks.Task.Delay(700, cancel)
            return answer
        }

    // The newest release as GitHub's own pages say it. (Not its API: api.github.com cannot be reached from every
    // network - the author's own, for one - where github.com itself can.) ".../releases/latest" leads on to
    // ".../releases/tag/<tag>"; the list of that release's files names the zip and its SHA-256.
    let private latestFromSite (repository: string) (cancel: CancellationToken) =
        task {
            try
                use! response = ask (sprintf "https://github.com/%s/releases/latest" repository) cancel
                if isNull response then return None else
                let landed = if isNull response.RequestMessage || isNull response.RequestMessage.RequestUri then "" else response.RequestMessage.RequestUri.AbsoluteUri
                let mark = "/releases/tag/"
                let at = landed.IndexOf(mark, StringComparison.OrdinalIgnoreCase)
                if not response.IsSuccessStatusCode || at < 0 then return None
                else
                    let tag = Uri.UnescapeDataString(landed.Substring(at + mark.Length).Split('?', '#').[0])
                    match parseVersion tag with
                    | None -> return None
                    | Some version ->
                        let! files =
                            task {
                                use! page = ask (sprintf "https://github.com/%s/releases/expanded_assets/%s" repository (Uri.EscapeDataString tag)) cancel
                                if isNull page || not page.IsSuccessStatusCode then return ""
                                else return! page.Content.ReadAsStringAsync(cancel)
                            }
                        let link = Text.RegularExpressions.Regex.Match(files, "href=\"(/[^\"]+/releases/download/[^\"]+\\.zip)\"", Text.RegularExpressions.RegexOptions.IgnoreCase)
                        // (The list not to be had: the name the publish gives the zip.)
                        let address =
                            if link.Success then "https://github.com" + Net.WebUtility.HtmlDecode link.Groups.[1].Value
                            elif String.Equals(repository, engineRepository, StringComparison.OrdinalIgnoreCase) then
                                sprintf "https://github.com/%s/releases/download/%s/SUPER-MOTION-Engine-%s.zip" repository (Uri.EscapeDataString tag) (version.ToString())
                            else ""
                        let hash =
                            if link.Success then
                                let digest = Text.RegularExpressions.Regex.Match(files.Substring link.Index, "sha256:([0-9a-fA-F]{64})")
                                if digest.Success then digest.Groups.[1].Value else ""
                            else ""
                        return Some { Version = version; Page = landed; Asset = address; Size = 0L; Sha256 = hash }
            with _ -> return None
        }

    // The same from an API shaped like GitHub's (the tests' stand-in server).
    let private latestFromApi (api: string) (repository: string) (cancel: CancellationToken) =
        task {
            try
                use! response = client.Value.GetAsync(sprintf "%s/repos/%s/releases/latest" api repository, cancel)
                if not response.IsSuccessStatusCode then return None
                else
                    let! text = response.Content.ReadAsStringAsync(cancel)
                    use document = JsonDocument.Parse text
                    let root = document.RootElement
                    let text (element: JsonElement) (name: string) =
                        match element.TryGetProperty name with
                        | true, value when value.ValueKind = JsonValueKind.String -> value.GetString()
                        | _ -> ""
                    match parseVersion (text root "tag_name") with
                    | None -> return None
                    | Some version ->
                        // The zip: the first attached file whose name ends in .zip.
                        let asset =
                            match root.TryGetProperty "assets" with
                            | true, assets when assets.ValueKind = JsonValueKind.Array ->
                                assets.EnumerateArray()
                                |> Seq.tryFind (fun a -> (text a "name").EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
                            | _ -> None
                        let address, size, hash =
                            match asset with
                            | Some a ->
                                let size = match a.TryGetProperty "size" with | true, s when s.ValueKind = JsonValueKind.Number -> s.GetInt64() | _ -> 0L
                                let digest = text a "digest"
                                text a "browser_download_url", size, (if digest.StartsWith "sha256:" then digest.Substring 7 else "")
                            | None -> "", 0L, ""
                        return Some { Version = version; Page = text root "html_url"; Asset = address; Size = size; Sha256 = hash }
            with _ -> return None
        }

    /// The newest release of a repository, or None: no connection, no release yet, an answer not understood.
    let latest (repository: string) (cancel: CancellationToken) =
        match testApi with
        | Some api -> latestFromApi api repository cancel
        | None -> latestFromSite repository cancel

    // One go at the file, from where `target` ends: true when all of it is there. A connection that says nothing
    // for forty seconds is given up (and taken up again by `download`).
    let private downloadPart (address: string) (target: string) (size: int64) (progress: float -> unit) (cancel: CancellationToken) =
        task {
            let have = if File.Exists target then FileInfo(target).Length else 0L
            use request = new HttpRequestMessage(HttpMethod.Get, address)
            if have > 0L then request.Headers.Range <- Net.Http.Headers.RangeHeaderValue(Nullable have, Nullable())
            use! response = client.Value.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel)
            if have > 0L && response.StatusCode = Net.HttpStatusCode.RequestedRangeNotSatisfiable then return true      // it was whole already
            else
                response.EnsureSuccessStatusCode() |> ignore
                let resumed = response.StatusCode = Net.HttpStatusCode.PartialContent
                let start = if resumed then have else 0L
                let rest = if response.Content.Headers.ContentLength.HasValue then response.Content.Headers.ContentLength.Value else 0L
                let total = if rest > 0L then start + rest else size
                downloadSize <- total
                use! from = response.Content.ReadAsStreamAsync(cancel)
                use into = new FileStream(target, (if resumed then FileMode.Append else FileMode.Create), FileAccess.Write, FileShare.None, 1 <<< 16, true)
                let buffer = Array.zeroCreate<byte> (1 <<< 16)
                let mutable got = start
                let mutable more = true
                while more do
                    use quiet = CancellationTokenSource.CreateLinkedTokenSource cancel
                    quiet.CancelAfter(TimeSpan.FromSeconds 40.)
                    let! read = from.ReadAsync(buffer.AsMemory(), quiet.Token)
                    if read = 0 then more <- false
                    else
                        do! into.WriteAsync(buffer.AsMemory(0, read), cancel)
                        got <- got + int64 read
                        downloaded <- got
                        if total > 0L then progress (min 1.0 (float got / float total))
                return total <= 0L || got >= total
        }

    /// A file downloaded to `target`, `progress` told how far along it is (0..1) on the way. A connection that
    /// breaks or stalls is taken up again from where the file ends - for as long as each go brings something,
    /// and six goes in a row that bring nothing are the end of it.
    let download (address: string) (target: string) (size: int64) (progress: float -> unit) (cancel: CancellationToken) =
        task {
            downloaded <- 0L
            downloadSize <- size
            let mutable whole = false
            let mutable empty = 0
            while not whole do
                let before = if File.Exists target then FileInfo(target).Length else 0L
                let! ended =
                    task {
                        try
                            let! all = downloadPart address target size progress cancel
                            return (if all then 1 else 0)
                        with
                        | :? OperationCanceledException when cancel.IsCancellationRequested -> return 2
                        | _ -> return 0
                    }
                if ended = 2 then raise (OperationCanceledException())
                elif ended = 1 then whole <- true
                else
                    let after = if File.Exists target then FileInfo(target).Length else 0L
                    empty <- if after > before then 0 else empty + 1
                    if empty >= 6 then failwith "the connection was lost"
                    do! Threading.Tasks.Task.Delay(TimeSpan.FromSeconds 3., cancel)
        }

/// Where the engine is: beside the program, in its Engine folder - the one a build puts there (Visual Studio:
/// used as it is, nothing downloaded) or the one the published app downloads for itself, which says its version
/// in engine-version.txt and is replaced by a newer one without touching the app. Only where the program's own
/// folder cannot be written to (installed for all users under Program Files) the downloaded engine goes to the
/// app's data folder instead.
module EngineStore =
    let private beside = Path.Combine(AppContext.BaseDirectory, "Engine")
    let private root = AppData.path [||]
    let private elsewhere = Path.Combine(root, "EngineFiles")
    let private writable =
        lazy (
            try
                let probe = Path.Combine(AppContext.BaseDirectory, sprintf "write-test-%d.tmp" Environment.ProcessId)
                File.WriteAllText(probe, "")
                File.Delete probe
                true
            with _ -> false)
    /// Where a downloaded engine is put.
    let folder = if writable.Value then beside else elsewhere
    let private versionName = "engine-version.txt"
    let private versionFile = Path.Combine(folder, versionName)
    let private has (where: string) = File.Exists(Path.Combine(where, "nvngx.dll"))

    /// The engine beside the program is a build's own (it says no version): it is the one, and it is not
    /// updated from the net.
    let isLocal () = has beside && not (File.Exists(Path.Combine(beside, versionName)))
    /// The engine's program, or None: there is no engine yet.
    let path () =
        if has beside then Some(Path.Combine(beside, "nvngx.dll"))
        elif writable.Value then
            // (An engine an earlier version downloaded into the data folder is not used where the program's own
            // folder can hold one: it goes, and the engine is downloaded to where it belongs.)
            (try (if Directory.Exists elsewhere then Directory.Delete(elsewhere, true)) with _ -> ())
            None
        elif has elsewhere then Some(Path.Combine(elsewhere, "nvngx.dll"))
        else None
    /// The version of the downloaded engine (None: none, or one of unknown version).
    let installed () =
        try
            let file = if has beside then Path.Combine(beside, versionName) else Path.Combine(elsewhere, versionName)
            if File.Exists file then Releases.parseVersion (File.ReadAllText file) else None
        with _ -> None

    let private isRunning () =
        try Diagnostics.Process.GetProcesses() |> Array.exists (fun p -> try String.Equals(p.ProcessName, "nvngx", StringComparison.OrdinalIgnoreCase) || String.Equals(p.ProcessName, "nvngx.dll", StringComparison.OrdinalIgnoreCase) with _ -> false)
        with _ -> false

    // A zip unpacked into a folder. False: it is not a whole zip.
    let private unpack (zip: string) (into: string) =
        try
            ZipFile.ExtractToDirectory(zip, into, true)
            true
        with _ -> false

    // The unpacked engine put where the engine is; what was there set aside first, and put back if that fails.
    let private place (from: string) (old: string) =
        if Directory.Exists folder then Directory.Move(folder, old)
        try
            Directory.Move(from, folder)
        with _ ->
            if Directory.Exists old && not (Directory.Exists folder) then Directory.Move(old, folder)
            reraise ()

    /// A release's zip downloaded, checked, unpacked and put in the engine's place. Error: what went wrong, in
    /// a word for the log; the engine that was there is still there.
    let install (release: Release) (progress: float -> unit) (cancel: CancellationToken) =
        task {
            // (The zip is kept by its version while it is not whole: a download that broke off goes on from
            // where it ended the next time, also after the app was closed.)
            let zip = Path.Combine(root, sprintf "engine-%s.zip.part" (release.Version.ToString()))
            let fresh = folder + ".new"
            let old = folder + ".old"
            let dropZip () = try (if File.Exists zip then File.Delete zip) with _ -> ()
            let clean () =
                try (if Directory.Exists fresh then Directory.Delete(fresh, true)) with _ -> ()
                try (if Directory.Exists old then Directory.Delete(old, true)) with _ -> ()
            try
                if String.IsNullOrEmpty release.Asset then return Error "the release has no zip file"
                elif isRunning () then return Error "the engine is running"
                else
                    Directory.CreateDirectory root |> ignore
                    clean ()
                    do! Releases.download release.Asset zip release.Size (fun part -> progress (part * 0.9)) cancel
                    let hashOk =
                        String.IsNullOrEmpty release.Sha256 ||
                        (use stream = File.OpenRead zip
                         String.Equals(Convert.ToHexString(SHA256.HashData stream), release.Sha256, StringComparison.OrdinalIgnoreCase))
                    if not hashOk then
                        dropZip ()
                        clean ()
                        return Error "the downloaded file is not the one that was published"
                    else
                        let unpacked = unpack zip fresh
                        progress 0.97
                        // The engine may lie at the zip's top or in a folder of it: where nvngx.dll is, nearest the top.
                        let found =
                            (if unpacked && Directory.Exists fresh then Directory.GetFiles(fresh, "nvngx.dll", SearchOption.AllDirectories) else [||])
                            |> Array.sortBy (fun file -> file.Length)
                            |> Array.tryHead
                        match found with
                        | None ->
                            dropZip ()
                            clean ()
                            return Error "the zip holds no engine (nvngx.dll)"
                        | Some program ->
                            place (Path.GetDirectoryName program) old
                            File.WriteAllText(versionFile, release.Version.ToString())
                            dropZip ()
                            clean ()
                            progress 1.0
                            return Ok()
            with
            | :? OperationCanceledException -> clean (); return Error "cancelled"
            | error -> clean (); return Error error.Message
        }
