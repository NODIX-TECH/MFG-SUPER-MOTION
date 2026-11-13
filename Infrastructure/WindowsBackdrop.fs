namespace ARMO.Infrastructure

open System
open System.Runtime.InteropServices

/// Windows owns the frame, caption buttons and backdrop. The UI keeps software rendering.
module WindowsBackdrop =
    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type private FrameBounds =
        val Left: int
        val Top: int
        val Right: int
        val Bottom: int

    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type private Margins =
        val Left: int
        val Right: int
        val Top: int
        val Bottom: int
        new(value) = { Left = value; Right = value; Top = value; Bottom = value }

    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type private BlurBehind =
        val Flags: uint32
        val Enabled: int
        val Region: nativeint
        val Transition: int
        new(enabled) = { Flags = 1u; Enabled = (if enabled then 1 else 0); Region = 0n; Transition = 0 }

    [<DllImport("dwmapi.dll")>]
    extern int private DwmSetWindowAttribute(nativeint window, int attribute, int& value, int size)
    [<DllImport("dwmapi.dll")>]
    extern int private DwmExtendFrameIntoClientArea(nativeint window, Margins& margins)
    [<DllImport("dwmapi.dll")>]
    extern int private DwmEnableBlurBehindWindow(nativeint window, BlurBehind& blur)
    [<DllImport("dwmapi.dll")>]
    extern int private DwmGetWindowAttribute(nativeint window, int attribute, FrameBounds& bounds, int size)

    let visibleFrameSize handle scaling =
        if not (OperatingSystem.IsWindows()) || handle = 0n then None
        else
            let mutable bounds = Unchecked.defaultof<FrameBounds>
            if DwmGetWindowAttribute(handle, 9, &bounds, sizeof<FrameBounds>) <> 0 then None
            else Some (float (bounds.Right - bounds.Left) / scaling, float (bounds.Bottom - bounds.Top) / scaling)

    let private setAttribute handle attribute value =
        let mutable value = value
        DwmSetWindowAttribute(handle, attribute, &value, sizeof<int>)

    let setTheme handle dark =
        // 1.1.0 (GitHub 87, the Windows 10 fix): Windows 10 has a dark title bar too - DWMWA_USE_IMMERSIVE_DARK_MODE is 20
        // from build 18985 on, 19 before it (1809 - 1903). Only the title bar's darkness there: the caption colours and
        // the rounded corners below are Windows 11's.
        if handle <> 0n && OperatingSystem.IsWindowsVersionAtLeast(10, 0, 17763) && not (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000)) then
            let attribute = if OperatingSystem.IsWindowsVersionAtLeast(10, 0, 18985) then 20 else 19
            setAttribute handle attribute (if dark then 1 else 0) |> ignore
        if handle <> 0n && OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000) then
            setAttribute handle 20 (if dark then 1 else 0) |> ignore
            // COLORREF is 0x00BBGGRR. These match the stable canvas palette.
            setAttribute handle 35 (if dark then 0x0028201B else 0x00FAF7F5) |> ignore
            setAttribute handle 36 (if dark then 0x00FAF8F5 else 0x00271A11) |> ignore
            setAttribute handle 33 2 |> ignore // DWMWCP_ROUND; Windows handles maximization.

    let trySet handle enabled dark =
        if handle = 0n || not (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22621)) then false
        else
            setTheme handle dark
            // Desktop Acrylic, with a subtle tint supplied by the canvas brush.
            let result = setAttribute handle 38 (if enabled then 3 else 1)
            let mutable blur = BlurBehind(false)
            DwmEnableBlurBehindWindow(handle, &blur) |> ignore
            let mutable margins = Margins(if enabled then -1 else 0)
            let extended = DwmExtendFrameIntoClientArea(handle, &margins) = 0
            result = 0 && extended && enabled
