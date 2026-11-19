namespace ARMO.Infrastructure

open System
open System.Net
open System.Net.NetworkInformation
open System.Net.Sockets
open System.Runtime.InteropServices
open System.Text
open System.Threading.Tasks
open System.IO
open ARMO.Models

/// Console Link (2026-11-16, the owner): a PlayStation 5 / 4 or an Xbox Series S / X on the home network, its picture
/// shown here with frame generation on it. This part: the PlayStations on the network found, and how this PC is joined
/// to it. (The stream itself goes through Chiaki - the PlayStations' Remote Play protocol; Xbox's through
/// xbox-xcloud-player.)
type ConsoleKind =
    | PS5
    | PS4

/// Ready: on and waiting; Standby: in rest mode (it can be woken once the PC is registered with it).
type ConsoleState =
    | Ready
    | Standby
    | Unknown

type ConsoleInfo =
    { Kind: ConsoleKind
      Name: string
      HostId: string
      Address: string
      State: ConsoleState
      SystemVersion: string
      /// The port it takes the Remote Play session's requests on.
      RequestPort: int }

/// How this PC reaches the network: a cable is what Remote Play wants (less delay, no drops), Wi-Fi works.
type LinkKind =
    | Cable
    | WiFi
    | NoLink

type PcLink =
    { Kind: LinkKind
      /// The adapter's own words (e.g. "Intel(R) Ethernet Connection") and its speed, megabits a second.
      Adapter: string
      SpeedMbps: int64 }

module ConsoleLink =
    // The PlayStations' own device discovery (the one their Remote Play apps use): a request sent to every machine
    // of the network on the console's port; a console answers "200 Ok" when on, "620 Server Standby" in rest mode,
    // with its name, its id and its kind. PS5: port 9302, protocol 00030010; PS4: port 987, protocol 00020020.
    let private probes =
        [ PS5, 9302, "00030010"
          PS4, 987, "00020020" ]

    let private request (protocol: string) =
        Encoding.ASCII.GetBytes(sprintf "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:%s\n" protocol)

    /// One answer read: its status line and its "key:value" lines. None: not a console's answer.
    let parseAnswer (kind: ConsoleKind) (address: string) (text: string) =
        let lines = text.Replace("\r", "").Split('\n', StringSplitOptions.RemoveEmptyEntries)
        if lines.Length = 0 || not (lines[0].StartsWith "HTTP/1.1") then None
        else
            let values =
                lines
                |> Array.skip 1
                |> Array.choose (fun line ->
                    match line.IndexOf ':' with
                    | -1 -> None
                    | at -> Some(line.Substring(0, at).Trim().ToLowerInvariant(), line.Substring(at + 1).Trim()))
                |> dict
            let value key = match values.TryGetValue key with | true, v -> v | _ -> ""
            let state =
                if lines[0].Contains " 200" then Ready
                elif lines[0].Contains " 620" then Standby
                else Unknown
            let kind =
                match value "host-type" with
                | t when t.Equals("PS5", StringComparison.OrdinalIgnoreCase) -> PS5
                | t when t.Equals("PS4", StringComparison.OrdinalIgnoreCase) -> PS4
                | _ -> kind
            let port = match Int32.TryParse(value "host-request-port") with | true, p -> p | _ -> 997
            Some { Kind = kind; Name = (match value "host-name" with "" -> string kind | n -> n); HostId = value "host-id"
                   Address = address; State = state; SystemVersion = value "system-version"; RequestPort = port }

    /// Active non-loopback/non-tunnel IPv4 adapter addresses paired with their directed subnet broadcast.
    let private adapterBroadcasts () =
        try
            NetworkInterface.GetAllNetworkInterfaces()
            |> Seq.filter (fun n ->
                n.OperationalStatus = OperationalStatus.Up
                && n.NetworkInterfaceType <> NetworkInterfaceType.Loopback
                && n.NetworkInterfaceType <> NetworkInterfaceType.Tunnel)
            |> Seq.collect (fun n -> n.GetIPProperties().UnicastAddresses)
            |> Seq.filter (fun a -> a.Address.AddressFamily = AddressFamily.InterNetwork && not (isNull a.IPv4Mask))
            |> Seq.map (fun a ->
                let ip = a.Address.GetAddressBytes()
                let mask = a.IPv4Mask.GetAddressBytes()
                let bcast = IPAddress(Array.init 4 (fun i -> ip[i] ||| (~~~mask[i])))
                a.Address, bcast)
            |> Seq.toList
        with _ -> []

    /// Every PlayStation that answers within `wait` (a console in rest mode answers too). Binds one UDP socket per
    /// active IPv4 adapter so virtual/VPN adapters (WSL, Hyper-V, VirtualBox) never steal broadcasts, plus an
    /// unbound socket for any manually typed address.
    let discover (wait: TimeSpan) (direct: string list) : Task<ConsoleInfo list> =
        task {
            let listeners = ResizeArray<UdpClient * IPAddress list>()
            try
                for (localIp, bcastIp) in adapterBroadcasts () do
                    try
                        let u = new UdpClient(IPEndPoint(localIp, 0))
                        u.EnableBroadcast <- true
                        let targets = [ bcastIp; IPAddress.Broadcast ] |> List.distinct
                        listeners.Add(u, targets)
                    with _ -> ()
                let directIps = direct |> List.choose (fun d -> match IPAddress.TryParse(d.Trim()) with | true, a -> Some a | _ -> None) |> List.distinct
                if listeners.Count = 0 || not directIps.IsEmpty then
                    try
                        let u = new UdpClient(AddressFamily.InterNetwork)
                        u.EnableBroadcast <- true
                        let targets = (if listeners.Count = 0 then [ IPAddress.Broadcast ] else []) @ directIps |> List.distinct
                        listeners.Add(u, targets)
                    with _ -> ()

                // Twice, a moment apart: a console waking its network answers the second.
                for round in 0 .. 1 do
                    for (_, port, protocol) in probes do
                        let bytes = request protocol
                        for (sock, targets) in listeners do
                            for target in targets do
                                try sock.Send(bytes, bytes.Length, IPEndPoint(target, port)) |> ignore with _ -> ()
                    if round = 0 then do! Task.Delay 250

                let found = Collections.Generic.Dictionary<string, ConsoleInfo>()
                let gate = obj ()
                let until = DateTime.UtcNow + wait
                let receiveLoop (sock: UdpClient) =
                    task {
                        while DateTime.UtcNow < until do
                            let left = until - DateTime.UtcNow
                            if left > TimeSpan.Zero then
                                try
                                    let receive = sock.ReceiveAsync()
                                    let! finished = Task.WhenAny(receive, Task.Delay left)
                                    if obj.ReferenceEquals(finished, receive) && receive.Status = TaskStatus.RanToCompletion then
                                        let answer = receive.Result
                                        let address = answer.RemoteEndPoint.Address.ToString()
                                        let kind = if answer.RemoteEndPoint.Port = 987 then PS4 else PS5
                                        match parseAnswer kind address (Encoding.ASCII.GetString answer.Buffer) with
                                        | Some console ->
                                            let key = if console.HostId <> "" then console.HostId else address
                                            lock gate (fun () -> found[key] <- console)
                                        | None -> ()
                                with _ -> ()
                    }
                do! Task.WhenAll([ for (sock, _) in listeners -> receiveLoop sock :> Task ])
                return found.Values |> Seq.sortBy (fun c -> c.Name) |> Seq.toList
            finally
                for (sock, _) in listeners do
                    try sock.Dispose() with _ -> ()
        }

    /// How this PC reaches its network: the adapter that holds the default way out (a gateway), cable or Wi-Fi.
    let pcLink () =
        try
            let adapters =
                NetworkInterface.GetAllNetworkInterfaces()
                |> Seq.filter (fun n ->
                    n.OperationalStatus = OperationalStatus.Up
                    && n.NetworkInterfaceType <> NetworkInterfaceType.Loopback
                    && n.NetworkInterfaceType <> NetworkInterfaceType.Tunnel
                    && n.GetIPProperties().GatewayAddresses |> Seq.exists (fun g -> g.Address.AddressFamily = AddressFamily.InterNetwork && not (g.Address.Equals IPAddress.Any)))
                |> Seq.toList
            let wired = adapters |> List.tryFind (fun n -> n.NetworkInterfaceType = NetworkInterfaceType.Ethernet || n.NetworkInterfaceType = NetworkInterfaceType.GigabitEthernet)
            let wireless = adapters |> List.tryFind (fun n -> n.NetworkInterfaceType = NetworkInterfaceType.Wireless80211)
            match wired, wireless with
            | Some n, _ -> { Kind = Cable; Adapter = n.Description; SpeedMbps = n.Speed / 1_000_000L }
            | None, Some n -> { Kind = WiFi; Adapter = n.Description; SpeedMbps = n.Speed / 1_000_000L }
            | None, None ->
                match adapters with
                | n :: _ -> { Kind = Cable; Adapter = n.Description; SpeedMbps = n.Speed / 1_000_000L }
                | [] -> { Kind = NoLink; Adapter = ""; SpeedMbps = 0L }
        with _ -> { Kind = NoLink; Adapter = ""; SpeedMbps = 0L }

/// Puts active Windows Wi-Fi adapters into low-latency media streaming mode (and 1 ms OS timer resolution) while a
/// Console Link stream runs, suppressing periodic background SSID scans that cause packet-loss spikes; restored on end.
module WifiStreaming =
    [<DllImport("wlanapi.dll")>]
    extern uint32 private WlanOpenHandle(uint32 dwClientVersion, IntPtr pReserved, uint32& pdwNegotiatedVersion, nativeint& phClientHandle)
    [<DllImport("wlanapi.dll")>]
    extern uint32 private WlanCloseHandle(nativeint hClientHandle, IntPtr pReserved)
    [<DllImport("wlanapi.dll")>]
    extern uint32 private WlanEnumInterfaces(nativeint hClientHandle, IntPtr pReserved, nativeint& ppInterfaceList)
    [<DllImport("wlanapi.dll")>]
    extern uint32 private WlanQueryInterface(nativeint hClientHandle, Guid& pInterfaceGuid, uint32 opCode, IntPtr pReserved, uint32& pdwDataSize, nativeint& ppData, uint32& pWlanOpcodeValueType)
    [<DllImport("wlanapi.dll")>]
    extern uint32 private WlanSetInterface(nativeint hClientHandle, Guid& pInterfaceGuid, uint32 opCode, uint32 dwDataSize, IntPtr pData, IntPtr pReserved)
    [<DllImport("wlanapi.dll")>]
    extern void private WlanFreeMemory(nativeint pMemory)
    [<DllImport("winmm.dll")>]
    extern uint32 private timeBeginPeriod(uint32 uPeriod)
    [<DllImport("winmm.dll")>]
    extern uint32 private timeEndPeriod(uint32 uPeriod)

    let private gate = obj ()
    let mutable private active = false
    let mutable private wlanHandle = 0n
    let private bgScanRestored = Collections.Generic.List<Guid>()

    let enable () =
        if OperatingSystem.IsWindows() then
            lock gate (fun () ->
                if not active then
                    active <- true
                    try timeBeginPeriod 1u |> ignore with _ -> ()
                    try
                        let mutable ver = 0u
                        let mutable h = 0n
                        if WlanOpenHandle(2u, IntPtr.Zero, &ver, &h) = 0u && h <> 0n then
                            wlanHandle <- h
                            let mutable listPtr = 0n
                            if WlanEnumInterfaces(h, IntPtr.Zero, &listPtr) = 0u && listPtr <> 0n then
                                let ptrOne = Marshal.AllocHGlobal 4
                                let ptrZero = Marshal.AllocHGlobal 4
                                try
                                    Marshal.WriteInt32(ptrOne, 1)
                                    Marshal.WriteInt32(ptrZero, 0)
                                    let count = min 32 (max 0 (Marshal.ReadInt32(listPtr, 0)))
                                    for i in 0 .. count - 1 do
                                        // sizeof(WLAN_INTERFACE_INFO) = 532 bytes; InterfaceInfo[0] starts at byte offset 8.
                                        let mutable guid = Marshal.PtrToStructure<Guid>(IntPtr.Add(listPtr, 8 + i * 532))
                                        // Opcode 7 = wlan_intf_opcode_media_streaming_mode (stays active while handle is open).
                                        WlanSetInterface(h, &guid, 7u, 4u, ptrOne, IntPtr.Zero) |> ignore
                                        // Opcode 2 = wlan_intf_opcode_background_scan_enabled (best-effort; may require admin).
                                        let mutable ds = 0u
                                        let mutable dp = 0n
                                        let mutable vt = 0u
                                        if WlanQueryInterface(h, &guid, 2u, IntPtr.Zero, &ds, &dp, &vt) = 0u && dp <> 0n then
                                            let bgVal = Marshal.ReadInt32 dp
                                            WlanFreeMemory dp
                                            if bgVal <> 0 && WlanSetInterface(h, &guid, 2u, 4u, ptrZero, IntPtr.Zero) = 0u then
                                                bgScanRestored.Add guid
                                finally
                                    Marshal.FreeHGlobal ptrOne
                                    Marshal.FreeHGlobal ptrZero
                                    WlanFreeMemory listPtr
                    with _ -> ())

    let disable () =
        if OperatingSystem.IsWindows() then
            lock gate (fun () ->
                if active then
                    active <- false
                    try
                        if wlanHandle <> 0n then
                            if bgScanRestored.Count > 0 then
                                let ptrOne = Marshal.AllocHGlobal 4
                                try
                                    Marshal.WriteInt32(ptrOne, 1)
                                    for guid in bgScanRestored do
                                        let mutable g = guid
                                        WlanSetInterface(wlanHandle, &g, 2u, 4u, ptrOne, IntPtr.Zero) |> ignore
                                finally
                                    bgScanRestored.Clear()
                                    Marshal.FreeHGlobal ptrOne
                            WlanCloseHandle(wlanHandle, IntPtr.Zero) |> ignore
                            wlanHandle <- 0n
                    with _ -> wlanHandle <- 0n
                    try timeEndPeriod 1u |> ignore with _ -> ())

    do AppDomain.CurrentDomain.ProcessExit.Add(fun _ -> disable ())

/// The PSN account id a PS5's link needs (8 bytes, base64): the person signs in to their own PlayStation account on
/// Sony's page, as PlayStation's own Remote Play app does (chiaki-ng's flow and its Remote Play client id); the page
/// they land on afterwards carries a code, which is exchanged here for their account's id. Nothing is kept but the id.
module Psn =
    let private clientId = "ba495a24-818c-472b-b12d-ff231c1b5745"
    let private clientSecret = "mvaiZkRsAsI1IBkY"
    let private scope = "psn:clientapp referenceDataService:countryConfig.read pushNotification:webSocket.desktop.connect sessionManager:remotePlaySession.system.update"
    let private redirect = "https://remoteplay.dl.playstation.net/remoteplay/redirect"
    let private tokenUrl = "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/token"
    let loginUrl =
        "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize?service_entity=urn:service-entity:psn&response_type=code&client_id="
        + clientId + "&redirect_uri=" + Uri.EscapeDataString redirect + "&scope=" + Uri.EscapeDataString scope
        + "&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&"

    /// The code from what was pasted: the whole address of the page landed on, or the code alone.
    let codeOf (pasted: string) =
        let text = if isNull pasted then "" else pasted.Trim()
        match text.IndexOf "code=" with
        | -1 -> if text.Length >= 6 && text |> Seq.forall Char.IsLetterOrDigit then Some text else None
        | at ->
            let rest = text.Substring(at + 5)
            let stop = rest.IndexOfAny [| '&'; '#' |]
            Some (if stop < 0 then rest else rest.Substring(0, stop))

    /// An id typed as it is (base64 of 8 bytes) is taken as it is.
    let isAccountId (text: string) =
        not (String.IsNullOrWhiteSpace text) && (try Convert.FromBase64String(text.Trim()).Length = 8 with _ -> false)

    let accountId (pasted: string) : Task<Result<string, string>> =
        task {
            match codeOf pasted with
            | None -> return Error "no code"
            | Some code ->
                try
                    use http = new Http.HttpClient(Timeout = TimeSpan.FromSeconds 20.0)
                    let basic = Convert.ToBase64String(Encoding.ASCII.GetBytes(clientId + ":" + clientSecret))
                    http.DefaultRequestHeaders.Authorization <- Http.Headers.AuthenticationHeaderValue("Basic", basic)
                    use content =
                        new Http.FormUrlEncodedContent([|
                            Collections.Generic.KeyValuePair("grant_type", "authorization_code")
                            Collections.Generic.KeyValuePair("code", code)
                            Collections.Generic.KeyValuePair("scope", scope)
                            Collections.Generic.KeyValuePair("redirect_uri", redirect) |])
                    let! answer = http.PostAsync(tokenUrl, content)
                    let! text = answer.Content.ReadAsStringAsync()
                    if not answer.IsSuccessStatusCode then return Error (sprintf "sign-in refused (%d)" (int answer.StatusCode))
                    else
                        use token = Json.JsonDocument.Parse(text)
                        let access = token.RootElement.GetProperty("access_token").GetString()
                        let! info = http.GetStringAsync(tokenUrl + "/" + access)
                        use who = Json.JsonDocument.Parse(info)
                        let userId = UInt64.Parse(who.RootElement.GetProperty("user_id").GetString())
                        return Ok (Convert.ToBase64String(BitConverter.GetBytes userId))     // (little-endian, as the console takes it)
                with ex -> return Error ex.Message
        }

/// The player (Console\mfgsm-console.exe beside the app: libchiaki, FFmpeg) run for one task; its "@" lines read.
module ConsolePlayer =
    let path = Path.Combine(AppContext.BaseDirectory, "Console", "mfgsm-console.exe")
    let isThere () = File.Exists path

    let private start (arguments: string) (onLine: string -> unit) =
        let info = Diagnostics.ProcessStartInfo(FileName = path, Arguments = arguments, WorkingDirectory = Path.GetDirectoryName path,
                                                UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, StandardOutputEncoding = Encoding.UTF8,
                                                RedirectStandardInput = true)     // (the stream takes the app's commands there: quality, front, quit)
        let p = new Diagnostics.Process(StartInfo = info, EnableRaisingEvents = true)
        p.OutputDataReceived.Add(fun a -> if not (isNull a.Data) && a.Data.StartsWith "@" then onLine a.Data)
        p.Start() |> ignore
        p.BeginOutputReadLine()
        p

    let private quote (s: string) = "\"" + s.Replace("\"", "") + "\""

    /// Linked with a console by its PIN (Settings > System > Remote Play > Link Device on it).
    let register (console: ConsoleInfo) (accountId: string) (pin: string) : Task<Result<LinkedConsole, string>> =
        task {
            let mutable result = Error "the console did not answer"
            let kind = match console.Kind with PS5 -> "ps5" | PS4 -> "ps4"
            use p = start (sprintf "register %s %s %s %s" console.Address kind (quote accountId) pin) (fun line ->
                if line.StartsWith "@registered " then
                    try
                        use doc = Json.JsonDocument.Parse(line.Substring 12)
                        let r = doc.RootElement
                        let key = r.GetProperty("regist_key").GetString()
                        result <- Ok { HostId = console.HostId; Name = (match r.GetProperty("name").GetString() with "" -> console.Name | n -> n)
                                       Address = console.Address; Ps5 = (console.Kind = PS5); RegistKey = key; Morning = r.GetProperty("morning").GetString() }
                    with ex -> result <- Error ex.Message
                elif line.StartsWith "@error " then result <- Error (line.Substring 7))
            do! p.WaitForExitAsync()
            return result
        }

    /// A console in rest mode woken (its link's key is the credential).
    let wake (linked: LinkedConsole) : Task<bool> =
        task {
            let mutable woken = false
            use p = start (sprintf "wake %s %s %s" linked.Address (if linked.Ps5 then "ps5" else "ps4") linked.RegistKey) (fun line -> if line = "@woken" then woken <- true)
            do! p.WaitForExitAsync()
            return woken
        }

    /// The stream: its window and its lines (@window, @connected, @fg, @quality, @quit, @ended, @error ...).
    let stream (linked: LinkedConsole) (c: ConsoleOptions) (onLine: string -> unit) =
        let heights = [| 720; 1080 |]
        let widths = [| 1280; 1920 |]
        let res = min 1 (max 0 c.Resolution)
        let args =
            sprintf "stream %s %s %s %s %d %d %d %d %d %d %s %s" linked.Address (if linked.Ps5 then "ps5" else "ps4") linked.RegistKey (quote linked.Morning)
                widths[res] heights[res] (if c.Fps = 0 then 30 else 60) c.Bitrate c.Codec c.Multiplier
                (quote c.Passcode) (quote linked.Name)
        let p = start args onLine
        WindowsPerformance.applyToProcess p Diagnostics.ProcessPriorityClass.AboveNormal
        p

    /// One command to a running stream (quality r f b c | front | quit).
    let tell (p: Diagnostics.Process) (line: string) =
        try (if not (isNull p) && not p.HasExited then p.StandardInput.WriteLine line; p.StandardInput.Flush()) with _ -> ()

    [<Runtime.InteropServices.DllImport("user32.dll")>]
    extern bool AllowSetForegroundWindow(int processId)

    /// Back to the game: the stream's window may come in front (it is the one asking).
    let front (p: Diagnostics.Process) =
        if not (isNull p) && not p.HasExited then
            (try AllowSetForegroundWindow p.Id |> ignore with _ -> ())
            tell p "front"
