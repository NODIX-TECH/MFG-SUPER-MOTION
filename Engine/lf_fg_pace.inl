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
//     1  their 97th percentile and half a millisecond (the default; the 90th until round 46 - a pair in ten came late,
//        and on a game as uneven as GTA V each was a moment the picture stood and then leapt)
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
        const unsigned queue = FgQueueTarget();
        if (queue == 0) return sorted[count / 2];
        if (queue == 1) return sorted[std::min(count - 1, static_cast<unsigned>(0.97 * (count - 1) + 0.5))] + 0.0005;
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
            delay -= std::min(above, std::max(0.00005, above > 0.004 ? above / 12.0 : 0.0));
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
static struct FgClock
{
    double moment = 0.0, before = 0.0;   // this real frame's and the one before it
    double every = 0.0;                  // the average time between taken frames
    unsigned known = 0;                  // frames the pace was learnt from
    int off_beat = 0;                    // frames in a row the stamp was far off the clock
    double spans[8] = {};                // the last times between two taken frames
    unsigned filled = 0, next = 0;

    void Restart(double stamp) { moment = stamp; every = 0.0; before = 0.0; known = 0; off_beat = 0; filled = next = 0; }

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
            if (std::fabs(off) <= slack) moment += every + 0.05 * off;
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
        if (filled == 8)
        {
            double least = spans[0], most = spans[0], sum = 0.0;
            for (const double s : spans) { least = std::min(least, s); most = std::max(most, s); sum += s; }
            const double recent = sum / 8.0;
            if (most - least <= std::max(0.3 * recent, 1.05 * grid) && std::fabs(recent - every) > 0.1 * every)
            {
                every = recent;
                known = 8;
                off_beat = 0;
                moment = stamp;
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
        // Left-out frames: whole beats later.
        const double beats = std::round(off / every);
        const double rest = off - beats * every;
        if (std::fabs(off) <= slack)
        {
            // On the beat: the pace follows the frames slowly, the clock the stamps slowly.
            every += (known < 30 ? 0.1 : 0.03) * (span - every);
            moment += every + (known < 30 ? 0.12 : 0.05) * off;
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
            every = span > 0.0015 && span < 0.25 ? span : every;
            known = 1; off_beat = 0;
            moment = stamp;
        }
        else moment = stamp;        // one odd frame: shown at its own time, the pace kept
        if (moment <= before) moment = stamp > before ? stamp : before + 0.001;
        return moment;
    }
} g_fg_clock;
