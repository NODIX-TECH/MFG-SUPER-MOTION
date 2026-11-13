namespace ARMO.Views

// App Mods (round 72): mods made by others, run by the app. A mod is a folder - or a .zip of one - with mod.json and a
// .NET 8 library built against MfgSuperMotion.ModSdk.dll (ModKit\README.md says how). The user picks it with "Add
// mod"; only where it is, is kept (%LOCALAPPDATA%\MFG SUPER MOTION\Mods\mods.json), and it is run from there - a zip is
// read as it is, nothing is unpacked. A mod may do anything the app does: pages and controls of its own, any option,
// the engine's lines, the look - but never the app's own download places, its maker or its name (kept as they are:
// Protect, below). A mod that throws is stopped, not the app; a mod that brings the app down while it loads leaves it
// to start without mods the next time (Safe start, below).

open System
open System.Collections.Generic
open System.Diagnostics
open System.IO
open System.IO.Compression
open System.Reflection
open System.Runtime.Loader
open System.Text.Json
open Avalonia
open Avalonia.Controls
open Avalonia.Controls.Primitives
open Avalonia.Controls.Shapes
open Avalonia.Interactivity
open Avalonia.Layout
open Avalonia.Media
open Avalonia.Platform.Storage
open Avalonia.Styling
open Avalonia.Threading
open MfgSuperMotion.Mods
open ARMO.Infrastructure
open ARMO.ViewModels

/// mod.json.
[<CLIMutable>]
type ModManifest =
    { Id: string; Name: string; Version: string; Author: string; Description: string; Entry: string; MinAppVersion: string }

/// One line of mods.json: where a mod is, and whether it is on.
[<CLIMutable>]
type ModEntry = { Path: string; Enabled: bool }

/// A mod's files, wherever they are: a folder (picked by its mod.json or itself) or a .zip - in the zip, mod.json may
/// lie at the top or in one folder (a zip made of the mod's folder).
type ModSource(path: string) =
    let isZip = path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) && File.Exists path
    let folder = if isZip then "" elif File.Exists path then System.IO.Path.GetDirectoryName path else path
    let zipRoot =
        lazy (
            if not isZip then ""
            else
                try
                    use zip = ZipFile.OpenRead path
                    zip.Entries
                    |> Seq.map (fun e -> e.FullName.Replace('\\', '/'))
                    |> Seq.filter (fun n -> n.Equals("mod.json", StringComparison.OrdinalIgnoreCase) || n.EndsWith("/mod.json", StringComparison.OrdinalIgnoreCase))
                    |> Seq.sortBy _.Length
                    |> Seq.tryHead
                    |> Option.map (fun n -> n.Substring(0, n.Length - "mod.json".Length))
                    |> Option.defaultValue ""
                with _ -> "")
    member _.Path = path
    member _.IsZip = isZip
    /// A file of the mod by its path inside it ("images/logo.png"); None when it is not there.
    member _.Read(relative: string) : byte[] option =
        if String.IsNullOrWhiteSpace relative || relative.Contains ".." then None
        else
            let name = relative.Replace('\\', '/').TrimStart('/')
            try
                if isZip then
                    use zip = ZipFile.OpenRead path
                    let wanted = zipRoot.Force() + name
                    match zip.Entries |> Seq.tryFind (fun e -> e.FullName.Replace('\\', '/').Equals(wanted, StringComparison.OrdinalIgnoreCase)) with
                    | Some entry ->
                        use stream = entry.Open()
                        use memory = new MemoryStream()
                        stream.CopyTo memory
                        Some (memory.ToArray())
                    | None -> None
                else
                    let file = System.IO.Path.Combine(folder, name.Replace('/', System.IO.Path.DirectorySeparatorChar))
                    if File.Exists file then Some (File.ReadAllBytes file) else None
            with _ -> None

/// Where a mod's own libraries come from: what the app has (.NET, Avalonia, the mod kit) is the app's - one Avalonia
/// for the window and every mod - and the rest is looked for in the mod itself.
type ModLoadContext(source: ModSource, name: string) =
    inherit AssemblyLoadContext(name, false)
    override this.Load(wanted: AssemblyName) =
        let has =
            AssemblyLoadContext.Default.Assemblies |> Seq.exists (fun a -> a.GetName().Name = wanted.Name)
            || File.Exists(System.IO.Path.Combine(AppContext.BaseDirectory, wanted.Name + ".dll"))
        if has then null
        else
            match source.Read(wanted.Name + ".dll") with
            | Some bytes -> this.LoadFromStream(new MemoryStream(bytes))
            | None -> null

/// What became of a mod listed in mods.json.
type ModState =
    { Entry: ModEntry
      Manifest: ModManifest option
      Loaded: bool
      Problem: string          // why it is not loaded, or what stopped it
      RestartNeeded: bool }    // turned off or removed while loaded: gone at the next start

type ModManager(window: MainWindow, model: MainViewModel) as this =
    let folder = AppData.path [| "Mods" |]
    let listFile = System.IO.Path.Combine(folder, "mods.json")
    let logFile = System.IO.Path.Combine(folder, "mods.log")
    let loadingMark = System.IO.Path.Combine(folder, "loading")
    let json = JsonSerializerOptions(PropertyNameCaseInsensitive = true, WriteIndented = true)
    let states = List<ModState>()
    let apis = Dictionary<string, obj>()                       // mod id -> its host (ModApi)
    let modAssemblies = Dictionary<Assembly, string>()        // a mod's library -> the mod's name
    let engineExtras = Dictionary<string, Dictionary<string, string>>()
    let mutable crashedName = ""     // Safe start: the mod that was loading when the app went down last time (turned off)
    let mutable note = ""

    // ---- the log
    let log (text: string) =
        try
            Directory.CreateDirectory folder |> ignore
            File.AppendAllText(logFile, sprintf "%s  %s%s" (DateTime.Now.ToString "yyyy-MM-dd HH:mm:ss") text Environment.NewLine)
        with _ -> ()

    // ---- mods.json
    let readList () =
        try
            if File.Exists listFile then
                match JsonSerializer.Deserialize<ModEntry[]>(File.ReadAllText listFile, json) with
                | null -> [||]
                | entries -> entries |> Array.filter (fun e -> not (isNull (box e)) && not (String.IsNullOrWhiteSpace e.Path))
            else [||]
        with _ -> [||]
    let saveList () =
        try
            Directory.CreateDirectory folder |> ignore
            File.WriteAllText(listFile, JsonSerializer.Serialize(states |> Seq.map _.Entry |> Seq.toArray, json))
        with ex -> log ("mods.json could not be written: " + ex.Message)

    let readManifest (source: ModSource) =
        match source.Read "mod.json" with
        | None -> None
        | Some bytes ->
            try
                match JsonSerializer.Deserialize<ModManifest>(System.Text.Encoding.UTF8.GetString(bytes).TrimStart(char 0xFEFF), json) with
                | m when not (isNull (box m)) && not (String.IsNullOrWhiteSpace m.Id) ->
                    let orEmpty (s: string) = if isNull s then "" else s.Trim()
                    Some { m with Id = m.Id.Trim(); Name = (if String.IsNullOrWhiteSpace m.Name then m.Id.Trim() else m.Name.Trim())
                                  Version = orEmpty m.Version; Author = orEmpty m.Author; Description = orEmpty m.Description
                                  Entry = orEmpty m.Entry; MinAppVersion = orEmpty m.MinAppVersion }
                | _ -> None
            with _ -> None

    let appVersion =
        let v = Assembly.GetEntryAssembly().GetName().Version
        if isNull v then "0.0.0" else sprintf "%d.%d.%d" v.Major v.Minor (max 0 v.Build)

    // ---- the engine: every line to every mod; the mods' start lines together
    let engineLine = Event<string>()
    do model.ModEngine.Line.Add(fun line -> engineLine.Trigger line)
    let pushExtras () =
        let merged = Dictionary<string, string>()
        for KeyValue(_, lines) in engineExtras do
            for KeyValue(key, value) in lines do merged[key] <- value
        model.ModEngine.SetExtras [ for KeyValue(k, v) in merged -> k, v ]

    // ---- the pages: slots at the end of the app's own, and the mods' own pages
    let slotNames = dict [ Pages.Display, "ModSlotDisplay"; Pages.Video, "ModSlotVideo"; Pages.Support, "ModSlotSupport"
                           Pages.Settings, "ModSlotSettings"; Pages.Help, "ModSlotHelp" ]
    let modsSlot = StackPanel(Spacing = 12.0, IsVisible = false)
    let slotItems = Dictionary<StackPanel, List<int * int * Control>>()
    let mutable sequence = 0
    let modPages = Dictionary<string, ModPage * Button * Border * StackPanel * bool ref>()   // page, its button, its card, its slot, built
    let navList () = window.FindControl<StackPanel>("ModNav")
    let pagesPanel () = window.FindControl<StackPanel>("ModPages")
    let slotFor (page: string) =
        if isNull page then None
        elif page = Pages.Mods then Some modsSlot
        else
            match slotNames.TryGetValue page with
            | true, name -> window.FindControl<StackPanel>(name) |> Option.ofObj
            | _ ->
                match modPages.TryGetValue page with
                | true, (_, _, _, slot, _) -> Some slot
                | _ -> None
    let place (slot: StackPanel) (order: int) (control: Control) =
        let items = match slotItems.TryGetValue slot with | true, l -> l | _ -> (let l = List() in slotItems[slot] <- l; l)
        sequence <- sequence + 1
        items.Add((order, sequence, control))
        slot.Children.Clear()
        for _, _, c in items |> Seq.sortBy (fun (o, s, _) -> o, s) do slot.Children.Add c
        slot.IsVisible <- true
    let showPages () =
        let current = model.CurrentPage
        for KeyValue(id, (page, button, card, _, built)) in modPages do
            let shown = current = id
            if shown && not built.Value then
                built.Value <- true
                try
                    match page.Content.Invoke() with
                    | null -> ()
                    | content -> (card.Child :?> StackPanel).Children.Insert(0, content)
                with ex -> log (sprintf "%s: its page could not be made: %s" id (ex.ToString()))
            card.IsVisible <- shown
            button.Classes.Set("active", shown)
    let pageChanged = Event<EventHandler<string>, string>()
    let languageChanged = Event<EventHandler<string>, string>()

    // ---- Protect: the app's own download places, its maker, its name and version stay as the app has them. Looked at
    // every two seconds while mods are loaded, and put back. (A mod is code run with the app's rights: this keeps a mod
    // from changing them by the app's own controls; it is no lock against a mod written to do harm - Add mod says to
    // install mods only from people one trusts.)
    let guarded = List<Control * Panel * int * (unit -> unit)>()
    let guard (control: Control) (restore: unit -> unit) =
        if not (isNull control) then
            match control.Parent with
            | :? Panel as parent -> guarded.Add((control, parent, parent.Children.IndexOf control, restore))
            | _ -> guarded.Add((control, null, -1, restore))
    let protectedLinks = set [ "github"; "site"; "youtube"; "app-releases"; "patreon" ]
    let rec descendants (c: Control) : Control seq =
        seq {
            yield c
            match c with
            | :? Panel as p -> for child in p.Children do yield! descendants child
            | :? Decorator as d when not (isNull d.Child) -> yield! descendants d.Child
            | :? ContentControl as cc ->
                match cc.Content with
                | :? Control as child -> yield! descendants child
                | _ -> ()
            | _ -> ()
        }
    let watchProtected () =
        let keepShown (c: Control) () =
            if not c.IsVisible then c.IsVisible <- true
            if c.Opacity < 1.0 then c.Opacity <- 1.0
        let all = descendants window |> Seq.toArray
        // Every button of a protected link: its target, its command, its words, shown.
        for c in all do
            match c with
            | :? Button as b when (match b.CommandParameter with :? string as s -> protectedLinks.Contains s | _ -> false) ->
                let parameter, command, content = b.CommandParameter, b.Command, b.Content
                let tip = ToolTip.GetTip b
                let visible = b.IsVisible
                guard b (fun () ->
                    if not (obj.ReferenceEquals(b.CommandParameter, parameter)) && b.CommandParameter <> parameter then b.CommandParameter <- parameter
                    if not (obj.ReferenceEquals(b.Command, command)) then b.Command <- command
                    match content with
                    | :? string as s when (match b.Content with :? string as now -> now <> s | _ -> true) -> b.Content <- s
                    | _ -> ()
                    if not (obj.Equals(ToolTip.GetTip b, tip)) then ToolTip.SetTip(b, tip)
                    if visible then keepShown b ())
            | _ -> ()
        // The Help page's official places and its maker.
        match window.FindControl<Border>("HelpOfficial") with
        | null -> ()
        | official ->
            guard official (keepShown official)
            match official.Child with
            | :? StackPanel as panel ->
                let texts = panel.Children |> Seq.choose (function :? TextBlock as t -> Some t | _ -> None) |> Seq.truncate 2 |> Seq.toArray
                if texts.Length = 2 then
                    guard texts[0] (fun () -> keepShown texts[0] (); if texts[0].Text <> model.Text.HelpOfficial then texts[0].Text <- model.Text.HelpOfficial)
                    guard texts[1] (fun () -> keepShown texts[1] (); if texts[1].Text <> model.Text.HelpMaker then texts[1].Text <- model.Text.HelpMaker)
            | _ -> ()
        // The version and the look for a newer app.
        match window.FindControl<TextBlock>("VersionText") with
        | null -> ()
        | version -> guard version (fun () -> keepShown version (); if version.Text <> model.VersionText then version.Text <- model.VersionText)
        match window.FindControl<Button>("CheckUpdatesButton") with
        | null -> ()
        | check ->
            let command = check.Command
            guard check (fun () -> keepShown check (); if not (obj.ReferenceEquals(check.Command, command)) then check.Command <- command)
    let protectTimer = DispatcherTimer(Interval = TimeSpan.FromSeconds 2.0)
    do protectTimer.Tick.Add(fun _ ->
        try
            if window.Title <> "MFG SUPER MOTION" then window.Title <- "MFG SUPER MOTION"
            for control, parent, index, restore in guarded do
                // (Taken out of its place: put back where it was.)
                if not (isNull parent) && not (obj.ReferenceEquals(control.Parent, parent)) then
                    match control.Parent with
                    | :? Panel as other -> other.Children.Remove control |> ignore
                    | :? Decorator as d -> d.Child <- null
                    | :? ContentControl as cc -> cc.Content <- null
                    | _ -> ()
                    parent.Children.Insert(min index parent.Children.Count, control)
                restore ()
        with ex -> log ("protect: " + ex.Message))

    // ---- a mod that throws on the window's thread is stopped there, not the app
    let fromMod (ex: exn) : string option =
        let rec all (e: exn) = seq { if not (isNull e) then yield e; if not (isNull e) && not (isNull e.InnerException) then yield! all e.InnerException }
        all ex |> Seq.tryPick (fun e ->
            try
                StackTrace(e, false).GetFrames()
                |> Array.tryPick (fun f ->
                    match f.GetMethod() with
                    | null -> None
                    | m when not (isNull m.Module) ->
                        match modAssemblies.TryGetValue m.Module.Assembly with
                        | true, name -> Some name
                        | _ -> None
                    | _ -> None)
            with _ -> None)
    let mutable rebuild = fun () -> ()

    // ---- loading one mod
    let rec load (index: int) =
        // (Safe start: while a mod loads, the mark says which - left behind, the app went down in it.)
        try File.WriteAllText(loadingMark, states[index].Entry.Path) with _ -> ()
        try loadOne index
        finally try File.Delete loadingMark with _ -> ()
    and loadOne (index: int) =
        let state = states[index]
        let source = ModSource(state.Entry.Path)
        match readManifest source with
        | None -> states[index] <- { state with Manifest = None; Loaded = false; Problem = model.Text.ModsBadFile }
        | Some manifest ->
            let problem =
                if apis.ContainsKey manifest.Id then Some model.Text.ModsAlready
                elif manifest.MinAppVersion <> "" && (match Version.TryParse manifest.MinAppVersion, Version.TryParse appVersion with
                                                     | (true, need), (true, have) -> need > have
                                                     | _ -> false) then
                    Some (sprintf "MFG SUPER MOTION %s+" manifest.MinAppVersion)
                else None
            match problem with
            | Some p -> states[index] <- { state with Manifest = Some manifest; Loaded = false; Problem = p }
            | None ->
                try
                    let entry =
                        if manifest.Entry <> "" then manifest.Entry
                        else manifest.Id + ".dll"
                    let bytes = match source.Read entry with Some b -> b | None -> failwithf "%s is not in the mod" entry
                    let symbols = source.Read(System.IO.Path.ChangeExtension(entry, ".pdb"))
                    let context = ModLoadContext(source, "mod:" + manifest.Id)
                    let assembly =
                        match symbols with
                        | Some pdb -> context.LoadFromStream(new MemoryStream(bytes), new MemoryStream(pdb))
                        | None -> context.LoadFromStream(new MemoryStream(bytes))
                    modAssemblies[assembly] <- manifest.Name
                    let kind =
                        assembly.GetTypes()
                        |> Array.tryFind (fun t -> t.IsClass && not t.IsAbstract && t.IsPublic && typeof<IMod>.IsAssignableFrom t)
                    match kind with
                    | None -> failwith "it has no public class that implements IMod"
                    | Some t ->
                        let instance = Activator.CreateInstance t :?> IMod
                        let data = System.IO.Path.Combine(folder, "Data", manifest.Id)
                        Directory.CreateDirectory data |> ignore
                        let api = this.MakeApi(manifest.Id, source, data)
                        apis[manifest.Id] <- api
                        instance.Load(api :?> IModHost)
                        states[index] <- { state with Manifest = Some manifest; Loaded = true; Problem = "" }
                        log (sprintf "%s %s loaded from %s" manifest.Id manifest.Version state.Entry.Path)
                with ex ->
                    let ex = match ex with :? TargetInvocationException as t when not (isNull t.InnerException) -> t.InnerException | e -> e
                    log (sprintf "%s could not be loaded: %s" manifest.Id (ex.ToString()))
                    states[index] <- { state with Manifest = Some manifest; Loaded = false; Problem = ex.Message }

    // ---- the App Mods page
    let switchTheme = match window.TryFindResource("ArmoSwitch") with | true, (:? ControlTheme as t) -> Some t | _ -> None
    let modsPage () =
        let text = model.Text
        let page = StackPanel(Spacing = 16.0, MaxWidth = 720.0, HorizontalAlignment = HorizontalAlignment.Stretch)
        let header = StackPanel(Spacing = 10.0, HorizontalAlignment = HorizontalAlignment.Center)
        match window.TryFindResource("ModsIcon") with
        | true, (:? Geometry as icon) ->
            let path = Path(Data = icon, StrokeThickness = 1.6, StrokeJoin = PenLineJoin.Round, Width = 44.0, Height = 44.0, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center)
            path.Bind(Shape.StrokeProperty, window.GetResourceObservable "AccentBrush") |> ignore
            header.Children.Add path
        | _ -> ()
        let title = TextBlock(Text = "App Mods", FontSize = 24.0, HorizontalAlignment = HorizontalAlignment.Center, FlowDirection = FlowDirection.LeftToRight)
        title.Classes.Add "heading"
        header.Children.Add title
        page.Children.Add header
        page.Children.Add(TextBlock(Text = text.ModsIntro, FontSize = 14.0, TextWrapping = TextWrapping.Wrap, LineHeight = 22.0))
        let buttons = StackPanel(Orientation = Orientation.Horizontal, Spacing = 12.0)
        let add = Button(Content = text.ModsAdd, Name = "ModsAddButton")
        add.Classes.Add "primary"
        add.Click.Add(fun _ -> this.AddFromPicker())
        let kit = Button(Content = text.ModsKit, Name = "ModsKitButton")
        kit.Click.Add(fun _ ->
            let kitFolder = System.IO.Path.Combine(AppContext.BaseDirectory, "ModKit")
            if Directory.Exists kitFolder then
                try Process.Start(ProcessStartInfo("explorer.exe", "\"" + kitFolder + "\"", UseShellExecute = true)) |> ignore with _ -> ())
        buttons.Children.Add add
        buttons.Children.Add kit
        page.Children.Add buttons
        let muted (s: string) =
            let t = TextBlock(Text = s, FontSize = 13.0, TextWrapping = TextWrapping.Wrap)
            t.Classes.Add "muted"
            t
        page.Children.Add(muted text.ModsWarning)
        if crashedName <> "" then
            let t = TextBlock(Text = String.Format(text.ModsSafeStart, crashedName), FontSize = 13.0, TextWrapping = TextWrapping.Wrap)
            t.Bind(TextBlock.ForegroundProperty, window.GetResourceObservable "AccentBrush") |> ignore
            page.Children.Add t
        if note <> "" then
            let t = TextBlock(Text = note, FontSize = 13.0, TextWrapping = TextWrapping.Wrap)
            t.Bind(TextBlock.ForegroundProperty, window.GetResourceObservable "AccentBrush") |> ignore
            page.Children.Add t
        if states.Count = 0 then page.Children.Add(muted text.ModsEmpty)
        for i in 0 .. states.Count - 1 do
            let state = states[i]
            let card = Border(CornerRadius = CornerRadius 12.0, Padding = Thickness(16.0, 12.0))
            card.Bind(Border.BackgroundProperty, window.GetResourceObservable "SelectionBrush") |> ignore
            let grid = Grid(ColumnDefinitions = ColumnDefinitions "*,Auto")
            let info = StackPanel(Spacing = 4.0)
            let name =
                match state.Manifest with
                | Some m -> if m.Version <> "" then sprintf "%s  %s" m.Name m.Version else m.Name
                | None -> System.IO.Path.GetFileName(state.Entry.Path.TrimEnd('\\', '/'))
            info.Children.Add(TextBlock(Text = name, FontSize = 15.0, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap, FlowDirection = FlowDirection.LeftToRight, HorizontalAlignment = HorizontalAlignment.Left))
            match state.Manifest with
            | Some m when m.Author <> "" -> info.Children.Add(muted (String.Format(text.ModsBy, m.Author)))
            | _ -> ()
            match state.Manifest with
            | Some m when m.Description <> "" -> info.Children.Add(TextBlock(Text = m.Description, FontSize = 13.0, TextWrapping = TextWrapping.Wrap))
            | _ -> ()
            let where = muted state.Entry.Path
            where.FontSize <- 11.0
            where.FlowDirection <- FlowDirection.LeftToRight
            where.HorizontalAlignment <- HorizontalAlignment.Left
            info.Children.Add where
            let status =
                if state.RestartNeeded then text.ModsRestart
                elif state.Problem <> "" && state.Entry.Enabled then String.Format(text.ModsFailed, state.Problem)
                else ""
            if status <> "" then
                let t = TextBlock(Text = status, FontSize = 12.0, TextWrapping = TextWrapping.Wrap)
                t.Bind(TextBlock.ForegroundProperty, window.GetResourceObservable "AccentBrush") |> ignore
                info.Children.Add t
            grid.Children.Add info
            let controls = StackPanel(Orientation = Orientation.Horizontal, Spacing = 12.0, VerticalAlignment = VerticalAlignment.Center, Margin = Thickness(12.0, 0.0, 0.0, 0.0))
            let switch = ToggleButton(IsChecked = state.Entry.Enabled, VerticalAlignment = VerticalAlignment.Center)
            match switchTheme with Some t -> switch.Theme <- t | None -> ()
            Automation.AutomationProperties.SetName(switch, name)
            let index = i
            switch.IsCheckedChanged.Add(fun _ -> this.SetEnabled(index, switch.IsChecked = Nullable true))
            let remove = Button(Content = text.ModsRemove, VerticalAlignment = VerticalAlignment.Center)
            remove.Click.Add(fun _ -> this.Remove index)
            controls.Children.Add switch
            controls.Children.Add remove
            Grid.SetColumn(controls, 1)
            grid.Children.Add controls
            card.Child <- grid
            page.Children.Add card
        (match modsSlot.Parent with :? Panel as p -> p.Children.Remove modsSlot |> ignore | _ -> ())
        page.Children.Add modsSlot
        page
    do rebuild <- fun () ->
        match window.FindControl<Border>("ModsCard") with
        | null -> ()
        | card ->
            card.Padding <- Thickness(28.0, 32.0)
            card.Child <- modsPage ()

    // ---- start: the list, Safe start, each mod that is on
    member _.Start() =
        try Directory.CreateDirectory folder |> ignore with _ -> ()
        for entry in readList () do states.Add { Entry = entry; Manifest = None; Loaded = false; Problem = ""; RestartNeeded = false }
        // Safe start: the mark is left by a start that never got through a mod's loading - the app went down in it. That
        // mod alone is turned off; the others load as ever.
        let crashed = if File.Exists loadingMark then (try File.ReadAllText(loadingMark).Trim() with _ -> "") else ""
        try File.Delete loadingMark with _ -> ()
        watchProtected ()
        Dispatcher.UIThread.UnhandledException.Add(fun args ->
            match fromMod args.Exception with
            | Some name ->
                args.Handled <- true
                log (name + " threw on the window's thread: " + args.Exception.ToString())
                note <- name + " - " + String.Format(model.Text.ModsRunError, args.Exception.Message)
                Dispatcher.UIThread.Post(fun () -> rebuild ())
            | None -> ())
        model.PropertyChanged.Add(fun args ->
            match args.PropertyName with
            | "CurrentPage" ->
                showPages ()
                (try pageChanged.Trigger(this, model.CurrentPage) with ex -> log ("PageChanged: " + ex.Message))
            | "Text" ->
                rebuild ()
                (try languageChanged.Trigger(this, model.LanguageCode) with ex -> log ("LanguageChanged: " + ex.Message))
            | _ -> ())
        for i in 0 .. states.Count - 1 do
            let s = states[i]
            if s.Entry.Enabled && crashed <> "" && String.Equals(s.Entry.Path, crashed, StringComparison.OrdinalIgnoreCase) then
                let manifest = readManifest (ModSource s.Entry.Path)
                states[i] <- { s with Entry = { s.Entry with Enabled = false }; Manifest = manifest }
                crashedName <- (match manifest with Some m -> m.Name | None -> System.IO.Path.GetFileName(s.Entry.Path.TrimEnd('\\', '/')))
                saveList ()
                log (sprintf "the app went down while %s loaded last time: it is turned off" crashedName)
            elif s.Entry.Enabled then load i
            else states[i] <- { s with Manifest = readManifest (ModSource s.Entry.Path) }
        if apis.Count > 0 then protectTimer.Start()
        rebuild ()
        showPages ()

    member _.AddFromPicker() =
        task {
            let text = model.Text
            let mods = FilePickerFileType("Mod", Patterns = [| "*.zip"; "mod.json" |])
            let options = FilePickerOpenOptions(Title = text.ModsAdd, AllowMultiple = false, FileTypeFilter = [| mods; FilePickerFileTypes.All |])
            let! files = window.StorageProvider.OpenFilePickerAsync(options)
            match files |> Seq.tryHead |> Option.bind (fun f -> match f.TryGetLocalPath() with | null -> None | p -> Some p) with
            | Some path -> this.Add path
            | None -> ()
        } |> ignore

    /// Add a mod by where it is: its .zip, its mod.json, or its folder.
    member _.Add(path: string) =
        let path =
            if File.Exists path && System.IO.Path.GetFileName(path).Equals("mod.json", StringComparison.OrdinalIgnoreCase) then System.IO.Path.GetDirectoryName path
            else path
        note <- ""
        match readManifest (ModSource path) with
        | None -> note <- model.Text.ModsBadFile
        | Some manifest ->
            let same = states |> Seq.exists (fun s ->
                String.Equals(System.IO.Path.GetFullPath s.Entry.Path, System.IO.Path.GetFullPath path, StringComparison.OrdinalIgnoreCase)
                || (match s.Manifest with Some m -> m.Id = manifest.Id | None -> false))
            if same then note <- model.Text.ModsAlready
            else
                states.Add { Entry = { Path = path; Enabled = true }; Manifest = Some manifest; Loaded = false; Problem = ""; RestartNeeded = false }
                saveList ()
                load (states.Count - 1)
                if apis.Count > 0 then protectTimer.Start()
                showPages ()
        rebuild ()

    member _.SetEnabled(index: int, on: bool) =
        if index >= 0 && index < states.Count && states[index].Entry.Enabled <> on then
            let s = states[index]
            states[index] <- { s with Entry = { s.Entry with Enabled = on }; RestartNeeded = s.Loaded && not on }
            saveList ()
            if on && not s.Loaded then
                states[index] <- { states[index] with RestartNeeded = false }
                load index
                if apis.Count > 0 then protectTimer.Start()
                showPages ()
            elif on && s.Loaded then states[index] <- { states[index] with RestartNeeded = false }
            Dispatcher.UIThread.Post(fun () -> rebuild ())

    member _.Remove(index: int) =
        if index >= 0 && index < states.Count then
            let s = states[index]
            states.RemoveAt index
            saveList ()
            note <- if s.Loaded then model.Text.ModsRestart else ""
            Dispatcher.UIThread.Post(fun () -> rebuild ())

    // ---- what a mod is handed
    member internal _.MakeApi(modId: string, source: ModSource, data: string) : obj =
        let engineHandlers = Event<EventHandler<string>, string>()
        engineLine.Publish.Add(fun line ->
            try engineHandlers.Trigger(null, line)
            with ex -> log (sprintf "%s: EngineLine threw: %s" modId ex.Message))
        let pages = pageChanged
        let languages = languageChanged
        { new IModHost with
            member _.ModId = modId
            member _.AppVersion = appVersion
            member _.Language = model.LanguageCode
            member _.RightToLeft = model.IsRightToLeft
            [<CLIEvent>]
            member _.LanguageChanged = languages.Publish
            member _.MainWindow = window :> Window
            member _.ViewModel = box model
            member _.AddPage(page) =
                if not (isNull page) then
                    let id = modId + "/" + (if String.IsNullOrWhiteSpace page.Id then "page" else page.Id.Trim())
                    if not (modPages.ContainsKey id) then
                        let button = Button(Name = "ModNav_" + id.Replace('/', '_').Replace('.', '_'))
                        button.Classes.Add "nav"
                        button.Classes.Add "utility"
                        Automation.AutomationProperties.SetName(button, page.Title)
                        let row = Grid(ColumnDefinitions = ColumnDefinitions "16,*")
                        let icon = Path(Data = (try Geometry.Parse(if String.IsNullOrWhiteSpace page.Icon then "M 3,3 L 21,3 L 21,21 L 3,21 Z" else page.Icon) with _ -> Geometry.Parse "M 3,3 L 21,3 L 21,21 L 3,21 Z"))
                        icon.Classes.Add "nav-icon"
                        row.Children.Add icon
                        let label = TextBlock(Text = page.Title, Margin = Thickness(10.0, 0.0, 0.0, 0.0), TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center)
                        Grid.SetColumn(label, 1)
                        row.Children.Add label
                        button.Content <- row
                        button.Click.Add(fun _ -> model.Navigate id)
                        let card = Border(IsVisible = false, VerticalAlignment = VerticalAlignment.Top)
                        card.Classes.Add "card"
                        let inside = StackPanel(Spacing = 12.0)
                        let slot = StackPanel(Spacing = 12.0, IsVisible = false)
                        inside.Children.Add slot
                        card.Child <- inside
                        modPages[id] <- (page, button, card, slot, ref false)
                        match navList () with null -> () | nav -> nav.Children.Add button
                        match pagesPanel () with null -> () | panel -> panel.Children.Add card
                        showPages ()
            member _.AddToPage(page, control, order) =
                if not (isNull control) then
                    match slotFor page with
                    | Some slot -> place slot order control
                    | None -> log (sprintf "%s: AddToPage - no page \"%s\"" modId page)
            member _.ShowPage(page) = model.Navigate page
            member _.CurrentPage = model.CurrentPage
            [<CLIEvent>]
            member _.PageChanged = pages.Publish
            member _.OptionNames = model.OptionNames :> IReadOnlyList<string>
            member _.GetOption(name) = model.GetOptionText name
            member _.SetOption(name, value) = model.SetOptionText(name, value)
            member _.IsMotionRunning = model.IsMotionActive
            member _.ToggleMotion() = model.ToggleMotion()
            member _.SendToEngine(key, value) = if not (String.IsNullOrWhiteSpace key) then model.ModEngine.SendLine(key, value)
            member _.SetEngineStartOption(key, value) =
                if not (String.IsNullOrWhiteSpace key) then
                    let lines = match engineExtras.TryGetValue modId with | true, d -> d | _ -> (let d = Dictionary() in engineExtras[modId] <- d; d)
                    lines[key.Trim()] <- (if isNull value then "" else value)
                    pushExtras ()
            [<CLIEvent>]
            member _.EngineLine = engineHandlers.Publish
            member _.ReadFile(path) = match source.Read path with Some b -> b | None -> null
            member _.DataFolder = data
            member _.Log(text) = log (modId + ": " + text)
            member _.ShowMessage(title, text) = window.ShowWhatIs(title, text)
            member _.OpenUrl(url) =
                if not (isNull url) && (url.StartsWith("https://", StringComparison.OrdinalIgnoreCase) || url.StartsWith("http://", StringComparison.OrdinalIgnoreCase)) then
                    try Process.Start(ProcessStartInfo(url, UseShellExecute = true)) |> ignore with _ -> () }
        |> box
