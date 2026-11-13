namespace ARMO

open Avalonia
open Avalonia.Controls.ApplicationLifetimes
open Avalonia.Controls
open Avalonia.Media
open Avalonia.Styling
open Avalonia.Threading
open Avalonia.Markup.Xaml
open ARMO.ViewModels
open ARMO.Views

type App() =
    inherit Application()

    override this.Initialize() =
            AvaloniaXamlLoader.Load(this)

    override this.OnFrameworkInitializationCompleted() =
        match this.ApplicationLifetime with
        | :? IClassicDesktopStyleApplicationLifetime as desktop ->
             let model = new MainViewModel()
             let window = MainWindow(DataContext = model)
             let tray = new TrayController(window, model)
             let applyTheme () =
                 this.RequestedThemeVariant <-
                     match model.ThemeIndex with
                     | 1 -> ThemeVariant.Dark
                     | 2 -> ThemeVariant.Default
                     | _ -> ThemeVariant.Light
             let applyLanguage () =
                 window.FlowDirection <- if model.IsRightToLeft then FlowDirection.RightToLeft else FlowDirection.LeftToRight
                 window.Classes.Set("rtl", model.IsRightToLeft)
                 window.FontFamily <- FontFamily(model.UiFontFamily)
             applyTheme ()
             applyLanguage ()
             window.SetTransparency(model.TransparencyEnabled)
             let applyAdvanced () =
                 let arrow = window.FindControl<PathIcon>("AdvancedArrow")
                 arrow.RenderTransform <- RotateTransform(if model.IsAdvancedOpen then 180. else 0.)
             applyAdvanced ()
             model.PersistenceChanged.Add(fun () ->
                 Dispatcher.UIThread.Post(fun () -> model.Notify("HasSaveError")))
             model.PropertyChanged.Add(fun args ->
                 match args.PropertyName with
                 | "ThemeIndex" -> applyTheme ()
                 | "IsRightToLeft" -> applyLanguage ()
                 | "TransparencyEnabled" -> window.SetTransparency(model.TransparencyEnabled)
                 | "IsEditorOpen" when model.IsEditorOpen -> window.FocusProfileName()
                 | "IsAdvancedOpen" -> applyAdvanced ()
                 | _ -> ())
             // App Mods (round 72): the mods that are on, loaded once the window is up (Views\ModHost.fs).
             let mods = ModManager(window, model)
             let mutable modsStarted = false
             window.Opened.Add(fun _ ->
                 if not modsStarted then
                     modsStarted <- true
                     mods.Start())
             window.Closed.Add(fun _ ->
                 (tray :> System.IDisposable).Dispose()
                 (model :> System.IDisposable).Dispose())
             desktop.MainWindow <- window
        | _ -> ()

        base.OnFrameworkInitializationCompleted()
