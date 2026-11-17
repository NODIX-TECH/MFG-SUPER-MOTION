// console_scan.fsx - Console Link's search tried from the command line: dotnet fsi tools\console_scan.fsx [address ...]
#r "../bin/Debug/net8.0/Mfg Super Motion.dll"
open System
open ARMO.Infrastructure
let link = ConsoleLink.pcLink ()
printfn "PC link: %A, %s, %d Mbit/s" link.Kind link.Adapter link.SpeedMbps
let found = (ConsoleLink.discover (TimeSpan.FromSeconds 3.0) (fsi.CommandLineArgs |> Array.skip 1 |> Array.toList)).Result
printfn "%d console(s)" found.Length
for c in found do printfn "  %A %s at %s - %A, system %s, id %s" c.Kind c.Name c.Address c.State c.SystemVersion c.HostId
// (a made-up answer, as a PS5 in rest mode gives it)
printfn "%A" (ConsoleLink.parseAnswer PS5 "192.168.1.50" "HTTP/1.1 620 Server Standby\nhost-id:AABBCCDDEEFF\nhost-type:PS5\nhost-name:PS5-123\nhost-request-port:997\nsystem-version:09000004\ndevice-discovery-protocol-version:00030010\n")
