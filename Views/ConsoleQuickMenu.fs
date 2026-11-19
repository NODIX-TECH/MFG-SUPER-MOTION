namespace ARMO.Views

open System
open System.ComponentModel
open System.Runtime.InteropServices
open Avalonia
open Avalonia.Controls
open Avalonia.Controls.Primitives
open Avalonia.Input
open Avalonia.Layout
open Avalonia.Media
open Avalonia.Styling
open Avalonia.Threading
open ARMO.ViewModels

module private QuickMenuNative =
    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type RECT =
        val Left: int
        val Top: int
        val Right: int
        val Bottom: int

    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type POINT =
        val X: int
        val Y: int

    [<DllImport("user32.dll")>]
    extern bool GetWindowRect(nativeint hwnd, RECT& rect)

    [<DllImport("user32.dll")>]
    extern bool GetCursorPos(POINT& point)

    [<DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")>]
    extern nativeint GetWindowLongPtr64(nativeint hWnd, int nIndex)

    [<DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")>]
    extern nativeint SetWindowLongPtr64(nativeint hWnd, int nIndex, nativeint dwNewLong)

    [<DllImport("user32.dll")>]
    extern bool SetWindowPos(nativeint hWnd, nativeint hWndInsertAfter, int X, int Y, int cx, int cy, uint32 uFlags)

    let GWL_EXSTYLE = -20
    let WS_EX_TOPMOST = 0x00000008n
    let WS_EX_TOOLWINDOW = 0x00000080n
    let WS_EX_NOACTIVATE = 0x08000000n
    let HWND_TOPMOST = nativeint -1
    let SWP_NOSIZE = 0x0001u
    let SWP_NOMOVE = 0x0002u
    let SWP_NOACTIVATE = 0x0010u
    let SWP_FRAMECHANGED = 0x0020u
    let SWP_SHOWWINDOW = 0x0040u

    let applyNoActivateTopmost (hwnd: nativeint) =
        if hwnd <> 0n then
            let cur = GetWindowLongPtr64(hwnd, GWL_EXSTYLE)
            let desired = cur ||| WS_EX_NOACTIVATE ||| WS_EX_TOOLWINDOW ||| WS_EX_TOPMOST
            if desired <> cur then SetWindowLongPtr64(hwnd, GWL_EXSTYLE, desired) |> ignore
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE ||| SWP_NOSIZE ||| SWP_NOACTIVATE ||| SWP_SHOWWINDOW ||| SWP_FRAMECHANGED) |> ignore

/// Compact, zero-activation floating Quick Menu for Console Mode (opened/toggled by F1 or Guide+Start / PS+Options).
type ConsoleQuickMenu(model: MainViewModel, onBack: unit -> unit, onOpenFullApp: unit -> unit) as this =
    inherit Window()

    let mutable syncing = false
    let mutable dragging = false
    let mutable dragStartCursorX = 0
    let mutable dragStartCursorY = 0
    let mutable dragStartWinX = 0
    let mutable dragStartWinY = 0

    let bgOuter = SolidColorBrush(Color.Parse("#F00D121C"))
    let borderOuter = SolidColorBrush(Color.Parse("#701E9BFA"))
    let bgSection = SolidColorBrush(Color.Parse("#E0151C28"))
    let borderSection = SolidColorBrush(Color.Parse("#22FFFFFF"))
    let bgSubtle = SolidColorBrush(Color.Parse("#202838"))
    let borderSubtle = SolidColorBrush(Color.Parse("#364256"))
    let bgAccent = SolidColorBrush(Color.Parse("#1E9BFA"))
    let bgViolet = SolidColorBrush(Color.Parse("#7C3AED"))
    let borderViolet = SolidColorBrush(Color.Parse("#A78BFA"))
    let bgGreen = SolidColorBrush(Color.Parse("#76B900"))
    let bgDanger = SolidColorBrush(Color.Parse("#3B1B22"))
    let borderDanger = SolidColorBrush(Color.Parse("#99EF4444"))
    let fgDanger = SolidColorBrush(Color.Parse("#FCA5A5"))
    let fgPrimary = SolidColorBrush(Color.Parse("#F1F5F9"))
    let fgMuted = SolidColorBrush(Color.Parse("#94A3B8"))
    let fgCyan = SolidColorBrush(Color.Parse("#7DD3FC"))

    let makeStack (orientation: Orientation) (spacing: float) (items: Control seq) =
        let p = StackPanel(Orientation = orientation, Spacing = spacing)
        for item in items do p.Children.Add(item)
        p

    let makeCard (child: Control) =
        Border(
            Background = bgSection,
            BorderBrush = borderSection,
            BorderThickness = Thickness(1.0),
            CornerRadius = CornerRadius(10.0),
            Padding = Thickness(10.0, 8.0),
            Child = child
        )

    let makePill (label: string) (onClick: unit -> unit) =
        let tb = TextBlock(Text = label, TextTrimming = TextTrimming.CharacterEllipsis, FontSize = 11.5, FontWeight = FontWeight.SemiBold, TextAlignment = TextAlignment.Center)
        let btn =
            Button(
                Content = tb,
                Padding = Thickness(6.0, 5.0),
                CornerRadius = CornerRadius(7.0),
                BorderThickness = Thickness(1.0),
                HorizontalAlignment = HorizontalAlignment.Stretch,
                HorizontalContentAlignment = HorizontalAlignment.Center,
                VerticalContentAlignment = VerticalAlignment.Center
            )
        btn.Tag <- tb
        btn.Click.Add(fun _ -> if not syncing then onClick ())
        btn

    let setPillText (btn: Button) (txt: string) =
        match btn.Tag with
        | :? TextBlock as tb -> tb.Text <- txt
        | _ -> btn.Content <- txt

    let stylePill (btn: Button) (active: bool) (enabled: bool) (violetAccent: bool) =
        btn.IsEnabled <- enabled
        if active && violetAccent then
            btn.Background <- bgViolet
            btn.BorderBrush <- borderViolet
            btn.Foreground <- Brushes.White
        elif active then
            btn.Background <- bgAccent
            btn.BorderBrush <- bgAccent
            btn.Foreground <- Brushes.White
        else
            btn.Background <- bgSubtle
            btn.BorderBrush <- borderSubtle
            btn.Foreground <- (if enabled then fgPrimary else fgMuted)

    let makeSectionLabel (txt: string) =
        TextBlock(Text = txt, FontSize = 11.5, FontWeight = FontWeight.SemiBold, Foreground = fgMuted, VerticalAlignment = VerticalAlignment.Center)

    let streamDot = Border(Width = 8.0, Height = 8.0, CornerRadius = CornerRadius(4.0), Background = bgGreen, VerticalAlignment = VerticalAlignment.Center)
    let titleLabel = TextBlock(Text = "Console Link", FontSize = 13.0, FontWeight = FontWeight.SemiBold, Foreground = fgPrimary, TextTrimming = TextTrimming.CharacterEllipsis, VerticalAlignment = VerticalAlignment.Center)
    let f1Badge =
        Border(
            Background = SolidColorBrush(Color.Parse("#241E9BFA")),
            BorderBrush = SolidColorBrush(Color.Parse("#6638BDF8")),
            BorderThickness = Thickness(1.0),
            CornerRadius = CornerRadius(5.0),
            Padding = Thickness(6.0, 2.0),
            VerticalAlignment = VerticalAlignment.Center,
            Child = TextBlock(Text = "F1", FontSize = 10.5, FontWeight = FontWeight.Bold, Foreground = fgCyan)
        )
    let closeBtn =
        Button(
            Content = "×",
            Width = 26.0,
            Height = 26.0,
            Padding = Thickness(0.0),
            CornerRadius = CornerRadius(7.0),
            Background = bgSubtle,
            BorderBrush = borderSubtle,
            BorderThickness = Thickness(1.0),
            Foreground = fgPrimary,
            FontSize = 15.0,
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center
        )

    let fgLabel = makeSectionLabel ""
    let fgToggle = ToggleButton(Width = 54.0, Height = 25.0, CornerRadius = CornerRadius(12.5), FontSize = 11.0, FontWeight = FontWeight.Bold, HorizontalContentAlignment = HorizontalAlignment.Center, VerticalContentAlignment = VerticalAlignment.Center)

    let genLabel = makeSectionLabel ""
    let genPanel = Grid(ColumnSpacing = 5.0)
    let mutable genButtons: Button array = [||]
    let mutable genKey: string array = [||]

    let modeLabel = makeSectionLabel ""
    let fixedBtn = makePill "Fixed" (fun () -> model.FrameModeIndex <- 0)
    let adaptiveBtn = makePill "Adaptive" (fun () -> model.FrameModeIndex <- 1)

    let multLabel = makeSectionLabel ""
    let multButtons = [| 0 .. 4 |] |> Array.map (fun i -> makePill (sprintf "×%d" (i + 2)) (fun () -> model.ModeIndex <- i))
    let fixedRow = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto"))

    let adaptiveLabel = makeSectionLabel ""
    let adaptivePresets = [| 60; 90; 120; 144 |]
    let adaptivePresetButtons = adaptivePresets |> Array.map (fun fps -> makePill (string fps) (fun () -> model.AdaptiveTarget <- Nullable(decimal fps)))
    let adaptiveMinus = makePill "−" (fun () ->
        let cur = if model.AdaptiveTarget.HasValue then int model.AdaptiveTarget.Value else 120
        model.AdaptiveTarget <- Nullable(decimal (max 30 (cur - 5))))
    let adaptiveValueText = TextBlock(FontSize = 12.0, FontWeight = FontWeight.Bold, Foreground = fgCyan, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center, MinWidth = 44.0, TextAlignment = TextAlignment.Center)
    let adaptivePlus = makePill "+" (fun () ->
        let cur = if model.AdaptiveTarget.HasValue then int model.AdaptiveTarget.Value else 120
        model.AdaptiveTarget <- Nullable(decimal (min 1000 (cur + 5))))
    let adaptiveRow = StackPanel(Spacing = 6.0)

    let gameFpsLabel = makeSectionLabel ""
    let gameFpsTag =
        Border(
            Background = SolidColorBrush(Color.Parse("#2276B900")),
            BorderBrush = SolidColorBrush(Color.Parse("#5584CC16")),
            BorderThickness = Thickness(1.0),
            CornerRadius = CornerRadius(4.0),
            Padding = Thickness(5.0, 1.0),
            Child = TextBlock(Text = "MAX CAP", FontSize = 9.5, FontWeight = FontWeight.Bold, Foreground = SolidColorBrush(Color.Parse("#BEF264")))
        )
    let gameFpsLabels = [| "Auto"; "≤ 30"; "≤ 40"; "≤ 60" |]
    let gameFpsButtons = gameFpsLabels |> Array.mapi (fun i lbl -> makePill lbl (fun () -> model.ConsoleGameFpsIndex <- i))

    let upscalerLabel = makeSectionLabel ""
    let upscalerGrid = Grid(ColumnDefinitions = ColumnDefinitions("*,*,*,*"), RowDefinitions = RowDefinitions("Auto,Auto"), ColumnSpacing = 5.0, RowSpacing = 5.0, FlowDirection = FlowDirection.LeftToRight)
    let mutable upscalerButtons: Button array = [||]
    let mutable upscalerIsDlss5: bool array = [||]
    let mutable upscalerCountKey = -1

    let sharpLabel = makeSectionLabel ""
    let sharpSlider = Slider(Minimum = 0.0, Maximum = 100.0, TickFrequency = 1.0, VerticalAlignment = VerticalAlignment.Center)
    let sharpValue = TextBlock(FontSize = 11.5, Foreground = fgMuted, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right)
    let sharpRow = Grid(ColumnDefinitions = ColumnDefinitions("Auto,*,36"), ColumnSpacing = 8.0)

    let counterLabel = makeSectionLabel ""
    let counterButtons = [| 0; 1; 2 |] |> Array.map (fun idx -> makePill "" (fun () -> model.FpsCounterModeIndex <- idx))

    let smoothLabel = makeSectionLabel ""
    let smoothToggle = ToggleButton(Width = 54.0, Height = 25.0, CornerRadius = CornerRadius(12.5), FontSize = 11.0, FontWeight = FontWeight.Bold, HorizontalContentAlignment = HorizontalAlignment.Center, VerticalContentAlignment = VerticalAlignment.Center)

    let backBtn =
        Button(
            Padding = Thickness(10.0, 8.0),
            CornerRadius = CornerRadius(8.0),
            Background = bgAccent,
            BorderBrush = bgAccent,
            Foreground = Brushes.White,
            FontSize = 12.0,
            FontWeight = FontWeight.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center
        )
    let fullAppBtn =
        Button(
            Padding = Thickness(10.0, 8.0),
            CornerRadius = CornerRadius(8.0),
            Background = bgSubtle,
            BorderBrush = borderSubtle,
            BorderThickness = Thickness(1.0),
            Foreground = fgPrimary,
            FontSize = 11.5,
            FontWeight = FontWeight.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center
        )
    let stopBtn =
        Button(
            Padding = Thickness(10.0, 8.0),
            CornerRadius = CornerRadius(8.0),
            Background = bgDanger,
            BorderBrush = borderDanger,
            BorderThickness = Thickness(1.0),
            Foreground = fgDanger,
            FontSize = 11.5,
            FontWeight = FontWeight.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            HorizontalContentAlignment = HorizontalAlignment.Center
        )

    let styleToggle (btn: ToggleButton) (isOn: bool) =
        btn.IsChecked <- Nullable(isOn)
        btn.Content <- (if isOn then "ON" else "OFF")
        if isOn then
            btn.Background <- bgGreen
            btn.BorderBrush <- bgGreen
            btn.Foreground <- SolidColorBrush(Color.Parse("#0B1528"))
        else
            btn.Background <- bgSubtle
            btn.BorderBrush <- borderSubtle
            btn.Foreground <- fgMuted

    let rebuildGenerators () =
        let gens = model.Generators
        if gens <> genKey then
            genKey <- gens
            genPanel.Children.Clear()
            genPanel.ColumnDefinitions.Clear()
            genButtons <- Array.zeroCreate gens.Length
            for i in 0 .. gens.Length - 1 do
                genPanel.ColumnDefinitions.Add(ColumnDefinition(GridLength(1.0, GridUnitType.Star)))
                let shortName = gens[i].Replace(" 1.4", "").Replace(" 1.0", "")
                let btn = makePill shortName (fun () -> model.SetConsoleGenerator i)
                Grid.SetColumn(btn, i)
                genPanel.Children.Add(btn)
                genButtons[i] <- btn

    let shortUpscalerLabel (lbl: string) =
        if String.IsNullOrEmpty lbl then ""
        elif lbl.Equals("Nearest Neighbor", StringComparison.OrdinalIgnoreCase) then "Nearest"
        elif lbl.Equals("Sharpness Only", StringComparison.OrdinalIgnoreCase) || lbl.Equals("Sharpening Only", StringComparison.OrdinalIgnoreCase) then "Sharp"
        else lbl

    let rebuildUpscalers () =
        let items = model.Upscalers
        if items.Length <> upscalerCountKey then
            upscalerCountKey <- items.Length
            upscalerGrid.Children.Clear()
            upscalerButtons <- Array.zeroCreate items.Length
            upscalerIsDlss5 <- Array.zeroCreate items.Length
            for i in 0 .. items.Length - 1 do
                let lbl = shortUpscalerLabel items[i].Label
                upscalerIsDlss5[i] <- items[i].Label = "DLSS 5"
                let btn = makePill lbl (fun () -> model.UpscalerIndex <- i)
                Grid.SetRow(btn, i / 4)
                Grid.SetColumn(btn, i % 4)
                upscalerGrid.Children.Add(btn)
                upscalerButtons[i] <- btn
        else
            for i in 0 .. items.Length - 1 do
                upscalerIsDlss5[i] <- items[i].Label = "DLSS 5"
                setPillText upscalerButtons[i] (shortUpscalerLabel items[i].Label)

    do
        this.Title <- "MFG SUPER MOTION · Console Quick Menu"
        this.Width <- 396.0
        this.SizeToContent <- SizeToContent.Height
        this.CanResize <- false
        this.ShowInTaskbar <- false
        this.ShowActivated <- false
        this.Topmost <- true
        this.WindowDecorations <- WindowDecorations.None
        this.Background <- Brushes.Transparent
        this.TransparencyLevelHint <- [| WindowTransparencyLevel.Transparent |]
        this.RequestedThemeVariant <- ThemeVariant.Dark
        TextOptions.SetTextRenderingMode(this, TextRenderingMode.Antialias)

        // Ensure Win32 window never activates, steals focus, or reveals Windows Taskbar over fullscreen streams.
        if OperatingSystem.IsWindows() then
            Win32Properties.AddWindowStylesCallback(this,
                Win32Properties.CustomWindowStylesCallback(fun style exStyle ->
                    struct (style, exStyle ||| uint32 QuickMenuNative.WS_EX_NOACTIVATE ||| uint32 QuickMenuNative.WS_EX_TOOLWINDOW ||| uint32 QuickMenuNative.WS_EX_TOPMOST)))
            Win32Properties.AddWndProcHookCallback(this,
                Win32Properties.CustomWndProcHookCallback(fun _ msg _ _ handled ->
                    if msg = 0x0021u then // WM_MOUSEACTIVATE -> MA_NOACTIVATE
                        handled <- true
                        nativeint 3
                    else 0n))

        closeBtn.Click.Add(fun _ -> onBack ())
        fgToggle.IsCheckedChanged.Add(fun _ ->
            if not syncing then model.FrameGenerationEnabled <- (fgToggle.IsChecked = Nullable(true)))
        smoothToggle.IsCheckedChanged.Add(fun _ ->
            if not syncing then model.UltraSmoothEnabled <- (smoothToggle.IsChecked = Nullable(true)))
        sharpSlider.PropertyChanged.Add(fun e ->
            if not syncing && e.Property = RangeBase.ValueProperty then
                model.Sharpness <- sharpSlider.Value)

        backBtn.Click.Add(fun _ -> onBack ())
        fullAppBtn.Click.Add(fun _ -> onOpenFullApp ())
        stopBtn.Click.Add(fun _ ->
            this.Hide()
            if model.ConsoleLink.StopCommand.CanExecute(null) then
                model.ConsoleLink.StopCommand.Execute(null))

        this.AddHandler(InputElement.KeyDownEvent, (fun _ (args: KeyEventArgs) ->
            if args.Key = Key.F1 || args.Key = Key.Escape then
                args.Handled <- true
                onBack ()), Avalonia.Interactivity.RoutingStrategies.Tunnel)

        let headerRight = makeStack Orientation.Horizontal 6.0 [ f1Badge; closeBtn ]
        let headerLeft = makeStack Orientation.Horizontal 8.0 [ streamDot; titleLabel ]
        let headerGrid = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto"), ColumnSpacing = 8.0)
        Grid.SetColumn(headerLeft, 0)
        Grid.SetColumn(headerRight, 1)
        headerGrid.Children.Add(headerLeft)
        headerGrid.Children.Add(headerRight)
        let header = Border(Background = Brushes.Transparent, Padding = Thickness(2.0, 0.0, 2.0, 4.0), Child = headerGrid)

        // Manual non-activating pointer drag using Win32 cursor coordinates (zero taskbar flash).
        header.PointerPressed.Add(fun e ->
            if e.GetCurrentPoint(this).Properties.IsLeftButtonPressed && OperatingSystem.IsWindows() then
                let mutable pt = QuickMenuNative.POINT()
                if QuickMenuNative.GetCursorPos(&pt) then
                    dragging <- true
                    dragStartCursorX <- pt.X
                    dragStartCursorY <- pt.Y
                    dragStartWinX <- this.Position.X
                    dragStartWinY <- this.Position.Y
                    e.Pointer.Capture(header))
        header.PointerMoved.Add(fun _ ->
            if dragging && OperatingSystem.IsWindows() then
                let mutable pt = QuickMenuNative.POINT()
                if QuickMenuNative.GetCursorPos(&pt) then
                    this.Position <- PixelPoint(dragStartWinX + (pt.X - dragStartCursorX), dragStartWinY + (pt.Y - dragStartCursorY)))
        header.PointerReleased.Add(fun e ->
            if dragging then
                dragging <- false
                e.Pointer.Capture(null))

        // Section 1: Frame Generation & Generator
        let fgRow = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto"), ColumnSpacing = 10.0)
        Grid.SetColumn(fgLabel, 0); Grid.SetColumn(fgToggle, 1)
        fgRow.Children.Add(fgLabel); fgRow.Children.Add(fgToggle)
        let genSection = makeStack Orientation.Vertical 4.0 [ genLabel; genPanel ]
        let fgCard = makeCard (makeStack Orientation.Vertical 7.0 [ fgRow; genSection ])

        // Section 2: Mode, Multiplier / Adaptive & Max FPS Cap
        let modePills = Grid(ColumnDefinitions = ColumnDefinitions("*,*"), ColumnSpacing = 5.0)
        Grid.SetColumn(fixedBtn, 0); Grid.SetColumn(adaptiveBtn, 1)
        modePills.Children.Add(fixedBtn); modePills.Children.Add(adaptiveBtn)
        let modeSection = makeStack Orientation.Vertical 4.0 [ modeLabel; modePills ]

        let multGrid = Grid(ColumnDefinitions = ColumnDefinitions("*,*,*,*,*"), ColumnSpacing = 4.0, FlowDirection = FlowDirection.LeftToRight)
        for i in 0 .. multButtons.Length - 1 do
            Grid.SetColumn(multButtons[i], i)
            multGrid.Children.Add(multButtons[i])
        fixedRow.ColumnDefinitions <- ColumnDefinitions("Auto,*")
        fixedRow.ColumnSpacing <- 8.0
        Grid.SetColumn(multLabel, 0); Grid.SetColumn(multGrid, 1)
        fixedRow.Children.Add(multLabel); fixedRow.Children.Add(multGrid)

        let adPresetGrid = Grid(ColumnDefinitions = ColumnDefinitions("*,*,*,*"), ColumnSpacing = 4.0, FlowDirection = FlowDirection.LeftToRight)
        for i in 0 .. adaptivePresetButtons.Length - 1 do
            Grid.SetColumn(adaptivePresetButtons[i], i)
            adPresetGrid.Children.Add(adaptivePresetButtons[i])
        let adStepper = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto,Auto,Auto"), ColumnSpacing = 4.0, FlowDirection = FlowDirection.LeftToRight)
        adaptiveMinus.Width <- 30.0
        adaptivePlus.Width <- 30.0
        Grid.SetColumn(adaptiveLabel, 0); Grid.SetColumn(adaptiveMinus, 1); Grid.SetColumn(adaptiveValueText, 2); Grid.SetColumn(adaptivePlus, 3)
        adStepper.Children.Add(adaptiveLabel); adStepper.Children.Add(adaptiveMinus); adStepper.Children.Add(adaptiveValueText); adStepper.Children.Add(adaptivePlus)
        adaptiveRow.Children.Add(adStepper)
        adaptiveRow.Children.Add(adPresetGrid)

        let gameFpsHeader = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto"), ColumnSpacing = 6.0)
        Grid.SetColumn(gameFpsLabel, 0); Grid.SetColumn(gameFpsTag, 1)
        gameFpsHeader.Children.Add(gameFpsLabel); gameFpsHeader.Children.Add(gameFpsTag)

        let gameFpsGrid = Grid(ColumnDefinitions = ColumnDefinitions("*,*,*,*"), ColumnSpacing = 4.0, FlowDirection = FlowDirection.LeftToRight)
        for i in 0 .. gameFpsButtons.Length - 1 do
            Grid.SetColumn(gameFpsButtons[i], i)
            gameFpsGrid.Children.Add(gameFpsButtons[i])
        let gameFpsSection = makeStack Orientation.Vertical 4.0 [ gameFpsHeader; gameFpsGrid ]

        let paceCard = makeCard (makeStack Orientation.Vertical 7.0 [ modeSection; fixedRow; adaptiveRow; gameFpsSection ])

        // Section 3: Inline Upscaler Pills & Sharpness
        Grid.SetColumn(sharpLabel, 0); Grid.SetColumn(sharpSlider, 1); Grid.SetColumn(sharpValue, 2)
        sharpRow.FlowDirection <- FlowDirection.LeftToRight
        sharpRow.Children.Add(sharpLabel); sharpRow.Children.Add(sharpSlider); sharpRow.Children.Add(sharpValue)
        let upCard = makeCard (makeStack Orientation.Vertical 6.0 [ upscalerLabel; upscalerGrid; sharpRow ])

        // Section 4: FPS Counter & Ultra Smooth
        let counterGrid = Grid(ColumnDefinitions = ColumnDefinitions("*,*,*"), ColumnSpacing = 4.0)
        for i in 0 .. counterButtons.Length - 1 do
            Grid.SetColumn(counterButtons[i], i)
            counterGrid.Children.Add(counterButtons[i])
        let smoothRow = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto"), ColumnSpacing = 10.0)
        Grid.SetColumn(smoothLabel, 0); Grid.SetColumn(smoothToggle, 1)
        smoothRow.Children.Add(smoothLabel); smoothRow.Children.Add(smoothToggle)
        let hudCard = makeCard (makeStack Orientation.Vertical 7.0 [ counterLabel; counterGrid; smoothRow ])

        let footer = Grid(ColumnDefinitions = ColumnDefinitions("*,Auto,Auto"), ColumnSpacing = 6.0, Margin = Thickness(0.0, 2.0, 0.0, 0.0))
        Grid.SetColumn(backBtn, 0); Grid.SetColumn(fullAppBtn, 1); Grid.SetColumn(stopBtn, 2)
        footer.Children.Add(backBtn); footer.Children.Add(fullAppBtn); footer.Children.Add(stopBtn)

        let body = makeStack Orientation.Vertical 8.0 [ header; fgCard; paceCard; upCard; hudCard; footer ]

        this.Content <-
            Border(
                Background = bgOuter,
                BorderBrush = borderOuter,
                BorderThickness = Thickness(1.0),
                CornerRadius = CornerRadius(14.0),
                Padding = Thickness(12.0),
                Child = body
            )

        this.Opened.Add(fun _ ->
            if OperatingSystem.IsWindows() then
                let h = this.TryGetPlatformHandle()
                if not (isNull h) then QuickMenuNative.applyNoActivateTopmost h.Handle)

        let onVmChanged = PropertyChangedEventHandler(fun _ _ ->
            if this.IsVisible then Dispatcher.UIThread.Post(fun () -> this.Refresh()))
        model.PropertyChanged.AddHandler(onVmChanged)
        let onConsoleChanged = PropertyChangedEventHandler(fun _ _ ->
            if this.IsVisible then Dispatcher.UIThread.Post(fun () -> this.Refresh()))
        model.ConsoleLink.PropertyChanged.AddHandler(onConsoleChanged)
        this.Closed.Add(fun _ ->
            model.PropertyChanged.RemoveHandler(onVmChanged)
            model.ConsoleLink.PropertyChanged.RemoveHandler(onConsoleChanged))

        this.Refresh()

    member this.Refresh() =
        syncing <- true
        try
            let t = model.Text
            this.FlowDirection <- (if model.IsRightToLeft then FlowDirection.RightToLeft else FlowDirection.LeftToRight)
            titleLabel.Text <- (if String.IsNullOrWhiteSpace model.ConsoleLink.PlayingText then "Console Link" else model.ConsoleLink.PlayingText)
            fgLabel.Text <- t.FrameGeneration
            styleToggle fgToggle model.FrameGenerationEnabled

            rebuildGenerators ()
            genLabel.Text <- t.Generator
            let genIdx = model.GeneratorIndex
            for i in 0 .. genButtons.Length - 1 do
                stylePill genButtons[i] (i = genIdx) true false

            modeLabel.Text <- t.Mode
            let fgOn = model.FrameGenerationEnabled
            stylePill fixedBtn model.IsFixed fgOn false
            stylePill adaptiveBtn model.IsAdaptive fgOn false

            fixedRow.IsVisible <- model.IsFixed
            multLabel.Text <- t.Multiplier
            let multIdx = model.ModeIndex
            for i in 0 .. multButtons.Length - 1 do
                stylePill multButtons[i] (i = multIdx) fgOn false

            adaptiveRow.IsVisible <- model.IsAdaptive
            adaptiveLabel.Text <- t.AdaptiveTarget
            let adTarget = if model.AdaptiveTarget.HasValue then int model.AdaptiveTarget.Value else 120
            adaptiveValueText.Text <- sprintf "%d Hz" adTarget
            stylePill adaptiveMinus false fgOn false
            stylePill adaptivePlus false fgOn false
            for i in 0 .. adaptivePresetButtons.Length - 1 do
                stylePill adaptivePresetButtons[i] (adaptivePresets[i] = adTarget) fgOn false

            gameFpsLabel.Text <- t.GameFps
            setPillText gameFpsButtons[0] t.GameAuto
            let fpsIdx = model.ConsoleGameFpsIndex
            for i in 0 .. gameFpsButtons.Length - 1 do
                stylePill gameFpsButtons[i] (i = fpsIdx) true false

            upscalerLabel.Text <- t.Upscaler
            rebuildUpscalers ()
            let upIdx = model.UpscalerIndex
            for i in 0 .. upscalerButtons.Length - 1 do
                stylePill upscalerButtons[i] (i = upIdx) true upscalerIsDlss5[i]

            sharpRow.IsVisible <- model.IsSharpnessOpen
            sharpLabel.Text <- t.Sharpness
            sharpSlider.Value <- model.Sharpness
            sharpValue.Text <- model.SharpnessLabel

            counterLabel.Text <- t.FpsCounter
            setPillText counterButtons[0] t.Off
            setPillText counterButtons[1] t.CounterOn
            setPillText counterButtons[2] t.CounterAdvanced
            let cntIdx = model.FpsCounterModeIndex
            for i in 0 .. counterButtons.Length - 1 do
                stylePill counterButtons[i] (i = cntIdx) true false

            smoothLabel.Text <- t.UltraSmooth
            styleToggle smoothToggle model.UltraSmoothEnabled

            backBtn.Content <- t.ConsoleBack
            fullAppBtn.Content <- (if model.IsArabic then "التطبيق الكامل" else "Full App")
            stopBtn.Content <- t.ConsoleStop
        finally
            syncing <- false

    member this.PositionOverWindow(hwnd: uint64, fallback: Window) =
        try
            let mutable rect = QuickMenuNative.RECT()
            let hasRect = hwnd <> 0UL && OperatingSystem.IsWindows() && QuickMenuNative.GetWindowRect(nativeint hwnd, &rect)
            let screen =
                if hasRect then this.Screens.ScreenFromPoint(PixelPoint((rect.Left + rect.Right) / 2, (rect.Top + rect.Bottom) / 2))
                elif not (isNull fallback) then this.Screens.ScreenFromWindow(fallback)
                else this.Screens.Primary
            let screen = if isNull screen then this.Screens.Primary else screen
            if not (isNull screen) then
                let wa = screen.WorkingArea
                let scale = screen.Scaling
                let cardW = int (Math.Round(this.Width * scale))
                let estH = if this.Bounds.Height > 10.0 then this.Bounds.Height else 520.0
                let cardH = int (Math.Round(estH * scale))
                let margin = int (24.0 * scale)
                let rawX = if hasRect then rect.Right - cardW - margin else wa.X + wa.Width - cardW - margin
                let rawY = if hasRect then rect.Top + margin else wa.Y + margin
                let clampedX = Math.Clamp(rawX, wa.X + 8, max (wa.X + 8) (wa.X + wa.Width - cardW - 8))
                let clampedY = Math.Clamp(rawY, wa.Y + 8, max (wa.Y + 8) (wa.Y + wa.Height - cardH - 8))
                this.Position <- PixelPoint(clampedX, clampedY)
        with _ -> ()

    member this.ShowNoActivateOver(hwnd: uint64, fallback: Window) =
        this.ShowActivated <- false
        this.PositionOverWindow(hwnd, fallback)
        if not this.IsVisible then this.Show()
        this.PositionOverWindow(hwnd, fallback)
        if OperatingSystem.IsWindows() then
            let h = this.TryGetPlatformHandle()
            if not (isNull h) then QuickMenuNative.applyNoActivateTopmost h.Handle
