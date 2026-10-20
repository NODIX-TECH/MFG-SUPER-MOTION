namespace ARMO.Infrastructure

open System
open System.Diagnostics
open System.Runtime.InteropServices
open System.Threading

module private HotkeyNative =
    [<Struct; StructLayout(LayoutKind.Sequential)>]
    type MSG =
        val mutable hwnd: nativeint
        val mutable message: uint32
        val mutable wParam: nativeint
        val mutable lParam: nativeint
        val mutable time: uint32
        val mutable x: int
        val mutable y: int

    type KeyboardProc = delegate of int * nativeint * nativeint -> nativeint

    [<DllImport("user32.dll", SetLastError = true)>]
    extern bool RegisterHotKey(nativeint hWnd, int id, uint32 fsModifiers, uint32 vk)

    [<DllImport("user32.dll")>]
    extern bool UnregisterHotKey(nativeint hWnd, int id)

    [<DllImport("user32.dll", EntryPoint = "GetMessageW")>]
    extern int GetMessage(MSG& lpMsg, nativeint hWnd, uint32 wMsgFilterMin, uint32 wMsgFilterMax)

    [<DllImport("user32.dll", EntryPoint = "PeekMessageW")>]
    extern bool PeekMessage(MSG& lpMsg, nativeint hWnd, uint32 wMsgFilterMin, uint32 wMsgFilterMax, uint32 wRemoveMsg)

    [<DllImport("user32.dll", EntryPoint = "PostThreadMessageW")>]
    extern bool PostThreadMessage(uint32 idThread, uint32 Msg, nativeint wParam, nativeint lParam)

    [<DllImport("user32.dll", EntryPoint = "SetWindowsHookExW", SetLastError = true)>]
    extern nativeint SetWindowsHookEx(int idHook, KeyboardProc lpfn, nativeint hMod, uint32 dwThreadId)

    [<DllImport("user32.dll")>]
    extern bool UnhookWindowsHookEx(nativeint hhk)

    [<DllImport("user32.dll")>]
    extern nativeint CallNextHookEx(nativeint hhk, int nCode, nativeint wParam, nativeint lParam)

    [<DllImport("user32.dll")>]
    extern int16 GetAsyncKeyState(int vKey)

    [<DllImport("user32.dll")>]
    extern nativeint GetForegroundWindow()

    [<DllImport("user32.dll")>]
    extern uint32 GetWindowThreadProcessId(nativeint hWnd, uint32& processId)

    [<DllImport("kernel32.dll", EntryPoint = "GetModuleHandleW")>]
    extern nativeint GetModuleHandle(nativeint name)

    [<DllImport("kernel32.dll")>]
    extern uint32 GetCurrentThreadId()

    let WM_HOTKEY = 0x0312u
    let WM_QUIT = 0x0012u
    let WM_APP_SET = 0x8001u
    let MOD_NOREPEAT = 0x4000u
    let WH_KEYBOARD_LL = 13

/// One system-wide shortcut (START / STOP MOTION), heard while a game has the
/// keyboard and while the window is in the tray. It lives on a thread of its
/// own that only waits for Windows' messages - nothing is polled.
///
/// Heard two ways (round 42). Windows' own shortcut (RegisterHotKey) is never
/// delivered while a game reads the keyboard raw and asks for no shortcuts
/// (RIDEV_NOHOTKEYS) - "F6 in the game, and nothing happened". A keyboard hook
/// (WH_KEYBOARD_LL, what Lossless Scaling listens with) still sees the key
/// then; Windows' shortcut is the one that still comes while a game running as
/// administrator has the keyboard, which the hook is not told of. One press is
/// counted once, whichever hears it first. The key itself goes on to the game
/// untouched. Pressed says whether a window of this app was in front.
type GlobalHotkey() =
    let pressed = Event<bool>()
    let ready = new ManualResetEventSlim(false)
    let applied = new AutoResetEvent(false)
    let mutable threadId = 0u
    let mutable wanted = 0          // modifiers * 65536 + virtual key; 0 none
    let mutable registered = false
    let mutable taken = false       // the shortcut wanted belongs to another program
    let mutable started = false
    let mutable hook = 0n
    let mutable keyDown = false     // the shortcut's key is held (a held key repeats: counted once)
    let mutable lastFired = 0L
    let ownProcess = uint32 Environment.ProcessId

    let oursInFront () =
        let mutable owner = 0u
        let front = HotkeyNative.GetForegroundWindow()
        front <> 0n && HotkeyNative.GetWindowThreadProcessId(front, &owner) <> 0u && owner = ownProcess

    let fire () =
        let now = Stopwatch.GetTimestamp()
        if now - lastFired > Stopwatch.Frequency * 4L / 10L then
            lastFired <- now
            pressed.Trigger(oursInFront ())

    // Alt 1, Ctrl 2, Shift 4, Win 8 (RegisterHotKey's), as they are held now.
    let modifiersHeld () =
        let held (vk: int) = HotkeyNative.GetAsyncKeyState vk < 0s
        (if held 0x12 then 1 else 0) ||| (if held 0x11 then 2 else 0) ||| (if held 0x10 then 4 else 0)
        ||| (if held 0x5B || held 0x5C then 8 else 0)

    let onKey =
        HotkeyNative.KeyboardProc(fun code wParam lParam ->
            if code >= 0 then
                let value = Volatile.Read(&wanted)
                let vk = Marshal.ReadInt32 lParam
                if value > 0 && vk = (value &&& 0xFFFF) then
                    match int wParam with
                    | 0x100 | 0x104 ->      // WM_KEYDOWN, WM_SYSKEYDOWN
                        if not keyDown then
                            keyDown <- true
                            if modifiersHeld () = ((value >>> 16) &&& 0xF) then fire ()
                    | 0x101 | 0x105 -> keyDown <- false
                    | _ -> ()
            HotkeyNative.CallNextHookEx(0n, code, wParam, lParam))

    let apply () =
        if registered then
            HotkeyNative.UnregisterHotKey(0n, 1) |> ignore
            registered <- false
        if hook <> 0n then
            HotkeyNative.UnhookWindowsHookEx hook |> ignore
            hook <- 0n
        keyDown <- false
        let value = Volatile.Read(&wanted)
        if value > 0 then
            let modifiers = (uint32 (value >>> 16) &&& 0xFu) ||| HotkeyNative.MOD_NOREPEAT
            registered <- HotkeyNative.RegisterHotKey(0n, 1, modifiers, uint32 (value &&& 0xFFFF))
            taken <- not registered
            hook <- HotkeyNative.SetWindowsHookEx(HotkeyNative.WH_KEYBOARD_LL, onKey, HotkeyNative.GetModuleHandle 0n, 0u)
        else taken <- false
        applied.Set() |> ignore

    let run () =
        let mutable message = HotkeyNative.MSG()
        // The first call makes this thread's message queue.
        HotkeyNative.PeekMessage(&message, 0n, 0u, 0u, 0u) |> ignore
        threadId <- HotkeyNative.GetCurrentThreadId()
        apply ()
        ready.Set()
        let mutable alive = true
        while alive do
            let result = HotkeyNative.GetMessage(&message, 0n, 0u, 0u)
            if result <= 0 then alive <- false
            elif message.message = HotkeyNative.WM_HOTKEY then fire ()
            elif message.message = HotkeyNative.WM_APP_SET then apply ()
        if registered then HotkeyNative.UnregisterHotKey(0n, 1) |> ignore
        if hook <> 0n then HotkeyNative.UnhookWindowsHookEx hook |> ignore

    let thread = Thread(run, IsBackground = true, Name = "hotkey")

    /// The shortcut was pressed: true when a window of this app was in front.
    member _.Pressed = pressed.Publish

    /// True when Windows refused the shortcut last asked for (another program holds it).
    member _.IsTaken = taken

    /// modifiers * 65536 + virtual key (RegisterHotKey's values); 0 for none.
    member _.Set(value: int) =
        Volatile.Write(&wanted, value)
        if not started then
            started <- true
            thread.Start()
            ready.Wait 2000 |> ignore
        else
            applied.Reset() |> ignore
            if HotkeyNative.PostThreadMessage(threadId, HotkeyNative.WM_APP_SET, 0n, 0n) then
                applied.WaitOne 500 |> ignore

    interface IDisposable with
        member _.Dispose() =
            if threadId <> 0u then HotkeyNative.PostThreadMessage(threadId, HotkeyNative.WM_QUIT, 0n, 0n) |> ignore
