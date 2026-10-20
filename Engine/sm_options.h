// sm_options.h - what the app asks of the engine: one value per control on
// the MFG SUPER MOTION page. The app writes them into the engine's start
// file and, while it runs, sends each change as a line (sm_control.inl); the
// loop's thread takes the lines up between two frames, so the values only
// ever change there. The presenter and the window threads read them.
#pragma once

struct Options
{
    // Frame generation.
    std::atomic<int> mfg{1};           // DLSS MFG on
    std::atomic<int> multiplier{4};    // x2..x6
    std::atomic<int> flow{35};         // frames in between are made at this share of the picture, percent (game 7 only)
    std::atomic<int> game{7};          // the kind of game (kGameKinds); 7: as set, by flow
    std::atomic<int> queue{1};         // Queue target: 0 least delay, 1 balanced, 2 smoothest
    std::atomic<int> still{1};         // what does not move stays put (lf_fg_still.inl)
    std::atomic<int> guard{0};         // 1: no frames in between while the picture moves too fast for them (sm_guard.inl)
    std::atomic<int> msm_search{0};    // MSM FG's search for the motion: 0 in full, 1 the lighter one (msm_fg.h, MsmAsk::fast)
    std::atomic<int> aml{0};           // Adaptive Motion Learning: what the hand does to the picture is learnt, and the generator told (sm_learn.inl)
    std::atomic<int> frame_log{0};     // round 45: frames.log - every frame shown, its gap, the engine's lines among them (the app's development build only)
    std::atomic<int> log{1};           // round 39: 1 the engine keeps its log (engine.log); the app asks for it only when the person turns it on
    std::atomic<int> aml_flow{1};      // Adaptive Flow Scale: with aml, the size frames in between are made at follows how fast the game's picture moves (sm_learn.inl)
    std::atomic<int> dlssg_preset{0};  // round 42, DLSS MFG: 0 the driver's own, 1 preset A (Balanced), 2 B (Quality), 3 C (Performance) - sm_dlssg_preset.inl
    std::atomic<int> frame_limit{0};   // most frames a second shown, 0 none
    std::atomic<int> game_fps{0};      // round 42: the game's frame rate as the person gives it (20..500); 0 the engine reads it from the frames
    // Round 43, as Lossless Scaling has them (sm_cursor.inl, lf_show.inl, lf_desktop.inl).
    std::atomic<int> multi_display{0}; // 1: the picture stays while another window, on another screen, is in front
    std::atomic<int> crop_l{0}, crop_t{0}, crop_r{0}, crop_b{0};   // pixels left out of the captured window, each side
    std::atomic<int> cursor_clip{0};   // the pointer kept inside the picture while the game is in front
    std::atomic<int> cursor_speed{0};  // scaled onto the screen: the pointer's speed follows the scale
    std::atomic<int> cursor_hide{0};   // no pointer over the picture
    std::atomic<int> cursor_scale{0};  // scaled onto the screen: the pointer drawn as large as the picture's scale
    std::atomic<int> screen_limit{0};  // 0: the multiplier's own count; 1: only as many frames in between as fill the screen, a whole number a pair
    // The output.
    std::atomic<int> vrr{0};
    std::atomic<int> sync{1};          // on which refreshes a frame may be shown (lf_vrr.inl): 0 any moment, tear lines and all; 1 the newest at each; 2..4 VSync on every, every second, every third
    std::atomic<int> low_latency{1};
    std::atomic<int> hdr{0};
    std::atomic<int> gpu_class{1};     // the card's scheduling class: 1 realtime, 0 normal
    std::atomic<int> counter{0};       // the FPS counter, drawn into the frame
    std::atomic<int> spout{0};         // every frame shown is shared with other programs as a Spout sender (sm_spout.inl)
    std::atomic<int> link{1};          // 1: frames from DLSS 5 MANAGER's LIVE FLOW when it offers them (sm_dlss5_link.inl)
    // Upscaling (lf_window_fill.inl): what is done to every frame on its way to the screen.
    std::atomic<int> upscaler{0};      // 0 off, 1 sharpening only, 2 integer, 3 nearest neighbour, 4 Anime4K (lf_window_fill.inl)
    std::atomic<int> sharpness{50};    // how hard edges are drawn out, 0..100 - within what the picture's own contrast leaves room for
    std::atomic<int> over_sharpness{0};// 0..100 on top of that, with no such limit
    // The source.
    std::atomic<int> source{0};        // 0 the window in front when the countdown ends, 1 its whole screen
    std::atomic<int> capture_api{0};   // 0 DXGI (Desktop Duplication), 1 WGC (Windows Graphics Capture)
    std::atomic<int> window_mode{0};   // 0 borderless over its screen, 1 scaled onto its screen, 2 as it is
    std::atomic<int> monitor{0};       // whole screen: 0 the one the window in front is on, n = \\.\DISPLAYn
    std::atomic<int> delay{5};         // seconds before the window in front is taken
    std::atomic<int> motion{2};        // 2 the generator's own, 0 NVIDIA Optical Flow, 1 the engine's GPU estimate
    std::atomic<int> gpu{-1};          // the card, -1 the first NVIDIA one
    std::atomic<unsigned long long> hwnd{0};   // a window named outright (tests): no countdown
    std::atomic<unsigned long> owner{0};       // the app's process: its windows are never the source
};
static Options smOpt;

// Which generator makes the frames in between ("Generator" in the app):
// DLSS MFG (lf_dlssg.inl, NVIDIA RTX cards), XeSS MFG (xe_mfg.inl, any card
// Intel's library runs on) or MSM FG (msm_fg.h, the engine's own: any card).
// Read once, before the card is picked and before any one's libraries are
// loaded: they never share a process, and the app starts the engine anew to
// change it. Everything else in this file means the same for all three.
static int g_generator = 0; // 0 DLSS, 1 XeSS, 2 MSM
static bool g_generator_locked = false;
static bool NgxSelected() { return g_generator == 0; }
static bool XeSelected() { return g_generator == 1; }
static bool MsmSelected() { return g_generator == 2; }
static const char *GeneratorName() { return XeSelected() ? "XeSS MFG" : MsmSelected() ? "MSM FG" : "DLSS MFG"; }

// The kind of game the engine is told to expect ("Game type" in the app).
//
// What it decides is the size frames in between are made at. The generator
// follows a movement only so many of its own pixels from one real frame to
// the next: made small, a frame in between stays right for fast movement
// (and costs least); made larger it is finer while the picture moves slowly
// and breaks sooner when it moves fast. Measured on scenes whose every frame
// is known (docs\research\SUPER-MOTION-ROUND-4-20261005.md), share of pixels
// off, at 35 / 50 / 75 / 100% of a 2560x1440 picture: a pan of 32 pixels a
// frame 3.5 / 2.3 / 1.9 / 3.3%; a pan of 64 pixels 3.6 / 14 / 5 / 16%. And the
// first frame in between costs 1.0 / 1.7 / 2.3 / 3.7 ms on an RTX 3060.
//
// So: a flat picture that scrolls (2D) is made finest, one with some depth
// (2.5D) in between, and everything a camera turns or drives through at the
// size that follows furthest. "As set" leaves it to the Flow slider - which
// until round 5 stood alone, and at 100% was both the dearest setting and the
// one that broke first.
struct GameKind { const char *name; int flow; };
static const GameKind kGameKinds[] = {
    {"auto", 35}, {"2D", 75}, {"2.5D", 50}, {"first person", 35}, {"third person", 35},
    {"racing", 35}, {"open world", 35}, {"as set", 0},
};
static const int kGameAsSet = 7;

// Adaptive Flow Scale's pick for the game played (sm_learn.inl, LearnFlowDecide), percent; 0: none yet.
static std::atomic<int> g_learn_flow{0};

// The share of the picture frames in between are made at, percent. With Adaptive Flow Scale on (and Adaptive
// Motion Learning, which it is part of) and a pick made for the game, that pick - over the kind of game and Flow.
// Round 42, "Game FPS" in the app: the time between two of the game's frames as the person gave it, or 0 (read from
// the frames). Given, the engine stops guessing the game's pace from what the capture hands over: a picture the same
// as the one before is a repeat only within one frame of the game (after that the game is standing still, and it is
// its frame), frames are not let go unseen by a pace learnt, the frames in between a pair are counted from it, and
// the moments of the real frames keep its beat. (A user's report: "capture and output fps fluctuate with in-game
// actions ... add a manual override for capture fps".)
static double GivenFrameTime()
{
    const int fps = smOpt.game_fps.load();
    return fps >= 20 && fps <= 500 ? 1.0 / fps : 0.0;
}

static int FlowShare()
{
    if (smOpt.aml.load() && smOpt.aml_flow.load() && g_learn_flow.load() > 0) return g_learn_flow.load();
    const int game = smOpt.game.load();
    return game >= 0 && game < kGameAsSet ? kGameKinds[game].flow : smOpt.flow.load();
}

// Is the generator handed a motion field of the engine's (NVIDIA Optical Flow
// or the GPU estimate), or left to find the motion itself. Measured on the
// scenes of tests\scene.cpp: its frames in between come out the same with the
// engine's field, with none, and with a wrong one - it goes by its own - while
// the optical flow keeps every pair waiting about one frame of the game when
// the game fills the card (16 ms at 60 FPS). So its own is the default.
static bool MotionGiven()
{
    // (MSM FG finds the motion itself, on the card, as part of what it is: msm_fg.h.)
    return !MsmSelected() && smOpt.motion.load() != 2;
}

// The folder the engine keeps its own files in (the log, NGX's cache, the
// RTX 20/30 compatibility files): %LOCALAPPDATA%\MFG SUPER MOTION\Engine, the temporary
// folder when that cannot be made.
static void EngineDataPath(wchar_t (&path)[MAX_PATH])
{
    path[0] = 0;
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n && n < MAX_PATH - 32)
    {
        wcscpy_s(path, base);
        wcscat_s(path, L"\\MFG SUPER MOTION");
        CreateDirectoryW(path, nullptr);
        wcscat_s(path, L"\\Engine");
        if (CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return;
    }
    const DWORD size = GetTempPathW(MAX_PATH, path);
    if (size && size < MAX_PATH - 24)
    {
        wcscat_s(path, L"SuperMotionEngine");
        if (CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return;
    }
    GetCurrentDirectoryW(MAX_PATH, path);
}
