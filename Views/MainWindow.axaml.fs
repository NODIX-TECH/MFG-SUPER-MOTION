namespace ARMO.Views

open Avalonia
open Avalonia.Controls
open Avalonia.Input
open Avalonia.Markup.Xaml
open Avalonia.Media
open Avalonia.Platform.Storage
open Avalonia.Styling
open Avalonia.Threading
open ARMO.Infrastructure
open ARMO.ViewModels
open ARMO.Models

type MainWindow() as this =
    inherit Window()
    let mutable transparencyEnabled = false
    let mutable nativeBackdrop = false
    let mutable firstOpen = true

    do
        this.InitializeComponent()
        TextOptions.SetTextRenderingMode(this, TextRenderingMode.Antialias)
        this.SizeChanged.Add(fun _ -> this.Classes.Set("compact", this.ClientSize.Width < UiMetrics.compactWidth))
        this.Opened.Add(fun _ ->
            if firstOpen then
                firstOpen <- false
                this.FitToScreen()
            this.SetTransparency(transparencyEnabled))
        this.PropertyChanged.Add(fun args ->
            if args.Property = TopLevel.ActualTransparencyLevelProperty then this.UpdateMaterial())
        this.ActualThemeVariantChanged.Add(fun _ ->
            WindowsBackdrop.setTheme (this.NativeHandle()) (this.ActualThemeVariant = ThemeVariant.Dark)
            this.InvalidateVisual())
        // While the shortcut is being picked every key is its: Esc leaves it as
        // it was, Backspace or Delete takes it away, anything else with a
        // modifier becomes the shortcut - and alone, one key that types nothing
        // (F1..F24, Insert, Home, End, Page Up/Down, Pause, Scroll Lock, the
        // number pad's digits): F6 by itself. (A letter, a digit or Space alone
        // would go off while the player types or plays.)
        this.AddHandler(InputElement.KeyDownEvent, (fun _ (args: KeyEventArgs) ->
            match this.DataContext with
            | :? MainViewModel as model when model.IsCapturingShortcut ->
                args.Handled <- true
                match args.Key with
                | Key.Escape -> model.EndShortcutCapture 0
                | Key.Back | Key.Delete -> model.EndShortcutCapture -1
                | Key.LeftCtrl | Key.RightCtrl | Key.LeftAlt | Key.RightAlt | Key.LeftShift | Key.RightShift
                | Key.LWin | Key.RWin | Key.System | Key.None -> ()
                | key ->
                    let has modifier = args.KeyModifiers.HasFlag modifier
                    let modifiers =
                        (if has KeyModifiers.Alt then 1 else 0) ||| (if has KeyModifiers.Control then 2 else 0)
                        ||| (if has KeyModifiers.Shift then 4 else 0) ||| (if has KeyModifiers.Meta then 8 else 0)
                    let name = key.ToString()
                    let vk = Shortcut.virtualKey name
                    let alone = (vk >= 0x70 && vk <= 0x87) || (vk >= 0x60 && vk <= 0x69) || vk = 0x2D || (vk >= 0x21 && vk <= 0x24) || vk = 0x13 || vk = 0x91
                    if vk <> 0 && (modifiers <> 0 || alone) then model.EndShortcutCapture((modifiers <<< 16) ||| vk)
            | _ -> ()), Avalonia.Interactivity.RoutingStrategies.Tunnel)
        this.Deactivated.Add(fun _ ->
            match this.DataContext with
            | :? MainViewModel as model when model.IsCapturingShortcut -> model.EndShortcutCapture 0
            | _ -> ())
        this.KeyDown.Add(fun args ->
            match this.DataContext with
            | :? MainViewModel as model when args.Key = Key.Escape && model.IsDialogOpen ->
                model.CancelDialog()
                args.Handled <- true
            | _ -> ())

    member private this.InitializeComponent() =
        AvaloniaXamlLoader.Load(this)
        // The "?" beside one of the app's own techniques: what it is, in a few words, in a small window of its own.
        for name, pick in [ "AmlWhatIs", (fun (m: MainViewModel) -> m.Text.AdaptiveLearning, m.Text.WhatIsAml)
                            "AfsWhatIs", (fun (m: MainViewModel) -> m.Text.AdaptiveFlow, m.Text.WhatIsAfs) ] do
            let button = this.FindControl<Button>(name)
            if not (isNull button) then
                button.Click.Add(fun _ ->
                    match this.DataContext with
                    | :? MainViewModel as model -> this.ShowWhatIs(pick model)
                    | _ -> ())
        // Crop input's boxes take digits only (round 43).
        // Round 55: Enter or Escape leaves such a box, and so does a click anywhere else (Avalonia keeps the caret
        // in it otherwise); what it would not take is put back to what is kept.
        let leave () = this.Focusable <- true; this.Focus() |> ignore
        for name in [ "CropLeftBox"; "CropTopBox"; "CropRightBox"; "CropBottomBox"; "GameFpsBox"; "AdaptiveFpsBox" ] do
            let box = this.FindControl<Control>(name)
            if not (isNull box) then
                box.AddHandler(InputElement.TextInputEvent, (fun _ (args: TextInputEventArgs) ->
                    if not (isNull args.Text) && args.Text |> Seq.exists (fun c -> not (System.Char.IsDigit c)) then args.Handled <- true),
                               Avalonia.Interactivity.RoutingStrategies.Tunnel)
                box.AddHandler(InputElement.KeyDownEvent, (fun _ (args: KeyEventArgs) ->
                    if args.Key = Key.Enter || args.Key = Key.Escape then
                        leave ()
                        args.Handled <- true),
                               Avalonia.Interactivity.RoutingStrategies.Tunnel)
                box.LostFocus.Add(fun _ ->
                    Dispatcher.UIThread.Post(fun () ->
                        match this.DataContext, box with
                        | (:? MainViewModel as model), _ when box.IsKeyboardFocusWithin -> ()
                        | (:? MainViewModel as model), (:? NumericUpDown as spin) -> spin.Value <- model.AdaptiveTarget
                        | (:? MainViewModel as model), (:? TextBox as text) ->
                            text.Text <-
                                match name with
                                | "GameFpsBox" -> model.GameFpsText
                                | "CropLeftBox" -> model.CropLeftText
                                | "CropTopBox" -> model.CropTopText
                                | "CropRightBox" -> model.CropRightText
                                | _ -> model.CropBottomText
                        | _ -> ()))
        this.AddHandler(InputElement.PointerPressedEvent, (fun _ (args: PointerPressedEventArgs) ->
            match this.FocusManager.GetFocusedElement(), args.Source with
            | (:? TextBox), (:? Visual as source) ->
                // (A click in a box, this one or another, leaves the caret to it.)
                let inBox = Avalonia.VisualTree.VisualExtensions.FindAncestorOfType<TextBox>(source, true)
                if isNull inBox then leave ()
            | _ -> ()),
                        Avalonia.Interactivity.RoutingStrategies.Tunnel, true)
        // Super Video (round 48): the videos to make, and the folder they go to - picked, never assumed.
        let videoModel () = match this.DataContext with | :? MainViewModel as model -> Some model | _ -> None
        let addButton = this.FindControl<Button>("VideoAddButton")
        if not (isNull addButton) then
            addButton.Click.Add(fun _ ->
                match videoModel () with
                | Some model ->
                    task {
                        let videos = FilePickerFileType("Video", Patterns = [| "*.mp4"; "*.mkv"; "*.mov"; "*.avi"; "*.wmv"; "*.webm"; "*.m4v"; "*.ts"; "*.mts"; "*.m2ts"; "*.flv" |])
                        let options = FilePickerOpenOptions(Title = model.Text.VideoAdd, AllowMultiple = true, FileTypeFilter = [| videos; FilePickerFileTypes.All |])
                        let! files = this.StorageProvider.OpenFilePickerAsync(options)
                        files
                        |> Seq.choose (fun f -> match f.TryGetLocalPath() with | null -> None | path -> Some path)
                        |> model.Video.AddPaths
                    } |> ignore
                | None -> ())
        // (Videos dropped on the page are added the same way.)
        let videoCard = this.FindControl<Border>("VideoCard")
        if not (isNull videoCard) then
            DragDrop.SetAllowDrop(videoCard, true)
            videoCard.AddHandler(DragDrop.DropEvent, (fun _ (args: DragEventArgs) ->
                match videoModel (), args.DataTransfer.TryGetFiles() with
                | Some model, files when not (isNull files) && not model.Video.IsRunning ->
                    files
                    |> Seq.choose (fun f -> match f.TryGetLocalPath() with | null -> None | path -> Some path)
                    |> model.Video.AddPaths
                | _ -> ()))
        let folderButton = this.FindControl<Button>("VideoFolderButton")
        if not (isNull folderButton) then
            folderButton.Click.Add(fun _ ->
                match videoModel () with
                | Some model ->
                    task {
                        let options = FolderPickerOpenOptions(Title = model.Text.VideoChooseFolder, AllowMultiple = false)
                        let! folders = this.StorageProvider.OpenFolderPickerAsync(options)
                        match folders |> Seq.tryHead |> Option.bind (fun f -> match f.TryGetLocalPath() with | null -> None | path -> Some path) with
                        | Some path -> model.Video.SetFolder path
                        | None -> ()
                    } |> ignore
                | None -> ())
        // Performance (DLSS MFG's preset C) picked: why it is not advised, in the same small window.
        let mutable heard = null
        this.DataContextChanged.Add(fun _ ->
            match this.DataContext with
            | :? MainViewModel as model when not (obj.ReferenceEquals(heard, model)) ->
                heard <- model
                model.PresetCPicked.Add(fun () -> this.ShowWhatIs(model.Text.PresetPerformance, model.Text.PresetCWarning))
            | _ -> ())

    member private this.ShowWhatIs(title: string, text: string) =
        let panel = StackPanel(Spacing = 16.0, Margin = Thickness(24.0, 20.0, 24.0, 20.0))
        panel.Children.Add(TextBlock(Text = title, FontSize = 18.0, FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap))
        panel.Children.Add(TextBlock(Text = text, FontSize = 14.0, TextWrapping = TextWrapping.Wrap, MaxWidth = 400.0))
        let ok = Button(Content = "OK", MinWidth = 96.0, HorizontalAlignment = Layout.HorizontalAlignment.Right, HorizontalContentAlignment = Layout.HorizontalAlignment.Center)
        ok.Classes.Add "primary"
        panel.Children.Add ok
        let dialog =
            Window(Title = title, Content = panel, SizeToContent = SizeToContent.WidthAndHeight, CanResize = false,
                   ShowInTaskbar = false, WindowStartupLocation = WindowStartupLocation.CenterOwner,
                   FlowDirection = this.FlowDirection, RequestedThemeVariant = this.ActualThemeVariant, Icon = this.Icon)
        dialog.Name <- "WhatIsWindow"
        // (The app's own card colour, as its pages have it, in its light and dark look.)
        match this.TryFindResource("SurfaceBrush", this.ActualThemeVariant) with
        | true, (:? IBrush as brush) -> dialog.Background <- brush
        | _ -> ()
        ok.Click.Add(fun _ -> dialog.Close())
        dialog.ShowDialog(this) |> ignore

    member private _.UpdateMaterial() =
        let level = this.ActualTransparencyLevel
        let supported = level = WindowTransparencyLevel.Mica || level = WindowTransparencyLevel.AcrylicBlur || level = WindowTransparencyLevel.Blur
        this.Classes.Set("material", transparencyEnabled && (supported || nativeBackdrop))

    member private _.NativeHandle() =
        let handle = this.TryGetPlatformHandle()
        if isNull handle then 0n else handle.Handle

    member _.SetTransparency(enabled: bool) =
        transparencyEnabled <- enabled
        if nativeBackdrop then
            WindowsBackdrop.trySet (this.NativeHandle()) false (this.ActualThemeVariant = ThemeVariant.Dark) |> ignore
            nativeBackdrop <- false
        this.TransparencyLevelHint <-
            if enabled then [| WindowTransparencyLevel.AcrylicBlur; WindowTransparencyLevel.Transparent |]
            else [| WindowTransparencyLevel.None |]
        if enabled && this.ActualTransparencyLevel = WindowTransparencyLevel.Transparent then
            nativeBackdrop <- WindowsBackdrop.trySet (this.NativeHandle()) true (this.ActualThemeVariant = ThemeVariant.Dark)
            if not nativeBackdrop then this.TransparencyLevelHint <- [| WindowTransparencyLevel.None |]
        WindowsBackdrop.setTheme (this.NativeHandle()) (this.ActualThemeVariant = ThemeVariant.Dark)
        this.UpdateMaterial()
        this.InvalidateVisual()

    member private _.FitToScreen() =
        let screen = this.Screens.ScreenFromWindow(this)
        if not (isNull screen) then
            let availableWidth = float screen.WorkingArea.Width / screen.Scaling
            let availableHeight = float screen.WorkingArea.Height / screen.Scaling
            let outerWidth, outerHeight =
                WindowsBackdrop.visibleFrameSize (this.NativeHandle()) this.RenderScaling
                |> Option.defaultValue (this.ClientSize.Width, this.ClientSize.Height)
            let decorationWidth = max 0. (outerWidth - this.ClientSize.Width)
            let decorationHeight = max 0. (outerHeight - this.ClientSize.Height)
            let targetWidth = min UiMetrics.defaultWidth (availableWidth * 0.94)
            let targetHeight = min UiMetrics.defaultHeight (availableHeight * 0.94)
            this.MinWidth <- min UiMetrics.minimumWidth (max 1. (targetWidth - decorationWidth))
            this.MinHeight <- min UiMetrics.minimumHeight (max 1. (targetHeight - decorationHeight))
            // Width/Height describe the client area; the reference includes Windows' frame.
            this.Width <- targetWidth - decorationWidth
            this.Height <- targetHeight - decorationHeight
            this.Position <- PixelPoint(
                screen.WorkingArea.X + int ((availableWidth - targetWidth) * screen.Scaling / 2.),
                screen.WorkingArea.Y + int ((availableHeight - targetHeight) * screen.Scaling / 2.))

    member _.FocusProfileName() =
        Dispatcher.UIThread.Post(fun () ->
            let input = this.FindControl<TextBox>("ProfileNameBox")
            if not (isNull input) then
                input.Focus() |> ignore
                input.SelectAll())

