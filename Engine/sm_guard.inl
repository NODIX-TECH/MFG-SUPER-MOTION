// sm_guard.inl - the picture moving faster than the generator can follow.
//
// The report: "things look like jelly moving in front of you while playing".
// Looked at on a scene whose every frame is known (tests\scene.cpp,
// tests\jelly.cpp): DLSS MFG finds the motion between two real frames itself,
// and it follows it only so far. A picture moving 1.25% of its width from one
// real frame to the next (32 pixels of 2560) comes out right - 3% of its
// pixels off; at 2.5% a sixth of them are off, at 3.75% more than half, at 5%
// nine in ten: a frame of bent and smeared patches. That is the jelly. The
// size frames in between are made at does not change it, nor does a motion
// field handed to the generator (it goes by its own).
//
// Such a frame is worse than none. So the small luma every capture already
// leaves (lf_luma.inl, 320 pixels across) is searched for how far the picture
// moved since the real frame before - twenty-four blocks, each its own way - and
// when the picture as a whole moved further than the generator follows (a
// camera turning fast), or nearly all of it changed in a way no movement in
// reach explains, this pair gets no frames in between: the game's own frames
// are shown, at their moments, until it slows down. One thing moving fast
// across a picture at rest, or the road rushing by under a steady horizon, is
// left to the generator. The search
// costs a tenth of a millisecond on the processor and nothing on the card;
// a pair passed over saves the generator's whole work for it.
//
// "Fast motion guard" in the app turns it on. Off unless asked for (round 4):
// tried in a game, frames in between going missing in every fast turn was
// found worse than the frames it kept from the screen.
//
// The same search says where the picture went, block by block: that is where
// a small frame in between starts looking for its detail in the real frames
// (g_fg_prior, sm_rebuild.inl) - found whether the guard is on or off.

#include <intrin.h>

static bool GuardWanted()
{
    return smOpt.guard.load() != 0;
}

static struct GuardState
{
    std::vector<BYTE> last, last_half, now_half;
    UINT w = 0, h = 0;
    // Since the last summary.
    unsigned pairs = 0, passed = 0;
    ULONGLONG told = 0;
    double sum_share = 0.0;
    std::vector<unsigned> half_sads;     // one block's search at half size, every shift
    unsigned settled = 0;               // blocks put right by the others' movement, since the last summary
} g_guard;

// The sum of absolute differences of two rows of bytes.
static inline unsigned GuardRow(const BYTE *a, const BYTE *b, int count)
{
    __m128i sum = _mm_setzero_si128();
    int x = 0;
    for (; x + 16 <= count; x += 16)
        sum = _mm_add_epi64(sum, _mm_sad_epu8(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + x)),
                                              _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + x))));
    unsigned total = static_cast<unsigned>(_mm_cvtsi128_si32(sum)) + static_cast<unsigned>(_mm_cvtsi128_si32(_mm_srli_si128(sum, 8)));
    for (; x < count; ++x) total += static_cast<unsigned>(std::abs(int(a[x]) - int(b[x])));
    return total;
}

// A block of `now` against the block of `before` it would have come from had
// the picture moved by (dx, dy).
static unsigned GuardBlock(const BYTE *now, const BYTE *before, int stride, int x, int y, int bw, int bh, int dx, int dy)
{
    unsigned total = 0;
    for (int row = 0; row < bh; ++row)
        total += GuardRow(now + static_cast<size_t>(y + row) * stride + x,
                          before + static_cast<size_t>(y + row - dy) * stride + (x - dx), bw);
    return total;
}

static void GuardHalf(const BYTE *from, UINT w, UINT h, std::vector<BYTE> &to)
{
    const UINT hw = w / 2, hh = h / 2;
    to.resize(static_cast<size_t>(hw) * hh);
    for (UINT y = 0; y < hh; ++y)
    {
        const BYTE *a = from + static_cast<size_t>(2 * y) * w, *b = a + w;
        BYTE *out = to.data() + static_cast<size_t>(y) * hw;
        for (UINT x = 0; x < hw; ++x) out[x] = static_cast<BYTE>((a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2);
    }
}

// Did the picture move, since the real frame before, further than the
// generator follows? Uses the luma the capture just wrote; called once for
// every new real frame. `restart`: the frame before is not this scene's.
static bool GuardTooFast(bool restart)
{
    GuardState &g = g_guard;
    if (!lfLuma.mapped || !lfCap.luma_ok || lfLuma.view == nullptr || lfLuma.w < 64 || lfLuma.h < 36) return false;
    const UINT w = lfLuma.w, h = lfLuma.h;
    const size_t n = static_cast<size_t>(w) * h;
    const bool comparable = !restart && g.w == w && g.h == h && g.last.size() == n;
    GuardHalf(lfLuma.view, w, h, g.now_half);
    bool fast = false;
    g_fg_prior = RebuildPrior();
    if (comparable)
    {
        // In pixels of the luma: what the generator follows, and how far it is
        // looked - half as far again, enough to tell "further than that".
        // Frames in between rebuilt from the real frames (sm_rebuild.inl) stay
        // right half as far again as the generator's own: measured on a pan,
        // 3.6% of their pixels off at 64 pixels of 2560 a frame, 14% at 96,
        // 19% at 112 - where the generator's own were 16% off at 64 and 59%
        // at 96.
        const double follows = g_rebuild.ready ? 0.042 : 0.028;
        const double limit = follows * w;
        const int reach = static_cast<int>(std::lround(0.065 * w)) / 2 * 2;      // even: searched at half size first
        const int hw = static_cast<int>(w / 2), hreach = reach / 2;
        const int across = RebuildPrior::kAcross, down = RebuildPrior::kDown;
        const int bw = (static_cast<int>(w) - 2 * reach) / across, bh = (static_cast<int>(h) - 2 * reach) / down;
        unsigned followed = 0, lost = 0, told_apart = 0;
        double sum_x = 0.0, sum_y = 0.0;
        const int blocks = across * down;
        // 0 nothing happened there, 1 a movement explains it, 2 none in reach does
        int kind[RebuildPrior::kAcross * RebuildPrior::kDown] = {};
        // What repeats - a building's floors, a fence, a tiled wall - looks
        // the same one row on as where it is: a block of it has several
        // movements that explain it equally, and the best of them by a hair
        // is as often the wrong one. (The tower block of tests\world.h,
        // tilted half a floor a frame: its face came out one floor off, or
        // torn between the two - "as in an earthquake".) So a block keeps
        // every movement that explains it nearly as well as its best, and
        // where there is more than one the rest of the picture decides: the
        // blocks that have only one say how the picture moves - a shift, and
        // a growing or shrinking about its middle - and the block takes the
        // movement of its own nearest to that.
        struct Way { int x, y; unsigned sad; };
        Way ways[RebuildPrior::kAcross * RebuildPrior::kDown][4] = {};
        int way_count[RebuildPrior::kAcross * RebuildPrior::kDown] = {};
        int moved_x[RebuildPrior::kAcross * RebuildPrior::kDown] = {}, moved_y[RebuildPrior::kAcross * RebuildPrior::kDown] = {};
        const int side = 2 * hreach + 1;
        g.half_sads.resize(static_cast<size_t>(side) * side);
        if (bw >= 16 && bh >= 12)
            for (int j = 0; j < down; ++j)
                for (int i = 0; i < across; ++i)
                {
                    const int x = reach + i * bw, y = reach + j * bh, at = j * across + i;
                    const unsigned still = GuardBlock(lfLuma.view, g.last.data(), static_cast<int>(w), x, y, bw, bh, 0, 0);
                    const double per_pixel = double(still) / (double(bw) * bh);
                    if (per_pixel < 1.5) continue;                 // nothing happened here
                    ++told_apart;
                    // Half size first, every shift; then the pixels around the best of them.
                    unsigned best = UINT_MAX;
                    for (int dy = -hreach; dy <= hreach; ++dy)
                        for (int dx = -hreach; dx <= hreach; ++dx)
                        {
                            const unsigned sad = GuardBlock(g.now_half.data(), g.last_half.data(), hw, x / 2, y / 2, bw / 2, bh / 2, dx, dy);
                            g.half_sads[static_cast<size_t>(dy + hreach) * side + (dx + hreach)] = sad;
                            if (sad < best) best = sad;
                        }
                    // Every shift that is the least among its neighbours and
                    // within a third of the best: the four best of them, each
                    // looked at again at full size.
                    Way found[4] = {};
                    int count = 0;
                    for (int dy = -hreach; dy <= hreach; ++dy)
                        for (int dx = -hreach; dx <= hreach; ++dx)
                        {
                            const unsigned sad = g.half_sads[static_cast<size_t>(dy + hreach) * side + (dx + hreach)];
                            if (sad > best + best / 3 + 16) continue;
                            bool least = true;
                            for (int ny = std::max(-hreach, dy - 1); ny <= std::min(hreach, dy + 1) && least; ++ny)
                                for (int nx = std::max(-hreach, dx - 1); nx <= std::min(hreach, dx + 1); ++nx)
                                    if (g.half_sads[static_cast<size_t>(ny + hreach) * side + (nx + hreach)] < sad) { least = false; break; }
                            if (!least) continue;
                            int place = count < 4 ? count : -1;
                            if (place < 0) for (int k = 0; k < 4; ++k) if (found[k].sad > sad && (place < 0 || found[k].sad > found[place].sad)) place = k;
                            if (place < 0) continue;
                            found[place] = { dx, dy, sad };
                            if (count < 4) ++count;
                        }
                    unsigned fine = UINT_MAX;
                    for (int k = 0; k < count; ++k)
                    {
                        Way way = { 0, 0, UINT_MAX };
                        for (int dy = 2 * found[k].y - 1; dy <= 2 * found[k].y + 1; ++dy)
                            for (int dx = 2 * found[k].x - 1; dx <= 2 * found[k].x + 1; ++dx)
                            {
                                if (std::abs(dx) > reach || std::abs(dy) > reach) continue;
                                const unsigned sad = GuardBlock(lfLuma.view, g.last.data(), static_cast<int>(w), x, y, bw, bh, dx, dy);
                                if (sad < way.sad) way = { dx, dy, sad };
                            }
                        found[k] = way;
                        if (way.sad < fine) fine = way.sad;
                    }
                    // A movement in reach explains the change, or the block
                    // changed a good deal with none that does: further than
                    // was looked, or not a movement at all.
                    if (fine <= 0.6 * still)
                    {
                        kind[at] = 1;
                        for (int k = 0; k < count; ++k)
                            if (found[k].sad <= fine + fine / 3 + 32 && found[k].sad <= 0.6 * still)
                            {
                                Way &slot = ways[at][way_count[at]++];
                                slot = found[k];
                                if (slot.sad == fine) std::swap(slot, ways[at][0]);      // its best first
                            }
                        moved_x[at] = ways[at][0].x; moved_y[at] = ways[at][0].y;
                    }
                    else
                    {
                        kind[at] = 2;
                        if (per_pixel > 8.0) ++lost;
                    }
                }
        // How the picture moves, by the blocks that are sure: v = shift + grow * (place - middle).
        double shift_x = 0.0, shift_y = 0.0, grow = 0.0, mid_x = 0.0, mid_y = 0.0;
        int sure = 0;
        auto place_x = [&](int at) { return reach + (at % across) * bw + 0.5 * bw; };
        auto place_y = [&](int at) { return reach + (at / across) * bh + 0.5 * bh; };
        bool left_out[RebuildPrior::kAcross * RebuildPrior::kDown] = {};
        for (int round = 0; round < 2; ++round)
        {
            double sx = 0.0, sy = 0.0, px = 0.0, py = 0.0;
            sure = 0;
            for (int at = 0; at < blocks; ++at)
                if (kind[at] == 1 && way_count[at] == 1 && !left_out[at]) { ++sure; sx += moved_x[at]; sy += moved_y[at]; px += place_x(at); py += place_y(at); }
            if (sure < 3) break;
            shift_x = sx / sure; shift_y = sy / sure; mid_x = px / sure; mid_y = py / sure;
            double with = 0.0, spread = 0.0;
            for (int at = 0; at < blocks; ++at)
                if (kind[at] == 1 && way_count[at] == 1 && !left_out[at])
                {
                    const double qx = place_x(at) - mid_x, qy = place_y(at) - mid_y;
                    with += qx * (moved_x[at] - shift_x) + qy * (moved_y[at] - shift_y);
                    spread += qx * qx + qy * qy;
                }
            grow = spread > 1.0 ? with / spread : 0.0;
            if (round == 0)
                for (int at = 0; at < blocks; ++at)
                    if (kind[at] == 1 && way_count[at] == 1)
                    {
                        // One that goes its own way (a car crossing) does not speak for the picture.
                        const double ex = moved_x[at] - (shift_x + grow * (place_x(at) - mid_x)), ey = moved_y[at] - (shift_y + grow * (place_y(at) - mid_y));
                        left_out[at] = ex * ex + ey * ey > 3.0 * 3.0;
                    }
        }
        if (sure >= 3)
            for (int at = 0; at < blocks; ++at)
            {
                const double want_x = shift_x + grow * (place_x(at) - mid_x), want_y = shift_y + grow * (place_y(at) - mid_y);
                if (kind[at] == 1 && way_count[at] > 1)
                {
                    int nearest = 0;
                    double least = 1e30;
                    for (int k = 0; k < way_count[at]; ++k)
                    {
                        const double ex = ways[at][k].x - want_x, ey = ways[at][k].y - want_y, off = ex * ex + ey * ey;
                        if (off < least) { least = off; nearest = k; }
                    }
                    if (nearest != 0) ++g.settled;
                    moved_x[at] = ways[at][nearest].x; moved_y[at] = ways[at][nearest].y;
                }
                if (kind[at] == 1)
                {
                    // And a block whose one movement is not the picture's:
                    // does the picture's own explain it nearly as well? Then
                    // it was a row of the repeating thing too (its other
                    // movements fell just outside "nearly as well as the
                    // best") and it goes with the picture; a thing really
                    // moving by itself - a car crossing - is not explained
                    // by the picture's movement at all, and keeps its own.
                    const double ex = moved_x[at] - want_x, ey = moved_y[at] - want_y;
                    if (ex * ex + ey * ey > 1.5 * 1.5)
                    {
                        const int x = reach + (at % across) * bw, y = reach + (at / across) * bh;
                        const int near_x = static_cast<int>(std::lround(want_x)), near_y = static_cast<int>(std::lround(want_y));
                        Way with = { 0, 0, UINT_MAX };
                        for (int dy = near_y - 1; dy <= near_y + 1; ++dy)
                            for (int dx = near_x - 1; dx <= near_x + 1; ++dx)
                            {
                                if (std::abs(dx) > reach || std::abs(dy) > reach) continue;
                                const unsigned sad = GuardBlock(lfLuma.view, g.last.data(), static_cast<int>(w), x, y, bw, bh, dx, dy);
                                if (sad < with.sad) with = { dx, dy, sad };
                            }
                        const unsigned own = GuardBlock(lfLuma.view, g.last.data(), static_cast<int>(w), x, y, bw, bh, moved_x[at], moved_y[at]);
                        if (with.sad != UINT_MAX && with.sad <= own + own / 3 + 48)
                        {
                            moved_x[at] = with.x; moved_y[at] = with.y;
                            ++g.settled;
                        }
                    }
                }
                else if (kind[at] != 1)
                {
                    // A block that could not be followed starts from where the
                    // others went - and so does one in which nothing was seen
                    // to happen: a sky has no movement of its own to show, and
                    // left at "none" it pulled the blocks beside it half way
                    // back (the top floors of the tower block, next to it,
                    // stayed torn). What really stands still is found
                    // standing still all the same: that is looked for first.
                    g_fg_prior.moved[at][0] = static_cast<float>(want_x) / w;
                    g_fg_prior.moved[at][1] = static_cast<float>(want_y) / w;
                }
            }
        for (int at = 0; at < blocks; ++at)
            if (kind[at] == 1)
            {
                ++followed; sum_x += moved_x[at]; sum_y += moved_y[at];
                g_fg_prior.moved[at][0] = static_cast<float>(moved_x[at]) / w;
                g_fg_prior.moved[at][1] = static_cast<float>(moved_y[at]) / w;
            }
        if (followed && sure < 3)
            for (int at = 0; at < blocks; ++at)
                if (kind[at] == 2)
                {
                    g_fg_prior.moved[at][0] = static_cast<float>(sum_x / followed) / w;
                    g_fg_prior.moved[at][1] = static_cast<float>(sum_y / followed) / w;
                }
        // The picture as a whole: the blocks' movements taken together. A
        // camera turning moves them all one way; driving ahead moves them
        // apart - the road fast, the horizon hardly - and that the generator
        // is left to: only the fast part of its frame would be poor.
        const double whole = followed ? std::sqrt(sum_x * sum_x + sum_y * sum_y) / followed : 0.0;
        fast = GuardWanted() && told_apart >= 5 &&
               ((followed * 10 >= told_apart * 6 && whole > limit) || (told_apart >= 12 && lost * 10 >= told_apart * 7));
        ++g.pairs;
        if (fast) ++g.passed;
        g.sum_share += whole / w;
        const ULONGLONG tick = GetTickCount64();
        if (tick - g.told >= 5000)
        {
            if (g.passed)
                Trace("[guard] in %u of %u pairs the picture moved further than frames in between follow (%.1f%% of its width a "
                      "frame; %.1f%% on average) - the game's own frames were shown for them", g.passed, g.pairs, 100.0 * follows,
                      100.0 * g.sum_share / g.pairs);
            static const bool diag = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
            if (diag && g.settled)
                Trace("[guard] %u blocks of a repeating picture took the movement the rest of the picture has (of %u pairs)", g.settled, g.pairs);
            g.told = tick; g.pairs = g.passed = 0; g.sum_share = 0.0; g.settled = 0;
        }
    }
    g.last.assign(lfLuma.view, lfLuma.view + n);
    g.last_half.swap(g.now_half);
    g.w = w; g.h = h;
    return fast;
}
