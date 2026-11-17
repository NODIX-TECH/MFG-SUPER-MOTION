namespace ARMO.Infrastructure

open System
open System.Diagnostics
open System.IO
open System.Reflection

/// Where the app keeps what is the user's: settings, what Adaptive Motion Learning has learnt, the engine's
/// log - %LOCALAPPDATA%\MFG SUPER MOTION, the app's own name. It was %LOCALAPPDATA%\ARMO: a folder of that name
/// from an earlier version is taken over whole, once, the first time this version is opened. (The engine uses
/// the same folder: sm_options.h, EngineDataPath.)
module AppData =
    let private local = Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData
    let folder = Path.Combine(local, "MFG SUPER MOTION")

    let currentVersion =
        let v = Assembly.GetExecutingAssembly().GetName().Version
        if isNull v then "1.1.1" else sprintf "%d.%d.%d" v.Major v.Minor (max 0 v.Build)

    let private cleanedFile = Path.Combine(folder, "cleaned-version.txt")

    let private takenOver =
        lazy (
            try
                let old = Path.Combine(local, "ARMO")
                if not (Directory.Exists folder) && Directory.Exists old then Directory.Move(old, folder)
            with _ -> ())

    /// The folder (an older version's taken over first), or a place in it.
    let path (parts: string[]) =
        takenOver.Force()
        Path.Combine(Array.append [| folder |] parts)

    let needsClean () =
        takenOver.Force()
        try
            not (File.Exists cleanedFile) || File.ReadAllText(cleanedFile).Trim() <> currentVersion
        with _ -> true

    let cleanAndRestart () =
        takenOver.Force()
        let keepDownloadedEngine = not (File.Exists(Path.Combine(AppContext.BaseDirectory, "Engine", "nvngx.dll")))
        try
            if Directory.Exists folder then
                for file in Directory.GetFiles folder do
                    try File.Delete file with _ -> ()
                for dir in Directory.GetDirectories folder do
                    if not (keepDownloadedEngine && String.Equals(Path.GetFileName dir, "EngineFiles", StringComparison.OrdinalIgnoreCase)) then
                        try Directory.Delete(dir, true) with _ -> ()
        with _ -> ()
        if OperatingSystem.IsLinux() then
            let clearDir d =
                try
                    if Directory.Exists d then
                        for f in Directory.GetFiles d do try File.Delete f with _ -> ()
                        for s in Directory.GetDirectories d do try Directory.Delete(s, true) with _ -> ()
                with _ -> ()
            let home = Environment.GetFolderPath Environment.SpecialFolder.UserProfile
            let xdg varName fallback =
                match Environment.GetEnvironmentVariable varName with
                | null | "" -> Path.Combine(home, fallback)
                | v -> v
            clearDir (Path.Combine(xdg "XDG_CACHE_HOME" ".cache", "mfgsm"))
            clearDir (Path.Combine(xdg "XDG_CONFIG_HOME" ".config", "mfgsm"))
        try
            Directory.CreateDirectory folder |> ignore
            File.WriteAllText(cleanedFile, currentVersion)
        with _ -> ()
        try
            let exe = Environment.ProcessPath
            if not (String.IsNullOrEmpty exe) then
                let info = ProcessStartInfo(exe, UseShellExecute = false)
                let argv = Environment.GetCommandLineArgs()
                if Path.GetFileNameWithoutExtension(exe).Equals("dotnet", StringComparison.OrdinalIgnoreCase) then
                    for a in argv do info.ArgumentList.Add a
                else
                    for a in argv |> Array.skip 1 do info.ArgumentList.Add a
                Process.Start info |> ignore
        with _ -> ()
        Environment.Exit 0
