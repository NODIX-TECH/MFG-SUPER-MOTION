// MFG SUPER MOTION engine - the process the app starts with "START MOTION".
// It takes the game's window (or its whole screen) straight on the graphics
// card, has a generator make frames in between from the captured pictures -
// NVIDIA's DLSS Multi Frame Generation, or Intel's XeSS Frame Generation on
// any card that runs it, whichever the app picked - and shows real and made
// frames in its own click-through window over the game. The app only steers
// it: a start file, a line on stdin for every control that moves, status
// lines back on stdout (sm_control.inl).
//
// The file is short on purpose: everything it needs is declared up front
// (lf_declare.h), so the topic files below can come in any order:
//   sm_options      one value per control in the app
//   lf_trace        the log, and failure reports
//   lf_gpu_core     the D3D12 device, the command ring, fences
//   lf_settings     the frame limit, capture pacing, the motion field's size
//   sm_control      lines from and to the app
//   sm_source       which window is taken, borderless over its screen
//   lf_desktop      the source: Desktop Duplication (DXGI), Graphics Capture (WGC)
//   lf_luma         small luma copies for scene cuts and optical flow
//   lf_ofa          NVIDIA Optical Flow
//   lf_estimate     the engine's own GPU motion estimate
//   lf_own_motion   scene cut, repeated frames, the estimate's passes
//   lf_pipeline     the picture's and the motion field's textures
//   lf_ngx          NGX and the architecture answer for every RTX card
//   lf_dlssg        DLSS MFG: the runtime; and the presenter both generators
//                   show their frames through (lf_fg_pace, lf_fg_scale,
//                   lf_fg_still, sm_rebuild, lf_dlssg_compat, lf_dlssg_ada)
//   xe_mfg          XeSS MFG: Intel's libraries (xe_api.h) and the tap that
//                   takes their frames (xe_tap.h)
//   lf_show         the output window and its swap chain
//   lf_window_fill  a window scaled onto its whole screen
//   lf_vrr, lf_latency, lf_hdr_show, lf_counter   VRR, Low Latency, HDR, the FPS counter
//   lf_timing       optional timings (LF_PHASE=1)
//   sm_loop         the loop that ties them together
// Built as nvngx.dll (build.cmd): NGX's runtimes check that file name of
// their caller.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <setupapi.h>
#include <bcrypt.h>     // (NTSTATUS, which hidpi.h takes for granted)
#include <hidusage.h>
#include <hidpi.h>
#include <tlhelp32.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <memory>
#include <map>
#include <set>
#include "sm_options.h"
#include "dlss5_link.h"
#include "lf_hdr_display.h"
#include "lf_hdr_shaders.h"
#include "lf_signature.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#include "lf_types.h"
#include "lf_declare.h"
#include "lf_state.h"
#include "lf_shader_cache.inl"

#include "capture_shortcuts.inl"
#include "lf_show_relay.inl"
#include "lf_window_fill.inl"
#include "lf_vrr.inl"
#include "lf_latency.inl"
#include "lf_counter.inl"
#include "sm_banner.inl"
#include "sm_learn.inl"
#include "sm_rtss.inl"
#include "sm_dlssg_preset.inl"
#include "lf_hdr_show.inl"
#include "lf_ofa.inl"
#include "lf_dlssg.inl"
#include "lf_estimate.inl"
#include "lf_own_motion.inl"
#include "sm_guard.inl"

#include "lf_trace.inl"
#include "lf_gpu_core.inl"
#include "sm_spout.inl"
#include "lf_settings.inl"
#include "sm_control.inl"
#include "lf_desktop.inl"
#include "sm_dlss5_link.inl"
#include "lf_luma.inl"
#include "lf_pipeline.inl"
#include "lf_ngx.inl"
#include "lf_show.inl"
#include "xe_mfg.inl"
#include "lf_timing.inl"
#include "lf_shader_warm.inl"
#include "sm_source.inl"
#include "sm_cursor.inl"
#include "sm_video.inl"
#include "sm_loop.inl"

// The game's window is put back whatever ends the engine.
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *info)
{
    Trace("[engine] stopped by an exception 0x%08lX", info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0ul);
    UpscaleGoHome();
    BorderlessRelease();
    cursorfx::Restore();
    return EXCEPTION_EXECUTE_HANDLER;
}

static BOOL WINAPI ConsoleStop(DWORD)
{
    g_control.stop = true;
    return TRUE;
}

// NGX starts helper processes of the driver's (its updater) with every start;
// left alone they stay for about forty minutes each. They end with the engine.
static void KeepChildrenWithUs()
{
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess()))
        CloseHandle(job);
}

int wmain(int argc, wchar_t **argv)
{
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
    {
        typedef BOOL(WINAPI * DpiContextFn)(HANDLE);
        auto set_context = reinterpret_cast<DpiContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context == nullptr || !set_context(reinterpret_cast<HANDLE>(-4)))
            SetProcessDPIAware();
    }
    bool read_stdin = true;
    const wchar_t *video = nullptr;      // --video <job file>: Super Video (sm_video.inl)
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--config") == 0 && i + 1 < argc) ReadStartFile(argv[++i]);
        else if (wcscmp(argv[i], L"--verbose") == 0) g_trace_echo = true;
        else if (wcscmp(argv[i], L"--no-stdin") == 0) read_stdin = false;
        else if (wcscmp(argv[i], L"--video") == 0 && i + 1 < argc) { video = argv[++i]; ReadStartFile(video); }
        else if (wcscmp(argv[i], L"--cards") == 0) { CardsTell(); return 0; }      // for the app's "Preferred GPU" (lf_gpu_core.inl)
        else if (wcscmp(argv[i], L"--vrr") == 0) { VrrTell(); return 0; }          // for the app's VRR switch (lf_vrr.inl)
        else if (wcscmp(argv[i], L"--rtss-quiet") == 0) { rtss::Quiet(); return 0; }    // the app at its start: RTSS's overlay once, not twice
        else if (wcschr(argv[i], L'=') != nullptr)
        {
            char line[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, line, sizeof(line) - 1, nullptr, nullptr);
            ApplyLine(line);
        }
    }
    // The log only when it is asked for (log=1, the engine's own default; the app asks for it when the person
    // turns "Engine log" on): a player has no use for a file of a few megabytes an evening.
    // Super Video: a video file, not the screen - its own log (video.log), SMFG on the card picked, nothing else.
    if (video)
    {
        supervideo::ReadJob(video);
        g_generator = 2;
        g_generator_locked = true;
        if (smOpt.log.load()) TraceOpen(L"video");
        SetUnhandledExceptionFilter(CrashFilter);
        Status("@state starting");
        if (!GpuCoreStart()) { Status("@error video No compatible graphics card was found"); return 1; }
        const int code = supervideo::Run();
        Trace("[engine] video ended with code %d", code);
        return code;
    }
    if (smOpt.log.load()) TraceOpen();
    {
        char v[4] = {};
        if (smOpt.frame_log.load() || (GetEnvironmentVariableA("SM_FRAME_LOG", v, sizeof(v)) > 0 && v[0] == '1')) FramesOpen();
    }
    g_generator_locked = true;
    Trace("MFG SUPER MOTION engine, built %s %s", __DATE__, __TIME__);
    SetUnhandledExceptionFilter(CrashFilter);
    SetConsoleCtrlHandler(ConsoleStop, TRUE);
    KeepChildrenWithUs();
    Status("@state starting");
    // DLSS MFG's preset into the driver's profile of the engine, before anything of the engine is on the card.
    if (NgxSelected()) dlssgpreset::Apply(smOpt.dlssg_preset.load());
    if (!GpuCoreStart())
    {
        Status("@error nogpu No compatible graphics card was found");
        Status("@state stopped");
        return 1;
    }
    // RTX 20/30: the compatibility package is in place before NGX starts and
    // before any other thread of ours runs.
    if (NgxSelected())
    {
        wchar_t directory[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, directory, MAX_PATH);
        if (auto slash = wcsrchr(directory, L'\\')) *(slash + 1) = 0;
        if (!FgPrepareCompatibility(directory, true)) g_fg.failed = true;
    }
    if (!ControlStart(read_stdin))
    {
        Status("@error busy Another engine is still running");
        Status("@state stopped");
        return 1;
    }
    // The shaders compile and NGX comes up while the person clicks the game.
    ShaderWarmStart();
    // (XeSS MFG: Intel's libraries and the unlock plugin instead - the plugin needs a second before the library is used.)
    // (MSM FG: nothing of anyone else's to start - its passes are among the shaders compiling above.)
    if (XeSelected()) { if (!XeStart()) g_fg.failed = true; }
    else if (NgxSelected() && !NgxStart()) Trace("[card] NGX did not start - DLSS MFG is unavailable, the picture is shown as it is");
    const int code = EngineRun();
    cursorfx::Restore();      // (the pointer as it was: sm_cursor.inl)
    SpoutClose();
    if (NgxSelected()) NgxShutdown();
    Status("@state stopped");
    Trace("[engine] ended with code %d", code);
    if (g_control.instance) { ReleaseMutex(g_control.instance); CloseHandle(g_control.instance); }
    return code;
}
