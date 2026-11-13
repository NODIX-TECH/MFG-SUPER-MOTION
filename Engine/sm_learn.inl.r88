// sm_learn.inl - Adaptive Motion Learning: what the player's hand does to the
// picture, learnt game by game while it is played.
//
// The idea is the user's own ("Input-Conditioned Adaptive Predictive MFG",
// the paper beside the app's sources). Taken from it is the idea; how frames
// in between are made stays what it is.
//
// A camera turned by the mouse, or by a pad's right stick, moves the whole
// picture: so many counts of the mouse, so many pixels. That number is the
// game's own (its sensitivity, its field of view), and so is how late the
// picture follows the hand (the game reads the mouse, draws, and the frame is
// seen some tens of milliseconds on). Neither is known beforehand, both show
// in what the engine already has:
//   - how far the picture moved from one real frame to the next - counted for
//     every pair, twenty-four blocks of it (sm_guard.inl);
//   - how far the hand moved meanwhile. This file listens for that: the
//     mouse's own counts (raw input) and the right stick of a pad. Only how
//     far, summed over a millisecond - never where the pointer is, never a
//     key or a button - and none of it is kept: what is kept is the two
//     numbers learnt from it.
// For every pair of real frames with the hand moving, the picture's movement
// is set against the hand's under fourteen delays, 0 to 130 ms. The delay
// under which the picture follows the hand best is the game's, the slope
// under it its gain (LearnModel). A few hundred pairs settle both; they go on
// being corrected for as long as the game is played, older pairs counting
// less and less, so a sensitivity changed in the game's menu is learnt anew
// by itself.
//
// What is learnt is kept for each game by its program's name:
// %LOCALAPPDATA%\MFG SUPER MOTION\Learned\<name>.aml, a few lines of text. "Settings" in
// the app lists them and deletes one to have it learnt from the start.
//
// What it is used for: where the generator looks. MSM FG (msm_fg.h) finds a
// piece of the picture within some 7% of the picture's width of where it was;
// a camera turned fast at 30 frames a second goes further than that between
// two frames, and such a pair comes out torn. Told beforehand where the
// picture as a whole went - the hand's movement under the game's delay, times
// its gain - it looks there too. On a photograph moved 260 pixels a frame
// (tests\msmtruth.exe photo 260): 8.7% of the pixels off without, 1.7% with;
// at 400 pixels 19.8% and 5.1%. It is an added place to look, never the only
// one: an expectation that is wrong costs nothing.
//
// When it is trusted: once learnt (enough pairs, and the picture following
// the hand under the delay found), and only while the last few expectations
// were near what the picture then did. In a menu the mouse moves a pointer
// and the picture stays; in a cutscene the picture moves and the hand does
// not: neither is taken for the camera, and neither is learnt from as if it
// were.
//
// The study. A game not studied yet is studied in front of the player: two
// lines at the top of the frame, in the middle (sm_banner.inl) - how far
// along it is and about how long it will take, and under it one plain thing
// to do, one after the other. Which things depends on the kind of game picked
// in the app ("Game type"):
//   first person, third person, auto, custom
//       1. Stand still. Move the camera left and right.
//       2. Now move the camera up and down.
//       3. Now walk forward.
//   open world: those three, and
//       4. Now drive a car. No car? Just stand still.
//   racing
//       1. Drive straight ahead.   2. Now take some turns.
//   2D, 2.5D
//       1. Move left and right.
// A step counts the pairs that are of its kind and of no other, from the
// moment it is asked for: a turn is the hand moving more across than down
// with the picture following, a look the other way round (round 18 counted
// both at once while the player turned, and the look was through before it
// was ever shown); walking and driving are the picture streaming outwards
// from its middle. A step is shown for three seconds at least and waited for
// 25 at most (the car: five seconds of a picture standing still is "no
// car"), so a player who does something else is not kept waiting - the
// learning goes on without the lines. Once through, the game counts as
// studied and the lines do not come back for it; "Delete" in the app's
// Settings starts it over.
//
// Not done with it here: moving the frames that are shown by what the hand
// did since they were drawn (the paper's "late warp"). See
// docs\ADAPTIVE-MOTION-LEARNING.md for why.
//
// It leans on Trace, Status and smOpt.aml; tests\learntruth.cpp brings its
// own of each and plays a game with a known delay and gain at it.

static bool LearnWanted()
{
    return smOpt.aml.load() != 0;
}

// One kind of hand - the mouse, or a pad's right stick: what the picture did against what it did.
struct LearnModel
{
    static constexpr int kDelays = 14;      // 0, 10 .. 130 ms
    // Each way (across, down): the hand's movement squared, the hand's times the picture's - under each delay -
    // and the picture's squared. Older pairs count less (kForget a pair).
    double hand[2][kDelays] = {}, both[2][kDelays] = {}, picture[2] = {};
    double pairs = 0.0;                     // pairs learnt from, older ones counting less
    unsigned long long seen = 0;            // all of them, ever
    // What follows from the sums (LearnSettle).
    int delay = -1;                         // which of the delays; -1: nothing yet
    double gain[2] = {};                    // the picture's movement (a share of its width) for one of the hand's
    double fit = 0.0;                       // how much of the picture's movement the hand explains, 0..1
    // While it runs: the last expectations against what the picture did.
    double miss = 1.0;                      // 0 right .. 1 wrong, evened out
    double scale = 1.0;                     // what the gain is times just now (aiming down a sight halves it)
};

static struct LearnState
{
    static constexpr int kEvents = 1024;    // a second of the hand, a millisecond each
    static constexpr double kForget = 0.997;
    static constexpr double kEnough = 150.0;        // pairs with the hand moving before a game counts as learnt (older ones counting less: some 200 in all)
    // The study's steps: what the player is asked to do, and how many seconds of it are enough (by the clock,
    // not by the frame: a game at 120 frames a second is studied as long as one at 30).
    // Round 40: kFree - "play as you like for thirty seconds": everything else (running, aiming, the buttons, the
    // read-outs) is learnt from that, and from all play after it, without being asked for.
    enum { kTurn, kLook, kWalk, kDrive, kStraight, kTurns, kScroll, kBack, kHud, kRun, kAim, kFree, kSteps };
    static constexpr double kStepSeconds[kSteps] = { 3.5, 2.0, 2.5, 3.5, 3.5, 2.5, 3.5, 2.0, 5.0, 3.0, 3.0, 30.0 };
    // Round 20. The keys, the mouse's buttons and a pad's buttons, triggers and left stick: which are held is
    // listened to, and for each what the picture does while it is held is summed - how far it goes as a whole and
    // how much it grows, a second. Nothing else of them is kept: never their order, never what was typed.
    //   0..255 a key or a mouse button, by its number; 256..271 a pad's buttons; 272, 273 its triggers;
    //   274..277 its left stick right, left, up, down.
    // Round 35: a pad that is not an XInput one (DualSense, DualShock, Switch Pro, any other gamepad or joystick)
    // is heard through raw input under the same numbers (LearnHidReport).
    static constexpr int kControls = 278;
    // Round 35, what each control does, three ways:
    //   - held alone (solo...): how it moves the picture by itself - a key that turns, the stick that strafes;
    //   - held with one other (duo, below): how fast the picture goes then against that other alone - running;
    //   - held while the camera's hand turns (aim / free: the gain the picture follows the hand with, with it
    //     held and without): aiming down a sight, a scope, a slower walk - taken the moment it is pressed.
    struct Control
    {
        double seconds = 0.0, across = 0.0, down = 0.0, grows = 0.0, across2 = 0.0, down2 = 0.0, grows2 = 0.0;
        double solo = 0.0, solo_across = 0.0, solo_down = 0.0, solo_across2 = 0.0, solo_down2 = 0.0, solo_speed = 0.0, solo_moving = 0.0;
        double aim = 0.0, aim_n = 0.0, free = 0.0, free_n = 0.0;
        bool told_aim = false;              // (said in the log, this run)
    };
    std::atomic<unsigned char> held[kControls] = {};
    Control control[kControls];
    // Two held together, the camera's hand at rest: seconds, and the picture's speed times seconds (key: the
    // smaller number times 65536 plus the other).
    struct Duo { double seconds = 0.0, speed = 0.0; bool told = false; };
    std::map<unsigned, Duo> duo;
    int held_before[8] = {}, held_before_n = 0;    // what was held the pair before (the gain is set anew when it changes)
    // Adaptive Flow Scale (round 35): how far the picture goes from one real frame to the next while it moves,
    // in pixels of a 2560-wide picture, four to a bin (the last: 252 and more), older pairs counting less.
    static constexpr int kSpeedBins = 64;
    double speed_bins[kSpeedBins] = {};
    int flow_want = 0, flow_seen = 0;       // the size a different pick would be, and for how many seconds in a row
    ULONGLONG flow_at = 0;                  // when the pick last changed
    // What stays on the screen while the picture moves - the read-outs (HUD): learnt on the small picture, a
    // number a pixel of it (up: it stayed where the picture went; down: it went along), kept with the game.
    static constexpr int kHudMost = 320 * 200;
    std::vector<float> hud_score;
    std::vector<unsigned char> hud_before, hud_cells;      // the small picture of the frame before; 1: a read-out
    int hud_w = 0, hud_h = 0;
    bool hud_shift_known = false;           // this pair: the picture went as a whole by ...
    int hud_shift[2] = {};                  // ... so many pixels of the small picture
    double hud_lasted = 0.0;                // ... in so long
    unsigned hud_pairs = 0, hud_told = 0;   // pairs learnt from; and when the cells were last made of them
    bool hud_fresh = false, hud_changed = false;        // cells the generator has not taken yet; a file not written yet
    std::mutex hud_lock;
    static constexpr ULONGLONG kStepLeast = 3000, kStepMost = 20000;    // a step is shown at least, and waited for at most, so long (ms)
    static constexpr ULONGLONG kNoCar = 5000;       // "now drive a car": the picture has stood this long (ms), and there is none
    static constexpr double kFaster = 1.4;          // driving: the picture grows at least so many times as fast as it did walking
    static constexpr double kGrows = 0.02;          // going forward: the picture grows by at least this much of itself a second ...
    static constexpr double kGrowsBy = 0.25;        // ... and that growing is this much of what its blocks do besides going one way together
    static constexpr double kFits = 0.45;           // ... and the hand explains this much of the picture's movement
    static constexpr double kFloor = 0.004;         // a movement of the picture smaller than this is none (a share of its width)
    struct Event { double at; float move[4]; };     // the mouse across and down (counts), the stick across and down (its throw times seconds)
    std::mutex lock;
    Event events[kEvents] = {};
    unsigned next = 0;
    HANDLE thread = nullptr;
    DWORD thread_id = 0;
    std::atomic<bool> quit{false};
    LearnModel model[2];                    // 0 the mouse, 1 the stick
    double last_stamp = 0.0;
    bool expected = false;                  // for the pair at hand
    float expect[2] = {};                   // a share of the picture's width, from the frame before to the frame after
    // The game.
    wchar_t file[MAX_PATH] = {};
    char game[160] = {}, title[160] = {};
    unsigned w = 0, h = 0;
    bool changed = false;
    ULONGLONG saved_at = 0, told_at = 0;
    int told_state = -1, told_percent = -1, told_delay = -1;
    unsigned long long used = 0, offered = 0;       // pairs an expectation was given for, pairs with the hand moving (since the last word in the log)
    // The study, in front of the player.
    bool studied = false;                   // this game was taken through it (kept with the game)
    int plan[8] = {}, plan_count = 0;       // its steps, for the kind of game picked in the app ...
    int plan_for = -1;                      // ... which was this one when they were laid out
    int step = 0;                           // which of them is asked for now
    double got = 0.0;                       // seconds of its kind since it was asked for
    ULONGLONG step_from = 0, done_at = 0;
    ULONGLONG moved_at = 0;                 // when the picture last moved (a passer-by is not the picture)
    // How fast the picture grows about its middle when the player walks, and when he drives: a share of itself
    // a second (kept with the game). [0] walking, [1] driving.
    double pace[2] = {}, pace_sum = 0.0;
    unsigned pace_pairs = 0;
    // The small picture of the frame before, and what it says of this pair (LearnLooked): seen finely.
    std::vector<unsigned char> luma_before;
    int luma_w = 0;
    double luma_at = 0.0, fine_at = 0.0;    // when it was taken; when the last word below was found
    bool fine = false;                      // it has something to say of this pair
    double fine_grow = 0.0;                 // how much the picture grows about its middle, a share of itself a second
    double fine_shift = 0.0, fine_edge = 0.0;       // how far it went as a whole, and its edge by growing: pixels of the small picture
    double fine_part = 0.0;                 // how much of what changed in it that explains, 0..1
    // What the lines say (the app sends its own language's: aml_say0 .. aml_say8). 0: the first line, "{0}" how
    // far along and "{1}" about how many seconds are left; 4: the last word; the others are the steps'.
    int pair_held = 0;                      // this pair (for the study): how many controls are held ...
    bool pair_aims = false;                 // ... and one of them a mouse button, a pad's button or trigger
    wchar_t say[15][192] = {
        L"Adaptive Motion Learning \x00B7 {0}% \x00B7 about {1} s",
        L"Stand still. Move the camera left and right.",
        L"Now move the camera up and down.",
        L"Now walk forward.",
        L"Adaptive Motion Learning \x00B7 done",
        L"Now drive a car. No car? Just stand still.",
        L"Drive straight ahead.",
        L"Now take some turns.",
        L"Move left and right.",
        L"Learning costs a little performance. It gets better when done.",
        L"Now walk backward.",
        L"Keep moving. Switch weapons or items a few times.",
        L"Now run: hold your run button while you move.",
        L"Now aim: hold your aim button and move the camera.",
        L"Now play as you like for 30 seconds." };
    ULONGLONG said_at = 0;
} g_learn;

static double LearnNow()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(frequency.QuadPart);
}

// The hand moved: summed into the millisecond it falls in.
static void LearnPush(double at, float mouse_x, float mouse_y, float stick_x, float stick_y)
{
    LearnState &g = g_learn;
    std::lock_guard<std::mutex> hold(g.lock);
    LearnState::Event &last = g.events[(g.next + LearnState::kEvents - 1) % LearnState::kEvents];
    LearnState::Event *into = &last;
    if (last.at <= 0.0 || at - last.at >= 0.001 || at < last.at)
    {
        into = &g.events[g.next];
        g.next = (g.next + 1) % LearnState::kEvents;
        *into = LearnState::Event();
        into->at = at;
    }
    into->move[0] += mouse_x; into->move[1] += mouse_y; into->move[2] += stick_x; into->move[3] += stick_y;
}

// What the hand did between two moments (the first not counted, the second counted).
static void LearnSum(double from, double to, float sum[4])
{
    LearnState &g = g_learn;
    sum[0] = sum[1] = sum[2] = sum[3] = 0.0f;
    std::lock_guard<std::mutex> hold(g.lock);
    for (int back = 1; back <= LearnState::kEvents; ++back)
    {
        const LearnState::Event &event = g.events[(g.next + LearnState::kEvents - back) % LearnState::kEvents];
        if (event.at <= from || event.at <= 0.0) break;
        if (event.at <= to) for (int i = 0; i < 4; ++i) sum[i] += event.move[i];
    }
}

// Is the hand turning the camera fast just now - a hundred counts of the mouse, or a third of the stick's
// throw, within the last tenth of a second? (Only asked while the switch is on: the hand is listened to then.)
static bool LearnTurning()
{
    if (!LearnWanted() || g_learn.thread == nullptr) return false;
    float sum[4];
    const double now = LearnNow();
    LearnSum(now - 0.1, now, sum);
    return std::fabs(sum[0]) + std::fabs(sum[1]) >= 100.0f || std::fabs(sum[2]) + std::fabs(sum[3]) >= 0.033f;
}

// ---- Listening to the hand --------------------------------------------------

// Round 35: every other pad - a DualSense or DualShock, a Switch Pro, a gamepad or joystick of any make - as
// raw input tells of it (HID), read by what its own report says it holds (hid.dll's HidP_ calls, asked for by
// name). Its right stick turns the camera as an XInput pad's does; its buttons, triggers, d-pad and left stick
// are held under the same numbers (256..277), so a game learnt with one pad knows the other. A pad XInput reads
// already ("IG_" in its name: every Xbox one) is left to it.
typedef LONG (__stdcall *HidCapsFn)(void *, HIDP_CAPS *);
typedef LONG (__stdcall *HidValueCapsFn)(HIDP_REPORT_TYPE, HIDP_VALUE_CAPS *, USHORT *, void *);
typedef LONG (__stdcall *HidUsagesFn)(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE *, ULONG *, void *, char *, ULONG);
typedef LONG (__stdcall *HidValueFn)(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, ULONG *, void *, char *, ULONG);
static struct { HidCapsFn caps; HidValueCapsFn value_caps; HidUsagesFn usages; HidValueFn value; bool tried; } g_hid = {};

struct LearnHidPad
{
    bool usable = false;
    bool sony = false;                      // Sony's pads: the right stick on Z and Rz, the triggers on Rx and Ry
    std::vector<unsigned char> preparsed;
    // Generic Desktop's X, Y, Z, Rx, Ry, Rz and the hat: there or not, the least and most it reports, its rest.
    struct Axis { bool there = false; LONG least = 0, most = 0; double rest = 0.0; bool rested = false; } axis[7];
    double value[7] = {};                   // -1 .. 1 (the hat: its raw step, -1 at rest)
    unsigned pressed = 0;                   // as an XInput pad's buttons (16), triggers (2), left stick right, left, up, down
    float right_x = 0.0f, right_y = 0.0f;   // the right stick's throw: 0 at rest (its slack left out) .. 1, down positive
};
static std::map<HANDLE, LearnHidPad> g_hid_pads;     // (the listening thread's own)

static float LearnThrown(double v)
{
    const double rest = 0.24;
    return std::fabs(v) < rest ? 0.0f : static_cast<float>((v > 0.0 ? 1.0 : -1.0) * (std::fabs(v) - rest) / (1.0 - rest));
}

static LearnHidPad &LearnHidPadOf(HANDLE device)
{
    auto found = g_hid_pads.find(device);
    if (found != g_hid_pads.end()) return found->second;
    LearnHidPad &pad = g_hid_pads[device];
    if (!g_hid.tried)
    {
        g_hid.tried = true;
        if (HMODULE hid = LoadLibraryW(L"hid.dll"))
        {
            g_hid.caps = reinterpret_cast<HidCapsFn>(GetProcAddress(hid, "HidP_GetCaps"));
            g_hid.value_caps = reinterpret_cast<HidValueCapsFn>(GetProcAddress(hid, "HidP_GetValueCaps"));
            g_hid.usages = reinterpret_cast<HidUsagesFn>(GetProcAddress(hid, "HidP_GetUsages"));
            g_hid.value = reinterpret_cast<HidValueFn>(GetProcAddress(hid, "HidP_GetUsageValue"));
        }
    }
    if (!g_hid.caps || !g_hid.value_caps || !g_hid.usages || !g_hid.value) return pad;
    wchar_t name[512] = {};
    UINT size = 511;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &size) != static_cast<UINT>(-1) && (wcsstr(name, L"IG_") || wcsstr(name, L"ig_"))) return pad;
    RID_DEVICE_INFO info = {};
    info.cbSize = sizeof(info);
    size = sizeof(info);
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1) || info.dwType != RIM_TYPEHID) return pad;
    size = 0;
    GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, nullptr, &size);
    if (size == 0 || size > 65536) return pad;
    pad.preparsed.resize(size);
    if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, pad.preparsed.data(), &size) == static_cast<UINT>(-1)) return pad;
    HIDP_CAPS caps = {};
    if (g_hid.caps(pad.preparsed.data(), &caps) != HIDP_STATUS_SUCCESS || caps.NumberInputValueCaps == 0) return pad;
    std::vector<HIDP_VALUE_CAPS> values(caps.NumberInputValueCaps);
    USHORT count = caps.NumberInputValueCaps;
    if (g_hid.value_caps(HidP_Input, values.data(), &count, pad.preparsed.data()) != HIDP_STATUS_SUCCESS) return pad;
    static const USAGE kAxes[7] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x39 };     // X, Y, Z, Rx, Ry, Rz, the hat
    for (USHORT i = 0; i < count; ++i)
    {
        const HIDP_VALUE_CAPS &v = values[i];
        if (v.UsagePage != 0x01) continue;
        const USAGE first = v.IsRange ? v.Range.UsageMin : v.NotRange.Usage, last = v.IsRange ? v.Range.UsageMax : v.NotRange.Usage;
        for (int a = 0; a < 7; ++a)
        {
            if (kAxes[a] < first || kAxes[a] > last || pad.axis[a].there) continue;
            LearnHidPad::Axis &axis = pad.axis[a];
            axis.there = true;
            axis.least = v.LogicalMin;
            axis.most = v.LogicalMax;
            // (A report of unsigned values often says its most as a negative number: it is all its bits.)
            if (axis.most <= axis.least && v.BitSize > 0 && v.BitSize < 32) { axis.least = 0; axis.most = static_cast<LONG>((1u << v.BitSize) - 1u); }
        }
    }
    pad.sony = info.hid.dwVendorId == 0x054C;
    pad.usable = pad.axis[0].there && pad.axis[1].there;
    char said[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, said, sizeof(said) - 1, nullptr, nullptr);
    Trace("[aml] a pad heard through raw input (vendor %04lX, product %04lX%s): %s", info.hid.dwVendorId, info.hid.dwProductId,
          pad.sony ? ", Sony's" : "", pad.usable ? "its sticks, buttons and triggers are listened to" : "not a pad with sticks - passed over");
    return pad;
}

// One report of a pad: its sticks, triggers, hat and buttons, as an XInput pad's would be.
static void LearnHidReport(LearnHidPad &pad, char *report, ULONG length)
{
    void *pre = pad.preparsed.data();
    static const USAGE kAxes[7] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x39 };
    for (int a = 0; a < 7; ++a)
    {
        LearnHidPad::Axis &axis = pad.axis[a];
        ULONG raw = 0;
        if (!axis.there || g_hid.value(HidP_Input, 0x01, 0, kAxes[a], &raw, pre, report, length) != HIDP_STATUS_SUCCESS) continue;
        if (a == 6)
        {
            const LONG step = static_cast<LONG>(raw) - axis.least;
            pad.value[6] = step >= 0 && step <= 7 ? step : -1;
            continue;
        }
        const double span = std::max(1.0, double(axis.most) - double(axis.least));
        pad.value[a] = std::clamp(2.0 * (double(static_cast<LONG>(raw)) - axis.least) / span - 1.0, -1.0, 1.0);
        if (!axis.rested) { axis.rested = true; axis.rest = pad.value[a]; }
    }
    // Which axes are the right stick and which the triggers: Sony's put the stick on Z and Rz; most others on Rx
    // and Ry where they have them, the triggers on Z (and Rz) then.
    const bool r_on_rxy = !pad.sony && pad.axis[3].there && pad.axis[4].there;
    const int rx = r_on_rxy ? 3 : 2, ry = r_on_rxy ? 4 : 5;
    const int lt = pad.sony ? 3 : r_on_rxy ? 2 : -1, rt = pad.sony ? 4 : r_on_rxy ? 5 : -1;
    pad.right_x = pad.axis[rx].there ? LearnThrown(pad.value[rx]) : 0.0f;
    pad.right_y = pad.axis[ry].there ? LearnThrown(pad.value[ry]) : 0.0f;
    unsigned pressed = 0;
    // A trigger is held when it has gone half its way from where it first rested (some rest at -1, some at 0).
    if (lt >= 0 && pad.axis[lt].there && std::fabs(pad.value[lt] - pad.axis[lt].rest) > 0.5) pressed |= 1u << 16;
    if (rt >= 0 && pad.axis[rt].there && std::fabs(pad.value[rt] - pad.axis[rt].rest) > 0.5) pressed |= 1u << 17;
    if (pad.value[0] > 0.5) pressed |= 1u << 18;
    if (pad.value[0] < -0.5) pressed |= 1u << 19;
    if (pad.value[1] < -0.5) pressed |= 1u << 20;       // (up is less on a HID pad)
    if (pad.value[1] > 0.5) pressed |= 1u << 21;
    // The hat as the d-pad: 0 up, then round to 7 up-left.
    if (pad.axis[6].there && pad.value[6] >= 0.0)
    {
        static const unsigned kHat[8] = { 0x1, 0x1 | 0x8, 0x8, 0x2 | 0x8, 0x2, 0x2 | 0x4, 0x4, 0x1 | 0x4 };
        pressed |= kHat[static_cast<int>(pad.value[6]) & 7];
    }
    // The buttons, by their number, as XInput's (Sony's: square, cross, circle, triangle, L1, R1, L2, R2, share,
    // options, L3, R3, PS, touch pad; the others' usual order: A, B, X, Y, LB, RB, back, start, L3, R3, home).
    static const unsigned kSony[14] = { 0x4000, 0x1000, 0x2000, 0x8000, 0x100, 0x200, 1u << 16, 1u << 17, 0x20, 0x10, 0x40, 0x80, 0x400, 0x800 };
    static const unsigned kOther[12] = { 0x1000, 0x2000, 0x4000, 0x8000, 0x100, 0x200, 0x20, 0x10, 0x40, 0x80, 0x400, 0x800 };
    USAGE buttons[64];
    ULONG many = 64;
    if (g_hid.usages(HidP_Input, 0x09, 0, buttons, &many, pre, report, length) == HIDP_STATUS_SUCCESS)
        for (ULONG i = 0; i < many; ++i)
        {
            const int b = static_cast<int>(buttons[i]) - 1;
            if (pad.sony ? (b >= 0 && b < 14) : (b >= 0 && b < 12)) pressed |= pad.sony ? kSony[b] : kOther[b];
        }
    pad.pressed = pressed;
}

static LRESULT CALLBACK LearnWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL) g_hid_pads.erase(reinterpret_cast<HANDLE>(lparam));
    if (message == WM_INPUT)
    {
        static std::vector<unsigned char> buffer;
        UINT size = 0;
        GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
        if (size < sizeof(RAWINPUTHEADER) || size > (1u << 20)) return DefWindowProcW(window, message, wparam, lparam);
        buffer.resize(std::max<size_t>(size, sizeof(RAWINPUT)));
        RAWINPUT &raw = *reinterpret_cast<RAWINPUT *>(buffer.data());
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1))
        {
            if (raw.header.dwType == RIM_TYPEHID)
            {
                LearnHidPad &pad = LearnHidPadOf(raw.header.hDevice);
                if (pad.usable && raw.data.hid.dwSizeHid > 0)
                    for (DWORD r = 0; r < raw.data.hid.dwCount; ++r)
                        LearnHidReport(pad, reinterpret_cast<char *>(raw.data.hid.bRawData) + size_t(r) * raw.data.hid.dwSizeHid, raw.data.hid.dwSizeHid);
            }
            else
            if (raw.header.dwType == RIM_TYPEMOUSE)
            {
                if ((raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (raw.data.mouse.lLastX != 0 || raw.data.mouse.lLastY != 0))
                    LearnPush(LearnNow(), static_cast<float>(raw.data.mouse.lLastX), static_cast<float>(raw.data.mouse.lLastY), 0.0f, 0.0f);
                // Its buttons: held or not, under the numbers keys have (1, 2, 4, 5, 6).
                const USHORT buttons = raw.data.mouse.usButtonFlags;
                static const struct { USHORT down, up; int key; } kButtons[] = {
                    { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON }, { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON },
                    { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON }, { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, VK_XBUTTON1 },
                    { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, VK_XBUTTON2 } };
                for (const auto &button : kButtons)
                {
                    if (buttons & button.down) g_learn.held[button.key] = 1;
                    if (buttons & button.up) g_learn.held[button.key] = 0;
                }
            }
            // A key: held or not. (Which, never in what order.)
            else if (raw.header.dwType == RIM_TYPEKEYBOARD && raw.data.keyboard.VKey > 6 && raw.data.keyboard.VKey < 255)
                g_learn.held[raw.data.keyboard.VKey] = (raw.data.keyboard.Flags & RI_KEY_BREAK) ? 0 : 1;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// A pad as XInput tells of it (its library is asked for by name: the engine starts without it too).
struct LearnPad { DWORD packet; WORD buttons; BYTE left_trigger, right_trigger; SHORT left_x, left_y, right_x, right_y; };
typedef DWORD (WINAPI *LearnPadRead)(DWORD, LearnPad *);

static DWORD WINAPI LearnThread(LPVOID)
{
    LearnState &g = g_learn;
    WNDCLASSW kind = {};
    kind.lpfnWndProc = LearnWindowProc;
    kind.hInstance = GetModuleHandleW(nullptr);
    kind.lpszClassName = L"MfgSuperMotionHand";
    RegisterClassW(&kind);
    const HWND window = CreateWindowExW(0, kind.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, kind.hInstance, nullptr);
    // The mouse and the keys, also while another window has them.
    RAWINPUTDEVICE devices[2] = { { 0x01, 0x02, RIDEV_INPUTSINK, window }, { 0x01, 0x06, RIDEV_INPUTSINK, window } };
    const bool heard = window != nullptr && RegisterRawInputDevices(devices, 2, sizeof(devices[0])) != FALSE;
    // And every pad and joystick (round 35): joysticks, gamepads, multi-axis controllers - and told when one goes.
    RAWINPUTDEVICE pads[3] = { { 0x01, 0x04, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window }, { 0x01, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window },
                               { 0x01, 0x08, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window } };
    const bool heard_pads = window != nullptr && RegisterRawInputDevices(pads, 3, sizeof(pads[0])) != FALSE;
    g_hid_pads.clear();
    for (auto &held : g.held) held = 0;
    LearnPadRead read = nullptr;
    for (const wchar_t *name : { L"xinput1_4.dll", L"xinput9_1_0.dll" })
    {
        const HMODULE library = read ? nullptr : LoadLibraryW(name);
        if (library) read = reinterpret_cast<LearnPadRead>(GetProcAddress(library, "XInputGetState"));
    }
    Trace("[aml] listening: the mouse and the keys %s, an Xbox pad %s, any other pad %s (which are held, and how far the mouse and the sticks go - nothing else)",
          heard ? "yes" : "no", read ? "yes" : "no", heard_pads ? "yes" : "no");
    bool there[4] = {};
    ULONGLONG looked = 0;
    double last = LearnNow();
    while (!g.quit.load())
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 4, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) g.quit = true;
            DispatchMessageW(&message);
        }
        const double now = LearnNow(), span = std::clamp(now - last, 0.0, 0.05);
        last = now;
        // Which pads are there: asked every two seconds (asking for one that is not costs a millisecond).
        const ULONGLONG tick = GetTickCount64();
        const bool look = tick - looked >= 2000;
        if (look) looked = tick;
        float across = 0.0f, down = 0.0f;
        unsigned pressed = 0;       // the pads' buttons (16), triggers (2), left stick right, left, up, down
        for (DWORD pad = 0; read && pad < 4; ++pad)
        {
            if (!there[pad] && !look) continue;
            LearnPad state = {};
            there[pad] = read(pad, &state) == ERROR_SUCCESS;
            if (!there[pad]) continue;
            pressed |= state.buttons;
            if (state.left_trigger > 60) pressed |= 1u << 16;
            if (state.right_trigger > 60) pressed |= 1u << 17;
            if (state.left_x > 16000) pressed |= 1u << 18;
            if (state.left_x < -16000) pressed |= 1u << 19;
            if (state.left_y > 16000) pressed |= 1u << 20;
            if (state.left_y < -16000) pressed |= 1u << 21;
            // Its throw, 0 at rest (the stick's own slack left out) .. 1; up is up on a stick and down in a picture.
            auto thrown = [](SHORT value) {
                const float v = static_cast<float>(value) / 32767.0f, rest = 0.24f;
                return std::fabs(v) < rest ? 0.0f : (v > 0.0f ? 1.0f : -1.0f) * (std::fabs(v) - rest) / (1.0f - rest);
            };
            across += thrown(state.right_x);
            down -= thrown(state.right_y);
        }
        // The other pads (raw input, LearnHidReport): their buttons held as well; their right stick only when no
        // XInput pad's turns the camera just now - a pad Steam or DS4Windows shows as an Xbox one is heard both ways.
        {
            const bool xinput_turns = across != 0.0f || down != 0.0f;
            for (const auto &one : g_hid_pads)
            {
                if (!one.second.usable) continue;
                pressed |= one.second.pressed;
                if (!xinput_turns) { across += one.second.right_x; down += one.second.right_y; }
            }
        }
        if (across != 0.0f || down != 0.0f) LearnPush(now, 0.0f, 0.0f, across * static_cast<float>(span), down * static_cast<float>(span));
        for (int bit = 0; bit < 22; ++bit) g.held[256 + bit] = (pressed >> bit) & 1u;
    }
    if (window) DestroyWindow(window);
    UnregisterClassW(kind.lpszClassName, kind.hInstance);
    return 0;
}

static void LearnListen(bool on)
{
    LearnState &g = g_learn;
    if (on == (g.thread != nullptr)) return;
    if (on)
    {
        g.quit = false;
        g.thread = CreateThread(nullptr, 0, LearnThread, nullptr, 0, &g.thread_id);
        return;
    }
    g.quit = true;
    PostThreadMessageW(g.thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(g.thread, 2000);
    CloseHandle(g.thread);
    g.thread = nullptr;
}

// ---- What is learnt ---------------------------------------------------------

// The delay under which the picture follows the hand best, the gain under it, and how well it follows.
static void LearnSettle(LearnModel &m)
{
    double least = 0.0, all = m.picture[0] + m.picture[1];
    int found = -1;
    for (int k = 0; k < LearnModel::kDelays; ++k)
    {
        // What of the picture's movement the hand leaves unexplained under this delay, both ways.
        double left = 0.0;
        for (int way = 0; way < 2; ++way)
            left += m.picture[way] - (m.hand[way][k] > 1e-12 ? m.both[way][k] * m.both[way][k] / m.hand[way][k] : 0.0);
        if (found < 0 || left < least) { least = left; found = k; }
    }
    m.delay = found;
    for (int way = 0; way < 2; ++way)
        m.gain[way] = found >= 0 && m.hand[way][found] > 1e-12 ? m.both[way][found] / m.hand[way][found] : 0.0;
    m.fit = all > 1e-12 ? std::clamp(1.0 - least / all, 0.0, 1.0) : 0.0;
}

static bool LearnKnown(const LearnModel &m)
{
    return m.delay >= 0 && m.pairs >= LearnState::kEnough && m.fit >= LearnState::kFits;
}

// The study's steps for the kind of game picked in the app (sm_options.h, kGameKinds), from the first one.
static void LearnPlan()
{
    LearnState &g = g_learn;
    const int game = smOpt.game.load();
    g.plan_for = game;
    g.plan_count = 0;
    auto add = [&](int step) { g.plan[g.plan_count++] = step; };
    // Round 40, shorter (the user: fewer steps, the rest learnt in the background): what only a step can show -
    // the camera both ways, a walk forward - then thirty seconds of play as the player likes, from which the
    // buttons (running, aiming), the read-outs and the rest are learnt without being asked for.
    if (game == 1 || game == 2) add(LearnState::kScroll);                                   // 2D, 2.5D: no camera, no car
    else if (game == 5) { add(LearnState::kStraight); add(LearnState::kTurns); }            // racing
    else { add(LearnState::kTurn); add(LearnState::kLook); add(LearnState::kWalk); }
    add(LearnState::kFree);
    g.step = 0;
    g.got = 0.0;
    g.step_from = 0;
    g.pace_sum = 0.0; g.pace_pairs = 0;
}

// How far along the study is, 0..100.
static int LearnPercent()
{
    const LearnState &g = g_learn;
    if (g.studied || g.plan_count == 0) return g.studied ? 100 : 0;
    if (g.step >= g.plan_count) return 100;
    // (By the seconds each step asks for: the thirty of free play are most of it.)
    double all = 0.0, done = 0.0;
    for (int i = 0; i < g.plan_count; ++i)
    {
        const double seconds = LearnState::kStepSeconds[g.plan[i]];
        all += seconds;
        if (i < g.step) done += seconds;
        else if (i == g.step) done += std::min(seconds, g.got);
    }
    return all > 0.0 ? static_cast<int>(100.0 * done / all) : 0;
}

// About how many seconds of the study are left: what each step still asks for, and two more for each to begin.
static int LearnSecondsLeft()
{
    const LearnState &g = g_learn;
    double left = 0.0;
    for (int i = g.step; i < g.plan_count; ++i)
        left += LearnState::kStepSeconds[g.plan[i]] + 2.0 - (i == g.step ? std::min(g.got, LearnState::kStepSeconds[g.plan[i]]) : 0.0);
    return std::max(1, static_cast<int>(std::ceil(left)));
}

// A pair, as the study sees it - `lasted` seconds of play: counted for the step asked for when it is of that
// step's kind.
//   hand_across, hand_down: how far the hand went, the larger of the two kinds of hand (mouse counts, or the
//   stick's throw as counts); follows: the picture as a whole went with it (across, down); outwards: the picture
//   grows about its middle (walking, driving) - at `pace`, a share of itself a second; across: it goes sideways
//   as a whole; stands: nothing moved, or next to nothing.
static void LearnStudyPair(double lasted, double hand_across, double hand_down, bool follows_across, bool follows_down, bool outwards, bool inwards, double pace, bool across, bool stands)
{
    LearnState &g = g_learn;
    if (g.studied || g.step >= g.plan_count) return;
    if (!stands) g.moved_at = GetTickCount64();
    bool counts = false;
    switch (g.plan[g.step])
    {
    case LearnState::kTurn:     counts = follows_across && hand_across >= 1.5 * hand_down; break;
    case LearnState::kLook:     counts = follows_down && hand_down >= 1.5 * hand_across; break;
    case LearnState::kWalk:
    case LearnState::kStraight: counts = outwards; break;
    // Driving is walking, faster: told from it by the pace the walk before it had (a game walked to a car in
    // would otherwise be through with the car before it was reached).
    case LearnState::kDrive:    counts = outwards && (g.pace[0] <= 0.0 || pace >= LearnState::kFaster * g.pace[0]); break;
    case LearnState::kTurns:    counts = across; break;
    case LearnState::kScroll:   counts = !stands; break;
    case LearnState::kBack:     counts = inwards; break;
    // (The read-outs are learnt from a pair in which the picture went somewhere as a whole: LearnSeen.)
    case LearnState::kHud:      counts = g.hud_shift_known; break;
    // Running: two held or more (a way to go and the run button), the camera's hand at rest, the picture going.
    case LearnState::kRun:      counts = g.pair_held >= 2 && hand_across < 4.0 && hand_down < 4.0 && !stands; break;
    // Aiming: a mouse button or a pad's button or trigger held while the picture follows the camera's hand.
    case LearnState::kAim:      counts = g.pair_aims && (follows_across || follows_down); break;
    // Playing as the player likes: any pair in which something happened - the picture or the hand moved.
    case LearnState::kFree:     counts = !stands || hand_across >= 4.0 || hand_down >= 4.0; break;
    }
    if (!counts) return;
    g.got += lasted;
    if (outwards) { g.pace_sum += pace; ++g.pace_pairs; }
}

// A line of the study in the app's language (UTF-8), in place of the engine's own English one.
static void LearnText(int index, const char *text)
{
    if (index < 0 || index > 14 || !text || !text[0]) return;
    wchar_t wide[192] = {};
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 191) > 1) wcscpy_s(g_learn.say[index], wide);
}

// Going forward, seen finely. The blocks count the picture's movement in steps of 8 pixels (of 2560): a drive
// shows in them, a walk does not - it moves the picture's edge by two or three pixels a frame, its inside by
// none. So while the study asks for a walk or a drive, the small picture itself is asked (320 across, the one
// sm_guard.inl counts on): how its brightness changed from the frame before, set against its slopes, says how
// far it went as a whole and how much it grew about its middle - least squares over all of it, good up to a
// pixel and a half of the small picture; beyond that the blocks speak. Called for every new real frame, taken
// at `stamp`, before LearnFrame; it does nothing, and keeps nothing, at any other time. A game at a hundred
// frames a second moves a third as far a frame as one at thirty: the picture is set against the one from at
// least a thirtieth of a second before, and what was found holds for the frames in between.
static void LearnLooked(const unsigned char *luma, int w, int h, double stamp)
{
    LearnState &g = g_learn;
    const int kind = !g.studied && g.plan_count && g.step < g.plan_count ? g.plan[g.step] : -1;
    const bool asked = LearnWanted() && (kind == LearnState::kWalk || kind == LearnState::kDrive || kind == LearnState::kStraight || kind == LearnState::kBack);
    if (!asked || luma == nullptr || w < 64 || h < 36 || w > 1024 || h > 1024)
    {
        g.fine = false;
        if (!g.luma_before.empty()) std::vector<unsigned char>().swap(g.luma_before);
        return;
    }
    const size_t n = static_cast<size_t>(w) * h;
    const bool follows = g.luma_before.size() == n && g.luma_w == w && stamp > g.luma_at && stamp - g.luma_at <= 0.25;
    if (g.fine && stamp - g.fine_at > 0.1) g.fine = false;
    if (follows && stamp - g.luma_at < 0.030) return;
    if (follows)
    {
        g.fine = false;
        const unsigned char *a = g.luma_before.data(), *b = luma;
        double m[3][3] = {}, r[3] = {}, all = 0.0;
        for (int y = 2; y < h - 2; ++y)
            for (int x = 2; x < w - 2; ++x)
            {
                const size_t at = static_cast<size_t>(y) * w + x;
                const double ix = (int(b[at + 1]) - int(b[at - 1]) + int(a[at + 1]) - int(a[at - 1])) * 0.25;
                const double iy = (int(b[at + w]) - int(b[at - w]) + int(a[at + w]) - int(a[at - w])) * 0.25;
                const double it = int(b[at]) - int(a[at]);
                const double row[3] = { ix, iy, ix * (x - w * 0.5) + iy * (y - h * 0.5) };
                for (int i = 0; i < 3; ++i)
                {
                    for (int j = i; j < 3; ++j) m[i][j] += row[i] * row[j];
                    r[i] -= row[i] * it;
                }
                all += it * it;
            }
        m[1][0] = m[0][1]; m[2][0] = m[0][2]; m[2][1] = m[1][2];
        // Three unknowns: across, down, growing.
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (std::fabs(det) > 1e-3 && all > 0.0)
        {
            const double u0 = (r[0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (r[1] * m[2][2] - m[1][2] * r[2]) + m[0][2] * (r[1] * m[2][1] - m[1][1] * r[2])) / det;
            const double u1 = (m[0][0] * (r[1] * m[2][2] - m[1][2] * r[2]) - r[0] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * r[2] - r[1] * m[2][0])) / det;
            const double u2 = (m[0][0] * (m[1][1] * r[2] - r[1] * m[2][1]) - m[0][1] * (m[1][0] * r[2] - r[1] * m[2][0]) + r[0] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
            g.fine = true;
            g.fine_at = stamp;
            g.fine_grow = u2 / (stamp - g.luma_at);
            g.fine_shift = std::hypot(u0, u1);
            g.fine_edge = std::fabs(u2) * w * 0.5;
            g.fine_part = std::clamp((u0 * r[0] + u1 * r[1] + u2 * r[2]) / all, 0.0, 1.0);
        }
    }
    else g.fine = false;
    g.luma_before.assign(luma, luma + n);
    g.luma_w = w;
    g.luma_at = stamp;
}

// How far the picture as a whole moved, a share of its width, from the blocks' count of it (sm_guard.inl: each
// block's movement, 0 0 where nothing happened). False: the blocks do not agree - no one movement to speak of.
static bool LearnPicture(const float (*moved)[2], int blocks, double picture[2])
{
    picture[0] = picture[1] = 0.0;
    float across[64], down[64];
    int moving = 0;
    for (int i = 0; i < blocks && i < 64; ++i)
        if (moved[i][0] != 0.0f || moved[i][1] != 0.0f) { across[moving] = moved[i][0]; down[moving] = moved[i][1]; ++moving; }
    if (moving == 0) return true;                       // it stood still
    if (moving < blocks / 4) return false;              // a thing or two moving in a picture at rest: not the camera
    std::nth_element(across, across + moving / 2, across + moving);
    std::nth_element(down, down + moving / 2, down + moving);
    const float mx = across[moving / 2], my = down[moving / 2];
    int with = 0;
    for (int i = 0; i < blocks && i < 64; ++i)
        if (std::fabs(moved[i][0] - mx) <= 0.012f && std::fabs(moved[i][1] - my) <= 0.012f) ++with;
    if (with < blocks / 3) return false;
    picture[0] = mx; picture[1] = my;
    return true;
}

// What a control is known to do to the picture: held for three seconds in all at least, and the picture going one
// way under it steadily (its mean movement twice what it wavers by, and a fiftieth of its width a second).
// Round 35: by what it did held alone where there is enough of that (held with others, a key is given part of
// what they do).
static bool LearnControlKnown(const LearnState::Control &c, double &across, double &down)
{
    across = down = 0.0;
    const bool alone = c.solo >= 3.0;
    const double seconds = alone ? c.solo : c.seconds;
    if (seconds < 3.0) return false;
    const double ax = (alone ? c.solo_across : c.across) / seconds, ay = (alone ? c.solo_down : c.down) / seconds;
    const double wavers = std::sqrt(std::max(0.0, (alone ? c.solo_across2 : c.across2) / seconds - ax * ax) +
                                    std::max(0.0, (alone ? c.solo_down2 : c.down2) / seconds - ay * ay));
    const double goes = std::hypot(ax, ay);
    if (goes < 0.02 || goes < 2.0 * wavers) return false;
    across = ax; down = ay;
    return true;
}

// The picture's speed for a pair, as the controls' learning counts it, a share of it a second: how much it grew
// where it streamed by (a walk, a drive), how far it went as a whole otherwise (a strafe, a scroll).
static double LearnSpeed(double ax, double ay, double grows, bool streams) { return streams ? std::fabs(grows) : std::hypot(ax, ay); }

// Round 35: how much faster the picture goes with `with` held too than with `mover` held alone - running, a
// boost, a walk key (slower). False until both were seen long enough to say, or when it makes no difference.
static bool LearnDuoFactor(int mover, int with, double &factor)
{
    const LearnState &g = g_learn;
    factor = 1.0;
    const auto found = g.duo.find(mover < with ? static_cast<unsigned>(mover) * 65536u + with : static_cast<unsigned>(with) * 65536u + mover);
    const LearnState::Control &c = g.control[mover];
    if (found == g.duo.end() || found->second.seconds < 1.5 || c.solo_moving < 1.5) return false;
    const double alone = c.solo_speed / c.solo_moving, both = found->second.speed / found->second.seconds;
    if (alone < 0.01) return false;
    factor = std::clamp(both / alone, 0.25, 4.0);
    return std::fabs(factor - 1.0) >= 0.2;
}

// Round 35: the gain the picture follows the camera's hand with while `c` is held, against while it is not -
// aiming down a sight, a scope. False until there is enough of both, or when it makes no difference.
static bool LearnAimFactor(const LearnState::Control &c, double &factor, double &held_gain, double &free_gain)
{
    factor = 1.0;
    if (c.aim_n < 20.0 || c.free_n < 20.0) return false;
    held_gain = c.aim / c.aim_n; free_gain = c.free / c.free_n;
    factor = held_gain / std::max(free_gain, 0.05);
    return std::fabs(factor - 1.0) >= 0.15;
}

// A control's name, for the log.
static void LearnControlName(int i, char *out, size_t size)
{
    static const char *kPad[22] = { "the d-pad up", "the d-pad down", "the d-pad left", "the d-pad right", "Start", "Back", "the left stick's press",
                                    "the right stick's press", "LB", "RB", "the pad's home button", "the pad's touch pad", "A", "B", "X", "Y",
                                    "the left trigger", "the right trigger", "the left stick right", "the left stick left", "the left stick up", "the left stick down" };
    if (i >= 256 && i < 278) { strcpy_s(out, size, kPad[i - 256]); return; }
    if (i == VK_LBUTTON || i == VK_RBUTTON || i == VK_MBUTTON || i == VK_XBUTTON1 || i == VK_XBUTTON2)
    {
        sprintf_s(out, size, "the %s mouse button", i == VK_LBUTTON ? "left" : i == VK_RBUTTON ? "right" : i == VK_MBUTTON ? "middle" : i == VK_XBUTTON1 ? "fourth" : "fifth");
        return;
    }
    char key[64] = {};
    const UINT scan = MapVirtualKeyA(static_cast<UINT>(i), MAPVK_VK_TO_VSC);
    if (scan != 0 && GetKeyNameTextA(static_cast<LONG>(scan << 16), key, sizeof(key)) > 0) sprintf_s(out, size, "the %s key", key);
    else sprintf_s(out, size, "key %d", i);
}

// With the camera's hand at rest: how far the picture is expected to go in `lasted` seconds by what is held.
static bool LearnHeldExpect(double lasted, float &across, float &down)
{
    LearnState &g = g_learn;
    double sum_x = 0.0, sum_y = 0.0;
    int known = 0, held = 0, which[2] = {};
    for (int i = 0; i < LearnState::kControls; ++i)
    {
        if (!g.held[i].load()) continue;
        if (held < 2) which[held] = i;
        ++held;
        double ax, ay;
        if (LearnControlKnown(g.control[i], ax, ay)) { sum_x += ax; sum_y += ay; ++known; }
    }
    if (known == 0 || held > 6) return false;
    // Round 35: two held, one that moves the picture by itself and one that makes it go faster or slower (run,
    // walk): what the first does alone, times what the second does to it.
    if (held == 2)
        for (int k = 0; k < 2; ++k)
        {
            double ax, ay, factor;
            if (LearnControlKnown(g.control[which[k]], ax, ay) && LearnDuoFactor(which[k], which[1 - k], factor))
            {
                across = static_cast<float>(std::clamp(ax * factor * lasted, -0.5, 0.5));
                down = static_cast<float>(std::clamp(ay * factor * lasted, -0.5, 0.5));
                return true;
            }
        }
    // (Two that do the same - a key and the stick - are not twice the movement.)
    across = static_cast<float>(std::clamp(sum_x / known * lasted, -0.5, 0.5));
    down = static_cast<float>(std::clamp(sum_y / known * lasted, -0.5, 0.5));
    return true;
}

// The small picture of a new real frame, after LearnFrame has said how the picture went (round 20): the
// read-outs are learnt. Where the picture went by (sx, sy), a pixel of it is either where its place came from -
// the game's world - or what it was before - a read-out, the player's own car, a crosshair. Whichever of the two
// it is nearer to is counted for it, when they differ enough to tell; a see-through panel's inside goes with the
// world behind it, and that is what it shows. Seven in ten pairs one way for a while make a pixel a read-out.
static void LearnSeen(const unsigned char *luma, int w, int h)
{
    LearnState &g = g_learn;
    if (!LearnWanted() || luma == nullptr || w < 64 || h < 36 || w * h > LearnState::kHudMost)
    {
        g.hud_shift_known = false;
        return;
    }
    const size_t n = static_cast<size_t>(w) * h;
    if (g.hud_w != w || g.hud_h != h || g.hud_score.size() != n)
    {
        // (A map kept for the game at another size is let go: the picture is another one.)
        g.hud_w = w; g.hud_h = h;
        g.hud_score.assign(n, 0.0f);
        g.hud_before.clear();
        g.hud_shift_known = false;
    }
    if (g.hud_shift_known && g.hud_before.size() == n)
    {
        const int sx = g.hud_shift[0], sy = g.hud_shift[1];
        const unsigned char *a = g.hud_before.data();
        for (int y = 0; y < h; ++y)
        {
            const int from_y = y - sy;
            if (from_y < 0 || from_y >= h) continue;
            for (int x = 0; x < w; ++x)
            {
                const int from_x = x - sx;
                if (from_x < 0 || from_x >= w) continue;
                const size_t at = static_cast<size_t>(y) * w + x;
                const int stays = std::abs(int(luma[at]) - int(a[at])), moves = std::abs(int(luma[at]) - int(a[static_cast<size_t>(from_y) * w + from_x]));
                if (std::max(stays, moves) < 6) continue;
                const float says = std::clamp((moves - stays) / 8.0f, -1.0f, 1.0f);
                g.hud_score[at] = g.hud_score[at] * 0.97f + says;
            }
        }
        ++g.hud_pairs;
        g.changed = true;
    }
    g.hud_before.assign(luma, luma + n);
}

// The read-outs' cells, made of what was learnt: a pixel counted well up, and the ones beside it.
static void LearnHudCells()
{
    LearnState &g = g_learn;
    const int w = g.hud_w, h = g.hud_h;
    if (w <= 0 || g.hud_score.size() != static_cast<size_t>(w) * h) return;
    std::vector<unsigned char> sure(g.hud_score.size()), cells(g.hud_score.size());
    // Round 41: half as much evidence again before a pixel counts (12, was 8) - and only where a game puts its
    // read-outs: along the picture's edges (a fifth of it in from each side) and a small spot in its middle (the
    // crosshair). A piece of a wall or a tree that happened to look the same while the picture moved, anywhere
    // else, was a "read-out" held still by SMFG: a square that stood while everything around it moved.
    for (size_t i = 0; i < sure.size(); ++i)
    {
        const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
        const bool edge = x < w / 5 || x >= w - w / 5 || y < h / 5 || y >= h - h / 5;
        const bool middle = std::abs(2 * x - w) < w / 40 && std::abs(2 * y - h) < h / 24;      // a crosshair's size, not a character's
        sure[i] = g.hud_score[i] >= 12.0f && (edge || middle);
    }
    // And compact: a read-out is text, an icon, a map - a cell counts with eight or more of the twenty-four around it
    // counted as well. Lone cells scattered over the sky or a treetop were the picture, not a read-out.
    {
        std::vector<unsigned char> compact(sure.size(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                if (!sure[static_cast<size_t>(y) * w + x]) continue;
                int around = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                    {
                        const int nx = x + dx, ny = y + dy;
                        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < w && ny < h && sure[static_cast<size_t>(ny) * w + nx]) ++around;
                    }
                compact[static_cast<size_t>(y) * w + x] = around >= 8;
            }
        sure.swap(compact);
    }
    unsigned count = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            bool any = false;
            for (int dy = -1; dy <= 1 && !any; ++dy)
                for (int dx = -1; dx <= 1 && !any; ++dx)
                {
                    const int nx = x + dx, ny = y + dy;
                    any = nx >= 0 && ny >= 0 && nx < w && ny < h && sure[static_cast<size_t>(ny) * w + nx];
                }
            cells[static_cast<size_t>(y) * w + x] = any;
            count += any;
        }
    // Round 39: a read-out is small - a minimap, figures, a bar, a crosshair. A patch of cells that hang together and
    // cover more than three hundredths of the picture is something else that stayed where it was while the camera
    // turned: the player's own car or character, the road under it, a hood. Handed over as a read-out it was held
    // still by the generator - a GTA V player's car and the road behind it (a fifth of the picture) stood in the
    // middle of every frame in between, also when the player was on foot. Such patches are left out.
    {
        std::vector<int> stack;
        std::vector<unsigned char> seen(cells.size(), 0), dropped(cells.size(), 0);
        const size_t most = cells.size() * 3 / 100;     // (round 41: three hundredths - a minimap, a wide bar; was one)
        for (size_t start = 0; start < cells.size(); ++start)
        {
            if (!cells[start] || seen[start]) continue;
            std::vector<int> patch;
            stack.assign(1, static_cast<int>(start));
            seen[start] = 1;
            while (!stack.empty())
            {
                const int at = stack.back();
                stack.pop_back();
                patch.push_back(at);
                const int x = at % w, y = at / w;
                const int next[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
                for (const auto &n : next)
                {
                    if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h) continue;
                    const size_t i = static_cast<size_t>(n[1]) * w + n[0];
                    if (cells[i] && !seen[i]) { seen[i] = 1; stack.push_back(static_cast<int>(i)); }
                }
            }
            if (patch.size() > most)
                for (const int at : patch) { cells[at] = 0; --count; dropped[at] = 1; }
        }
        // And the crumbs around such a patch (its ragged edge, the road's grain beside the car): within eight
        // cells of it, nothing is a read-out either.
        std::vector<unsigned char> beside(cells.size(), 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                if (dropped[static_cast<size_t>(y) * w + x])
                    for (int dy = -8; dy <= 8; ++dy)
                        for (int dx = -8; dx <= 8; ++dx)
                        {
                            const int nx = x + dx, ny = y + dy;
                            if (nx >= 0 && ny >= 0 && nx < w && ny < h) beside[static_cast<size_t>(ny) * w + nx] = 1;
                        }
        for (size_t i = 0; i < cells.size(); ++i)
            if (cells[i] && beside[i]) { cells[i] = 0; --count; }
    }
    // A quarter of the picture "a read-out" is no read-out: the count of the picture's movement got it wrong.
    if (count * 4 > cells.size()) return;
    unsigned differ = 0;
    if (g.hud_cells.size() == cells.size()) { for (size_t i = 0; i < cells.size(); ++i) differ += cells[i] != g.hud_cells[i]; }
    else differ = count + 1;
    if (differ * 500 < cells.size() + 500 && !(differ && g.hud_cells.empty())) return;      // a fifth of a percent of it: not worth a new map
    {
        std::lock_guard<std::mutex> hold(g.hud_lock);
        g.hud_cells.swap(cells);
        g.hud_fresh = true;
    }
    if (LearnWanted()) Trace("[aml] the read-outs (HUD) of %s: %u of %zu cells of the picture stay while it moves (%u pairs learnt from) - handed to the generator",
          g.game[0] ? g.game : "this game", count, g.hud_cells.size(), g.hud_pairs);
}

// New cells for the generator? (Asked by it before each pair; true once for each map.)
// Round 41: only while Adaptive Motion Learning is on. Off, the read-outs kept for the game (its cache) were handed
// over all the same when the game was taken up - turned off, it was not off for SMFG. Now off hands over none, and
// takes back what was handed over before (an empty map clears the generator's); on again, they are handed anew.
static bool LearnHudTake(std::vector<unsigned char> &cells, int &w, int &h)
{
    LearnState &g = g_learn;
    static bool given = false;
    if (!LearnWanted())
    {
        if (!given) return false;
        given = false;
        g.hud_fresh = true;
        cells.clear(); w = h = 0;
        return true;
    }
    if (!g.hud_fresh) return false;
    std::lock_guard<std::mutex> hold(g.hud_lock);
    cells = g.hud_cells; w = g.hud_w; h = g.hud_h;
    g.hud_fresh = false;
    given = !cells.empty();
    return true;
}

// A new real frame, taken at `stamp` (the capture's clock: seconds, the counter the hand's moments are read
// from). `restart`: it does not follow the one before (a cut, the first). `moved`: the blocks' count of how the
// picture moved since the frame before, `blocks` of them. Afterwards LearnExpect says what was expected of
// this pair - asked before the pair itself was learnt from.
static void LearnFrame(double stamp, bool restart, const float (*moved)[2], int blocks)
{
    LearnState &g = g_learn;
    g.expected = false;
    g.hud_shift_known = false;
    if (!LearnWanted())
    {
        g.last_stamp = 0.0;
        return;
    }
    const double before = g.last_stamp;
    g.last_stamp = stamp;
    if (restart || before <= 0.0 || stamp <= before || stamp - before > 0.25) return;
    // What is held this pair (round 35: for the gain while aiming, and for the study's run and aim steps).
    int held_list[8] = {}, held_n = 0;
    bool aims = false;
    for (int i = 0; i < LearnState::kControls; ++i)
        if (g.held[i].load())
        {
            if (held_n < 8) held_list[held_n] = i;
            ++held_n;
            // A mouse button, or a pad's button or trigger (not its left stick).
            if ((i >= VK_LBUTTON && i <= VK_XBUTTON2 && i != VK_CANCEL) || (i >= 256 && i < 274)) aims = true;
        }
    g.pair_held = held_n;
    g.pair_aims = aims;
    // Something else is held than the pair before: the gain the picture follows the camera's hand with is what
    // it was learnt to be with that held - an aim button pressed is a slower camera from this pair on, not after
    // a dozen pairs of wrong expectations; let go, the gain it had without it.
    {
        bool same = held_n == g.held_before_n;
        for (int i = 0; same && i < std::min(held_n, 8); ++i) same = held_list[i] == g.held_before[i];
        if (!same && held_n <= 6)
        {
            double target = 0.0, factor, held_gain, free_gain;
            int known = 0;
            for (int i = 0; i < held_n; ++i)
                if (LearnAimFactor(g.control[held_list[i]], factor, held_gain, free_gain))
                    target = known++ == 0 ? held_gain : target * factor;
            for (int i = 0; known == 0 && i < std::min(g.held_before_n, 8); ++i)
                if (LearnAimFactor(g.control[g.held_before[i]], factor, held_gain, free_gain)) { target = free_gain; known = 1; }
            if (known)
                for (LearnModel &m : g.model) m.scale = std::clamp(target, 0.25, 2.5);
        }
        g.held_before_n = held_n;
        for (int i = 0; i < std::min(held_n, 8); ++i) g.held_before[i] = held_list[i];
    }
    // Adaptive Flow Scale: how far the picture went in this pair, a share of its width (the larger of what the
    // blocks saw and what the hand says - a fast turn goes further than the blocks can follow).
    auto note_speed = [&g](double share) {
        const double pixels = share * 2560.0;
        if (pixels < 2.0) return;
        for (double &bin : g.speed_bins) bin *= 0.9998;
        g.speed_bins[std::min(LearnState::kSpeedBins - 1, static_cast<int>(pixels / 4.0))] += 1.0;
        g.changed = true;
    };
    // What the hand did while this pair was drawn, under each delay.
    float hand[LearnModel::kDelays][4];
    bool active[2] = {}, went[4] = {};
    for (int k = 0; k < LearnModel::kDelays; ++k)
    {
        LearnSum(before - 0.010 * k, stamp - 0.010 * k, hand[k]);
        for (int i = 0; i < 4; ++i)
            if (std::fabs(hand[k][i]) >= (i < 2 ? 2.0f : 0.0015f)) went[i] = true;
    }
    // What the picture did - also for the study, which counts a walk with the hand at rest.
    double picture[2];
    const bool whole = LearnPicture(moved, blocks, picture);
    {
        // How far the hand went, the most under any delay (the stick's throw times seconds as counts: a third
        // of a throw for a thirtieth of a second is a hundred).
        double hand_across = 0.0, hand_down = 0.0;
        for (int k = 0; k < LearnModel::kDelays; ++k)
        {
            hand_across = std::max({hand_across, double(std::fabs(hand[k][0])), double(std::fabs(hand[k][2])) * 9000.0});
            hand_down = std::max({hand_down, double(std::fabs(hand[k][1])), double(std::fabs(hand[k][3])) * 9000.0});
        }
        // The blocks (six across, four down) as one picture: each goes `shift` with the others and `grow` times
        // its place from the middle besides - by least squares, which finds a growing far smaller than the
        // blocks' own steps of 8 pixels (a walk moves the picture's edge by one such step a frame, and its
        // inside by none). Going forward is a picture that grows.
        const int count = std::min(blocks, 24);
        const double tall = g.w && g.h ? double(g.h) / g.w : 0.5625;
        int moving = 0;
        double shift[2] = {}, with = 0.0, spread = 0.0, all = 0.0, left = 0.0, most = 0.0;
        for (int i = 0; i < count; ++i)
        {
            shift[0] += moved[i][0] / count; shift[1] += moved[i][1] / count;
            if (std::fabs(moved[i][0]) >= 0.0025 || std::fabs(moved[i][1]) >= 0.0025) ++moving;
            most = std::max(most, std::hypot(double(moved[i][0]), double(moved[i][1])));
        }
        for (int i = 0; i < count; ++i)
        {
            const double qx = ((i % 6) + 0.5) / 6.0 - 0.5, qy = (((i / 6) + 0.5) / 4.0 - 0.5) * tall;
            with += qx * (moved[i][0] - shift[0]) + qy * (moved[i][1] - shift[1]);
            spread += qx * qx + qy * qy;
        }
        const double grow = spread > 0.0 ? with / spread : 0.0;
        for (int i = 0; i < count; ++i)
        {
            const double qx = ((i % 6) + 0.5) / 6.0 - 0.5, qy = (((i / 6) + 0.5) / 4.0 - 0.5) * tall;
            const double ox = moved[i][0] - shift[0], oy = moved[i][1] - shift[1];
            all += ox * ox + oy * oy;
            left += (ox - grow * qx) * (ox - grow * qx) + (oy - grow * qy) * (oy - grow * qy);
        }
        double pace = grow / (stamp - before);
        bool streams = std::fabs(pace) >= LearnState::kGrows && all > 0.0 && all - left >= LearnState::kGrowsBy * all, stands = moving <= 2;
        // Where the blocks saw little or nothing - none of them more than its one step - the small picture
        // seen finely (LearnLooked), as long as what it says is within what it can see.
        if (g.fine && most < 0.0047 && g.fine_shift + g.fine_edge <= 1.5)
        {
            // It moves at all: a tenth of a pixel of the small one, and that explains a tenth of what changed.
            // It goes forward: it grows, and by no less than half of how far it goes as a whole besides (the
            // camera bobbing with each step).
            const bool moves = g.fine_part >= 0.1 && g.fine_shift + g.fine_edge >= 0.1;
            if (moves) stands = false;
            if (!streams && moves && std::fabs(g.fine_grow) >= LearnState::kGrows && g.fine_edge >= 0.5 * g.fine_shift)
            {
                streams = true;
                pace = g.fine_grow;
            }
        }
        // Forward: it grows. Back: it shrinks.
        const bool outwards = streams && pace > 0.0, inwards = streams && pace < 0.0;
        // The read-outs: this pair says something of them when the picture went somewhere as a whole - two
        // pixels of the small picture or more (LearnSeen, called next, does the learning).
        // Only a camera that turns says it: going forward, the picture's middle stays where it is and is no
        // read-out. So: it does not grow or shrink, and seven blocks in ten went the one way, to their own step.
        g.hud_shift_known = false;
        int agree = 0;
        for (int i = 0; i < count; ++i)
            if (std::fabs(moved[i][0] - picture[0]) <= 0.0035 && std::fabs(moved[i][1] - picture[1]) <= 0.0035) ++agree;
        if (whole && g.hud_w > 0 && !streams && agree * 10 >= count * 7)
        {
            const int sx = static_cast<int>(std::lround(picture[0] * g.hud_w)), sy = static_cast<int>(std::lround(picture[1] * g.hud_w));
            if (std::abs(sx) + std::abs(sy) >= 2 && std::abs(sx) <= 28 && std::abs(sy) <= 28)
            {
                g.hud_shift_known = true;
                g.hud_shift[0] = sx; g.hud_shift[1] = sy;
                g.hud_lasted = stamp - before;
            }
        }
        // What is held: summed for each what the picture does meanwhile - with the camera's hand at rest, or
        // it would be the hand's doing. (More than six at once: a hand on the keyboard for something else.)
        if (hand_across < 4.0 && hand_down < 4.0)
        {
            int which[6], count_held = 0;
            for (int i = 0; i < LearnState::kControls && count_held <= 6; ++i)
                if (g.held[i].load()) { if (count_held < 6) which[count_held] = i; ++count_held; }
            if (count_held >= 1 && count_held <= 6)
            {
                const double lasted = stamp - before;
                const double ax = whole ? picture[0] / lasted : 0.0, ay = whole ? picture[1] / lasted : 0.0, gr = streams ? pace : 0.0;
                // (Older play counts less: half after some five minutes of the control held.)
                const double keep = 1.0 - lasted / 400.0;
                for (int i = 0; i < count_held; ++i)
                {
                    LearnState::Control &c = g.control[which[i]];
                    c.seconds = c.seconds * keep + lasted;
                    c.across = c.across * keep + ax * lasted; c.down = c.down * keep + ay * lasted; c.grows = c.grows * keep + gr * lasted;
                    c.across2 = c.across2 * keep + ax * ax * lasted; c.down2 = c.down2 * keep + ay * ay * lasted; c.grows2 = c.grows2 * keep + gr * gr * lasted;
                }
                // Round 35. Held alone: what it does by itself. Two together: how fast the picture goes then, set
                // against each of them alone - the run button found by the picture going faster with it. (The
                // speeds only of pairs in which the picture was seen to go: one too slow to be seen is no speed.)
                // A picture seen to go: streaming by, or gone further as a whole than the least that counts.
                const bool goes = streams || (whole && std::hypot(picture[0], picture[1]) >= LearnState::kFloor);
                if (whole || streams)
                {
                    const double speed = goes ? LearnSpeed(ax, ay, gr, streams) : 0.0;
                    if (count_held == 1)
                    {
                        LearnState::Control &c = g.control[which[0]];
                        c.solo = c.solo * keep + lasted;
                        c.solo_across = c.solo_across * keep + ax * lasted; c.solo_down = c.solo_down * keep + ay * lasted;
                        c.solo_across2 = c.solo_across2 * keep + ax * ax * lasted; c.solo_down2 = c.solo_down2 * keep + ay * ay * lasted;
                        if (goes) { c.solo_speed = c.solo_speed * keep + speed * lasted; c.solo_moving = c.solo_moving * keep + lasted; }
                    }
                    else if (count_held == 2 && goes)
                    {
                        const int a = std::min(which[0], which[1]), b = std::max(which[0], which[1]);
                        LearnState::Duo &d = g.duo[static_cast<unsigned>(a) * 65536u + b];
                        d.seconds = d.seconds * keep + lasted;
                        d.speed = d.speed * keep + speed * lasted;
                        for (int k = 0; k < 2 && !d.told; ++k)
                        {
                            const int mover = k == 0 ? a : b, with = k == 0 ? b : a;
                            double factor;
                            if (!LearnDuoFactor(mover, with, factor)) continue;
                            d.told = true;
                            char first[80], second[80];
                            LearnControlName(with, first, sizeof(first));
                            LearnControlName(mover, second, sizeof(second));
                            Trace("[aml] %s with %s: the picture goes %.1f times as fast as with %s alone (%s)", first, second, factor, second,
                                  factor > 1.0 ? "running, a boost" : "walking, slowed down");
                        }
                    }
                }
                g.changed = true;
            }
        }
        LearnStudyPair(stamp - before, hand_across, hand_down, whole && hand_across >= 4.0 && picture[0] != 0.0, whole && hand_down >= 4.0 && picture[1] != 0.0,
                       outwards, inwards, pace, std::fabs(shift[0]) >= 0.004, stands);
        static const bool told = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
        if (told && !g.studied)
            Trace("[aml] pair: %d blocks moved, together %.4f %.4f, growing %.3f a second (%.0f%% of the rest); finely %s%.3f a second, its edge %.2f and all of it %.2f, %.0f%% of the change; the hand %.0f across %.0f down - %s",
                  moving, shift[0], shift[1], grow / (stamp - before), all > 0.0 ? 100.0 * (all - left) / all : 0.0, g.fine ? "" : "(none) ",
                  g.fine_grow, g.fine_edge, g.fine_shift, g.fine_part * 100.0, hand_across, hand_down, outwards ? "forward" : inwards ? "back" : stands ? "stands" : "other");
    }
    active[0] = went[0] || went[1];
    active[1] = went[2] || went[3];
    if (!active[0] && !active[1])
    {
        // The camera's hand rests: what is held may move the picture all the same - a 2D game scrolled by a key
        // or the left stick, a step sideways. Where that is known, the generator is told.
        float across = 0.0f, down = 0.0f;
        if (LearnHeldExpect(stamp - before, across, down))
        {
            g.expected = true;
            g.expect[0] = across; g.expect[1] = down;
        }
        note_speed(std::max(whole ? std::hypot(picture[0], picture[1]) : 0.0, g.expected ? std::hypot(double(g.expect[0]), double(g.expect[1])) : 0.0));
        return;
    }
    ++g.offered;
    // What is expected of the picture: each hand's movement under its delay, times its gain.
    double expect[2] = {};
    bool known = true;
    for (int kind = 0; kind < 2; ++kind)
    {
        if (!active[kind]) continue;
        const LearnModel &m = g.model[kind];
        if (!LearnKnown(m) || m.miss >= 0.5) { known = false; continue; }
        for (int way = 0; way < 2; ++way) expect[way] += m.gain[way] * m.scale * hand[m.delay][kind * 2 + way];
    }
    if (known)
    {
        g.expected = true;
        g.expect[0] = static_cast<float>(std::clamp(expect[0], -0.5, 0.5));
        g.expect[1] = static_cast<float>(std::clamp(expect[1], -0.5, 0.5));
        ++g.used;
    }
    note_speed(std::max(whole ? std::hypot(picture[0], picture[1]) : 0.0, known ? std::hypot(expect[0], expect[1]) : 0.0));
    // What the picture did. Learnt from when one hand moved (two at once cannot be told apart).
    if (!whole || active[0] == active[1]) return;
    const int kind = active[0] ? 0 : 1;
    LearnModel &m = g.model[kind];
    if (LearnKnown(m))
    {
        // The expectation against it: how wrong, as a share of the larger of the two.
        double said[2], off = 0.0, size = 0.0, was = 0.0;
        for (int way = 0; way < 2; ++way)
        {
            said[way] = m.gain[way] * m.scale * hand[m.delay][kind * 2 + way];
            off += (said[way] - picture[way]) * (said[way] - picture[way]);
            size += picture[way] * picture[way];
            was += said[way] * said[way];
        }
        const double large = std::max({std::sqrt(size), std::sqrt(was), LearnState::kFloor});
        m.miss += 0.2 * (std::clamp(std::sqrt(off) / large, 0.0, 1.0) - m.miss);
        // The gain just now: where both are large enough to say, the picture's movement over the expected one.
        if (std::sqrt(was) >= 2.0 * LearnState::kFloor && std::sqrt(size) >= 2.0 * LearnState::kFloor)
        {
            const double along = (said[0] * picture[0] + said[1] * picture[1]) / (was / (m.scale * m.scale)) / m.scale;
            m.scale += 0.15 * (std::clamp(along, 0.25, 2.5) - m.scale);
            // Round 35: that gain, for each control - with it held and without (more than six held: a hand on
            // the keyboard for something else).
            if (held_n <= 6)
            {
                const double gain_now = std::clamp(along, 0.25, 2.5);
                for (int i = 0; i < LearnState::kControls; ++i)
                {
                    LearnState::Control &c = g.control[i];
                    bool on = false;
                    for (int k = 0; k < held_n; ++k) on = on || held_list[k] == i;
                    if (on) { c.aim = c.aim * 0.995 + gain_now; c.aim_n = c.aim_n * 0.995 + 1.0; }
                    else { c.free = c.free * 0.995 + gain_now; c.free_n = c.free_n * 0.995 + 1.0; }
                    double factor, held_gain, free_gain;
                    if (on && !c.told_aim && LearnAimFactor(c, factor, held_gain, free_gain))
                    {
                        c.told_aim = true;
                        char name[80];
                        LearnControlName(i, name, sizeof(name));
                        Trace("[aml] %s held: the camera follows the hand at %.2f of its gain without it (%s) - taken the moment it is pressed",
                              name, factor, factor < 1.0 ? "aiming, a scope" : "a faster camera");
                    }
                }
            }
        }
    }
    // The hand moved and the picture stood: a pointer in a menu, a map. That takes the trust away for as long
    // as it lasts (above) and is not learnt from - an evening's menus would wear the gain down to nothing.
    if (picture[0] == 0.0 && picture[1] == 0.0) return;
    for (int way = 0; way < 2; ++way)
    {
        m.picture[way] = m.picture[way] * LearnState::kForget + picture[way] * picture[way];
        for (int k = 0; k < LearnModel::kDelays; ++k)
        {
            const double h = hand[k][kind * 2 + way];
            m.hand[way][k] = m.hand[way][k] * LearnState::kForget + h * h;
            m.both[way][k] = m.both[way][k] * LearnState::kForget + h * picture[way];
        }
    }
    const bool knew = LearnKnown(m);
    m.pairs = m.pairs * LearnState::kForget + 1.0;
    ++m.seen;
    LearnSettle(m);
    if (!knew && LearnKnown(m))
    {
        m.miss = 0.25; m.scale = 1.0;
        Trace("[aml] %s learnt for %s: the picture follows it %d ms late, %.3f%% of its width a %s across and %.3f%% down (it explains %.0f%% of the picture's movement, %llu pairs)",
              kind == 0 ? "the mouse" : "the stick", g.game[0] ? g.game : "this game", m.delay * 10,
              m.gain[0] * 100.0 * (kind == 0 ? 1.0 : 0.01), kind == 0 ? "count" : "hundredth of a second at full throw",
              m.gain[1] * 100.0 * (kind == 0 ? 1.0 : 0.01), m.fit * 100.0, m.seen);
    }
    g.changed = true;
}

// What was expected of the pair LearnFrame was last told of: how far the picture went from the frame before to
// the frame after, a share of its width. False: nothing is expected (the hand at rest, the game not learnt, or
// the last expectations wrong).
static bool LearnExpect(float &across, float &down)
{
    across = g_learn.expect[0]; down = g_learn.expect[1];
    return g_learn.expected;
}

// ---- Kept for each game -----------------------------------------------------

// The read-outs' file, beside the game's own: <game>.hud - a line saying its size, then a number a pixel of
// the small picture (what was learnt, in quarters, -124..124).
static bool LearnHudFile(wchar_t path[MAX_PATH])
{
    const LearnState &g = g_learn;
    const size_t length = wcslen(g.file);
    if (length < 5 || length >= MAX_PATH) return false;
    wcscpy_s(path, MAX_PATH, g.file);
    wcscpy_s(path + length - 4, 5, L".hud");
    return true;
}

static void LearnHudSave()
{
    LearnState &g = g_learn;
    wchar_t path[MAX_PATH], draft[MAX_PATH];
    if (!g.hud_changed || g.hud_w <= 0 || g.hud_pairs < 30 || !LearnHudFile(path)) return;
    swprintf_s(draft, L"%s.new", path);
    FILE *file = _wfsopen(draft, L"wb", _SH_DENYWR);
    if (!file) return;
    fprintf(file, "hud1 %d %d %u\n", g.hud_w, g.hud_h, g.hud_pairs);
    std::vector<signed char> kept(g.hud_score.size());
    for (size_t i = 0; i < kept.size(); ++i) kept[i] = static_cast<signed char>(std::clamp(std::lround(g.hud_score[i] * 4.0f), -124L, 124L));
    const bool whole = fwrite(kept.data(), 1, kept.size(), file) == kept.size();
    fclose(file);
    if (whole && MoveFileExW(draft, path, MOVEFILE_REPLACE_EXISTING)) g.hud_changed = false; else DeleteFileW(draft);
}

static void LearnHudLoad()
{
    LearnState &g = g_learn;
    g.hud_score.clear(); g.hud_before.clear();
    g.hud_w = g.hud_h = 0; g.hud_pairs = g.hud_told = 0;
    g.hud_changed = false;
    { std::lock_guard<std::mutex> hold(g.hud_lock); g.hud_cells.clear(); g.hud_fresh = false; }
    wchar_t path[MAX_PATH];
    FILE *file = LearnHudFile(path) ? _wfsopen(path, L"rb", _SH_DENYNO) : nullptr;
    if (!file) return;
    char line[64] = "";
    int w = 0, h = 0;
    unsigned pairs = 0;
    if (fgets(line, sizeof(line), file) && sscanf_s(line, "hud1 %d %d %u", &w, &h, &pairs) == 3 && w >= 64 && h >= 36 && w * h <= LearnState::kHudMost)
    {
        std::vector<signed char> kept(static_cast<size_t>(w) * h);
        if (fread(kept.data(), 1, kept.size(), file) == kept.size())
        {
            g.hud_w = w; g.hud_h = h; g.hud_pairs = pairs;
            g.hud_score.resize(kept.size());
            for (size_t i = 0; i < kept.size(); ++i) g.hud_score[i] = kept[i] / 4.0f;
        }
    }
    fclose(file);
    if (g.hud_w > 0) { LearnHudCells(); g.hud_told = g.hud_pairs; }
}

static void LearnSave()
{
    LearnState &g = g_learn;
    if (!g.file[0] || !g.changed) return;
    LearnHudSave();
    wchar_t draft[MAX_PATH];
    swprintf_s(draft, L"%s.new", g.file);
    FILE *file = _wfsopen(draft, L"w", _SH_DENYWR);
    if (!file) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    const bool known = LearnKnown(g.model[0]) || LearnKnown(g.model[1]);
    const LearnModel &lead = g.model[g.model[1].pairs > g.model[0].pairs ? 1 : 0];
    fprintf(file, "version=1\ngame=%s\ntitle=%s\npicture=%ux%u\nupdated=%04u-%02u-%02u %02u:%02u\nlearned=%d\npercent=%d\ndelay_ms=%d\nfit=%.0f\nstudied=%d\nwalk=%.5f\ndrive=%.5f\n",
            g.game, g.title, g.w, g.h, now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, known ? 1 : 0, LearnPercent(),
            lead.delay < 0 ? 0 : lead.delay * 10, lead.fit * 100.0, g.studied ? 1 : 0, g.pace[0], g.pace[1]);
    for (int kind = 0; kind < 2; ++kind)
    {
        const LearnModel &m = g.model[kind];
        const char *name = kind == 0 ? "mouse" : "stick";
        fprintf(file, "%s.pairs=%.3f\n%s.seen=%llu\n%s.picture=%.9g %.9g\n", name, m.pairs, name, m.seen, name, m.picture[0], m.picture[1]);
        for (int way = 0; way < 2; ++way)
        {
            fprintf(file, "%s.hand%d=", name, way);
            for (int k = 0; k < LearnModel::kDelays; ++k) fprintf(file, "%.9g ", m.hand[way][k]);
            fprintf(file, "\n%s.both%d=", name, way);
            for (int k = 0; k < LearnModel::kDelays; ++k) fprintf(file, "%.9g ", m.both[way][k]);
            fprintf(file, "\n");
        }
    }
    // What each control held does to the picture (those held two seconds and more): seconds, then the sums;
    // since round 35 then what it does held alone, and the camera's gain with it held and without.
    for (int i = 0; i < LearnState::kControls; ++i)
    {
        const LearnState::Control &c = g.control[i];
        if (c.seconds >= 2.0 || c.aim_n >= 5.0)
            fprintf(file, "control.%d=%.3f %.6g %.6g %.6g %.6g %.6g %.6g %.3f %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.6g %.3f\n", i, c.seconds, c.across, c.down,
                    c.grows, c.across2, c.down2, c.grows2, c.solo, c.solo_across, c.solo_down, c.solo_across2, c.solo_down2, c.solo_speed, c.aim, c.aim_n, c.free, c.free_n,
                    c.solo_moving);
    }
    // Two held together: seconds, and the picture's speed times seconds.
    for (const auto &d : g.duo)
        if (d.second.seconds >= 1.0) fprintf(file, "duo.%u.%u=%.3f %.6g\n", d.first / 65536u, d.first % 65536u, d.second.seconds, d.second.speed);
    // How far the picture goes a frame (Adaptive Flow Scale): pixels of 2560, four to a bin.
    fprintf(file, "speed=");
    for (double bin : g.speed_bins) fprintf(file, "%.4g ", bin);
    fprintf(file, "\n");
    fclose(file);
    if (MoveFileExW(draft, g.file, MOVEFILE_REPLACE_EXISTING)) { g.changed = false; g.saved_at = GetTickCount64(); }
}

static void LearnLoad()
{
    LearnState &g = g_learn;
    g.model[0] = LearnModel(); g.model[1] = LearnModel();
    g.studied = false;
    g.plan_for = -1;
    g.done_at = 0;
    g.pace[0] = g.pace[1] = 0.0;
    for (auto &control : g.control) control = LearnState::Control();
    g.duo.clear();
    g.held_before_n = 0;
    for (double &bin : g.speed_bins) bin = 0.0;
    g.flow_want = g.flow_seen = 0;
    g.flow_at = 0;
    g_learn_flow = 0;           // (another game: its own pick, once its speeds are known)
    LearnHudLoad();
    FILE *file = g.file[0] ? _wfsopen(g.file, L"r", _SH_DENYNO) : nullptr;
    if (!file) return;
    char line[1024];
    while (fgets(line, sizeof(line), file))
    {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *value = eq + 1;
        if (strcmp(line, "studied") == 0) g.studied = atoi(value) != 0;
        else if (strcmp(line, "walk") == 0) g.pace[0] = atof(value);
        else if (strcmp(line, "drive") == 0) g.pace[1] = atof(value);
        else if (strncmp(line, "control.", 8) == 0)
        {
            const int i = atoi(line + 8);
            if (i >= 0 && i < LearnState::kControls)
            {
                LearnState::Control &c = g.control[i];
                const int got = sscanf_s(value, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &c.seconds, &c.across, &c.down, &c.grows,
                                         &c.across2, &c.down2, &c.grows2, &c.solo, &c.solo_across, &c.solo_down, &c.solo_across2, &c.solo_down2, &c.solo_speed,
                                         &c.aim, &c.aim_n, &c.free, &c.free_n, &c.solo_moving);
                if (got != 7 && got != 18) c = LearnState::Control();
                else if (got == 7) { const LearnState::Control older = c; c = LearnState::Control(); c.seconds = older.seconds; c.across = older.across;
                                     c.down = older.down; c.grows = older.grows; c.across2 = older.across2; c.down2 = older.down2; c.grows2 = older.grows2; }
            }
        }
        else if (strncmp(line, "duo.", 4) == 0)
        {
            unsigned a = 0, b = 0;
            LearnState::Duo d;
            if (sscanf_s(line + 4, "%u.%u", &a, &b) == 2 && a < b && b < static_cast<unsigned>(LearnState::kControls) &&
                sscanf_s(value, "%lf %lf", &d.seconds, &d.speed) == 2)
                g.duo[a * 65536u + b] = d;
        }
        else if (strcmp(line, "speed") == 0)
        {
            char *end = nullptr;
            const char *at = value;
            for (int i = 0; i < LearnState::kSpeedBins; ++i) { g.speed_bins[i] = strtod(at, &end); if (end == at) { g.speed_bins[i] = 0.0; break; } at = end; }
        }
        for (int kind = 0; kind < 2; ++kind)
        {
            const char *name = kind == 0 ? "mouse." : "stick.";
            if (strncmp(line, name, 6) != 0) continue;
            LearnModel &m = g.model[kind];
            const char *what = line + 6;
            auto row = [&](double *into, int count) {
                char *end = nullptr;
                const char *at = value;
                for (int i = 0; i < count; ++i) { into[i] = strtod(at, &end); if (end == at) break; at = end; }
            };
            if (strcmp(what, "pairs") == 0) m.pairs = atof(value);
            else if (strcmp(what, "seen") == 0) m.seen = _strtoui64(value, nullptr, 10);
            else if (strcmp(what, "picture") == 0) row(m.picture, 2);
            else if (strcmp(what, "hand0") == 0) row(m.hand[0], LearnModel::kDelays);
            else if (strcmp(what, "hand1") == 0) row(m.hand[1], LearnModel::kDelays);
            else if (strcmp(what, "both0") == 0) row(m.both[0], LearnModel::kDelays);
            else if (strcmp(what, "both1") == 0) row(m.both[1], LearnModel::kDelays);
        }
    }
    fclose(file);
    for (LearnModel &m : g.model)
    {
        LearnSettle(m);
        if (LearnKnown(m)) m.miss = 0.25;       // trusted from the first pair on; a wrong one takes it away again
    }
}

// The game that is played: the program the source window belongs to (the window in front, for a whole screen).
// What was learnt for it before is taken up.
static void LearnGame(HWND window, unsigned w, unsigned h)
{
    LearnState &g = g_learn;
    if (window == nullptr) window = GetForegroundWindow();
    wchar_t program[MAX_PATH] = L"", title[160] = L"";
    DWORD process = 0;
    if (window && GetWindowThreadProcessId(window, &process) && process)
    {
        const HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
        if (handle)
        {
            DWORD size = MAX_PATH;
            if (!QueryFullProcessImageNameW(handle, 0, program, &size)) program[0] = 0;
            CloseHandle(handle);
        }
        GetWindowTextW(window, title, 160);
    }
    const wchar_t *base = wcsrchr(program, L'\\');
    std::wstring name = base ? base + 1 : (program[0] ? program : L"game");
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0) name.resize(name.size() - 4);
    for (wchar_t &c : name) if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_';
    wchar_t folder[MAX_PATH] = L"", next[MAX_PATH] = L"";
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, MAX_PATH);
    if (n && n < MAX_PATH - 100 && name.size() < 80)
    {
        wcscat_s(folder, L"\\MFG SUPER MOTION");
        CreateDirectoryW(folder, nullptr);
        wcscat_s(folder, L"\\Learned");
        CreateDirectoryW(folder, nullptr);
        swprintf_s(next, L"%s\\%s.aml", folder, name.c_str());
    }
    g.w = w; g.h = h;
    if (wcscmp(next, g.file) == 0) return;
    LearnSave();
    wcscpy_s(g.file, next);
    WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, g.game, sizeof(g.game), nullptr, nullptr);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, g.title, sizeof(g.title), nullptr, nullptr);
    for (char *c = g.title; *c; ++c) if (*c == '\n' || *c == '\r') *c = ' ';
    LearnLoad();
    g.changed = false;
    g.told_state = -1;
    Trace("[aml] %s: %s", g.game, LearnKnown(g.model[0]) || LearnKnown(g.model[1]) ? "learnt before - taken up" :
          (g.model[0].seen || g.model[1].seen) ? "partly learnt before - going on" : "not learnt yet");
}

// "Delete" in the app, for a game by its name. If that is the game played now, it is learnt from the start.
static void LearnForget(const char *game)
{
    LearnState &g = g_learn;
    if (_stricmp(game, g.game) != 0) return;
    g.model[0] = LearnModel(); g.model[1] = LearnModel();
    g.studied = false;
    g.plan_for = -1;
    g.done_at = 0;
    g.pace[0] = g.pace[1] = 0.0;
    g.changed = false;
    g.told_state = -1;
    for (auto &control : g.control) control = LearnState::Control();
    g.duo.clear();
    for (double &bin : g.speed_bins) bin = 0.0;
    g.flow_want = g.flow_seen = 0;
    g_learn_flow = 0;
    wchar_t hud[MAX_PATH];
    if (LearnHudFile(hud)) DeleteFileW(hud);
    g.hud_score.assign(g.hud_score.size(), 0.0f);
    g.hud_pairs = g.hud_told = 0; g.hud_changed = false;
    { std::lock_guard<std::mutex> hold(g.hud_lock); g.hud_cells.assign(g.hud_cells.size(), 0); g.hud_fresh = !g.hud_cells.empty(); }
    if (g.file[0]) DeleteFileW(g.file);
    Trace("[aml] what was learnt for %s is forgotten", g.game[0] ? g.game : "this game");
}

// Adaptive Flow Scale (round 35, the user's idea). The size frames in between are made at, from how far this
// game's picture goes from one real frame to the next while it moves - what Adaptive Motion Learning measured,
// the hand's word taken where the turn was too fast for the blocks. Made small, a frame in between stays right
// when the picture moves far and costs least; made larger it is finer while it moves little (sm_options.h, the
// kinds of game; measured at 35 / 50 / 75%: a pan of 32 pixels a frame 3.5 / 2.3 / 1.9% of pixels off, one of
// 64 pixels 3.6 / 14 / 5%; the first frame in between 1.0 / 1.7 / 2.3 ms). So: nine pairs in ten within 16
// pixels (of 2560) 75%, within 32 50%, further 35%. The first pick at once, once 300 moving pairs are in; another
// only after it has been the better one ten seconds running and a minute after the last (each change builds the
// generator's pictures anew - a moment's stand).
static void LearnFlowDecide(ULONGLONG now)
{
    LearnState &g = g_learn;
    if (!LearnWanted() || !smOpt.aml_flow.load()) { g.flow_seen = 0; return; }
    double all = 0.0;
    for (double bin : g.speed_bins) all += bin;
    if (all < 300.0) return;
    double run = 0.0;
    int bin = 0;
    for (; bin < LearnState::kSpeedBins - 1; ++bin)
    {
        run += g.speed_bins[bin];
        if (run >= 0.9 * all) break;
    }
    const double pixels = (bin + 1) * 4.0;
    const int pick = pixels <= 16.0 ? 75 : pixels <= 32.0 ? 50 : 35;
    const int had = g_learn_flow.load();
    if (pick == had) { g.flow_seen = 0; return; }
    if (pick != g.flow_want) { g.flow_want = pick; g.flow_seen = 0; }
    ++g.flow_seen;
    if (had != 0 && (g.flow_seen < 10 || now - g.flow_at < 60000)) return;
    g_learn_flow = pick;
    g.flow_at = now;
    g.flow_seen = 0;
    Trace("[aml] Adaptive Flow Scale for %s: nine pairs in ten move the picture %.0f pixels a frame or less (of 2560) - frames in between made at %d%% of it",
          g.game[0] ? g.game : "this game", pixels, pick);
}

// Once a second: the hand listened to or not, the app told how far along it is, the file kept up.
//   @aml off | learning <percent> | learned <percent> <delay ms>
static void LearnTell()
{
    LearnState &g = g_learn;
    const ULONGLONG now = GetTickCount64();
    if (now - g.told_at < 1000) return;
    g.told_at = now;
    const bool wanted = LearnWanted();
    LearnListen(wanted);
    LearnFlowDecide(now);
    const bool known = LearnKnown(g.model[0]) || LearnKnown(g.model[1]);
    const LearnModel &lead = g.model[g.model[1].pairs > g.model[0].pairs ? 1 : 0];
    // The study, in front of the player: which step is asked for, and for how long already.
    if (wanted && !g.studied)
    {
        if (g.plan_for != smOpt.game.load()) LearnPlan();
        if (g.step_from == 0) g.step_from = g.moved_at = now;
        const int kind = g.plan[std::min(g.step, g.plan_count - 1)];
        const ULONGLONG shown = now - g.step_from;
        // Through with it: enough of it is in (and it was there to be read); or it was not followed; or - the
        // car - the player stands, as he was asked to without one: the picture has not moved for five seconds
        // (by the clock: a picture that stands may send no frames at all).
        const bool enough = g.got >= LearnState::kStepSeconds[kind] && shown >= LearnState::kStepLeast;
        const bool no_car = kind == LearnState::kDrive && shown >= LearnState::kNoCar + 1000 && now - std::max(g.moved_at, g.step_from) >= LearnState::kNoCar;
        // (Thirty seconds of play are waited for three times as long as a step: a player may pause, open a menu.)
        const ULONGLONG most = kind == LearnState::kFree ? 3 * LearnState::kStepMost : LearnState::kStepMost;
        if (g.step < g.plan_count && (enough || no_car || shown >= most))
        {
            // How fast the picture streamed by, for the game's file - and for telling a drive from a walk.
            const bool walked = kind == LearnState::kWalk, drove = kind == LearnState::kDrive || kind == LearnState::kStraight;
            const double pace = g.pace_pairs >= 12 ? g.pace_sum / g.pace_pairs : 0.0;
            if (enough && pace > 0.0 && (walked || drove)) g.pace[walked ? 0 : 1] = pace;
            char streamed[96] = "";
            if (pace > 0.0 && (walked || drove)) sprintf_s(streamed, ", the picture growing by %.0f%% a second", pace * 100.0);
            Trace("[aml] the study's step %d of %d (%s): %.1f s of it in %.0f s%s%s", g.step + 1, g.plan_count,
                  kind == LearnState::kTurn ? "the camera left and right" : kind == LearnState::kLook ? "the camera up and down" :
                  kind == LearnState::kWalk ? "walking" : kind == LearnState::kDrive ? "driving" : kind == LearnState::kStraight ? "driving straight" :
                  kind == LearnState::kTurns ? "taking turns" : kind == LearnState::kBack ? "walking back" : kind == LearnState::kHud ? "the read-outs" :
                  kind == LearnState::kRun ? "running" : kind == LearnState::kAim ? "aiming" : kind == LearnState::kFree ? "playing freely" : "moving about",
                  g.got, shown / 1000.0, streamed, enough ? "" : no_car ? " - no car: the player stands" : " - not followed, passed over");
            ++g.step;
            g.got = 0.0;
            g.pace_sum = 0.0; g.pace_pairs = 0;
            g.step_from = g.moved_at = now;
        }
        if (g.step >= g.plan_count)
        {
            g.studied = true;
            g.changed = true;
            g.done_at = now;
            const LearnModel &m = g.model[g.model[1].pairs > g.model[0].pairs ? 1 : 0];
            Trace("[aml] %s is studied; the picture follows the hand %d ms late (it explains %.0f%% of the picture's movement, miss %.2f)",
                  g.game[0] ? g.game : "this game", m.delay < 0 ? 0 : m.delay * 10, m.fit * 100.0, m.miss);
        }
    }
    const int percent = LearnPercent();
    if (!wanted || (g.studied && (g.done_at == 0 || now - g.done_at >= 4000))) BannerSay(L"", L"");
    else if (g.studied) BannerSay(g.say[4], L"");
    else
    {
        // "{0}" how far along, "{1}" about how many seconds are left: seven for a step when it is followed.
        std::wstring first = g.say[0];
        const std::wstring values[2] = { std::to_wstring(percent), std::to_wstring(LearnSecondsLeft()) };
        // (Every other four seconds the first line says instead that the learning costs a little while it lasts.)
        if ((now / 4000) % 3 == 2) first = g.say[9];
        for (int i = 0; i < 2; ++i)
        {
            const std::wstring mark = i == 0 ? L"{0}" : L"{1}";
            const size_t at = first.find(mark);
            if (at != std::wstring::npos) first.replace(at, mark.size(), values[i]);
        }
        // Under it the step, and which one of how many it is.
        static const int line[LearnState::kSteps] = { 1, 2, 3, 5, 6, 7, 8, 10, 11, 12, 13, 14 };
        const std::wstring second = std::to_wstring(g.step + 1) + L"/" + std::to_wstring(g.plan_count) + L"  \x00B7  " + g.say[line[g.plan[g.step]]];
        BannerSay(first.c_str(), second.c_str());
    }
    // The read-outs: a new map for the generator after every thirty pairs learnt from (while studied: sixty).
    if (wanted && g.hud_pairs >= g.hud_told + (g.studied ? 60u : 30u))
    {
        g.hud_told = g.hud_pairs;
        g.hud_changed = true;
        LearnHudCells();
    }
    const int state = !wanted ? 0 : g.studied ? 2 : 1, delay = lead.delay < 0 ? 0 : lead.delay * 10;
    if (state != g.told_state || percent != g.told_percent || (state == 2 && delay != g.told_delay))
    {
        g.told_state = state; g.told_percent = percent; g.told_delay = delay;
        if (state == 0) Status("@aml off");
        else if (state == 1) Status("@aml learning %d", percent);
        else Status("@aml learned %d %d", percent, delay);
    }
    if (wanted && g.changed && now - g.saved_at >= 20000) LearnSave();
    if (wanted && now - g.said_at >= 30000 && g.offered)
    {
        Trace("[aml] in %llu pairs with the hand moving, %llu were told where the picture went (mouse: %d ms, fit %.0f%%, miss %.2f, x%.2f; stick: %d ms, fit %.0f%%, miss %.2f)",
              g.offered, g.used, g.model[0].delay * 10, g.model[0].fit * 100.0, g.model[0].miss, g.model[0].scale,
              g.model[1].delay * 10, g.model[1].fit * 100.0, g.model[1].miss);
        g.offered = g.used = 0;
        g.said_at = now;
    }
}

static void LearnClose()
{
    LearnSave();
    LearnListen(false);
    BannerSay(L"", L"");
}
