// sm_wide.h - round 71: where the picture as a whole went from one real frame to the next (the engine's GuardWide,
// sm_guard.inl), on the luma the capture makes (320 x 180), as a function of its own - tests\msmdata.exe runs the
// very same one on recorded footage.
//
// What SMFG is told of it is where it looks first: a word half a screen wrong and every place of the picture is
// looked for in the wrong place (a user's GTA V locked at 30 FPS, round 71: a turn of some 500 pixels a pair taken
// for 992, the frame in between drawn as a mess of the pier's pillars). Two things made such words:
//   - the more the picture moved, the less of the two frames lies over each other, and a small piece that overlaps
//     - sky, a plain wall - matches better by chance than the whole picture moved by the right amount;
//   - nothing knew that a camera that turned 500 pixels in the pair before turns about as much in this one.
// mode (SM_WIDE / msmdata wide=): 0 as before round 71; bit 1: a movement far from the pair before's (or from none,
// when that is not known) costs more; bit 2: a movement under which less of the frames overlaps costs more.
#pragma once
#include <vector>
#include <cmath>
#include <cstdlib>
#include <algorithm>

struct WideShift { bool known = false; float x = 0.0f, y = 0.0f; };   // luma pixels, frame before -> now
static const int kWideMode = 0;     // the engine's mode unless SM_WIDE says another

static WideShift WideFind(const unsigned char *now, const unsigned char *before, int w, int h, const WideShift &last, int mode)
{
    WideShift out;
    struct Level { std::vector<int> a, b; int w = 0, h = 0, k = 1; };
    auto shrink = [&](int k, Level &l) {
        l.k = k; l.w = w / k; l.h = h / k;
        l.a.assign(static_cast<size_t>(l.w) * l.h, 0); l.b.assign(l.a.size(), 0);
        for (int y = 0; y < l.h * k; ++y)
            for (int x = 0; x < l.w * k; ++x)
            {
                const size_t to = static_cast<size_t>(y / k) * l.w + x / k;
                l.a[to] += now[static_cast<size_t>(y) * w + x];
                l.b[to] += before[static_cast<size_t>(y) * w + x];
            }
    };
    // The mean difference where they overlap, the picture moved by (dx, dy); none: a third or less overlaps.
    // (Round 66: the middle of the picture and its lower left corner left out - in a game seen from behind, the player
    // stands in the middle and does not move on the screen however fast the camera turns, and the map in the corner
    // turns in place: the two of them matched best where nothing moved, and a very fast turn - the world 200 to 400
    // pixels a frame past a player standing still - got no word of where it went. SMFG searched near "not at all",
    // found nothing, and drew the frame from words that were nowhere: the user's whole picture in waves, the player in
    // blocks. Where the world went is told by the world around them.)
    auto cost = [mode, &last](const Level &l, int dx, int dy) {
        const int x0 = std::max(0, dx), x1 = std::min(l.w, l.w + dx), y0 = std::max(0, dy), y1 = std::min(l.h, l.h + dy);
        if ((x1 - x0) * (y1 - y0) * 3 <= l.w * l.h) return 1e30;
        const int mx0 = l.w * 33 / 100, mx1 = l.w * 67 / 100, my0 = l.h * 25 / 100;     // the player
        const int cx1 = l.w * 22 / 100, cy0 = l.h * 70 / 100;                          // the map
        long long sum = 0, count = 0;
        for (int y = y0; y < y1; ++y)
        {
            const int *a = l.a.data() + static_cast<size_t>(y) * l.w, *b = l.b.data() + static_cast<size_t>(y - dy) * l.w - dx;
            for (int x = x0; x < x1; ++x)
            {
                if ((x >= mx0 && x < mx1 && y >= my0) || (x < cx1 && y >= cy0)) continue;
                sum += std::abs(a[x] - b[x]); ++count;
            }
        }
        if (count == 0) return 1e30;
        double c = double(sum) / (double(count) * l.k * l.k);
        // (Round 71, bit 2: half as much again where only a third overlaps, nothing more where all of it does.)
        if (mode & 2) c *= 1.0 + 0.75 * (1.0 - double(x1 - x0) * double(y1 - y0) / (double(l.w) * l.h));
        // (Round 71, bit 1: a quarter more for each half width away from the pair before's movement.)
        if (mode & 1)
        {
            const double px = last.known ? last.x / l.k : 0.0, py = last.known ? last.y / l.k : 0.0;
            c *= 1.0 + 0.5 * std::sqrt((dx - px) * (dx - px) + (dy - py) * (dy - py)) / l.w;
        }
        return c;
    };
    auto search = [&](const Level &l, int cx, int cy, int rx, int ry, int &bx, int &by) {
        double least = 1e30;
        for (int dy = cy - ry; dy <= cy + ry; ++dy)
            for (int dx = cx - rx; dx <= cx + rx; ++dx)
            {
                const double c = cost(l, dx, dy);
                if (c < least) { least = c; bx = dx; by = dy; }
            }
        return least;
    };
    Level l8, l4, l2;
    shrink(8, l8); shrink(4, l4); shrink(2, l2);
    if (l8.w < 16 || l8.h < 8) return out;
    int bx = 0, by = 0;
    search(l8, 0, 0, l8.w / 2, l8.h / 4, bx, by);
    int x4 = bx * 2, y4 = by * 2;
    search(l4, bx * 2, by * 2, 2, 2, x4, y4);
    int x2 = x4 * 2, y2 = y4 * 2;
    const double best = search(l2, x4 * 2, y4 * 2, 2, 2, x2, y2);
    const double still = cost(l2, 0, 0);
    const float x = float(x2 * 2), y = float(y2 * 2);
    // (Clearly better than standing still, and further than SMFG reaches by itself: 4% of the width.)
    // (Round 66: 0.85 - in a very fast turn the game's own motion blur leaves no match clearly good; a wrong word costs
    // nothing, SMFG tries "not at all" beside it.)
    if (best < 0.85 * still && std::sqrt(x * x + y * y) > 0.04f * w) { out.known = true; out.x = x; out.y = y; }
    return out;
}
