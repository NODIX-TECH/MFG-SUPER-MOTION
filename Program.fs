namespace ARMO

open System
open Avalonia

module Program =

    [<CompiledName "BuildAvaloniaApp">] 
    let buildAvaloniaApp () = 
        AppBuilder
            .Configure<App>()
            .UsePlatformDetect()
            .With(Win32PlatformOptions(
                DpiAwareness = Win32DpiAwareness.PerMonitorDpiAware,
                RenderingMode = [| Win32RenderingMode.Software |]))
            // (Linux, round 73: drawn as on Windows - with OpenGL, X11's see-through windows came out without their
            // backgrounds.)
            .With(X11PlatformOptions(RenderingMode = [| X11RenderingMode.Software |]))
#if DEBUG
            .WithDeveloperTools()
#endif
            .LogToTrace(areas = Array.empty)

    [<EntryPoint; STAThread>]
    let main argv =
        buildAvaloniaApp().StartWithClassicDesktopLifetime(argv)
