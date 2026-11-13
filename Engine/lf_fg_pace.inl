// lf_fg_pace.inl - when DLSS MFG's frames go to the screen (round 28).
//
// Before, the presenter began each pair the moment it arrived and spaced its
// frames from there. Under a game that fills the card pairs arrive unevenly,
// and the trace showed it plainly: a real frame, the next pair's frame in
// between 1 ms after it, then 13 ms of nothing - x2 that moved like the game
// alone, in steps.
//
// Motion is even when every picture is shown the same time after the moment
// of the game it stands for. A real frame stands for the moment it reached
// the desktop (the capture's own time stamp), a frame in between for its
// place between its two real ones. So each is shown at
//     its moment + delay
// and the delay is one figure for all of them, moved only slowly.
//
// The delay has to cover what a pair needs before its first frame in between
// can be shown: from the older real frame's moment until the pair is ready
// (the capture, the motion estimate, NGX), less one step. That need jitters;
// the delay is taken from the recent needs according to the Queue target:
//     0  their median       - least delay, about every second pair comes late
//     1  the largest of them and half a millisecond (the default). Round 88p: until then their 97th percentile (the
//        90th until round 46) - so three pairs in a hundred came late by design, and each was the owner's "micro
//        stutter" in GTA V: a pair's first frame 9 to 13 ms after the one before it, the rest 4 ms apart to catch up.
//        Measured on GTA, walking, x4 (PresentMon): late pairs 3.3 - 5.8% -> under 0.6%, with 3 to 5 ms more delay.
//     2  the largest of them and half a step - smoothest, most delay
// A pair that still comes late has its frames shown as soon as they can be;
// the delay follows the needs up at most a millisecond a pair and down a
// twentieth of one - a change of pace far below one percent, not seen.

// "Queue target" in the app.
static unsigned FgQueueTarget()
{
    return static_cast<unsigned>(std::clamp(smOpt.queue.load(), 0, 2));
}

struct FgPace
{
    static constexpr unsigned kNeeds = 96;
    static constexpr unsigned kSettled = 6;   // pairs after which the first guess is put right
    double needs[kNeeds] = {};
    unsigned count = 0, next = 0;
    bool settled = false;     // the first guess was put right
    double delay = 0.0;       // seconds; 0: not learnt yet
    // What drawing a frame into the back buffer takes (the copy and its
    // fence), a real frame's and a frame in between's each: a real one is
    // copied at the screen's size, one in between is scaled up from the size
    // it was made at. Drawn ahead of its time (FgPresenter), so only the
    // first frame of a pair has to count it in.
    double lead[2] = {0.0015, 0.0015};
    double call = 0.0004;     // the Present call itself
    double waited = 0.0;      // for the screen to take the frame before, per Present
    // Since the last summary.
    unsigned pairs = 0, late = 0, skipped = 0, dropped = 0;
    // The usual time between two real frames.
    double usual = 0.0;
    // Round 56, Adaptive MFG: the delay covers every pair of the last ones, not 97 in a hundred - a beat that
    // passes before its pair comes is a hole of three or four beats, the uneven step Adaptive is there to end.
    bool cover_all = false;

    void Forget() { count = next = 0; delay = 0.0; settled = false; }

    // A pair far longer than the ones around it (round 30): something on a
    // page changed after a rest - a line highlighted, a notice appearing.
    // There is nothing to make smooth between "before" and "after" of that:
    // its frames in between were shown in a burst, the content swept through
    // them in an instant (the "sudden pull"), and its long wait was taken for
    // the delay every later frame then had to keep. Such a pair shows its
    // real frame at once and teaches the pacing nothing. A pace that really
    // became slower stops being odd after a few pairs.
    bool Irregular(double span)
    {
        const bool odd = usual > 0.0 && span > std::max(2.5 * usual, usual + 0.025);
        usual = usual > 0.0 ? usual + (odd ? 0.04 : 0.12) * (span - usual) : span;
        return odd;
    }

    double Wanted(double step) const
    {
        if (count == 0) return delay;
        double sorted[kNeeds];
        std::copy(needs, needs + count, sorted);
        std::sort(sorted, sorted + count);
        // Round 96, "Ultra Smooth" (the owner: "an option that gives up delay for smoothness - the smoothest there
        // can be"): what a pair usually asks, and a frame of the game's on top, kept in hand. Lossless Scaling runs
        // 50 ms behind the game where the engine ran 34 - 40 (the triangles, x4 at 40 FPS), and that is all its
        // smoothness is: a frame that comes late by less than what is in hand is shown on the beat as if it had not,
        // and a hitch of the game's is a hold shorter by that much (its 60 ms hitch 1.8 beats, ours 6 to 11). The
        // usual need, not the largest: the largest IS a late frame, and following it the delay rose and fell with
        // every one. SM_SMOOTH_BUFFER=<percent of a frame> (100).
        if (SmoothOn())
        {
            static const double share = [] { char v[8] = {}; return GetEnvironmentVariableA("SM_SMOOTH_BUFFER", v, sizeof(v)) > 0 ? std::clamp(atoi(v), 10, 300) / 100.0 : 1.0; }();
            const double frame = usual > 0.0 ? usual : 4.0 * step;
            return sorted[count / 2] + std::clamp(share * frame, 0.008, 0.050);
        }
        const unsigned queue = FgQueueTarget();
        if (queue == 0) return sorted[count / 2];
        if (queue == 1) return sorted[count - 1] + 0.0005;      // (round 88p: every pair of the last ones covered - see above)
        return sorted[count - 1] + 0.5 * step;
    }

    // A pair was taken up. need: what its first frame in between asks of the
    // delay; step: the time between two shown frames of it. True when the
    // pair came late for the delay there was.
    bool Took(double need, double step)
    {
        ++pairs;
        needs[next] = need;
        next = (next + 1) % kNeeds;
        count = std::min(count + 1, kNeeds);
        if (delay <= 0.0) { delay = need + 0.25 * step; return false; }
        // The very first pair after a start or a pause is not like the ones
        // that follow it - the generator and the capture are still warming
        // up - and the delay it set was kept: it only comes down a twentieth
        // of a millisecond a pair, which at 20 FPS is a millisecond a second
        // (round 31: 50 ms sliding to 34 over seventeen seconds). Once a few
        // pairs are known the delay is what they ask, set once.
        //
        // The first pair itself is left out of that: the generator was made
        // between its two frames (160 ms where the others take 20), and at
        // Queue target 1 and 2 the largest of six needs is the one "wanted" -
        // the delay stayed at 113 ms and came down a millisecond and a half a
        // second, long enough to push pairs out of the four slots at 60 FPS.
        //
        // Once (round 3): the count went back to five here and came to six
        // again with the very next pair, so this was every pair's lot - the
        // delay was the largest of the last five needs, set anew each time,
        // and moved by a millisecond or four with every hiccup: an uneven
        // pace of the presenter's own making.
        if (!settled && count == kSettled)
        {
            settled = true;
            for (unsigned i = 1; i < count; ++i) needs[i - 1] = needs[i];
            --count;
            next = count % kNeeds;
            delay = Wanted(step);
            return need > delay;
        }
        // One late pair does not move the delay by itself (its frames are
        // shown as soon as they can be): raising it at once and coming back
        // down was a second of drift for one hiccup. Lateness that stays
        // shows in the needs, and the delay follows those.
        const double wanted = Wanted(step);
        const bool came_late = need > delay;
        if (came_late) ++late;
        // Within half a millisecond of what is wanted it stays where it is:
        // following every small move of the needs was itself a slow wave of
        // two or three milliseconds in the pace.
        const double off = wanted - delay;
        if (off > 0.0005) delay += std::min(off - 0.0005, came_late ? 0.001 : 0.0003);
        else if (off < -0.0005)
        {
            // Down a twentieth of a millisecond a pair - no faster, so the
            // pace does not show it. But far above what is asked (the first
            // pairs of a run left it there, or a stall that passed) that is
            // seconds of every frame shown later than it need be, and with
            // four pairs' room in flight it can be more than there is room
            // for: then a twelfth of the way each pair.
            const double above = -off - 0.0005;
            // (Ultra Smooth: half as fast - after a hitch it could not hide the delay is what the hitch left it, and
            // comes back over a second, a pace two or three hundredths faster, not a twelfth.)
            delay -= std::min(above, std::max(0.00005, above > 0.004 ? above / (SmoothOn() ? 30.0 : 12.0) : 0.0));
        }
        return came_late;
    }

    // What drawing a pair's first picture took, from when the pair's own work
    // was done (it is sent to wait behind that work).
    void Took(double draw_seconds, bool generated)
    {
        double &l = lead[generated ? 1 : 0];
        l += 0.1 * (std::clamp(draw_seconds, 0.0003, 0.008) - l);
    }

    // draw_seconds under 0: counted already (a pair's first picture).
    void Took(double draw_seconds, double wait_seconds, double call_seconds, bool generated)
    {
        double &l = lead[generated ? 1 : 0];
        if (draw_seconds >= 0.0) l += 0.1 * (std::clamp(draw_seconds, 0.0003, 0.008) - l);
        waited += 0.05 * (std::min(wait_seconds, 0.05) - waited);
        call += 0.05 * (std::clamp(call_seconds, 0.0001, 0.003) - call);
    }
};

// Which of the game's frames are taken when the game is faster than the
// screen. How many frames in between each pair gets is FgPresent's business
// (lf_dlssg.inl): as many as the screen still has room for.
//
// Round 28 made a whole choice here - every keep-th frame taken, `count`
// frames in between - for the most frames shown. It showed the most frames
// and threw the game's own away for them: a 130 FPS game at x4 on 200 Hz was
// taken at every second frame and less (48 real frames a second, three made up
// between each), where another program showed 95 of its frames. A frame of the
// game is worth more than one made up, so now every frame is taken as long as
// the screen can show them all, and only of a game faster than the screen
// itself every second (third) one - by the frames' own time stamps, not by a
// clock: a clock took a game a little over the limit later and later until a
// frame was lost.
static struct FgGate
{
    double seen = 0.0, taken = 0.0;      // the last frame looked at, the last taken (QPC seconds)
    double deltas[32] = {};
    unsigned filled = 0, next = 0;
    unsigned keep = 1;                   // every keep-th frame
    double every = 0.0;                  // the game's frame time
    bool passed_over = false;            // the frame before this take was let go
} g_fg_gate;

static void FgGateChoose(double hz, unsigned most_keep = 6)
{
    FgGate &g = g_fg_gate;
    // Their average: the stamps lie on the screen's refresh grid (a 130 FPS
    // game on 200 Hz comes as 5, 10, 5, 10 ms), so no single one says it.
    double sum = 0.0;
    for (unsigned i = 0; i < g.filled; ++i) sum += g.deltas[i];
    g.every = sum / g.filled;
    if (GivenFrameTime() > 0.0) g.every = GivenFrameTime();     // Game FPS given in the app
    const double rate = 1.0 / g.every;
    unsigned keep = 1;
    while (keep < most_keep && rate / keep > hz * 1.04) ++keep;
    if (g.keep > most_keep) g.keep = most_keep;
    if (keep == g.keep) return;
    // A rate at the edge does not flip it.
    if (keep > g.keep && rate / g.keep <= hz * 1.07) return;
    if (keep < g.keep && rate / keep > hz) return;
    g.keep = keep;
    if (keep == 1) Trace("[dlssg] the game draws %.0f frames a second: every one is taken", rate);
    else Trace("[dlssg] the game draws %.0f frames a second and the screen shows %.0f: every %s frame is taken",
               rate, hz, keep == 2 ? "second" : keep == 3 ? "third" : "few");
}

// Frames that were the picture before over again (lf_own_motion.inl,
// SourceSeen): their time stamps count frames the game never drew, and
// the gate would choose by a rate that is not the game's - "the game draws
// 114 frames a second: no room for frames in between" for a game drawing 72.
// While they keep coming a repeat cannot be told by its stamp, so every frame
// is taken and looked at, the repeats passed over - and the game's rate is
// counted from the frames that proved new (FgGateFresh), which still says how
// many frames in between the screen has room for. Three seconds after the
// last repeat the gate counts by the stamps again.
static double g_fg_repeat_at = -1e9;
static struct { unsigned asked = 0; double hz = 0.0; bool limited = false; double before = 0.0; } g_fg_repeat;

static bool FgGateRepeats() { return LatencyNow() - g_fg_repeat_at < 3.0; }

static void FgGateRepeat(double now)
{
    if (now - g_fg_repeat_at >= 3.0)
    {
        g_fg_gate.filled = g_fg_gate.next = 0;
        g_fg_gate.keep = 1;
        g_fg_repeat.before = 0.0;
    }
    g_fg_repeat_at = now;
}

// A frame that proved new while repeats come: the game's own pace.
static void FgGateFresh(double stamp)
{
    if (!FgGateRepeats() || g_fg_repeat.hz <= 0.0) return;
    FgGate &g = g_fg_gate;
    const double delta = stamp - g_fg_repeat.before;
    g_fg_repeat.before = stamp;
    if (delta > 0.0015 && delta < 0.1)
    {
        g.deltas[g.next] = delta;
        g.next = (g.next + 1) % 32;
        g.filled = std::min(g.filled + 1, 32u);
    }
    else if (delta >= 0.1 || delta < 0.0) g.filled = g.next = 0;
    if (g_fg_repeat.limited || g_fg_repeat.hz < 24.0) g.keep = 1;
    else if (g.filled >= 12) FgGateChoose(g_fg_repeat.hz, 1);
}

// A frame from the capture with its time stamp: is it taken?
static bool FgGateTakes(double stamp, unsigned asked, double hz, bool limited)
{
    FgGate &g = g_fg_gate;
    if (FgGateRepeats())
    {
        g_fg_repeat.asked = asked; g_fg_repeat.hz = hz; g_fg_repeat.limited = limited;
        g.keep = 1;
        g.seen = g.taken = stamp;
        return true;
    }
    const double delta = stamp - g.seen;
    g.seen = stamp;
    if (delta > 0.0015 && delta < 0.1)
    {
        g.deltas[g.next] = delta;
        g.next = (g.next + 1) % 32;
        g.filled = std::min(g.filled + 1, 32u);
    }
    else if (delta >= 0.1 || delta < 0.0) g.filled = g.next = 0;   // a pause: learnt again
    // An explicit frame limit paces the frames taken itself (lf_settings.inl).
    if (limited || hz < 24.0) g.keep = 1;
    else if (g.filled >= 12) FgGateChoose(hz);
    if (g.keep > 1 && stamp >= g.taken && stamp - g.taken < (g.keep - 0.5) * g.every) return false;
    g.taken = stamp;
    return true;
}

static void FgGateForget()
{
    g_fg_gate = FgGate();
}

// The moment of the game a real frame stands for. The capture's time stamp is
// when the compositor showed the frame - on the screen's refresh grid: a 130
// FPS game on a 200 Hz desktop arrives as 5, 10, 5, 10 ms, though it drew
// every 7.7. Taken as they are, those stamps put the grid's unevenness into
// every frame in between. So the moments run on a clock of their own: the
// last moment plus the game's average frame time, pulled a little towards the
// stamp each frame - while the stamp is within one refresh of it. Further
// off, it is the game itself that changed pace, and the stamp is taken.
//
// Under a game that fills the card the stamps are uneven for another reason
// too: the engine's own work goes first on the card and holds each frame of
// the game up by a different amount - 12, 19, 14 ms between frames a game
// would have drawn evenly. Following every one of those put that unevenness
// on the screen. So the clock keeps the pace it has learnt (a few seconds of
// frames) and lets a stamp be up to nearly half a frame off it before it
// believes the stamp; a frame the game left out (twice the time, or three
// times) moves the clock on by whole frames, keeping the beat; anything else
// that far off is another pace and starts it again.
// Round 56: how far a frame's stamp may lie off the beat and still be on it, a share of a frame. A quarter until now -
// and a user's GTA V came 12, 21, 33, 12, 21, 33 ms apart (24.5 on average, frames.log): two of three frames were
// "odd" and shown at their stamps, a pair's frames 2 ms apart, the next one's 3.5, the next 5.5 - the picture
// stepping up and down where Lossless Scaling's goes on evenly. Frames the game left out (whole frames later) and
// a pace that changed for good are told as before.
static constexpr double kClockHold = 0.6;

static struct FgClock
{
    double moment = 0.0, before = 0.0;   // this real frame's and the one before it
    double every = 0.0;                  // the average time between taken frames
    unsigned known = 0;                  // frames the pace was learnt from
    int off_beat = 0;                    // frames in a row the stamp was far off the clock
    double spans[8] = {};                // the last times between two taken frames
    unsigned filled = 0, next = 0;
    int median_off = 0, median_side = 0; // round 92: frames in a row whose median lay a pace away, and to which side
    int mean_off = 0, mean_side = 0;     // round 93: frames in a row whose mean of eight lay a pace away
    int drift_run = 0, drift_side = 0;   // round 92: frames in a row whose stamp fell further from the clock to one side
    double drift_off = 0.0;

    // Round 91: the pace learnt is kept across a rest (a pause, a cut, the generator starting fresh) - the game comes
    // back at the pace it left with, as a rule. Learnt anew from the first span after it, a capture's late first stamp
    // made it 20 ms for a game at 25 (tests\smooth, SMFG after a pause): pairs shown faster than they came, then late,
    // 13 - 5 - 5 ms for a second. Another pace after the rest is still found within six frames (the median, below).
    void Restart(double stamp) { moment = stamp; before = 0.0; known = std::min(known, 8u); off_beat = 0; }

    // Round 91 (the smoothness tests, tests\smooth): a moment put back onto its stamp in one step squeezed or
    // stretched the pair it ends. Measured on the screen with the game going from 40 to 55 FPS: the clock still on
    // 25 ms ran ahead of the frames, and every second or third pair the moment fell back onto the stamp - the pair's
    // two frames in between and its real frame shown within 1.6 ms, the picture leaping, for a second and more.
    // Now a pair is never shorter than half the beat nor longer than twice it; what is left of the way to the stamp
    // the next frames take up, a little each (the "on the beat" branch below pulls the clock to the stamps).
    // Round 93 - how far a stamp is believed (the owner: "the game slows down and speeds up for some reason").
    // A stamp is the game's moment plus the capture's own noise. Measured (tests\smooth, SM_STAMPS=1): a game presenting
    // every 25.00 ms had stamps 28.2, 21.8, 25.6, 24.5 ms apart - the compositor's turn, 1.4 ms of noise and now and
    // then 3 to 6. Such noise is undone by the next stamp (a late one is followed by a short span), and the clock was
    // made to hold its beat against it: a twentieth of the way to each stamp. But a game whose frames really come
    // unevenly (a full card: 25 +-4 ms, each on its own) drifts off any even beat for good, a little more each
    // frame, and a clock that follows a twentieth a frame was 7 ms behind it on average: the picture 31 to 48 ms
    // behind the game, wandering - shown slower, then faster, the game's own speed never.
    // The two are told apart by the spans themselves: noise on the stamps is undone from one span to the next, so
    // twelve spans together are no further off than one - nor is a pattern that comes round every two, three, four
    // or six frames (a game 12, 21, 33 ms apart, round 56: held to an even beat); true unevenness adds up, twelve
    // times one span's. From the
    // two shares the gain that follows best is the steady Kalman one; never under the old twentieth, never over a
    // half (a game may smooth its own step - then the stamps are less even than what it shows).
    double tr_mean = 0.0, tr_one = 0.0, tr_eight = 0.0, tr_devs[12] = {};
    unsigned tr_n = 0;
    double Trust(double span)
    {
        static const int fixed = [] { char v[8] = {}; return GetEnvironmentVariableA("SM_CLOCK_TRUST", v, sizeof(v)) > 0 ? atoi(v) : -1; }();
        if (tr_mean <= 0.0 || std::fabs(span - tr_mean) > 0.6 * tr_mean) { tr_mean = span; tr_n = 0; tr_one = tr_eight = 0.0; }
        tr_mean += (span - tr_mean) / 64.0;
        const double dev = span - tr_mean;
        tr_devs[tr_n % 12] = dev;
        ++tr_n;
        tr_one += (dev * dev - tr_one) / 96.0;
        if (tr_n >= 12)
        {
            double sum = 0.0;
            for (const double d : tr_devs) sum += d;
            tr_eight += (sum * sum - tr_eight) / 96.0;
        }
        if (fixed >= 0) return std::clamp(fixed / 100.0, 0.0, 1.0);       // (tests: a gain given, percent)
        if (tr_n < 96) return 0.05;                                        // (not known yet: as it always was)
        const double walk = std::max((tr_eight - tr_one) / 11.0, 0.0), noise = std::max((tr_one - walk) / 2.0, 0.0);
        // (Capped at 0.35: the capture's own lateness comes in runs too - measured against the game's true Presents,
        // tests\smooth\stamptruth.py: a frame reaches the engine 5 ms after the game showed it, and every fourth one
        // 16 ms after, by the compositor's turn - and runs look like unevenness. A third of the way a frame follows a
        // truly uneven game three times closer than a twentieth did, and lets a third of such noise through at most.)
        // Round 95, "Even pacing" (fixed_beat) on - the owner: "Lossless Scaling makes the game itself smoother; I do
        // not want all these oscillations in the frame time line": a tenth at most. The game's frames reach the
        // desktop 14 to 18 ms apart while its world moves on evenly (The Witcher 3 at 63 FPS: 7% of spread); followed a
        // third of the way, every one of those changes was in the time between the frames shown, times the multiplier
        // - Presents 4.84 to 5.67 ms at x3, against 5.04 to 5.48 followed a twentieth of the way.
        const double most = SmoothOn() ? 0.05 : EvenOn() ? 0.10 : 0.35;   // (round 96, Ultra Smooth: a twentieth - the delay it keeps in hand is there for what the clock no longer follows)
        if (noise <= 1e-12) return walk > 1e-10 ? most : 0.05;
        const double q = walk / noise;
        return std::clamp(0.5 * (-q + std::sqrt(q * q + 4.0 * q)), 0.05, most);
    }

    double Bounded(double target) const
    {
        if (every <= 0.0 || before <= 0.0) return target;
        // (Round 93: no longer "nor longer than twice it" - a frame the game brought late (a hitch) IS that late, and
        // held to two beats its moment lay 10 ms before its stamp: the frames after it all "came late" by that, and the
        // delay went up for them - seconds of every picture later after each hitch.)
        return std::max(target, before + 0.5 * every);
    }

    double Moment(double stamp, double span, double hz)
    {
        before = moment;
        if (span <= 0.0 || moment <= 0.0) { Restart(stamp); return moment; }
        // Game FPS given in the app: the beat is that one, known from the start and never learnt again. A frame on
        // it (within a quarter of a frame, or one refresh) keeps the beat; frames the game left out move it on by
        // whole frames; anything else is shown at its own time.
        if (const double given = GivenFrameTime(); given > 0.0)
        {
            every = given; known = 1000; off_beat = 0;
            const double grid = hz >= 24.0 ? 1.0 / hz : 0.0;
            const double slack = std::max(grid, 0.25 * every);
            const double off = stamp - (moment + every);
            const double beats = std::round(off / every);
            if (std::fabs(off) <= std::max(slack, kClockHold * every)) moment += every + 0.05 * off;
            else if (beats >= 1.0 && beats <= 3.0 && std::fabs(off - beats * every) <= slack) moment += (1.0 + beats) * every + 0.05 * (off - beats * every);
            else moment = stamp;
            if (moment <= before) moment = stamp > before ? stamp : before + 0.001;
            return moment;
        }
        spans[next] = span;
        next = (next + 1) % 8;
        if (filled < 8) ++filled;
        if (every <= 0.0) { every = span; known = 1; moment = stamp; return moment; }
        const double grid = hz >= 24.0 ? 1.0 / hz : 0.0;
        // Another pace, and the frames agree on it: taken up at once. A game
        // whose frames are taken every second or every third (the engine's
        // own work deciding which) changes its pace by a half or a third in
        // one step, as the work changes - and the clock held the old beat
        // against it. Read off a session's log (round 5): after the pairs
        // went from 30 to 45 a second the clock gave out moments 33 ms apart
        // for seventeen seconds; a third of the pairs found no place and were
        // thrown away, the rest went to the screen in jumps. Eight frames
        // that lie within three tenths of each other (or one refresh of the
        // screen, whose grid the stamps lie on) and a tenth off the pace are
        // the new pace.
        // Round 91: by their median, from six frames on - the stamps of a capture come 13 to 23 ms apart for a game
        // at 18 ms (the screen's grid and the capture's own jitter), so "all eight within three tenths of each other"
        // was never true and a new pace was only crept towards, three hundredths a frame: a second and more of
        // moments a third off (see Bounded). Most of them near their median and a tenth off the pace: the new pace.
        // Round 93: and by their mean - the pace IS the frames' mean, whatever their pattern (the sum of the spans is
        // the time gone by). A game 12, 21, 33 ms apart (22 on average) taken up after one at 18 ms kept the 18: its
        // median is 21, "near enough", and its 33s were never averaged in (off the beat, taken for frames left out) -
        // every sixth pair a double step (tests\smooth, gta@). The mean of eight a tenth off the pace for sixteen
        // frames running, to one side (a hitch alone is in the eight for eight): the pace.
        if (filled == 8)
        {
            double sum = 0.0;
            for (const double s : spans) sum += s;
            const double mean = sum / 8.0;
            const int side = mean > every * 1.1 ? 1 : mean < every * 0.9 ? -1 : 0;
            mean_off = side != 0 && side == mean_side ? mean_off + 1 : side != 0 ? 1 : 0;
            mean_side = side;
            if (mean_off >= 16 && mean > 0.0015 && mean < 0.25)
            {
                every = mean;
                known = 8; off_beat = 0; mean_off = 0; median_off = 0; drift_run = 0;
                moment = Bounded(stamp);
                if (moment <= before) moment = before + 0.001;
                return moment;
            }
        }
        if (filled >= 6)
        {
            double sorted[8];
            std::copy(spans, spans + filled, sorted);
            std::sort(sorted, sorted + filled);
            const double median = sorted[filled / 2];
            const double near_by = std::max(0.3 * median, 1.05 * grid);
            unsigned agree = 0;
            for (unsigned i = 0; i < filled; ++i) if (std::fabs(spans[i] - median) <= near_by) ++agree;
            // (Round 92: and so for eight frames in a row, to the same side. The owner, GTA V at 45 - 55 FPS with DLSS
            // MFG: "the frames in between much less smooth" - a game whose frames come a few milliseconds apart from
            // each other moves the median of eight a tenth now and then, and each time the pace was taken anew and the
            // moments put back onto the stamps: the stand-in at 48 FPS +-3 ms, x4, moments a quarter off 5.4 -> 7.4%.
            // A pace that really changed stays changed; a game that only wavers does not.)
            const int side = median > every * 1.12 ? 1 : median < every * 0.88 ? -1 : 0;
            const bool other = agree + 2 >= filled && median > 0.0015 && median < 0.25 && side != 0;
            median_off = other && side == median_side ? median_off + 1 : other ? 1 : 0;
            median_side = other ? side : 0;
            if (median_off >= 8)
            {
                every = median;
                known = 8;
                off_beat = 0;
                median_off = 0;
                moment = Bounded(stamp);
                if (moment <= before) moment = before + 0.001;
                return moment;
            }
        }
        // How far off the beat: a quarter of a frame once the pace is known
        // (it was nearly half: a pace a third faster passed for the old one,
        // every second frame "on the beat" and ten milliseconds from its
        // stamp), one refresh of the screen - what the stamps' own grid does -
        // before that.
        const double slack = known >= 30 ? std::max(grid, 0.25 * every) : grid;
        const double off = stamp - (moment + every);
        // Round 92: another pace, told by how it pulls: the stamps fall away from the clock to one side, further with
        // every frame (a game going from 40 to 55 FPS: 7, 14, 20 ms before the clock). A game that only wavers lies
        // to either side by turns, and a frame it left out is one frame off once, not more each time. Three frames
        // falling further away, each a third of a frame off or more: the pace of the last four, at once (waiting for
        // the median of eight, above, it was 19 - 28% of the moments a quarter off for a second).
        {
            const int drift = off > 0.3 * every ? 1 : off < -0.3 * every ? -1 : 0;
            // (Not "further each time": the frame that catches up - Bounded - comes nearer once, and the run began
            // anew there, two pairs at the old pace and one squeezed, for a third of a second. Three to one side
            // a third of a frame off is enough - a wavering game is not that far, that long, to one side.)
            drift_run = drift != 0 && drift == drift_side ? drift_run + 1 : drift != 0 ? 1 : 0;
            drift_side = drift;
            drift_off = off;
            if (drift_run >= 3 && filled >= 4)
            {
                double last[4];
                for (unsigned k = 0; k < 4; ++k) last[k] = spans[(next + 8 - 1 - k) % 8];
                std::sort(last, last + 4);
                const double recent = 0.5 * (last[1] + last[2]);
                drift_run = 0;
                // (Only a pace of their own, a tenth off the clock's: frames that waver to one side for a while at the
                // old pace - the capture's grid and the game's own unevenness together - move the clock as before.)
                // (A fifth: the middle of four spans of a game at 48 +-3 ms on a 5 ms grid is a tenth off now and then -
                // taken for a pace, it was 1.5% more of the moments a quarter off. 40 -> 55 FPS is 27%; a smaller
                // change is the median of eight's, above.)
                if (recent > 0.0015 && recent < 0.25 && std::fabs(recent - every) > 0.2 * every)
                {
                    every = recent;
                    known = 8; off_beat = 0; median_off = 0;
                    moment = Bounded(stamp);
                    if (moment <= before) moment = before + 0.001;
                    return moment;
                }
            }
        }
        // Left-out frames: whole beats later.
        const double beats = std::round(off / every);
        const double rest = off - beats * every;
        // (Round 56: also while the pace is being learnt - held to one refresh then, a game whose frames come 12, 21,
        // 33 ms apart never got through its first thirty: every frame "another pace", the clock its stamps.)
        // Round 95, "Even pacing" (fixed_beat): a frame up to a whole frame late keeps the beat (SM_CLOCK_HOLD=<percent>
        // to compare; 60: as without it). The Witcher 3 at 63 FPS draws a frame of 20 - 21 ms once or twice a second
        // (PresentMon: none over 22), the capture's own turn adds 4 or 5, and 10 ms late was "off the beat": the clock
        // took the stamp, and that pair's frames stood 8.6 ms apart where the others are 5.2 - three long gaps and the
        // picture at six tenths of its speed for one pair, 1.4 to 2.8 times a second (frames.log; the owner's "the
        // game slows down and speeds up"). On the beat the frame only comes later than its moment - what the delay
        // is there for - and the picture goes on evenly, as Lossless Scaling's does.
        static const double hold_given = [] { char v[8] = {}; return GetEnvironmentVariableA("SM_CLOCK_HOLD", v, sizeof(v)) > 0 ? atoi(v) / 100.0 : 0.0; }();
        const double hold = hold_given > 0.0 ? hold_given : EvenOn() ? 1.0 : kClockHold;
        if (off <= std::max(slack, hold * every) && -off <= std::max(slack, kClockHold * every))
        {
            // On the beat: the pace follows the frames slowly, the clock the stamps - how fast, Trust says.
            const double trust = Trust(span);
            every += (known < 30 ? 0.1 : 0.03) * (span - every);
            moment += every + (known < 30 ? 0.12 : trust) * off;
            if (known < 1000) ++known;
            off_beat = 0;
        }
        else if (beats >= 1.0 && beats <= 3.0 && std::fabs(rest) <= slack && off_beat == 0)
        {
            moment += (1.0 + beats) * every + 0.05 * rest;
            off_beat = 0;
        }
        else if (++off_beat >= 3 || known < 30 || std::fabs(off) > 3.5 * every)
        {
            // Another pace (a limit set, another scene's load): learnt again from here.
            // (Round 56: the last frames' average where there are four - one span of an uneven game is no pace.)
            double recent = 0.0;
            for (unsigned i = 0; i < filled; ++i) recent += spans[i];
            recent = filled >= 4 ? recent / filled : span;
            every = recent > 0.0015 && recent < 0.25 ? recent : every;
            known = 1; off_beat = 0;
            // (Round 91: onto the stamp only across a real break - more than three and a half frames off; else
            // within half and twice a beat of the frame before, Bounded.)
            moment = std::fabs(off) > 3.5 * every ? stamp : Bounded(stamp);
        }
        else moment = Bounded(stamp);   // one odd frame: towards its own time, the pace kept (round 91: never a squeezed pair)
        if (moment <= before) moment = stamp > before ? stamp : before + 0.001;
        return moment;
    }
} g_fg_clock;
