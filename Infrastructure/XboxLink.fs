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
open System.Text.Encodings.Web
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
                       "--disable-background-mode"; "--disable-backgrounding-occluded-windows"; "--disable-renderer-backgrounding"; "--disable-background-timer-throttling"
                       "--disable-features=msEdgeSidebarV2,msHubApps,msImplicitSignin,EdgeCollections,CalculateNativeWinOcclusion,WebRtcHideLocalIpsWithMdns"
                       "--autoplay-policy=no-user-gesture-required" ] @ extra @ [ "--app=" + url ] do
                info.ArgumentList.Add a
            try
                let p = Process.Start info
                if isNull p then None
                else
                    if profileName = "edge-player" then
                        WindowsPerformance.applyToProcess p ProcessPriorityClass.AboveNormal
                    Some p
            with _ -> None

module XboxAuth =
    let private appId = "000000004c20a908"
    let private titleId = "328178078"
    let redirectPrefix = "ms-xal-000000004c20a908://auth"
    let private savedPath = Path.Combine(Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData, "MFG SUPER MOTION", "Console", "xbox.bin")
    let private http = new HttpClient(Timeout = TimeSpan.FromSeconds 20.0)
    let private relaxedJson = JsonSerializerOptions(Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping)
    let private b64url (bytes: byte[]) = Convert.ToBase64String(bytes).TrimEnd('=').Replace('+', '-').Replace('/', '_')

    /// What is kept between runs: the device's key (Xbox's sign-in binds the tokens to it), the Microsoft account's
    /// refresh token, and the gamertag to show. Sealed with Windows' protection (DPAPI), never in the settings file.
    type Saved = { Key: string; Refresh: string; Gamertag: string }
    let mutable private saved: Saved option = None
    let mutable private stream: XboxStreamToken option = None
    let mutable private key: ECDsa = null
    let invalidateStream () = stream <- None
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
        stream <- None
        if not (isNull key) then
            (try key.Dispose() with _ -> ())
            key <- null
        try if File.Exists savedPath then File.Delete savedPath with _ -> ()

    // The device key: P-256, made once and kept with the sign-in.
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
            let text = body.ToJsonString(relaxedJson)
            use request = new HttpRequestMessage(HttpMethod.Post, url)
            request.Headers.Add("x-xbl-contract-version", "1")
            request.Headers.Add("Cache-Control", "no-store, must-revalidate, no-cache")
            request.Headers.Add("Signature", sign url "" text)
            request.Content <- new StringContent(text, Encoding.UTF8, "application/json")
            let! response = http.SendAsync request
            let! answer = response.Content.ReadAsStringAsync()
            if not response.IsSuccessStatusCode then
                let detail =
                    if answer <> "" then (if answer.Length > 200 then answer.Substring(0, 200) else answer)
                    else
                        let xerr = match response.Headers.TryGetValues "X-Err" with | true, v -> "X-Err=" + String.Join(",", v) | _ -> ""
                        let corr = match response.Headers.TryGetValues "X-XblCorrelationId" with | true, v -> "cid=" + String.Join(",", v) | _ -> ""
                        [ xerr; corr ] |> List.filter (fun s -> s <> "") |> String.concat " "
                failwithf "%s: %d %s" (Uri(url).Host) (int response.StatusCode) detail
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
            return answer, device
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
            let! (sisu, _) = sisuAuthorize (str user [ "access_token" ]) begun.Session
            let tag = str sisu [ "AuthorizationToken"; "DisplayClaims"; "xui"; "0"; "gtg" ]
            store { Key = keyText (); Refresh = str user [ "refresh_token" ]; Gamertag = tag }
            stream <- None
            return tag
        }

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
                let! (sisu, device) = sisuAuthorize (str user [ "access_token" ]) ""
                let tag = str sisu [ "AuthorizationToken"; "DisplayClaims"; "xui"; "0"; "gtg" ]
                store { s with Key = keyText (); Refresh = (if refresh <> "" then refresh else s.Refresh); Gamertag = (if tag <> "" then tag else s.Gamertag) }
                let xstsProps = JsonObject()
                xstsProps["SandboxId"] <- JsonValue.Create("RETAIL")
                let dev = match str sisu [ "DeviceToken" ] with "" -> device | d -> d
                if dev <> "" then xstsProps["DeviceToken"] <- JsonValue.Create(dev)
                let title = str sisu [ "TitleToken"; "Token" ]
                if title <> "" then xstsProps["TitleToken"] <- JsonValue.Create(title)
                xstsProps["UserTokens"] <- JsonArray(JsonValue.Create(str sisu [ "UserToken"; "Token" ]))
                let xsts =
                    JsonObject(dict [ "Properties", xstsProps :> JsonNode
                                      "RelyingParty", JsonValue.Create("http://gssv.xboxlive.com/"); "TokenType", JsonValue.Create("JWT") ])
                let! (gssv, _) = postSigned "https://xsts.auth.xboxlive.com/xsts/authorize" xsts
                let body = JsonObject(dict [ "token", JsonValue.Create(str gssv [ "Token" ]) :> JsonNode; "offeringId", JsonValue.Create("xhome") ]).ToJsonString(relaxedJson)
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
                    else regions |> Seq.tryFind (fun r -> not (isNull r) && String.Equals(str r [ "isDefault" ], "true", StringComparison.OrdinalIgnoreCase)) |> Option.defaultValue regions[0]
                let baseUri = if isNull region then "https://uks.gssv-play-prodxhome.xboxlive.com" else (str region [ "baseUri" ]).TrimEnd('/')
                let seconds = match Int32.TryParse(str node [ "durationInSeconds" ]) with | true, v -> v | _ -> 3600
                let t = { Token = str node [ "gsToken" ]; BaseUri = baseUri; Until = DateTime.UtcNow.AddSeconds(float seconds) }
                stream <- Some t
                return t
        }

    /// Transfer token (lpt) needed when an xHome session enters ReadyToConnect (/connect).
    let transferToken () =
        task {
            let s = match load () with Some s when s.Refresh <> "" -> s | _ -> failwith "not signed in"
            let! res =
                postForm "https://login.live.com/oauth20_token.srf"
                    [ "client_id", appId; "grant_type", "refresh_token"; "refresh_token", s.Refresh
                      "scope", "service::http://Passport.NET/purpose::PURPOSE_XBOX_CLOUD_CONSOLE_TRANSFER_TOKEN" ]
            let refresh = str res [ "refresh_token" ]
            if refresh <> "" && refresh <> s.Refresh then
                store { s with Key = keyText (); Refresh = refresh }
            let tok = match str res [ "lpt" ] with "" -> str res [ "access_token" ] | t -> t
            if tok = "" then failwith "missing transfer token"
            return tok
        }

    /// The account's consoles, as the streaming service lists them.
    let consoles () =
        task {
            let fetchOnce () =
                task {
                    let! t = streamToken ()
                    use request = new HttpRequestMessage(HttpMethod.Get, t.BaseUri + "/v6/servers/home")
                    request.Headers.Authorization <- Headers.AuthenticationHeaderValue("Bearer", t.Token)
                    request.Headers.TryAddWithoutValidation("X-Gssv-Client", "XboxComBrowser") |> ignore
                    let! response = http.SendAsync request
                    let! answer = response.Content.ReadAsStringAsync()
                    return response.StatusCode, answer
                }
            let! (status0, answer0) = fetchOnce ()
            let! (status, answer) =
                if status0 = HttpStatusCode.Unauthorized then
                    invalidateStream ()
                    fetchOnce ()
                else Task.FromResult(status0, answer0)
            if int status < 200 || int status >= 300 then failwithf "servers: %d" (int status)
            let node = JsonNode.Parse answer
            return
                match node["results"] with
                | :? JsonArray as list ->
                    [ for c in list do
                        if not (isNull c) then
                            yield { ServerId = str c [ "serverId" ]; Name = str c [ "deviceName" ]; ConsoleType = str c [ "consoleType" ]
                                    Power = str c [ "powerState" ]; OutOfHome = String.Equals(str c [ "outOfHomeWarning" ], "true", StringComparison.OrdinalIgnoreCase) } ]
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

    let private findBridgeExe () =
        let candidates =
            [ Path.Combine(AppContext.BaseDirectory, "Console", "mfgsm-console.exe")
              (match Path.GetDirectoryName(folder.TrimEnd('\\', '/')) with
               | null | "" -> ""
               | parent -> Path.Combine(parent, "mfgsm-console.exe"))
              Path.Combine(AppContext.BaseDirectory, "mfgsm-console.exe") ]
        candidates |> List.tryFind (fun p -> p <> "" && File.Exists p)

    /// Native Windows HID/XInput/WinMM controller bridge (mfgsm-console.exe pad-bridge) over /mfgsm/pad.
    type PadBridge(onMenu: unit -> unit) =
        let stdinLock = obj ()
        let sockets = ConcurrentDictionary<WebSocket, SemaphoreSlim>()
        let mutable proc: Process = null
        let mutable latestPadJson = """{"connected":false}"""
        let mutable stopped = 0

        let sendFrame (ws: WebSocket) (gate: SemaphoreSlim) (json: string) =
            task {
                if Volatile.Read(&stopped) = 0 && ws.State = WebSocketState.Open then
                    try
                        do! gate.WaitAsync()
                        try
                            if Volatile.Read(&stopped) = 0 && ws.State = WebSocketState.Open then
                                let bytes = Encoding.UTF8.GetBytes json
                                do! ws.SendAsync(ArraySegment bytes, WebSocketMessageType.Text, true, CancellationToken.None)
                        finally
                            gate.Release() |> ignore
                    with _ -> ()
            }

        let broadcast (json: string) =
            latestPadJson <- json
            for kv in sockets do
                if kv.Key.State = WebSocketState.Open then
                    sendFrame kv.Key kv.Value json |> ignore

        let parseAndBroadcast (line: string) =
            if line = "@menu" then onMenu ()
            elif line.StartsWith "@pad " then
                let p = line.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                if p.Length >= 10 then
                    match Int32.TryParse p[1], Int32.TryParse p[2], UInt32.TryParse p[3], UInt32.TryParse p[4], UInt32.TryParse p[5],
                          Int32.TryParse p[6], Int32.TryParse p[7], Int32.TryParse p[8], Int32.TryParse p[9] with
                    | (true, conn), (true, kind), (true, mask), (true, lt), (true, rt), (true, lx), (true, ly), (true, rx), (true, ry) ->
                        let json =
                            sprintf """{"connected":%s,"kind":%d,"mask":%u,"lt":%u,"rt":%u,"lx":%d,"ly":%d,"rx":%d,"ry":%d}"""
                                (if conn <> 0 then "true" else "false") kind mask lt rt lx ly rx ry
                        broadcast json
                    | _ -> ()

        member _.HasProcess = not (isNull proc) && not proc.HasExited
        member _.LatestJson = latestPadJson

        member _.Start() =
            if Volatile.Read(&stopped) = 0 && isNull proc then
                match findBridgeExe () with
                | None -> ()
                | Some exe ->
                    try
                        let info = ProcessStartInfo(exe, "pad-bridge", UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardInput = true, StandardOutputEncoding = Encoding.UTF8)
                        let p = new Process(StartInfo = info, EnableRaisingEvents = true)
                        p.OutputDataReceived.Add(fun args -> if not (isNull args.Data) then parseAndBroadcast (args.Data.Trim()))
                        if p.Start() then
                            proc <- p
                            p.BeginOutputReadLine()
                        else
                            p.Dispose()
                    with _ -> ()

        member _.SendRumble(left: int, right: int) =
            let l = Math.Clamp(left, 0, 255)
            let r = Math.Clamp(right, 0, 255)
            lock stdinLock (fun () ->
                if not (isNull proc) && not proc.HasExited then
                    try
                        proc.StandardInput.WriteLine(sprintf "rumble %d %d" l r)
                        proc.StandardInput.Flush()
                    with _ -> ())

        member this.AttachSocket(ws: WebSocket) =
            task {
                let gate = new SemaphoreSlim(1, 1)
                sockets[ws] <- gate
                try
                    do! sendFrame ws gate latestPadJson
                    let buf = Array.zeroCreate<byte> 1024
                    use msg = new MemoryStream()
                    while Volatile.Read(&stopped) = 0 && ws.State = WebSocketState.Open do
                        let! res = ws.ReceiveAsync(ArraySegment buf, CancellationToken.None)
                        if res.MessageType = WebSocketMessageType.Close then
                            try do! ws.CloseAsync(WebSocketCloseStatus.NormalClosure, "", CancellationToken.None) with _ -> ()
                        elif res.MessageType = WebSocketMessageType.Text && res.Count > 0 then
                            msg.Write(buf, 0, res.Count)
                            if res.EndOfMessage then
                                let txt = Encoding.UTF8.GetString(msg.ToArray()).Trim()
                                msg.SetLength 0L
                                if txt.StartsWith "rumble " then
                                    let parts = txt.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                                    if parts.Length >= 3 then
                                        match Int32.TryParse parts[1], Int32.TryParse parts[2] with
                                        | (true, l), (true, r) -> this.SendRumble(l, r)
                                        | _ -> ()
                with _ -> ()
                let mutable dropped = Unchecked.defaultof<SemaphoreSlim>
                sockets.TryRemove(ws, &dropped) |> ignore
                try ws.Dispose() with _ -> ()
            }

        member _.Stop() =
            if Interlocked.Exchange(&stopped, 1) = 0 then
                for kv in sockets do
                    try kv.Key.Abort() with _ -> ()
                let p =
                    lock stdinLock (fun () ->
                        let cur = proc
                        proc <- null
                        if not (isNull cur) then
                            try
                                if not cur.HasExited then
                                    cur.StandardInput.WriteLine "rumble 0 0"
                                    cur.StandardInput.WriteLine "quit"
                                    cur.StandardInput.Close()
                            with _ -> ()
                        cur)
                if not (isNull p) then
                    try
                        if not (p.WaitForExit 400) && not p.HasExited then p.Kill true
                    with _ -> ()
                    try p.Dispose() with _ -> ()

    /// One stream: its window, the lines to send its page, and its end.
    type Session(server: HttpListener, mark: string, onLine: string -> unit) as this =
        let commands = ConcurrentQueue<string>()
        let waiting = new SemaphoreSlim(0)
        let ended = TaskCompletionSource()
        let bridge = PadBridge(fun () -> this.OnLine "@menu")
        let mutable window = 0n
        let mutable connected = false
        let mutable told = false
        let mutable lastMenuMs = -1000L
        let mutable remoteSessionId = ""
        let mutable remoteDeleted = 0
        // (Frame generation goes on the window once the picture is there - not on the page's "connecting".)
        let tellWindow () =
            if connected && window <> 0n && not told then
                told <- true
                onLine (sprintf "@window %d" (int64 window))
        member _.Mark = mark
        member _.Bridge = bridge
        member _.HasBridge = bridge.HasProcess
        member _.StartBridge() = bridge.Start()
        member _.SetRemoteSession(id: string) =
            if id <> "" && id <> remoteSessionId then
                remoteSessionId <- id
                Volatile.Write(&remoteDeleted, 0)
        member _.MarkRemoteDeleted() = Volatile.Write(&remoteDeleted, 1)
        member _.RemoteSessionToClean() =
            if Interlocked.CompareExchange(&remoteDeleted, 1, 0) = 0 then remoteSessionId else ""
        member this.OnLine(line: string) =
            if line.StartsWith "@session " then
                this.SetRemoteSession(line.Substring(9).Trim())
            else
                if line = "@connected" then
                    connected <- true
                    tellWindow ()
                elif line = "@quit" then
                    this.MarkRemoteDeleted()
                if line = "@menu" then
                    let now = Environment.TickCount64
                    let prev = Volatile.Read(&lastMenuMs)
                    if now - prev >= 400L && Interlocked.CompareExchange(&lastMenuMs, now, prev) = prev then
                        onLine line
                else
                    onLine line
        member _.Window
            with get () = window
            and set v =
                window <- v
                tellWindow ()
        member _.Ended = ended.Task
        member _.End() =
            bridge.Stop()
            ended.TrySetResult() |> ignore
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
    let private relaxedJson = JsonSerializerOptions(Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping)
    let private types = dict [ ".html", "text/html; charset=utf-8"; ".js", "text/javascript; charset=utf-8"; ".css", "text/css"; ".png", "image/png"; ".svg", "image/svg+xml" ]
    let private debugLogPath = Path.Combine(Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData, "MFG SUPER MOTION", "Console", "xbox-debug.log")
    let private debugLogLock = obj ()
    let mutable private debugLogBytes = 0
    let private deleteCooldownLock = obj ()
    let mutable private lastDeleteAt = DateTime.MinValue
    let mutable private lastDeleteTask: Task = Task.CompletedTask
    let private resetDebugLog (header: string) =
        lock debugLogLock (fun () ->
            try
                Directory.CreateDirectory(Path.GetDirectoryName debugLogPath) |> ignore
                File.WriteAllText(debugLogPath, header + "\r\n", Encoding.UTF8)
                debugLogBytes <- header.Length + 2
            with _ -> ())
    let private appendDebugLog (line: string) =
        if not (String.IsNullOrWhiteSpace line) then
            lock debugLogLock (fun () ->
                try
                    if debugLogBytes < 65536 then
                        let entry = sprintf "[%s] %s\r\n" (DateTime.Now.ToString("HH:mm:ss.fff")) (line.Trim())
                        File.AppendAllText(debugLogPath, entry, Encoding.UTF8)
                        debugLogBytes <- debugLogBytes + Encoding.UTF8.GetByteCount entry
                with _ -> ())

    let private noteDeleteTime () =
        lock deleteCooldownLock (fun () -> lastDeleteAt <- DateTime.UtcNow)

    let private deleteRemoteSession (id: string) =
        if id <> "" then
            let t =
                task {
                    try
                        let! t = XboxAuth.streamToken ()
                        use req = new HttpRequestMessage(HttpMethod.Delete, t.BaseUri + "/v5/sessions/home/" + Uri.EscapeDataString id)
                        req.Headers.Authorization <- Headers.AuthenticationHeaderValue("Bearer", t.Token)
                        req.Headers.TryAddWithoutValidation("X-Gssv-Client", "XboxComBrowser") |> ignore
                        let! _ = http.SendAsync req
                        ()
                    with _ -> ()
                    noteDeleteTime ()
                } :> Task
            lock deleteCooldownLock (fun () ->
                lastDeleteAt <- DateTime.UtcNow
                lastDeleteTask <- t)

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
                    let isDelete = request.HttpMethod.Equals("DELETE", StringComparison.OrdinalIgnoreCase)
                    let isPlay = request.HttpMethod.Equals("POST", StringComparison.OrdinalIgnoreCase) && path.Equals("/v5/sessions/home/play", StringComparison.OrdinalIgnoreCase)
                    if isDelete then
                        current |> Option.iter (fun s -> s.MarkRemoteDeleted())
                        noteDeleteTime ()
                    if isPlay then
                        let prevTask, deletedAt = lock deleteCooldownLock (fun () -> lastDeleteTask, lastDeleteAt)
                        if not prevTask.IsCompleted then
                            let! _ = Task.WhenAny(prevTask, Task.Delay 3000)
                            ()
                        let deletedAt = lock deleteCooldownLock (fun () -> lastDeleteAt)
                        if deletedAt <> DateTime.MinValue then
                            let elapsedMs = (DateTime.UtcNow - deletedAt).TotalMilliseconds
                            if elapsedMs < 2000.0 then
                                do! Task.Delay(int (2000.0 - elapsedMs))
                    let isConnect = request.HttpMethod.Equals("POST", StringComparison.OrdinalIgnoreCase) && path.EndsWith("/connect", StringComparison.OrdinalIgnoreCase)
                    let! rawBody =
                        if not isConnect && request.HasEntityBody then
                            use r = new StreamReader(request.InputStream, Encoding.UTF8)
                            r.ReadToEndAsync()
                        else Task.FromResult ""
                    let! bodyText =
                        if isConnect then
                            task {
                                let! lpt = XboxAuth.transferToken ()
                                return JsonObject(dict [ "userToken", JsonValue.Create(lpt) :> JsonNode ]).ToJsonString(relaxedJson)
                            }
                        else Task.FromResult rawBody
                    let hasBody = isConnect || request.HasEntityBody
                    let forwardOnce () =
                        task {
                            let! t = XboxAuth.streamToken ()
                            use out = new HttpRequestMessage(HttpMethod(request.HttpMethod), t.BaseUri + request.Url.PathAndQuery)
                            out.Headers.Authorization <- Headers.AuthenticationHeaderValue("Bearer", t.Token)
                            out.Headers.TryAddWithoutValidation("X-Gssv-Client", "XboxComBrowser") |> ignore
                            for h in [ "Accept"; "X-MS-Device-Info" ] do
                                match request.Headers[h] with
                                | null -> ()
                                | v -> out.Headers.TryAddWithoutValidation(h, v) |> ignore
                            if hasBody then
                                out.Content <- new StringContent(bodyText, Encoding.UTF8, "application/json")
                            let! answer = http.SendAsync out
                            let! bytes = answer.Content.ReadAsByteArrayAsync()
                            let kind = match answer.Content.Headers.ContentType with null -> "application/json" | c -> c.ToString()
                            return answer.StatusCode, kind, bytes
                        }
                    let! (status0, kind0, bytes0) = forwardOnce ()
                    let! (status, kind, bytes) =
                        if status0 = HttpStatusCode.Unauthorized then
                            XboxAuth.invalidateStream ()
                            forwardOnce ()
                        else Task.FromResult(status0, kind0, bytes0)
                    if isDelete then noteDeleteTime ()
                    reply context (int status) kind bytes
                elif path = "/mfgsm/pad" then
                    if request.IsWebSocketRequest then
                        try
                            let! wsCtx = context.AcceptWebSocketAsync(null)
                            match current with
                            | Some s -> do! s.Bridge.AttachSocket wsCtx.WebSocket
                            | None ->
                                try
                                    do! wsCtx.WebSocket.CloseAsync(WebSocketCloseStatus.NormalClosure, "no session", CancellationToken.None)
                                    wsCtx.WebSocket.Dispose()
                                with _ -> ()
                        with _ -> ()
                    else
                        let body =
                            match current with
                            | Some s -> s.Bridge.LatestJson
                            | None -> """{"connected":false}"""
                        reply context 200 "application/json; charset=utf-8" (Encoding.UTF8.GetBytes body)
                elif path = "/mfgsm/say" then
                    use r = new StreamReader(request.InputStream, Encoding.UTF8)
                    let! line = r.ReadToEndAsync()
                    match current with
                    | Some s -> s.OnLine(line.Trim())
                    | None -> ()
                    reply context 204 "" [||]
                elif path = "/mfgsm/log" then
                    use r = new StreamReader(request.InputStream, Encoding.UTF8)
                    let! line = r.ReadToEndAsync()
                    appendDebugLog line
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
        session.StartBridge()
        resetDebugLog (sprintf "=== Xbox session %s console=%s (%s, power=%s, away=%b) res=%d kbps=%d ===" (DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss")) console.Name console.ConsoleType console.Power console.OutOfHome resolution bitrateKbps)
        let url =
            sprintf "http://127.0.0.1:%d/player.html?server=%s&res=%d&kbps=%d&name=%s&mark=%s" port (Uri.EscapeDataString console.ServerId) resolution bitrateKbps
                (Uri.EscapeDataString console.Name) mark
        match Edge.launch "edge-player" [ "--start-fullscreen"; "--disable-session-crashed-bubble" ] url with
        | None ->
            onLine "@error Microsoft Edge"
            session.End()
            if current = Some session then current <- None
            session
        | Some _ ->
            task {
                let until = DateTime.UtcNow.AddSeconds 30.0
                while session.Window = 0n && DateTime.UtcNow < until do
                    session.Window <- findWindow mark
                    if session.Window = 0n then do! Task.Delay 250
                if session.Window = 0n then
                    onLine "@error window"
                    deleteRemoteSession (session.RemoteSessionToClean())
                    session.End()
                else
                    while IsWindow session.Window do do! Task.Delay 500
                    deleteRemoteSession (session.RemoteSessionToClean())
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
        if not session.HasBridge then
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
