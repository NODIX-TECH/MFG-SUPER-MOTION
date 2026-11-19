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
#include <deque>
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
#include "lf_smsr.inl"
#include "lf_window_fill.inl"
#include "sm_front.inl"
#include "sm_nooverlay.inl"
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
#include "lf_bloom.inl"
#include "lf_dlss5.inl"
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

// (Round 83, a probe for tests: SM_DLL_PROBE=1 says when each DLL comes into the engine, and from whom.)
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } SmUnicode;
typedef struct { ULONG Flags; const SmUnicode *FullDllName; const SmUnicode *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } SmDllLoaded;
static VOID CALLBACK SmDllProbe(ULONG reason, const SmDllLoaded *data, PVOID)
{
    if (reason != 1 || !data || !data->BaseDllName) return;
    void *frames[12] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 12, frames, nullptr);
    char from[512] = {}; size_t at = 0;
    for (USHORT i = 0; i < n && at < sizeof(from) - 64; ++i)
    {
        HMODULE m = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(frames[i]), &m) && m)
        { wchar_t name[MAX_PATH] = {}; GetModuleFileNameW(m, name, MAX_PATH); const wchar_t *b = wcsrchr(name, L'\\'); at += sprintf_s(from + at, sizeof(from) - at, "%ls<", b ? b + 1 : name); }
    }
    Trace("[dll] %.*ls loaded, from %s", int(data->BaseDllName->Length / 2), data->BaseDllName->Buffer, from);
}

static int TestDlss5(UINT test_w, UINT test_h)
{
    smOpt.upscaler = 7;
    if (!NgxStart()) { printf("FAIL: NgxStart\n"); return 1; }
    const UINT W = test_w, H = test_h;
    const ULONGLONG t_warm0 = GetTickCount64();
    Dlss5WarmupStart();
    if (!Dlss5PrepareSession(W, H)) { printf("FAIL: Dlss5PrepareSession\n"); Dlss5Close(); return 1; }
    const ULONGLONG warm_ms = GetTickCount64() - t_warm0;
    Pipeline pipe = {};
    if (!PipelineBuild(pipe, W, H, 0, 0)) { printf("FAIL: PipelineBuild\n"); Dlss5Close(); return 1; }

    ID3D12Resource *readback = nullptr;
    D3D12_HEAP_PROPERTIES rb_hp = {};
    rb_hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = {};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = pipe.picture.staging->GetDesc().Width;
    rb_desc.Height = 1; rb_desc.DepthOrArraySize = 1; rb_desc.MipLevels = 1;
    rb_desc.SampleDesc.Count = 1; rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(lfCore.device->CreateCommittedResource(&rb_hp, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&readback))))
    {
        printf("FAIL: readback buffer\n");
        PipelineRelease(pipe);
        Dlss5Close();
        return 1;
    }

    auto fill_pattern = [&](std::vector<BYTE> &buf, int frame) {
        buf.resize(static_cast<size_t>(W) * H * 4);
        const int sx = frame * 3, sy = frame * 2;
        for (UINT y = 0; y < H; ++y)
        {
            BYTE *row = buf.data() + static_cast<size_t>(y) * W * 4;
            for (UINT x = 0; x < W; ++x)
            {
                const int px = static_cast<int>(x) + sx, py = static_cast<int>(y) + sy;
                int r = 0, g = 0, b = 0;
                if (x >= 420 && x < 780 && y >= 340 && y < 740)
                {
                    // Warm skin-like patch with fine shading.
                    r = 195 + ((px ^ py) & 15);
                    g = 142 + ((px + py) & 15);
                    b = 118 + ((py) & 15);
                }
                else if (x < 320 && y >= 760)
                {
                    // Low-light dark shadow region.
                    r = 10 + ((px / 8 + py / 8) & 1) * 18;
                    g = 12 + ((px / 16) & 1) * 14;
                    b = 16 + ((py / 16) & 1) * 20;
                }
                else
                {
                    const int checker = (((px / 8) + (py / 8)) & 1) ? 45 : 175;
                    r = std::clamp(checker + static_cast<int>((x * 60) / W), 0, 255);
                    g = std::clamp(checker + static_cast<int>((y * 70) / H), 0, 255);
                    b = std::clamp(220 - checker + (((px + py) / 4) & 31), 0, 255);
                }
                row[x * 4 + 0] = static_cast<BYTE>(r);
                row[x * 4 + 1] = static_cast<BYTE>(g);
                row[x * 4 + 2] = static_cast<BYTE>(b);
                row[x * 4 + 3] = 255;
            }
        }
    };

    auto read_picture = [&](std::vector<BYTE> &out) -> bool {
        out.resize(static_cast<size_t>(W) * H * 4);
        if (!CmdOpen()) return false;
        D3D12_RESOURCE_BARRIER in = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &in);
        D3D12_TEXTURE_COPY_LOCATION src_loc = {}, dst_loc = {};
        src_loc.pResource = pipe.picture.gpu_tex;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_loc.pResource = readback;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = pipe.picture.layout;
        lfCore.cmd->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        D3D12_RESOURCE_BARRIER back = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &back);
        const UINT64 fence = CmdSubmit();
        if (!fence || !AwaitMark(lfCore.timeline, fence, 10000)) return false;
        BYTE *mapped = nullptr;
        D3D12_RANGE range = { 0, static_cast<SIZE_T>(rb_desc.Width) };
        if (FAILED(readback->Map(0, &range, reinterpret_cast<void **>(&mapped)))) return false;
        for (UINT y = 0; y < H; ++y)
            memcpy(out.data() + static_cast<size_t>(y) * W * 4, mapped + static_cast<size_t>(y) * pipe.picture.layout.Footprint.RowPitch, static_cast<size_t>(W) * 4);
        D3D12_RANGE empty = { 0, 0 };
        readback->Unmap(0, &empty);
        return true;
    };

    auto diff_stats = [&](const std::vector<BYTE> &a, const std::vector<BYTE> &b, double &mean_abs, double &pct_changed, int &max_abs) {
        double sum = 0.0;
        size_t changed = 0;
        max_abs = 0;
        const size_t pixels = static_cast<size_t>(W) * H;
        for (size_t i = 0; i < pixels; ++i)
        {
            const int dr = abs(int(a[i * 4 + 0]) - int(b[i * 4 + 0]));
            const int dg = abs(int(a[i * 4 + 1]) - int(b[i * 4 + 1]));
            const int db = abs(int(a[i * 4 + 2]) - int(b[i * 4 + 2]));
            const int m = std::max(dr, std::max(dg, db));
            if (m > max_abs) max_abs = m;
            if (m > 0) ++changed;
            sum += (dr + dg + db) / 3.0;
        }
        mean_abs = sum / static_cast<double>(pixels);
        pct_changed = 100.0 * static_cast<double>(changed) / static_cast<double>(pixels);
    };

    std::vector<BYTE> src, out1, out2, restored;
    const ULONGLONG t_run0 = GetTickCount64();
    double dlss5_gpu_ms = 0.0;
    LARGE_INTEGER fq = {};
    QueryPerformanceFrequency(&fq);
    for (int f = 0; f < 15; ++f)
    {
        fill_pattern(src, f);
        if (!StagedFill(pipe.picture, src.data(), W * 4, H) || !CmdOpen()) { printf("FAIL: upload %d\n", f); return 2; }
        D3D12_RESOURCE_BARRIER b_in = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        lfCore.cmd->ResourceBarrier(1, &b_in);
        StagedCopy(lfCore.cmd, pipe.picture);
        D3D12_RESOURCE_BARRIER b_out = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &b_out);
        const UINT64 fence = CmdSubmit();
        if (!fence || !AwaitMark(lfCore.timeline, fence, 10000)) { printf("FAIL: upload fence %d\n", f); return 2; }
        LARGE_INTEGER qt0 = {}, qt1 = {};
        QueryPerformanceCounter(&qt0);
        if (!Dlss5Run(pipe, f == 0, false)) { printf("FAIL: Dlss5Run frame %d\n", f); return 2; }
        QueryPerformanceCounter(&qt1);
        if (f > 0) dlss5_gpu_ms += double(qt1.QuadPart - qt0.QuadPart) * 1000.0 / double(fq.QuadPart);
    }
    const ULONGLONG run_ms = GetTickCount64() - t_run0;
    if (!read_picture(out1)) { printf("FAIL: readback 1\n"); return 2; }
    double mean1 = 0.0, pct1 = 0.0; int max1 = 0;
    diff_stats(src, out1, mean1, pct1, max1);

    // Static re-evaluation via source_copy with tweaked sliders (Intensity 220%, Warm +50).
    smOpt.dlss5_intensity = 220;
    smOpt.dlss5_warm = 50;
    if (!Dlss5HasSourceCopy() || !Dlss5Run(pipe, false, true) || !read_picture(out2)) { printf("FAIL: slider re-eval\n"); return 3; }
    double mean2 = 0.0, pct2 = 0.0, delta12 = 0.0, pct12 = 0.0; int max2 = 0, max12 = 0;
    diff_stats(src, out2, mean2, pct2, max2);
    diff_stats(out1, out2, delta12, pct12, max12);

    // Raw source restore verification.
    if (!Dlss5RestoreSource(pipe) || !read_picture(restored)) { printf("FAIL: restore\n"); return 3; }
    double mean_r = 0.0, pct_r = 0.0; int max_r = 0;
    diff_stats(src, restored, mean_r, pct_r, max_r);

    // Verify Off -> On toggle (Dlss5Close -> Dlss5PrepareSession -> Dlss5Run) works in the same session.
    Dlss5Close();
    std::vector<BYTE> reopened;
    double mean_reopen = 0.0, pct_reopen = 0.0; int max_reopen = 0;
    const bool reopen_ok = Dlss5PrepareSession(W, H) && Dlss5Run(pipe, true, false) && read_picture(reopened);
    if (reopen_ok) diff_stats(src, reopened, mean_reopen, pct_reopen, max_reopen);

    readback->Release();
    PipelineRelease(pipe);
    Dlss5Close();
    if (lfCore.ngx_ready) NgxShutdown();

    printf("TEST_DLSS5 %ux%u scale=%d%% model=%d warm_ms=%llu run15_ms=%llu steady_eval_ms=%.2f mean_diff=%.3f pct=%.2f max=%d | redo_mean=%.3f delta_vs_p1=%.3f pct12=%.2f | restore_max=%d | reopen=%s(%.3f)\n",
           W, H, smOpt.dlss5_scale.load(), smOpt.dlss5_model.load(),
           warm_ms, run_ms, dlss5_gpu_ms / 14.0, mean1, pct1, max1, mean2, delta12, pct12, max_r,
           reopen_ok ? "ok" : "fail", mean_reopen);
    const bool ok = mean1 > 0.5 && pct1 > 10.0 && delta12 > 0.25 && max_r == 0 && reopen_ok && mean_reopen > 0.5;
    printf(ok ? "PASS: DLSS 5 GPU test succeeded\n" : "FAIL: DLSS 5 GPU test did not meet thresholds\n");
    return ok ? 0 : 4;
}
static void EnableHighPerformancePriority()
{
    const HANDLE self = GetCurrentProcess();
    const BOOL prioOk = SetPriorityClass(self, HIGH_PRIORITY_CLASS);
    SetProcessPriorityBoost(self, FALSE); // FALSE = priority boost enabled
    bool throttleOff = false;
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
    {
        using SetProcInfoFn = BOOL(WINAPI *)(HANDLE, int, LPVOID, DWORD);
        if (auto fn = reinterpret_cast<SetProcInfoFn>(GetProcAddress(k32, "SetProcessInformation")))
        {
            struct PowerThrottlingState { ULONG Version; ULONG ControlMask; ULONG StateMask; };
            // Separate calls: 0x1 (EXECUTION_SPEED) works on Win10+ and Win11;
            // 0x4 (IGNORE_TIMER_RESOLUTION) works on Win11 22H2+ (no-op on Win10).
            PowerThrottlingState execOff = { 1, 0x1, 0 };
            if (fn(self, 4, &execOff, sizeof(execOff))) throttleOff = true;
            PowerThrottlingState timerOff = { 1, 0x4, 0 };
            fn(self, 4, &timerOff, sizeof(timerOff));
        }
    }
    Trace("[priority] process priority %s, Windows power throttling %s",
          prioOk ? "HIGH" : "unchanged", throttleOff ? "disabled" : "unchanged");
}

int wmain(int argc, wchar_t **argv)
{
    {
        char v[4] = {};
        if (GetEnvironmentVariableA("SM_DLL_PROBE", v, sizeof(v)) > 0 && v[0] == '1')
        {
            typedef LONG(NTAPI * RegisterFn)(ULONG, PVOID, PVOID, PVOID *);
            static PVOID cookie = nullptr;
            if (auto reg = reinterpret_cast<RegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")))
                reg(0, reinterpret_cast<PVOID>(&SmDllProbe), nullptr, &cookie);
        }
    }
    // (1.0.3 + one line, round 83: no other program's code in the engine by Windows' old ways in - global window hooks
    // such as RivaTuner Statistics Server's, AppInit DLLs, IMEs. The owner: "RTSS must not touch our engine at all".)
    { PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY none_in = {}; none_in.DisableExtensionPoints = 1;
      SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &none_in, sizeof(none_in)); }
    nooverlay::Start();   // (round 83: NVIDIA's overlay kept out - sm_nooverlay.inl)
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
    {
        typedef BOOL(WINAPI * DpiContextFn)(HANDLE);
        auto set_context = reinterpret_cast<DpiContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context == nullptr || !set_context(reinterpret_cast<HANDLE>(-4)))
            SetProcessDPIAware();
    }
    bool read_stdin = true;
    int write_preset = -1;
    UINT test_dlss5_w = 0, test_dlss5_h = 0;
    const wchar_t *video = nullptr;      // --video <job file>: Super Video (sm_video.inl)
    for (int i = 1; i < argc; ++i)
    {
        if (wcscmp(argv[i], L"--config") == 0 && i + 1 < argc) ReadStartFile(argv[++i]);
        else if (wcscmp(argv[i], L"--verbose") == 0) g_trace_echo = true;
        else if (wcscmp(argv[i], L"--no-stdin") == 0) read_stdin = false;
        else if (wcscmp(argv[i], L"--test-dlss5") == 0) { test_dlss5_w = 1920; test_dlss5_h = 1080; }
        else if (wcscmp(argv[i], L"--test-dlss5-1440") == 0) { test_dlss5_w = 2560; test_dlss5_h = 1440; }
        else if (wcscmp(argv[i], L"--video") == 0 && i + 1 < argc) { video = argv[++i]; ReadStartFile(video); }
        else if (wcscmp(argv[i], L"--write-preset") == 0 && i + 1 < argc) write_preset = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"--cards") == 0) { CardsTell(); return 0; }      // for the app's "Preferred GPU" (lf_gpu_core.inl)
        else if (wcscmp(argv[i], L"--vrr") == 0) { VrrTell(); return 0; }          // for the app's VRR switch (lf_vrr.inl)
        else if (wcscmp(argv[i], L"--rtss-quiet") == 0) { rtss::Quiet(); return 0; }
        else if (wcscmp(argv[i], L"--warm-shaders") == 0) { const ULONGLONG t0 = GetTickCount64(); const int n = ShaderWarmAll(); printf("warm %d shaders %.1f s\n", n, (GetTickCount64() - t0) / 1000.0); return 0; }   // (round 84: the app, once after an update)
        else if (wcschr(argv[i], L'=') != nullptr)
        {
            char line[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, line, sizeof(line) - 1, nullptr, nullptr);
            ApplyLine(line);
        }
    }
    if (write_preset >= 0) { dlssgpreset::Apply(write_preset); return 0; }
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
    EnableHighPerformancePriority();
    SetUnhandledExceptionFilter(CrashFilter);
    SetConsoleCtrlHandler(ConsoleStop, TRUE);
    KeepChildrenWithUs();
    Status("@state starting");
    // DLSS MFG's preset into the driver's profile of the engine, before anything of the engine is on the card.
    // Written by a second process of our own: the driver's profile session hands a hybrid laptop's screen to the
    // generator's card (DXGI then lists it there and refuses to duplicate it), which would take the source away
    // from Desktop Duplication. The profile is in the driver's database either way, and NGX reads it when it starts.
    if (NgxSelected())
    {
        wchar_t self[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, self, MAX_PATH) != 0)
        {
            wchar_t command[MAX_PATH + 64];
            swprintf_s(command, L"\"%ls\" --write-preset %d log=0", self, smOpt.dlssg_preset.load());
            STARTUPINFOW start = { sizeof(start) };
            PROCESS_INFORMATION process = {};
            if (CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process))
            {
                WaitForSingleObject(process.hProcess, 10000);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
                Trace("[dlssg] the preset went into the driver's profile of the engine (written by a process of its own)");
            }
            else
                Trace("[dlssg] the preset's writer did not start (error %lu) - the driver's own preset is used", GetLastError());
        }
    }
    if (!GpuCoreStart())
    {
        Status("@error nogpu No compatible graphics card was found");
        Status("@state stopped");
        return 1;
    }
    // Round 85: XeSS MFG on the tensor cores - on an RTX 20/30 the compatibility package's XeSS-FG engine (Intel's model
    // on its own CUDA runner), reached as DLSS MFG is: from here the engine runs DLSS MFG's road with that engine in it.
    if (XeSelected() && smOpt.xess_tensor.load())
    {
        const int sm = FgComputeCapability();
        // (Round 86: every RTX - 20 and 30 on the package's own kernels, 40 and 50 by its route for them; GTX and other makers'
        // cards have no tensor cores for it.)
        if (sm == 75 || sm == 86 || sm == 89 || sm >= 100) { g_generator = 0; g_xess_tensor = true; Trace("[xess] XeSS MFG on the tensor cores (SM%d): the compatibility package's XeSS-FG engine", sm); }
        else Trace("[xess] XeSS MFG on the tensor cores needs an RTX card (this card: SM%d) - Intel's library is used", sm);
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
    if (test_dlss5_w > 0 && test_dlss5_h > 0) return TestDlss5(test_dlss5_w, test_dlss5_h);
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
    if ((NgxSelected() || Dlss5Wanted()) && !lfCore.ngx_ready && !NgxStart())
        Trace("[card] NGX did not start - DLSS MFG / DLSS 5 is unavailable, the picture is shown as it is");
    if (Dlss5Wanted()) Dlss5WarmupStart();
    const int code = EngineRun();
    cursorfx::Restore();      // (the pointer as it was: sm_cursor.inl)
    SpoutClose();
    Dlss5Close();
    if (lfCore.ngx_ready) NgxShutdown();
    Status("@state stopped");
    Trace("[engine] ended with code %d", code);
    if (g_control.instance) { ReleaseMutex(g_control.instance); CloseHandle(g_control.instance); }
    return code;
}
