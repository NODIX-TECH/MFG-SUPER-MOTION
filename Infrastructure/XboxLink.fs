namespace ARMO.Infrastructure

open System
open System.Collections.Concurrent
open System.Diagnostics
open System.IO
open System.Net
open System.Net.Http
open System.Net.WebSockets
open System.Runtime.InteropServices
open System.Security.Cryptography
open System.Text
open System.Text.Json
open System.Text.Json.Nodes
open System.Threading
open System.Threading.Tasks

/// Console Link - Xbox (2026-11-16, the owner: "start the Xbox version"; no Xbox here to try it on). An Xbox Series S / X
/// (or One) is played the way the Xbox app's "remote play" does it - xHome: the account signed in with Microsoft, the
/// consoles of that account listed by Xbox's streaming service (gssv), a session asked there and the picture and sound
/// over WebRTC straight from the console on the home network. The WebRTC side is xbox-xcloud-player (MIT,
/// Console\xbox\xCloudPlayer.min.js) in a window of Microsoft Edge (every Windows 10 / 11 has it: nothing to install);
/// this app is its server: the page, and the service's calls with the account's token added. Frame generation goes on
/// that window like on the PlayStation's.
type EnumWindowsProc = delegate of nativeint * nativeint -> bool

type XboxConsole =
    { ServerId: string
      Name: string
      /// XboxSeriesX, XboxSeriesS, XboxOne...
      ConsoleType: string
      /// On, ConnectedStandby, Off...
      Power: string
      /// The service's own word: the console seems to be on another network.
      OutOfHome: bool }

/// The account's streaming token for xHome and where its service answers.
type XboxStreamToken = { Token: string; BaseUri: string; Until: DateTime }

/// Windows' own protection of the saved sign-in (the user's Windows account only can read it back).
module private Protect =
    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type Blob = { mutable Size: int; mutable Data: nativeint }

    [<DllImport("crypt32.dll", SetLastError = true)>]
    extern bool CryptProtectData(Blob& dataIn, string description, nativeint entropy, nativeint reserved, nativeint prompt, int flags, Blob& dataOut)

    [<DllImport("crypt32.dll", SetLastError = true)>]
    extern bool CryptUnprotectData(Blob& dataIn, nativeint description, nativeint entropy, nativeint reserved, nativeint prompt, int flags, Blob& dataOut)

    [<DllImport("kernel32.dll")>]
    extern nativeint LocalFree(nativeint mem)

    let private run (protect: bool) (bytes: byte[]) =
        let handle = GCHandle.Alloc(bytes, GCHandleType.Pinned)
        try
            let mutable input = { Size = bytes.Length; Data = handle.AddrOfPinnedObject() }
            let mutable output = { Size = 0; Data = 0n }
            let ok =
                if protect then CryptProtectData(&input, "MFG SUPER MOTION Xbox", 0n, 0n, 0n, 1, &output)
                else CryptUnprotectData(&input, 0n, 0n, 0n, 0n, 1, &output)
            if not ok then None
            else
                let result = Array.zeroCreate<byte> output.Size
                Marshal.Copy(output.Data, result, 0, output.Size)
                LocalFree output.Data |> ignore
                Some result
        finally handle.Free()

    let seal bytes = run true bytes
    let unseal bytes = run false bytes

/// Microsoft Edge: the Xbox stream's window, and the sign-in's.
module Edge =
    let path =
        let roots = [ Environment.GetFolderPath Environment.SpecialFolder.ProgramFilesX86; Environment.GetFolderPath Environment.SpecialFolder.ProgramFiles
                      Path.Combine(Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData) ]
        roots
        |> List.filter (fun r -> not (String.IsNullOrEmpty r))
        |> List.map (fun r -> Path.Combine(r, "Microsoft", "Edge", "Application", "msedge.exe"))
        |> List.tryFind File.Exists
    let isThere () = path.IsSome
    /// Its own profile beside the app's data: none of the user's Edge (tabs, sign-ins, extensions) is touched or used.
    let profile (name: string) =
        let dir = Path.Combine(Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData, "MFG SUPER MOTION", "Console", name)
        Directory.CreateDirectory dir |> ignore
        dir
    let launch (profileName: string) (extra: string list) (url: string) =
        match path with
        | None -> None
        | Some exe ->
            let info = ProcessStartInfo(exe, UseShellExecute = false)
            for a in [ "--user-data-dir=" + profile profileName; "--no-first-run"; "--no-default-browser-check"; "--disable-sync"
                       "--disable-features=msEdgeSidebarV2,msHubApps,msImplicitSignin,EdgeCollections"; "--autoplay-policy=no-user-gesture-required" ] @ extra @ [ "--app=" + url ] do
                info.ArgumentList.Add a
            try Some(Process.Start info) with _ -> None

module XboxAuth =
    let private appId = "000000004c20a908"
    let private titleId = "328178078"
    let redirectPrefix = "ms-xal-000000004c20a908://auth"
    let private savedPath = Path.Combine(Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData, "MFG SUPER MOTION", "Console", "xbox.bin")
    let private http = new HttpClient(Timeout = TimeSpan.FromSeconds 20.0)
    let private b64url (bytes: byte[]) = Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')

    /// What is kept between runs: the device's key (Xbox's sign-in binds the tokens to it), the Microsoft account's
    /// refresh token, and the gamertag to show. Sealed with Windows' protection (DPAPI), never in the settings file.
    type Saved = { Key: string; Refresh: string; Gamertag: string }
    let mutable private saved: Saved option = None
    let private load () =
        match saved with
        | Some s -> Some s
        | None ->
            try
                if File.Exists savedPath then
                    match Protect.unseal (File.ReadAllBytes savedPath) with
                    | Some bytes ->
                        use doc = JsonDocument.Parse(bytes: byte[])
                        let r = doc.RootElement
                        let s = { Key = r.GetProperty("key").GetString(); Refresh = r.GetProperty("refresh").GetString(); Gamertag = r.GetProperty("gamertag").GetString() }
                        saved <- Some s
                        saved
                    | None -> None
                else None
            with _ -> None
    let private store (s: Saved) =
        saved <- Some s
        try
            Directory.CreateDirectory(Path.GetDirectoryName savedPath) |> ignore
            let json = JsonSerializer.SerializeToUtf8Bytes(dict [ "key", s.Key; "refresh", s.Refresh; "gamertag", s.Gamertag ])
            match Protect.seal json with
            | Some sealedBytes -> File.WriteAllBytes(savedPath, sealedBytes)
            | None -> ()
        with _ -> ()
    let gamertag () = match load () with Some s -> s.Gamertag | None -> ""
    let isSignedIn () = match load () with Some s -> s.Refresh <> "" | None -> false
    let signOut () =
        saved <- None
        try if File.Exists savedPath then File.Delete savedPath with _ -> ()

    // The device key: P-256, made once and kept with the sign-in.
    let mutable private key: ECDsa = null
    let private deviceKey () =
        if isNull key then
            let k = ECDsa.Create(ECCurve.NamedCurves.nistP256)
            match load () with
            | Some s when s.Key <> "" ->
                (try
                    let mutable read = 0
                    k.ImportPkcs8PrivateKey(ReadOnlySpan<byte>(Convert.FromBase64String s.Key), &read)
                 with _ -> ())
            | _ -> ()
            key <- k
        key
    let private keyText () = Convert.ToBase64String((deviceKey ()).ExportPkcs8PrivateKey())
    let private proofKey () =
        let p = (deviceKey ()).ExportParameters(false)
        JsonObject(dict [ "use", JsonValue.Create("sig") :> JsonNode; "alg", JsonValue.Create("ES256"); "kty", JsonValue.Create("EC"); "crv", JsonValue.Create("P-256")
                          "x", JsonValue.Create(b64url p.Q.X); "y", JsonValue.Create(b64url p.Q.Y) ])

    /// Xbox's request signature: version 1, the time (Windows' 100-ns ticks since 1601) and an ES256 signature over
    /// version, time, method, path, authorization and body - each followed by a zero byte.
    let sign (url: string) (authorization: string) (body: string) =
        let ticks = (DateTimeOffset.UtcNow.ToUnixTimeSeconds() + 11644473600L) * 10000000L
        let be32 (v: int) = [| byte (v >>> 24); byte (v >>> 16); byte (v >>> 8); byte v |]
        let be64 (v: int64) = Array.init 8 (fun i -> byte (v >>> (56 - 8 * i)))
        let path = Uri(url).AbsolutePath
        use data = new MemoryStream()
        let put (b: byte[]) = data.Write(b, 0, b.Length)
        put (be32 1); put [| 0uy |]; put (be64 ticks); put [| 0uy |]
        for part in [ "POST"; path; authorization; body ] do
            put (Encoding.UTF8.GetBytes part); put [| 0uy |]
        let signature = (deviceKey ()).SignData(data.ToArray(), HashAlgorithmName.SHA256)     // (IEEE P1363: r then s)
        Convert.ToBase64String(Array.concat [ be32 1; be64 ticks; signature ])

    let private postSigned (url: string) (body: JsonObject) =
        task {
            let text = body.ToJsonString()
            use request = new HttpRequestMessage(HttpMethod.Post, url)
            request.Headers.Add("x-xbl-contract-version", "1")
            request.Headers.Add("Cache-Control", "no-store, must-revalidate, no-cache")
            request.Headers.Add("Signature", sign url "" text)
            request.Content <- new StringContent(text, Encoding.UTF8, "application/json")
            let! response = http.SendAsync request
            let! answer = response.Content.ReadAsStringAsync()
            if not response.IsSuccessStatusCode then
                failwithf "%s: %d %s" (Uri(url).Host) (int response.StatusCode) (if answer.Length > 200 then answer.Substring(0, 200) else answer)
            let session = match response.Headers.TryGetValues "X-SessionId" with | true, v -> Seq.head v | _ -> ""
            return JsonNode.Parse(if answer = "" then "{}" else answer), session
        }
    let private postForm (url: string) (fields: (string * string) list) =
        task {
            use content = new FormUrlEncodedContent(fields |> List.map (fun (k, v) -> Collections.Generic.KeyValuePair(k, v)))
            let! response = http.PostAsync(url, content)
            let! answer = response.Content.ReadAsStringAsync()
            if not response.IsSuccessStatusCode then
                failwithf "%s: %d %s" (Uri(url).Host) (int response.StatusCode) (if answer.Length > 200 then answer.Substring(0, 200) else answer)
            return JsonNode.Parse answer
        }
    let private str (node: JsonNode) (path: string list) =
        let mutable n = node
        for p in path do
            if not (isNull n) then
                n <- (match Int32.TryParse p with
                      | true, i -> (match n with :? JsonArray as a when i < a.Count -> a[i] | _ -> null)
                      | _ -> n[p])
        if isNull n then "" else n.ToString()

    /// The device's token (a new one each time - it is cheap, and bound to the key).
    let deviceToken () =
        task {
            let id () = "{" + Guid.NewGuid().ToString().ToUpperInvariant() + "}"
            let body =
                JsonObject(dict [ "Properties", JsonObject(dict [ "AuthMethod", JsonValue.Create("ProofOfPossession") :> JsonNode; "Id", JsonValue.Create(id ())
                                                                  "DeviceType", JsonValue.Create("Android"); "SerialNumber", JsonValue.Create(id ())
                                                                  "Version", JsonValue.Create("15.0"); "ProofKey", proofKey () ]) :> JsonNode
                                  "RelyingParty", JsonValue.Create("http://auth.xboxlive.com"); "TokenType", JsonValue.Create("JWT") ])
            let! (answer, _) = postSigned "https://device.auth.xboxlive.com/device/authenticate" body
            return str answer [ "Token" ]
        }

    /// A sign-in begun: Microsoft's page to open, and what the answer is checked against.
    type Begun = { Url: string; State: string; Verifier: string; Session: string }
    let start () =
        task {
            let! device = deviceToken ()
            let verifier = b64url (RandomNumberGenerator.GetBytes 32)
            let challenge = b64url (SHA256.HashData(Encoding.ASCII.GetBytes verifier))
            let state = b64url (RandomNumberGenerator.GetBytes 64)
            let body =
                JsonObject(dict [ "AppId", JsonValue.Create(appId) :> JsonNode; "TitleId", JsonValue.Create(titleId); "RedirectUri", JsonValue.Create(redirectPrefix)
                                  "DeviceToken", JsonValue.Create(device); "Sandbox", JsonValue.Create("RETAIL"); "TokenType", JsonValue.Create("code")
                                  "Offers", JsonArray(JsonValue.Create("service::user.auth.xboxlive.com::MBI_SSL"))
                                  "Query", JsonObject(dict [ "display", JsonValue.Create("android_phone") :> JsonNode; "code_challenge", JsonValue.Create(challenge)
                                                             "code_challenge_method", JsonValue.Create("S256"); "state", JsonValue.Create(state) ]) ])
            let! (answer, session) = postSigned "https://sisu.xboxlive.com/authenticate" body
            return { Url = str answer [ "MsaOauthRedirect" ]; State = state; Verifier = verifier; Session = session }
        }

    let private sisuAuthorize (accessToken: string) (session: string) =
        task {
            let! device = deviceToken ()
            let body =
                JsonObject(dict [ "AccessToken", JsonValue.Create("t=" + accessToken) :> JsonNode; "AppId", JsonValue.Create(appId); "DeviceToken", JsonValue.Create(device)
                                  "Sandbox", JsonValue.Create("RETAIL"); "SiteName", JsonValue.Create("user.auth.xboxlive.com"); "UseModernGamertag", JsonValue.Create(true)
                                  "ProofKey", proofKey () ])
            if session <> "" then body["SessionId"] <- JsonValue.Create(session)
            let! (answer, _) = postSigned "https://sisu.xboxlive.com/authorize" body
            return answer
        }

    /// The page's answer (the address Microsoft sent the browser to) turned into the account's tokens - kept.
    let finish (begun: Begun) (answerUrl: string) =
        task {
            let query = Uri(answerUrl.Trim()).Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries)
                        |> Array.choose (fun kv -> match kv.Split('=', 2) with [| k; v |] -> Some(k, Uri.UnescapeDataString v) | _ -> None) |> dict
            let get k = match query.TryGetValue k with | true, v -> v | _ -> ""
            if get "error" <> "" then failwith (if get "error_description" <> "" then get "error_description" else get "error")
            if get "code" = "" then failwith "no code"
            if get "state" <> begun.State then failwith "state"
            let! user =
                postForm "https://login.live.com/oauth20_token.srf"
                    [ "client_id", appId; "code", get "code"; "code_verifier", begun.Verifier; "grant_type", "authorization_code"
                      "redirect_uri", redirectPrefix; "scope", "service::user.auth.xboxlive.com::MBI_SSL" ]
            let! sisu = sisuAuthorize (str user [ "access_token" ]) begun.Session
            let tag = str sisu [ "AuthorizationToken"; "DisplayClaims"; "xui"; "0"; "gtg" ]
            store { Key = keyText (); Refresh = str user [ "refresh_token" ]; Gamertag = tag }
            return tag
        }

    let mutable private stream: XboxStreamToken option = None
    /// xHome's streaming token: the account's refresh token renewed (and kept), SISU, XSTS for gssv, then gssv's login.
    let streamToken () =
        task {
            match stream with
            | Some s when s.Until > DateTime.UtcNow.AddMinutes 2.0 -> return s
            | _ ->
                let s = match load () with Some s when s.Refresh <> "" -> s | _ -> failwith "not signed in"
                let! user =
                    postForm "https://login.live.com/oauth20_token.srf"
                        [ "client_id", appId; "grant_type", "refresh_token"; "refresh_token", s.Refresh; "scope", "service::user.auth.xboxlive.com::MBI_SSL" ]
                let refresh = str user [ "refresh_token" ]
                let! sisu = sisuAuthorize (str user [ "access_token" ]) ""
                let tag = str sisu [ "AuthorizationToken"; "DisplayClaims"; "xui"; "0"; "gtg" ]
                store { s with Key = keyText (); Refresh = (if refresh <> "" then refresh else s.Refresh); Gamertag = (if tag <> "" then tag else s.Gamertag) }
                let xsts =
                    JsonObject(dict [ "Properties", JsonObject(dict [ "SandboxId", JsonValue.Create("RETAIL") :> JsonNode; "DeviceToken", JsonValue.Create(str sisu [ "DeviceToken" ])
                                                                      "TitleToken", JsonValue.Create(str sisu [ "TitleToken"; "Token" ])
                                                                      "UserTokens", JsonArray(JsonValue.Create(str sisu [ "UserToken"; "Token" ])) ]) :> JsonNode
                                      "RelyingParty", JsonValue.Create("http://gssv.xboxlive.com/"); "TokenType", JsonValue.Create("JWT") ])
                let! (gssv, _) = postSigned "https://xsts.auth.xboxlive.com/xsts/authorize" xsts
                let body = JsonObject(dict [ "token", JsonValue.Create(str gssv [ "Token" ]) :> JsonNode; "offeringId", JsonValue.Create("xhome") ]).ToJsonString()
                use request = new HttpRequestMessage(HttpMethod.Post, "https://xhome.gssv-play-prod.xboxlive.com/v2/login/user")
                request.Headers.Add("x-gssv-client", "XboxComBrowser")
                request.Headers.Add("Cache-Control", "no-store, must-revalidate, no-cache")
                request.Content <- new StringContent(body, Encoding.UTF8, "application/json")
                let! response = http.SendAsync request
                let! answer = response.Content.ReadAsStringAsync()
                if not response.IsSuccessStatusCode then failwithf "gssv: %d" (int response.StatusCode)
                let node = JsonNode.Parse answer
                let regions = match node["offeringSettings"] with null -> null | o -> (match o["regions"] with :? JsonArray as a -> a | _ -> null)
                let region =
                    if isNull regions || regions.Count = 0 then null
                    else regions |> Seq.tryFind (fun r -> not (isNull r) && str r [ "isDefault" ] = "true") |> Option.defaultValue regions[0]
                let baseUri = if isNull region then "https://uks.gssv-play-prodxhome.xboxlive.com" else (str region [ "baseUri" ]).TrimEnd('/')
                let seconds = match Int32.TryParse(str node [ "durationInSeconds" ]) with | true, v -> v | _ -> 3600
                let t = { Token = str node [ "gsToken" ]; BaseUri = baseUri; Until = DateTime.UtcNow.AddSeconds(float seconds) }
                stream <- Some t
                return t
        }

    /// The account's consoles, as the streaming service lists them.
    let consoles () =
        task {
            let! t = streamToken ()
            use request = new HttpRequestMessage(HttpMethod.Get, t.BaseUri + "/v6/servers/home")
            request.Headers.Authorization <- Headers.AuthenticationHeaderValue("Bearer", t.Token)
            let! response = http.SendAsync request
            let! answer = response.Content.ReadAsStringAsync()
            if not response.IsSuccessStatusCode then failwithf "servers: %d" (int response.StatusCode)
            let node = JsonNode.Parse answer
            return
                match node["results"] with
                | :? JsonArray as list ->
                    [ for c in list do
                        if not (isNull c) then
                            yield { ServerId = str c [ "serverId" ]; Name = str c [ "deviceName" ]; ConsoleType = str c [ "consoleType" ]
                                    Power = str c [ "powerState" ]; OutOfHome = str c [ "outOfHomeWarning" ] = "true" } ]
                | _ -> []
        }

    let rec private scanFor (found: TaskCompletionSource<string>) (n: JsonElement) =
        match n.ValueKind with
        | JsonValueKind.String ->
            let s = n.GetString()
            if s.StartsWith(redirectPrefix, StringComparison.OrdinalIgnoreCase) then found.TrySetResult s |> ignore
        | JsonValueKind.Object -> for p in n.EnumerateObject() do scanFor found p.Value
        | JsonValueKind.Array -> for e in n.EnumerateArray() do scanFor found e
        | _ -> ()
    /// Watches every page of the sign-in window over DevTools for Microsoft's answer address.
    let private watchLogin (edge: Process) (port: int) (url: string) (until: DateTime) (cancel: CancellationToken) =
        task {
            let found = TaskCompletionSource<string>()
            let watched = ConcurrentDictionary<string, bool>()
            // (The window opens blank; the sign-in page is asked once the first page is watched - a remembered account
            // answers at once, before a watch started after the page would see it.)
            let mutable sent = 0
            let scan = scanFor found
            let watch (wsUrl: string) =
                task {
                    try
                        use ws = new ClientWebSocket()
                        do! ws.ConnectAsync(Uri wsUrl, cancel)
                        let send (text: string) = ws.SendAsync(ArraySegment(Encoding.UTF8.GetBytes text), WebSocketMessageType.Text, true, cancel)
                        do! send """{"id":1,"method":"Network.enable"}"""
                        do! send """{"id":2,"method":"Page.enable"}"""
                        if Interlocked.Exchange(&sent, 1) = 0 then
                            do! send (JsonObject(dict [ "id", JsonValue.Create(3) :> JsonNode; "method", JsonValue.Create("Page.navigate")
                                                        "params", JsonObject(dict [ "url", JsonValue.Create(url) :> JsonNode ]) ]).ToJsonString())
                        let buffer = Array.zeroCreate<byte> (1 <<< 16)
                        use message = new MemoryStream()
                        let mutable go = true
                        while go && ws.State = WebSocketState.Open && not found.Task.IsCompleted do
                            let! r = ws.ReceiveAsync(ArraySegment buffer, cancel)
                            if r.MessageType = WebSocketMessageType.Close then go <- false
                            else
                                message.Write(buffer, 0, r.Count)
                                if r.EndOfMessage then
                                    let bytes = message.ToArray()
                                    message.SetLength 0L
                                    if Encoding.UTF8.GetString(bytes).Contains("ms-xal-", StringComparison.OrdinalIgnoreCase) then
                                        try
                                            use doc = JsonDocument.Parse(bytes: byte[])
                                            scan doc.RootElement
                                        with _ -> ()
                    with _ -> ()
                }
            use list = new HttpClient(Timeout = TimeSpan.FromSeconds 3.0)
            let mutable result = None
            while result.IsNone do
                if found.Task.IsCompleted then result <- Some(Ok found.Task.Result)
                elif cancel.IsCancellationRequested then result <- Some(Error "cancelled")
                elif DateTime.UtcNow > until then result <- Some(Error "timeout")
                else
                    let! targets = task { try return! list.GetStringAsync(sprintf "http://127.0.0.1:%d/json/list" port) with _ -> return "" }
                    if targets = "" && edge.HasExited then result <- Some(Error "closed")
                    elif targets <> "" then
                        use doc = JsonDocument.Parse targets
                        let pages = [ for t in doc.RootElement.EnumerateArray() do
                                        if t.GetProperty("type").GetString() = "page" then
                                            match t.TryGetProperty "webSocketDebuggerUrl" with
                                            | true, w -> yield (t.GetProperty("id").GetString(), w.GetString())
                                            | _ -> () ]
                        if pages.IsEmpty && found.Task.IsCompleted |> not then result <- Some(Error "closed")
                        for (id, ws) in pages do
                            if watched.TryAdd(id, true) then watch ws |> ignore
                    if result.IsNone then
                        let! _ = Task.WhenAny(found.Task, Task.Delay 400)
                        ()
            return result.Value
        }

    /// Microsoft's sign-in in an Edge window of its own: its answer is an address Windows has no program for
    /// (ms-xal-...://auth?code=...), so it is read from the window itself, over Edge's DevTools connection (only on
    /// this window's own profile, closed with it). Ended by the answer, by the window closed, or after ten minutes.
    /// (url: Microsoft's page; tests give their own.)
    let private readPort (portFile: string) =
        try
            if File.Exists portFile then
                use r = new StreamReader(new FileStream(portFile, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                match Int32.TryParse(r.ReadLine()) with
                | true, p -> p
                | _ -> 0
            else 0
        with _ -> 0
    let private dropFile (file: string) = try (if File.Exists file then File.Delete file) with _ -> ()
    let private kill (p: Process) = try (if not p.HasExited then p.Kill true) with _ -> ()
    let signInWindowAt (url: string) (cancel: CancellationToken) =
        task {
            let portFile = Path.Combine(Edge.profile "edge-login", "DevToolsActivePort")
            dropFile portFile
            match Edge.launch "edge-login" [ "--remote-debugging-port=0"; "--window-size=520,760" ] "about:blank" with
            | None -> return Error "edge"
            | Some edge ->
                let started = DateTime.UtcNow
                let mutable port = 0
                while port = 0 && DateTime.UtcNow < started.AddSeconds 30.0 do
                    port <- readPort portFile
                    if port = 0 then do! Task.Delay 200
                let! result =
                    if port = 0 then Task.FromResult(Error "devtools")
                    else watchLogin edge port url (started.AddMinutes 10.0) cancel
                kill edge
                return result
        }
    let signInWindow (begun: Begun) (cancel: CancellationToken) = signInWindowAt begun.Url cancel

/// The Xbox stream's window (Edge, the app's page) and the little server behind it: the page and xCloudPlayer, the
/// streaming service's calls passed on with the account's token, and the page's lines to the app (and back).
module XboxPlayer =
    /// The page and xCloudPlayer beside the program (tests point it at a build).
    let mutable folder = Path.Combine(AppContext.BaseDirectory, "Console", "xbox")
    let isThere () = File.Exists(Path.Combine(folder, "xCloudPlayer.min.js")) && File.Exists(Path.Combine(folder, "player.html")) && Edge.isThere ()

    module Native =
        [<DllImport("user32.dll")>]
        extern bool EnumWindows(EnumWindowsProc proc, nativeint lParam)
        [<DllImport("user32.dll", CharSet = CharSet.Unicode)>]
        extern int GetWindowText(nativeint hwnd, StringBuilder text, int count)
        [<DllImport("user32.dll")>]
        extern bool IsWindowVisible(nativeint hwnd)
        [<DllImport("user32.dll")>]
        extern bool IsWindow(nativeint hwnd)
        [<DllImport("user32.dll")>]
        extern bool PostMessage(nativeint hwnd, int msg, nativeint w, nativeint l)
        [<DllImport("user32.dll")>]
        extern bool SetForegroundWindow(nativeint hwnd)
        [<DllImport("user32.dll")>]
        extern bool ShowWindow(nativeint hwnd, int cmd)
        [<DllImport("user32.dll")>]
        extern bool IsIconic(nativeint hwnd)
    open Native

    let private findWindow (mark: string) =
        let mutable found = 0n
        let text = StringBuilder(512)
        EnumWindows(EnumWindowsProc(fun hwnd _ ->
            if IsWindowVisible hwnd then
                text.Clear() |> ignore
                GetWindowText(hwnd, text, 512) |> ignore
                if text.ToString().Contains mark then found <- hwnd
            found = 0n), 0n) |> ignore
        found

    /// One stream: its window, the lines to send its page, and its end.
    type Session(server: HttpListener, mark: string, onLine: string -> unit) =
        let commands = ConcurrentQueue<string>()
        let waiting = new SemaphoreSlim(0)
        let ended = TaskCompletionSource()
        let mutable window = 0n
        let mutable connected = false
        let mutable told = false
        // (Frame generation goes on the window once the picture is there - not on the page's "connecting".)
        let tellWindow () =
            if connected && window <> 0n && not told then
                told <- true
                onLine (sprintf "@window %d" (int64 window))
        member _.Mark = mark
        member _.OnLine(line: string) =
            if line = "@connected" then
                connected <- true
                tellWindow ()
            onLine line
        member _.Window
            with get () = window
            and set v =
                window <- v
                tellWindow ()
        member _.Ended = ended.Task
        member _.End() = ended.TrySetResult() |> ignore
        member _.Tell(line: string) = commands.Enqueue line; waiting.Release() |> ignore
        member _.Next(timeout: TimeSpan) =
            task {
                let! got = waiting.WaitAsync timeout
                return if got then (match commands.TryDequeue() with | true, l -> l | _ -> "") else ""
            }
        member _.Front() =
            if window <> 0n then
                if IsIconic window then ShowWindow(window, 9) |> ignore
                SetForegroundWindow window |> ignore
        /// Closed: the page ends the console's session first (quit), the window goes after.
        member this.Close() =
            this.Tell "quit"
            task {
                do! Task.Delay 2500
                if window <> 0n && IsWindow window then PostMessage(window, 0x0010, 0n, 0n) |> ignore
            } |> ignore

    let mutable private server: HttpListener = null
    let mutable private port = 0
    let mutable private current: Session option = None
    let private http = new HttpClient(Timeout = TimeSpan.FromSeconds 30.0)
    let private types = dict [ ".html", "text/html; charset=utf-8"; ".js", "text/javascript; charset=utf-8"; ".css", "text/css"; ".png", "image/png"; ".svg", "image/svg+xml" ]

    let private reply (context: HttpListenerContext) (status: int) (contentType: string) (body: byte[]) =
        try
            context.Response.StatusCode <- status
            if contentType <> "" then context.Response.ContentType <- contentType
            context.Response.Headers.Add("Cache-Control", "no-store")
            context.Response.ContentLength64 <- int64 body.Length
            if body.Length > 0 then context.Response.OutputStream.Write(body, 0, body.Length)
            context.Response.Close()
        with _ -> ()

    let private serve (context: HttpListenerContext) =
        task {
            let request = context.Request
            let path = request.Url.AbsolutePath
            try
                if path.StartsWith "/v5/" || path.StartsWith "/v6/" then
                    // (The streaming service: passed on with the account's token, as xbox-xcloud-player's own server does.)
                    let! t = XboxAuth.streamToken ()
                    use out = new HttpRequestMessage(HttpMethod(request.HttpMethod), t.BaseUri + request.Url.PathAndQuery)
                    out.Headers.Authorization <- Headers.AuthenticationHeaderValue("Bearer", t.Token)
                    for h in [ "Accept"; "X-MS-Device-Info" ] do
                        match request.Headers[h] with
                        | null -> ()
                        | v -> out.Headers.TryAddWithoutValidation(h, v) |> ignore
                    if request.HasEntityBody then
                        use r = new StreamReader(request.InputStream, Encoding.UTF8)
                        let! body = r.ReadToEndAsync()
                        out.Content <- new StringContent(body, Encoding.UTF8, "application/json")
                    let! answer = http.SendAsync out
                    let! bytes = answer.Content.ReadAsByteArrayAsync()
                    let kind = match answer.Content.Headers.ContentType with null -> "application/json" | c -> c.ToString()
                    reply context (int answer.StatusCode) kind bytes
                elif path = "/mfgsm/say" then
                    use r = new StreamReader(request.InputStream, Encoding.UTF8)
                    let! line = r.ReadToEndAsync()
                    match current with
                    | Some s -> s.OnLine(line.Trim())
                    | None -> ()
                    reply context 204 "" [||]
                elif path = "/mfgsm/next" then
                    match current with
                    | Some s ->
                        let! line = s.Next(TimeSpan.FromSeconds 20.0)
                        if line = "" then reply context 204 "" [||] else reply context 200 "text/plain; charset=utf-8" (Encoding.UTF8.GetBytes line)
                    | None -> reply context 200 "text/plain; charset=utf-8" (Encoding.UTF8.GetBytes "quit")
                else
                    let name = Path.GetFileName(if path = "/" then "player.html" else path)
                    let file = Path.Combine(folder, name)
                    if name <> "" && File.Exists file && types.ContainsKey(Path.GetExtension name) then
                        let! bytes = File.ReadAllBytesAsync file
                        reply context 200 types[Path.GetExtension name] bytes
                    else reply context 404 "text/plain" (Encoding.UTF8.GetBytes "not found")
            with e -> reply context 502 "text/plain; charset=utf-8" (Encoding.UTF8.GetBytes e.Message)
        }

    /// The little server's port (0 before the first stream) - tests ask it.
    let serverPort () = port
    let private ensureServer () =
        if isNull server then
            let free = new Sockets.TcpListener(IPAddress.Loopback, 0)
            free.Start()
            port <- (free.LocalEndpoint :?> IPEndPoint).Port
            free.Stop()
            let listener = new HttpListener()
            listener.Prefixes.Add(sprintf "http://127.0.0.1:%d/" port)
            listener.Start()
            server <- listener
            Task.Run(fun () ->
                task {
                    while listener.IsListening do
                        try
                            let! context = listener.GetContextAsync()
                            serve context |> ignore
                        with _ -> ()
                } :> Task) |> ignore

    /// Played: the page opened in its Edge window; the window found by the mark in its title, its lines to onLine
    /// ("@window <hwnd>" first, then "@connected", "@menu", "@error ...", "@ended"). The window gone = the end.
    let play (console: XboxConsole) (resolution: int) (bitrateKbps: int) (onLine: string -> unit) =
        ensureServer ()
        let mark = "MFGSM-" + Guid.NewGuid().ToString("N").Substring(0, 8)
        let session = Session(server, mark, onLine)
        current <- Some session
        let url =
            sprintf "http://127.0.0.1:%d/player.html?server=%s&res=%d&kbps=%d&name=%s&mark=%s" port (Uri.EscapeDataString console.ServerId) resolution bitrateKbps
                (Uri.EscapeDataString console.Name) mark
        match Edge.launch "edge-player" [ "--window-size=1280,760"; "--disable-session-crashed-bubble" ] url with
        | None -> onLine "@error Microsoft Edge"; session.End(); session
        | Some _ ->
            task {
                let until = DateTime.UtcNow.AddSeconds 30.0
                while session.Window = 0n && DateTime.UtcNow < until do
                    session.Window <- findWindow mark
                    if session.Window = 0n then do! Task.Delay 250
                if session.Window = 0n then onLine "@error window"; session.End()
                else
                    while IsWindow session.Window do do! Task.Delay 500
                    session.End()
                if current = Some session then current <- None
            } |> ignore
            session

    /// Guide + Start on an Xbox pad while the Xbox stream plays: Super Motion over it (the page cannot see Guide).
    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type private PadState = { Packet: uint32; Buttons: uint16; LeftTrigger: byte; RightTrigger: byte; LX: int16; LY: int16; RX: int16; RY: int16 }
    [<DllImport("xinput1_4.dll", EntryPoint = "#100")>]
    extern int private XInputGetStateEx(int user, PadState& state)
    let watchPad (session: Session) =
        Task.Run(fun () ->
            task {
                let mutable held = false
                while not session.Ended.IsCompleted do
                    let mutable both = false
                    for user in 0 .. 3 do
                        let mutable s = Unchecked.defaultof<PadState>
                        if (try XInputGetStateEx(user, &s) = 0 with _ -> false) && (s.Buttons &&& 0x0410us) = 0x0410us then both <- true
                    if both && not held then session.OnLine "@menu"
                    held <- both
                    do! Task.Delay 33
            } :> Task) |> ignore
