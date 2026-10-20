// sm_rebuild.inl - frames in between made small, shown at the picture's size.
//
// The generator's work grows with the size its frames are made at: on an RTX
// 3060 a first frame in between costs 3.7 ms at 2560x1440, 1.8 at 1280x720 and
// 1.3 at 640x360 - and under a game that fills the card each millisecond is a
// tenth of the game's own frames. "Flow" in the app is that size. Scaled back
// up as it is, such a frame is soft, and a soft frame between two sharp ones
// is what "the resolution drops" was.
//
// What a small frame in between says well is where everything is at that
// moment - the generator's own business. What it lacks, the detail, is in the
// two real frames around it, at the picture's full size. So the detail is
// taken from them:
//
//   1. Follow (FollowMain, on the small frames): for every block of the frame
//      in between, where that piece of picture lies in the real frame after
//      it and in the one before - a short search around where the picture as
//      a whole moved (the guard's blocks, sm_guard.inl) and around "not at
//      all" (a HUD, a weapon held still), then between two steps of it. How
//      well it was found is kept beside it.
//   2. Choose (ChooseMain, on the small frames): for every pixel of the small
//      frame in between, which of the four blocks around it says where it
//      went itself - where a thing ends and the picture behind it begins they
//      say different things. Done at the picture's size (round 3's first
//      form) this was most of the whole work: twelve reads a pixel of 3.7
//      million; here it is a ninth of them.
//   3. Build (BuildMain, at the picture's size): every pixel is the small
//      frame scaled up, plus what the real frame has there that its own small
//      copy has not - its detail - weighed by how well the two agree at that
//      very pixel. Found well: the real frame's own sharpness, where the
//      generator put it. Not found (a thing appearing, a block the search
//      lost): the scaled frame as it is, soft and never wrong. Where nothing
//      moved near (lf_fg_still.inl) the real frame's own pixels.
//
// And what the two real frames agree about is built from them alone - the
// pair's own movement (rounds 6 and 7). The generator finds the motion
// between two real frames itself, and it is wrong in two ways that show:
//
//   - What repeats defeats it. The floors of a tower block, moved half a floor
//     from one frame to the next, are as well explained by half a floor the
//     other way: its frame showed the face one floor off, or torn between the
//     two, a different way with each pair - "as in an earthquake"
//     (tests\world.h; 48 to 62% of the face's pixels off at a tilt of 32
//     pixels a frame, with the road, the houses and the rock beside it right).
//   - What is small it loses. A dark opening of five pixels by seven on a bare
//     wall is a pixel and a half of the frame the generator works on; it came
//     out beside its place or faded, differently in each of a pair's frames -
//     the dots "moving from their place" while the camera tilts (a third to
//     two thirds of their pixels off, at any speed).
//
// The engine knows better in both, because it knows how the rest of the
// picture moved (sm_guard.inl: the blocks that are sure decide for the ones
// that are not) and has the two real frames at hand. Once for every pair:
//
//   a. Pair (PairMain): for every block, the movement from the real frame
//      before to the one after under which the two show the same thing -
//      searched around the picture's own movement there (which wins a tie)
//      and around "not at all", then in quarter steps, on every pixel of the
//      block: a dot counts.
//   b. Settle (SettleMain): for every small pixel, the movement of the blocks
//      around it (blended; each by itself where the blend does not fit), and
//      how far the two real frames agree around the pixel, moved so. What is
//      allowed for is a fifth of a pixel's misplacement of the picture's own
//      contrast there - an edge is not unrest. And for every tile of eight by
//      eight small pixels, whether all of it agrees; Widen (WidenMain):
//      whether all the tiles around do as well.
//
// A pixel the two real frames agree about, moved by the pair's movement, is
// built from them: its base is their own picture there, its detail the nearer
// one's. All of a pair's frames in between go by the same movement, so what
// stands in the scene glides from one real frame to the next - it cannot be
// somewhere else in each. The generator keeps its say where they do not
// agree (a thing appearing or covered, a light changing), and around a thing
// that moves its own way: there a pixel is theirs only where the generator
// shows the same. A block that is theirs altogether is not searched for in
// the generator's frame at all - most of a picture a camera moves through.
//
// Checked against scenes whose every frame is known (tests\jelly.ps1).

static struct RebuildState
{
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> follow, choose, build, pair, settle, widen, still, mover, spread;
    // The build pass as a drawing onto a back buffer of one format (made when first asked for).
    winrt::com_ptr<ID3D12PipelineState> draw;
    winrt::com_ptr<ID3D12DescriptorHeap> draw_view;
    DXGI_FORMAT draw_format = DXGI_FORMAT_UNKNOWN;
    bool draw_failed = false;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    winrt::com_ptr<ID3D12Resource> offsets, trust;    // a texel a block: where to, and how well found
    winrt::com_ptr<ID3D12Resource> choice, sure;      // a texel a small pixel: the same, chosen among the blocks around it
    winrt::com_ptr<ID3D12Resource> all_moving;        // a still map that says "moving" everywhere, for when there is none
    // The pair's own movement: a texel a block, a texel a small pixel (with
    // how far the two real frames agree there), and a texel a tile of 8 x 8
    // small pixels - all of it agrees; all around it agrees.
    winrt::com_ptr<ID3D12Resource> field, settled, calm, wide;
    // A texel a pixel of the picture: a mark that stands at the same place of the screen in both real frames.
    winrt::com_ptr<ID3D12Resource> still_mark;
    // The blocks' movements of the pair before (where each block starts looking), and a texel a block:
    // the movement of a thing that goes its own way through it.
    winrt::com_ptr<ID3D12Resource> field_last, movers, movers_found;
    bool field_is_read = false, last_is_read = false;  // which of the two fields rest readable
    bool movers_on = false;                            // SM_MOVERS=1: the second movement (off: see MoverMain)
    bool repairs = true;                               // what the two real frames agree about is built from them
    bool field_read = false;                           // field, settled and wide rest readable (a pair's frames read them)
    UINT64 pair_done = ~0ull;                          // the pair they were made for
    UINT grid_w = 0, grid_h = 0, small_w = 0, small_h = 0, tiles_w = 0, tiles_h = 0;
    float unit = 1.0f;
    bool ready = false, failed = false;
} g_rebuild;

// SM_DIAG=1: where a frame in between's time on the card goes - a time stamp
// after each step of it (the presenter owns the heap and reads them).
struct RebuildMarks
{
    ID3D12QueryHeap *heap = nullptr;
    UINT next = 0;
    bool pair_made = false;
};
// A texture read by the compute passes and by the build pass drawn as pixels.
static const D3D12_RESOURCE_STATES kRebuildRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
static const UINT kRebuildMarks = 6;      // start, the pair's movement, follow, choose, build, on the back buffer

static void RebuildClose()
{
    RebuildState &r = g_rebuild;
    r.offsets = nullptr; r.trust = nullptr;
    r.choice = nullptr; r.sure = nullptr;
    r.field = nullptr; r.settled = nullptr; r.calm = nullptr; r.wide = nullptr; r.still_mark = nullptr;
    r.field_last = nullptr; r.movers = nullptr; r.movers_found = nullptr; r.field_is_read = r.last_is_read = false;
    r.field_read = false; r.pair_done = ~0ull;
    r.grid_w = r.grid_h = r.small_w = r.small_h = r.tiles_w = r.tiles_h = 0;
    r.ready = false;
}

static const char kRebuildHlsl[] = R"hlsl(
Texture2D<float4> made : register(t0);       // the frame in between, small
Texture2D<float4> afterFull : register(t1);  // the real frame after it
Texture2D<float4> beforeFull : register(t2); // the real frame before it
Texture2D<float4> afterSmall : register(t3); // their small copies
Texture2D<float4> beforeSmall : register(t4);
// Per block for ChooseMain (what FollowMain found), per small pixel for
// BuildMain (what ChooseMain chose): xy to the frame after, zw to the one
// before, in small pixels; and how far each is to be believed.
Texture2D<float4> whereTo : register(t5);
Texture2D<float4> howWell : register(t6);    // x, y: how far the two are believed; z: built from the real frames alone
Texture2D<float> moved : register(t7);       // 0 where nothing moved near
// The pair's own movement, per small pixel of the frame half way between the
// two real ones: xy from the frame before to the one after, in small pixels;
// z how far the two agree around it, moved so. And per tile of 8 x 8 small
// pixels: 1 where they agree in all of it and in all the tiles around.
Texture2D<float4> settled : register(t8);
Texture2D<float4> wideCalm : register(t9);
// Per pixel of the picture: 1 where the two real frames show the same mark at
// the same place of the screen - a game's own read-out, which the picture
// moves under and which does not move with it.
Texture2D<float> stillMark : register(t10);
// Per block: xy the movement, from the frame before to the one after, of a
// thing that goes its own way through the block; z > 0 where there is one.
Texture2D<float4> movers : register(t11);
RWTexture2D<float4> out0 : register(u0);
RWTexture2D<float4> out1 : register(u1);
SamplerState lin : register(s0);
cbuffer C : register(b0)
{
    uint gridW, gridH;       // blocks across and down
    float unit;              // small pixels a step of the search
    float phase;             // where the frame in between is: 0 the frame before, 1 the one after
    float4 prior[12];        // 6 x 4 movements from the frame before to the one after, small pixels, two a float4
    uint outW, outH;         // the picture
    float smallW, smallH;    // the small frames
    float phaseMade;         // where the generator's frame is that this one goes by (the same, or the nearest there is)
    float havePrev;          // 1: whereTo holds the blocks' movements of the pair before (PairMain)
    float2 unused;
};

#ifndef REPAIR
#define REPAIR 1
#endif
#ifndef SHOW
#define SHOW 0
#endif

float2 PriorAt(int x, int y)
{
    int k = y * 6 + x;
    float4 pair = prior[k >> 1];
    return (k & 1) ? pair.zw : pair.xy;
}

// The picture's own movement at a place (0..1 across and down).
float2 Prior(float2 at)
{
    float2 g = clamp(at * float2(6.0, 4.0) - 0.5, float2(0.0, 0.0), float2(5.0, 3.0));
    int2 a = int2(floor(g));
    int2 b = min(a + 1, int2(5, 3));
    float2 f = g - float2(a);
    return lerp(lerp(PriorAt(a.x, a.y), PriorAt(b.x, a.y), f.x), lerp(PriorAt(a.x, b.y), PriorAt(b.x, b.y), f.x), f.y);
}

// How much a real frame's small copy, at some place, looks like the frame in
// between here: 1 the same, 0 from six levels of 255 a colour apart on.
float Alike(float3 real, float3 soft)
{
    float3 off = abs(real - soft);
    return saturate(1.0 - (max(off.r, max(off.g, off.b)) - 0.02) / 0.08);
}

// The search is compute shaders; the last step, the frame at the picture's
// size, is compiled a second time without them (DRAW) as a drawing.
#ifndef DRAW
// A block is followed by sixty-four threads at once, each trying one place of
// its own. Tried one after another by a single thread (this pass's first
// form) the search was half a millisecond for 920 blocks - more than all the
// rest of a frame in between - with the card nearly idle under it: a thread
// is slow by itself, and there were only 920 of them.
groupshared float3 held[64];        // the block of the frame in between, 8 x 8 points
groupshared float tries[7][64];     // how far apart, for each place tried, step by step
groupshared float calmest[64];      // how far the two real frames agree, point by point

// How far the block of the frame in between is from the same block of a real
// frame moved by d.
float Apart(Texture2D<float4> real, float2 centre, float2 d, float2 inv)
{
    float sum = 0.0;
    [loop] for (int k = 0; k < 64; ++k)
    {
        float2 at = centre + (float2(k & 7, k >> 3) - 3.5) * 2.0 * unit + d;
        float3 e = abs(real.SampleLevel(lin, at * inv, 0).rgb - held[k]);
        sum += e.r + e.g + e.b;
    }
    return sum;
}

// The places tried, by number. First: around "not moved" (9), then around
// where the picture as a whole went (49).
float2 FirstTry(uint c, float2 guess)
{
    if (c < 9) return float2(float(c % 3) - 1.0, float(c / 3) - 1.0) * unit;
    c -= 9;
    return guess + float2(float(c % 7) - 3.0, float(c / 7) - 3.0) * unit;
}
// Further out, in double steps (81).
float2 WideTry(uint c) { return float2(float(c % 9) - 4.0, float(c / 9) - 4.0) * 2.0 * unit; }
// Next to a place (9).
float2 NextTry(uint c, float2 around) { return around + float2(float(c % 3) - 1.0, float(c / 3) - 1.0) * unit; }
// Between two steps, in quarters, both ways (25).
float2 FineTry(uint c, float2 around) { return around + float2(float(c % 5) - 2.0, float(c / 5) - 2.0) * 0.25 * unit; }
// Around a movement, two steps each way (25).
float2 MoveTry(uint c, float2 around, float step) { return around + float2(float(c % 5) - 2.0, float(c / 5) - 2.0) * step; }

)hlsl" R"hlsl(
// ---- once a pair: the two real frames by themselves ----

// How far apart the two real frames are over a block of the frame half way
// between them, the one after taken where the block would be in it had the
// picture moved by v from the frame before to it, the one before where it
// would have come from. Every pixel of the block is under one of the hundred
// places read: a thing of a pixel and a half counts.
float Twixt(float2 centre, float2 v, float2 inv)
{
    float sum = 0.0;
    [loop] for (int k = 0; k < 100; ++k)
    {
        float2 at = centre + (float2(k % 10, k / 10) - 4.5) * 1.6 * unit;
        float3 e = abs(afterSmall.SampleLevel(lin, (at + 0.5 * v) * inv, 0).rgb -
                       beforeSmall.SampleLevel(lin, (at - 0.5 * v) * inv, 0).rgb);
        sum += e.r + e.g + e.b;
    }
    return sum;
}

)hlsl" R"hlsl(
// What stands still on the screen while the picture moves under it: a game's
// own read-outs - a ring to aim by, a compass, figures. They are not part of
// the scene, and the scene's movement is not theirs: taken along with it, a
// thin mark was drawn twice in every frame in between, where the frame after
// had it and where the frame before had it, each moved by the picture's
// movement to another place - and not drawn where it stands. In a pair's five
// frames that is ten places: the read-outs "flicker very, very badly".
//
// A pixel is such a mark where the two real frames have the very same colour
// at the very same place, and it is not a flat place: a sky is the same in
// both frames too, but a thing may cross it between them.
[numthreads(8, 8, 1)]
void StillMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    int3 at = int3(id.xy, 0);
    float3 now = afterFull.Load(at).rgb;
    float3 same = abs(now - beforeFull.Load(at).rgb);
    float mark = 0.0;
    if (max(same.r, max(same.g, same.b)) < 0.006)
    {
        // Not flat - and not flat in both frames, the same way: beside a
        // thing of the scene that moves (a dot on a wall) the wall is the
        // same in both frames too, with the dot next to it in one of them
        // only. Taken for a mark, that piece of wall was drawn over the dot
        // when the dot came by.
        int2 last = int2(outW, outH) - 1;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            int3 next = int3(clamp(int2(id.xy) + int2(k == 0 ? -2 : k == 1 ? 2 : 0, k == 2 ? -2 : k == 3 ? 2 : 0), int2(0, 0), last), 0);
            float3 one = abs(afterFull.Load(next).rgb - now), other = abs(beforeFull.Load(next).rgb - now);
            if (max(one.r, max(one.g, one.b)) > 0.04 && max(other.r, max(other.g, other.b)) > 0.04) mark = 1.0;
        }
    }
    out0[id.xy] = float4(mark, 0.0, 0.0, 0.0);
}

// How much of a small pixel's place on the screen is such a mark (0..1).
float MarkShare(float2 at)
{
    float2 uv = at / float2(smallW, smallH), q = 0.25 / float2(smallW, smallH);
    return 0.25 * (stillMark.SampleLevel(lin, uv + float2(-q.x, -q.y), 0) + stillMark.SampleLevel(lin, uv + float2(q.x, -q.y), 0) +
                   stillMark.SampleLevel(lin, uv + float2(-q.x, q.y), 0) + stillMark.SampleLevel(lin, uv + float2(q.x, q.y), 0));
}

// Around the picture's own movement, three steps each way (49); around "not at all" (9).
float2 PairTry(uint c, float2 moves)
{
    if (c < 49) return moves + float2(float(c % 7) - 3.0, float(c / 7) - 3.0) * unit;
    c -= 49;
    return float2(float(c % 3) - 1.0, float(c / 3) - 1.0) * unit;
}
// For a block not found there. Far around the picture's own movement: every
// third step, twenty-four each way (289); and around what the block did in
// the pair before (9).
float2 FarTry(uint c, float2 moves, float2 lately)
{
    if (c < 289) return moves + float2(float(c % 17) - 8.0, float(c / 17) - 8.0) * 3.0 * unit;
    c -= 289;
    return lately + float2(float(c % 3) - 1.0, float(c / 3) - 1.0) * unit;
}

// The same on every other place (to look far around).
float TwixtHalf(float2 centre, float2 v, float2 inv)
{
    float sum = 0.0;
    [loop] for (int k = 0; k < 50; ++k)
    {
        int row = k / 5, column = ((k % 5) << 1) + (row & 1);
        float2 at = centre + (float2(column, row) - 4.5) * 1.6 * unit;
        float3 e = abs(afterSmall.SampleLevel(lin, (at + 0.5 * v) * inv, 0).rgb -
                       beforeSmall.SampleLevel(lin, (at - 0.5 * v) * inv, 0).rgb);
        sum += e.r + e.g + e.b;
    }
    return 2.0 * sum;
}

// How much there is in the block to tell one movement from another: the
// block against itself two steps on.
float Rough(float2 centre, float2 inv)
{
    float sum = 0.0;
    [loop] for (int k = 0; k < 100; ++k)
    {
        float2 at = centre + (float2(k % 10, k / 10) - 4.5) * 1.6 * unit;
        float3 e = abs(afterSmall.SampleLevel(lin, (at + float2(2.0, 1.0) * unit) * inv, 0).rgb - afterSmall.SampleLevel(lin, at * inv, 0).rgb);
        sum += e.r + e.g + e.b;
    }
    return sum;
}

// whereTo: the blocks of the pair before (havePrev). out0: xy the block's
// movement, z how far apart the two real frames still are, w 1 when it was
// found far from the picture's own.
//
// A road under a car at 30 frames a second moves a tenth of the picture's
// width from one frame to the next close to the camera and hardly at all at
// the horizon: nothing like the one movement (or the one growing) the
// picture's own search knows. Looked for only three steps around that, such
// a block was never found - and asphalt, which has little in it, passed for
// "agreed" under any movement: it was built from the two real frames moved
// wrongly, each block its own way. That is the road "turning to jelly". So a
// block that is not found near is looked for far around, and where it was in
// the pair before (a road keeps coming the same way). Only then: a block
// that is found near the picture's own movement is never asked where it was
// before - the floors of a tower block, right after the camera turned round,
// were "found" one floor off where they had been going, and shook again.
[numthreads(8, 8, 1)]
void PairMain(uint3 block : SV_GroupID, uint t : SV_GroupIndex)
{
    float2 size = float2(smallW, smallH), inv = 1.0 / size;
    float2 centre = (float2(block.xy) + 0.5) * size / float2(gridW, gridH);
    float2 moves = Prior(centre * inv);
    float2 lately = moves;
    if (havePrev > 0.5) lately = whereTo.Load(int3(block.xy, 0)).xy;
    uint c;
    // Marks of the screen in the block (a game's read-outs)?
    calmest[t] = MarkShare(centre + (float2(t & 7, t >> 3) - 3.5) * 2.0 * unit) > 0.1 ? 1.0 : 0.0;
    tries[0][t] = t < 58 ? Twixt(centre, PairTry(t, moves), inv) : t == 63 ? Rough(centre, inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    // The picture's own movement wins a tie: it is what the rest of the
    // picture says, and a wall with nothing on it says nothing.
    float best = tries[0][24];
    float2 where = moves;
    [loop] for (c = 0; c < 58; ++c) if (tries[0][c] < best * 0.98) { best = tries[0][c]; where = PairTry(c, moves); }
    // Not found: the two are still as far apart as the block is from itself two steps on.
    bool lost = best > 0.5 * tries[0][63] + 1.5;
    // Not looked for far around with read-outs in it: a row of squares that
    // stands still fits itself one square on, and was taken there - the
    // squares drawn between themselves, the road over where they stand.
    float marked = 0.0;
    [loop] for (c = 0; c < 64; ++c) marked += calmest[c];
    if (marked > 1.5) lost = false;
    float far_best = 1e9, far_which = 0.0;
    if (lost)
    {
        [loop] for (uint j = 0; j < 5; ++j)
        {
            uint n = t + 64 * j;
            if (n >= 298 || (n >= 289 && havePrev < 0.5)) break;
            float2 tried = FarTry(n, moves, lately);
            // The nearer of two that fit as well: what repeats fits a whole row on, too.
            float apart = TwixtHalf(centre, tried, inv) * (1.0 + 0.004 * length(tried - moves) / unit);
            if (apart < far_best) { far_best = apart; far_which = float(n); }
        }
    }
    tries[1][t] = far_best;
    tries[2][t] = far_which;
    GroupMemoryBarrierWithGroupSync();
    bool far = false;
    float2 around = where;
    if (lost)
    {
        float least = 1e9, which = 0.0;
        [loop] for (c = 0; c < 64; ++c) if (tries[1][c] < least) { least = tries[1][c]; which = tries[2][c]; }
        if (least < 0.7 * best) { far = true; around = FarTry(uint(which), moves, lately); }
    }
    if (far) tries[3][t] = t < 49 ? Twixt(centre, around + float2(float(t % 7) - 3.0, float(t / 7) - 3.0) * unit, inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    if (far)
    {
        best = 1e9;
        [loop] for (c = 0; c < 49; ++c) if (tries[3][c] < best) { best = tries[3][c]; where = around + float2(float(c % 7) - 3.0, float(c / 7) - 3.0) * unit; }
    }
    around = where;
    tries[4][t] = t < 25 ? Twixt(centre, MoveTry(t, around, 0.25 * unit), inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    best = tries[4][12];
    [loop] for (c = 0; c < 25; ++c) if (tries[4][c] < best * 0.995) { best = tries[4][c]; where = MoveTry(c, around, 0.25 * unit); }
    if (t != 0) return;
    out0[block.xy] = float4(where, best / (100.0 * 3.0), far ? 1.0 : 0.0);
}

)hlsl" R"hlsl(
// How far apart the two real frames are around a small pixel of the frame
// half way, moved by v: the most of nine places a pixel and a half apart,
// less half the picture's own contrast there. A line thinner than a small
// pixel (a balcony's rail) falls on one row of the small frame in one real
// frame and on two in the other, and the two differ by up to half of it
// with nothing having moved; with a fifth allowed for, every rail of the
// tower block counted as a thing moving, and the generator - wrong there at
// half a floor a frame - kept its say over the whole face. A thing really
// moving differs from what is behind it by all of its contrast.
float Unrest(float2 at, float2 v, float2 inv)
{
    float3 most = 0.0, low = 1e9, high = -1e9, all_after = 0.0, all_before = 0.0;
    [unroll] for (int k = 0; k < 9; ++k)
    {
        float2 o = (float2(k % 3, k / 3) - 1.0) * 1.5;
        float3 a = afterSmall.SampleLevel(lin, (at + o + 0.5 * v) * inv, 0).rgb;
        float3 b = beforeSmall.SampleLevel(lin, (at + o - 0.5 * v) * inv, 0).rgb;
        most = max(most, abs(a - b));
        low = min(low, min(a, b)); high = max(high, max(a, b));
        all_after += a; all_before += b;
    }
    float3 over = most - 0.5 * (high - low);
    // And the nine together (round 10). Half the local contrast is allowed
    // for, because a thin line of the scene falls differently on the small
    // frame's rows in each real frame - but so a thin thing that is not where
    // the scene is passed too: a lamp post against the sky, a palm's frond,
    // rushing past a car, were "agreed" to be the sky behind them, and drawn
    // twice at half strength where the two real frames have them, never where
    // they are in between ("the lower half of the post is gone"; with the
    // second movement of round 9 on top: "worse"). The difference between the
    // two cases: the line of the scene is in both frames' nine places, only
    // spread differently - what the nine hold together is the same. The thing
    // going its own way is in one frame's nine and not in the other's.
    float3 lacks = abs(all_after - all_before) / 9.0;
    float thin = max(lacks.r, max(lacks.g, lacks.b));
    // (From 7 levels of 255 on, wholly from 15: a strong edge found a fifth of a pixel off makes 5.)
    return max(max(over.r, max(over.g, over.b)), 0.03 + (thin - 0.028) * 1.33);
}

// whereTo: PairMain's blocks. out0: the movement and the agreement of every
// small pixel; out1: how much of a tile agrees.
[numthreads(8, 8, 1)]
void SettleMain(uint3 id : SV_DispatchThreadID, uint3 tile : SV_GroupID, uint t : SV_GroupIndex)
{
    float2 inv = 1.0 / float2(smallW, smallH);
    float calm = 1.0;
    if (id.x < (uint)smallW && id.y < (uint)smallH)
    {
        float2 at = float2(id.xy) + 0.5, uv = at * inv;
        // The blocks around, blended: a picture that grows from its middle
        // (driving ahead) moves a little differently in each.
        float2 v = whereTo.SampleLevel(lin, uv, 0).xy;
        // What one of the two real frames does not have - it lies beyond its
        // edge, coming into the picture or leaving it - cannot be compared.
        // Nothing says a thing moves there: it counts as agreed, and is built
        // from the frame that has it (ChooseMain). Counted as unrest, a strip
        // along the edge the picture moves towards was left to the generator,
        // and all the tiles next to it with it.
        float2 size = float2(smallW, smallH), in_after = at + 0.5 * v, in_before = at - 0.5 * v;
        bool beyond = any(in_after < 2.0) || any(in_after > size - 2.0) || any(in_before < 2.0) || any(in_before > size - 2.0);
        float e = beyond ? 0.0 : Unrest(at, v, inv);
        if (e > 0.03)
        {
            // Where a thing ends and another begins they say different
            // things: each by itself.
            float2 grid = uv * float2(gridW, gridH) - 0.5;
            int2 corner = int2(floor(grid)), last = int2(gridW, gridH) - 1;
            [loop] for (int k = 0; k < 4; ++k)
            {
                float2 one = whereTo.Load(int3(clamp(corner + int2(k & 1, k >> 1), int2(0, 0), last), 0)).xy;
                float with = Unrest(at, one, inv);
                if (with < e - 0.005) { e = with; v = one; }
            }
        }
        if (e > 0.03)
        {
            // One of the two real frames has a mark of the screen there, and
            // the other the scene: no thing moved - the scene is what the
            // other shows (ChooseMain).
            float in_one = MarkShare(at + 0.5 * v), in_other = MarkShare(at - 0.5 * v);
            if (abs(in_one - in_other) > 0.06) e = 0.0;
        }
        calm = saturate(1.0 - (e - 0.03) / 0.04);
        out0[id.xy] = float4(v, calm, MarkShare(at));
    }
    calmest[t] = calm;
    GroupMemoryBarrierWithGroupSync();
    if (t != 0) return;
    // How much of the tile agrees. Not "all of it or not": a pixel or two of
    // a game's own noise would then pass for a thing moving, and it is a
    // thing with some size that is looked for here.
    float share = 0.0;
    [loop] for (uint c = 0; c < 64; ++c) share += calmest[c];
    out1[tile.xy] = float4(share / 64.0, 0.0, 0.0, 0.0);
}

// howWell: SettleMain's tiles. out0: the least of a tile and the eight around it.
[numthreads(8, 8, 1)]
void WidenMain(uint3 id : SV_DispatchThreadID)
{
    int2 last = int2((int(smallW) + 7) / 8, (int(smallH) + 7) / 8) - 1;
    if (int(id.x) > last.x || int(id.y) > last.y) return;
    float least = 1.0;
    [unroll] for (int k = 0; k < 9; ++k)
        least = min(least, howWell.Load(int3(clamp(int2(id.xy) + int2(k % 3, k / 3) - 1, int2(0, 0), last), 0)).x);
    out0[id.xy] = float4(least, 0.0, 0.0, 0.0);
}

)hlsl" R"hlsl(
// A thing that goes its own way (rounds 8 and 9: a lamp post beside the road,
// a palm's trunk, a car): thin, fast, in front of a picture that moves
// another way. The generator loses it - a ghost, or nothing: "the lower half
// of the post is gone" - and what the generator loses the rebuild could not
// show. But both real frames have it. Where it stands in each, the two do
// not agree under the picture's own movement (SettleMain): two places of
// unrest, the thing's in the frame before and the thing's in the frame after.
// So for every block with unrest in it: by which movement do its places of
// unrest in the frame before come to lie on places of unrest in the frame
// after that look the same? Looked for 32 small pixels each way, every
// second one, then in thirds of a pixel around the best. A third of the
// block's unrest at least has to go by it, and three places.
//
// Which of the two real frames has the thing at a place of unrest, and which
// the scene behind it, the two frames do not say by themselves: a post
// against the sky going one way is, for them, the same as a piece of sky
// going the other way over something post-coloured. Round 9 took either, as
// it came: half the time the sky was "the thing" and was built over the post
// ("worse than before"). The scene is what goes on beside the place, where
// the two real frames agree: a place votes only when the frame before shows
// there what the calm picture beside it does not show, and the frame after
// what it does.
//
// Looked for 32 small pixels every way; and up to 96 (270 of the picture's)
// the way the picture itself moves there - what stands beside a road goes the
// road's way, only faster the nearer it is.
// settled: SettleMain's. out0: xy the thing's movement, z how many places went by it (0: none).
[numthreads(8, 8, 1)]
void MoverMain(uint3 block : SV_GroupID, uint3 point8 : SV_GroupThreadID, uint t : SV_GroupIndex)
{
    float2 size = float2(smallW, smallH), inv = 1.0 / size;
    float2 centre = (float2(block.xy) + 0.5) * size / float2(gridW, gridH);
    float2 mine = centre + (float2(point8.xy) - 3.5) * 2.0 * unit;
    float4 here = settled.SampleLevel(lin, mine * inv, 0);
    float2 was_at = mine - 0.5 * here.xy;
    float3 before_has = beforeSmall.SampleLevel(lin, was_at * inv, 0).rgb;
    float votes = 0.0;
    if (here.z < 0.5)
    {
        // Not what stands still on the screen: the frame after has that at the very same place.
        float3 stays = abs(afterSmall.SampleLevel(lin, was_at * inv, 0).rgb - before_has);
        if (max(stays.r, max(stays.g, stays.b)) >= 0.02)
        {
            float3 after_has = afterSmall.SampleLevel(lin, (mine + 0.5 * here.xy) * inv, 0).rgb;
            bool thing_before = true, scene_after = false;
            [unroll] for (int side = 0; side < 4; ++side)
            {
                float2 beside = mine + float2(side == 0 ? -4.0 : side == 1 ? 4.0 : 0.0, side == 2 ? -4.0 : side == 3 ? 4.0 : 0.0);
                float4 there = settled.SampleLevel(lin, beside * inv, 0);
                if (there.z >= 0.7)
                {
                    float3 scene = 0.5 * (beforeSmall.SampleLevel(lin, (beside - 0.5 * there.xy) * inv, 0).rgb +
                                          afterSmall.SampleLevel(lin, (beside + 0.5 * there.xy) * inv, 0).rgb);
                    float3 like_before = abs(scene - before_has), like_after = abs(scene - after_has);
                    if (max(like_before.r, max(like_before.g, like_before.b)) < 0.07) thing_before = false;
                    if (max(like_after.r, max(like_after.g, like_after.b)) < 0.07) scene_after = true;
                }
            }
            votes = thing_before && scene_after ? 1.0 : 0.0;
        }
    }
    held[t] = before_has;
    calmest[t] = votes;
    tries[5][t] = here.x;
    tries[6][t] = here.y;
    GroupMemoryBarrierWithGroupSync();
    uint c, k;
    float voters = 0.0;
    [loop] for (c = 0; c < 64; ++c) voters += calmest[c];
    float best = 0.0, which = -1.0;
    if (voters >= 3.0)
    {
        float2 goes = settled.SampleLevel(lin, centre * inv, 0).xy;
        float speed = length(goes);
        [loop] for (uint j = 0; j < 148; ++j)
        {
            uint n = t + 64 * j;
            if (n >= 9409) break;
            float2 beyond = float2(float(n % 97) - 48.0, float(n / 97) - 48.0) * 2.0;
            float far2 = dot(beyond, beyond);
            if (far2 < 12.0) continue;       // within three pixels of the picture's own: not a thing by itself
            if (far2 > 1024.0 && (speed < 1.0 || dot(beyond, goes) < 0.7 * sqrt(far2) * speed)) continue;
            float score = 0.0;
            [loop] for (k = 0; k < 64; ++k)
            {
                if (calmest[k] < 0.5) continue;
                float2 at = centre + (float2(k & 7, k >> 3) - 3.5) * 2.0 * unit;
                float2 v = float2(tries[5][k], tries[6][k]);
                float3 off = abs(afterSmall.SampleLevel(lin, (at + 0.5 * v + beyond) * inv, 0).rgb - held[k]);
                if (max(off.r, max(off.g, off.b)) < 0.06 && settled.SampleLevel(lin, (at + beyond) * inv, 0).z < 0.5) score += 1.0;
            }
            // The nearer of two that as many go by.
            score -= 0.001 * sqrt(far2);
            if (score > best) { best = score; which = float(n); }
        }
    }
    tries[0][t] = best;
    tries[1][t] = which;
    GroupMemoryBarrierWithGroupSync();
    float most = 0.0;
    which = -1.0;
    [loop] for (c = 0; c < 64; ++c) if (tries[0][c] > most) { most = tries[0][c]; which = tries[1][c]; }
    // Half of the places that vote have to go by it, and three.
    bool found = which >= 0.0 && most >= max(2.8, 0.5 * voters);
    float2 beyond = float2(0.0, 0.0);
    if (found) beyond = float2(float(uint(which) % 97) - 48.0, float(uint(which) / 97) - 48.0) * 2.0;
    // In thirds of a pixel around it: how alike, all its places together.
    float fine = -1.0;
    if (found && t < 49)
    {
        float2 tried = beyond + float2(float(t % 7) - 3.0, float(t / 7) - 3.0) / 3.0;
        fine = 0.0;
        [loop] for (k = 0; k < 64; ++k)
        {
            if (calmest[k] < 0.5) continue;
            float2 at = centre + (float2(k & 7, k >> 3) - 3.5) * 2.0 * unit;
            float2 v = float2(tries[5][k], tries[6][k]);
            float3 off = abs(afterSmall.SampleLevel(lin, (at + 0.5 * v + tried) * inv, 0).rgb - held[k]);
            fine += saturate(1.0 - max(off.r, max(off.g, off.b)) / 0.06);
        }
    }
    tries[2][t] = fine;
    GroupMemoryBarrierWithGroupSync();
    if (t != 0) return;
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    if (found)
    {
        float finest = -1.0;
        float2 by = beyond;
        [loop] for (c = 0; c < 49; ++c)
            if (tries[2][c] > finest) { finest = tries[2][c]; by = beyond + float2(float(c % 7) - 3.0, float(c / 7) - 3.0) / 3.0; }
        // What stands still on the screen (a game's read-outs, a weapon held) is not this pass's: shown as the
        // real frame has it, exactly, by the still tiles and the marks - a third of a pixel off is off.
        float2 goes = settled.SampleLevel(lin, centre * inv, 0).xy + by;
        if (dot(goes, goes) >= 1.0) result = float4(goes, most, 0.0);
    }
    out0[block.xy] = result;
}

// A thing found in the block the frame before has it in is, in a frame in
// between, some way along its way to where the frame after has it - up to
// four blocks on. So each thing's way is laid out: a block takes the thing
// (the one most places went by) whose way passes within a block of it.
// whereTo: MoverMain's blocks. out0: the same, for every block on a thing's way.
[numthreads(8, 8, 1)]
void SpreadMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gridW || id.y >= gridH) return;
    float2 size = float2(smallW, smallH), inv = 1.0 / size, cell = size / float2(gridW, gridH);
    float2 centre = (float2(id.xy) + 0.5) * cell;
    int2 last = int2(gridW, gridH) - 1;
    float4 taken = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (int n = 0; n < 121; ++n)
    {
        int2 from = int2(id.xy) + int2(n % 11, n / 11) - 5;
        if (any(from < 0) || any(from > last)) continue;
        float4 one = whereTo.Load(int3(from, 0));
        if (one.z <= taken.z) continue;
        // Its way: from where the frame before has it to where the frame after has it.
        float2 start = (float2(from) + 0.5) * cell;
        start -= 0.5 * settled.SampleLevel(lin, start * inv, 0).xy;
        float along = saturate(dot(centre - start, one.xy) / max(dot(one.xy, one.xy), 1e-3));
        float2 off = centre - (start + along * one.xy);
        if (abs(off.x) <= cell.x && abs(off.y) <= cell.y) taken = one;
    }
    out0[id.xy] = taken;
}

// The pair's movement at a place of a frame in between (small pixels): the
// frame half way holds it, and what is here now is there a little further on
// or back along its own way.
float4 SettledAt(float2 at, float2 inv)
{
    float2 first = settled.SampleLevel(lin, at * inv, 0).xy;
    return settled.SampleLevel(lin, (at + (0.5 - phase) * first) * inv, 0);
}
float WideAt(float2 at, float2 v)
{
    float2 tiles = float2((int(smallW) + 7) / 8, (int(smallH) + 7) / 8) * 8.0;
    // Calm far around: 19 in 20 of the least calm tile near agree; not from 3 in 4 down.
    return saturate((wideCalm.SampleLevel(lin, (at + (0.5 - phase) * v) / tiles, 0).x - 0.75) / 0.2);
}

)hlsl" R"hlsl(
)hlsl" R"hlsl(
// Is a place of this frame in between a thing's that goes its own way
// (MoverMain's blocks around)? Then both real frames have it, moved by the
// thing's own movement - the same colours, and at both ends a place the
// picture's own movement v does not explain. xy: that movement; z: 1 when it
// is. any: a thing goes its own way somewhere near.
float3 MoverAt(float2 at, float2 v, float2 inv, out float any)
{
    int2 last = int2(gridW, gridH) - 1;
    int2 home = clamp(int2(at * inv * float2(gridW, gridH)), int2(0, 0), last);
    float3 found = float3(0.0, 0.0, 0.0);
    float nearest = 0.05;
    any = 0.0;
    [loop] for (int k = 0; k < 9; ++k)
    {
        float4 one = movers.Load(int3(clamp(home + int2(k % 3, k / 3) - 1, int2(0, 0), last), 0));
        if (one.z <= 0.0) continue;
        any = 1.0;
        float2 in_after = at + (1.0 - phase) * one.xy, in_before = at - phase * one.xy;
        float3 off = abs(afterSmall.SampleLevel(lin, in_after * inv, 0).rgb - beforeSmall.SampleLevel(lin, in_before * inv, 0).rgb);
        float apart = max(off.r, max(off.g, off.b));
        if (apart >= nearest) continue;
        // Not what stands still on the screen: the frame after has that where the frame before has it.
        float3 stays = abs(afterSmall.SampleLevel(lin, in_before * inv, 0).rgb - beforeSmall.SampleLevel(lin, in_before * inv, 0).rgb);
        if (max(stays.r, max(stays.g, stays.b)) < 0.02) continue;
        if (settled.SampleLevel(lin, (in_after - 0.5 * v) * inv, 0).z < 0.5 && settled.SampleLevel(lin, (in_before + 0.5 * v) * inv, 0).z < 0.5)
        { nearest = apart; found = float3(one.xy, 1.0); }
    }
    return found;
}

// A place the two real frames do not agree about, the picture's movement v:
// does one of them show there a thing that goes its own way - which is
// elsewhere at this frame's moment? 1: the frame after does (the scene is what
// the frame before shows), 2: the frame before does, 0: neither, or both.
int MoverCovers(float2 at, float2 v, float2 inv)
{
    int2 last = int2(gridW, gridH) - 1;
    int2 home = clamp(int2(at * inv * float2(gridW, gridH)), int2(0, 0), last);
    float2 in_after = at + (1.0 - phase) * v, in_before = at - phase * v;
    float3 after_has = afterSmall.SampleLevel(lin, in_after * inv, 0).rgb, before_has = beforeSmall.SampleLevel(lin, in_before * inv, 0).rgb;
    // What stands at such a place in both frames alike (a game's read-out) is no thing that came or went.
    float3 stays_after = abs(beforeSmall.SampleLevel(lin, in_after * inv, 0).rgb - after_has);
    float3 stays_before = abs(afterSmall.SampleLevel(lin, in_before * inv, 0).rgb - before_has);
    bool after_came = max(stays_after.r, max(stays_after.g, stays_after.b)) >= 0.02;
    bool before_went = max(stays_before.r, max(stays_before.g, stays_before.b)) >= 0.02;
    bool after_is = false, before_is = false;
    [loop] for (int k = 0; k < 9; ++k)
    {
        float4 one = movers.Load(int3(clamp(home + int2(k % 3, k / 3) - 1, int2(0, 0), last), 0));
        if (one.z <= 0.0) continue;
        // The thing in the frame after came from there in the frame before; the one in the frame before went there.
        float3 came = abs(beforeSmall.SampleLevel(lin, (in_after - one.xy) * inv, 0).rgb - after_has);
        float3 went = abs(afterSmall.SampleLevel(lin, (in_before + one.xy) * inv, 0).rgb - before_has);
        if (after_came && max(came.r, max(came.g, came.b)) < 0.05 && settled.SampleLevel(lin, (in_after - one.xy + 0.5 * v) * inv, 0).z < 0.5) after_is = true;
        if (before_went && max(went.r, max(went.g, went.b)) < 0.05 && settled.SampleLevel(lin, (in_before + one.xy - 0.5 * v) * inv, 0).z < 0.5) before_is = true;
    }
    return after_is == before_is ? 0 : after_is ? 1 : 2;
}

// ---- every frame in between ----

// Where each block lies in the two real frames: around "not moved" first
// (what stays put wins a tie), then around where the picture as a whole went,
// further out when neither found it, then in quarter steps around the best of
// those - a line two pixels wide of the picture is half a pixel of the small
// frame, and its detail put back a pixel off is a line beside a line. In the
// frame before, next to where the frame after's answer turned round says
// (what lies ahead in the frame after lies as far behind, by the frame's
// place between the two, in the frame before). Beside each: how far apart
// they still are.
[numthreads(8, 8, 1)]
void FollowMain(uint3 block : SV_GroupID, uint3 point8 : SV_GroupThreadID, uint t : SV_GroupIndex)
{
    float2 size = float2(smallW, smallH), inv = 1.0 / size;
    float2 centre = (float2(block.xy) + 0.5) * size / float2(gridW, gridH);
    float2 mine = centre + (float2(point8.xy) - 3.5) * 2.0 * unit;
    held[t] = made.SampleLevel(lin, mine * inv, 0).rgb;
    uint c;
    // A block the two real frames agree about all over - with all around it
    // agreeing too, or with the generator's frame showing what they show -
    // is theirs: nothing of it is looked for in the generator's frame. (No
    // step of the search is left by a thread on its own: they all wait for
    // each other at every one.)
    bool theirs = false;
#if REPAIR
    float4 pair = SettledAt(mine, inv);
    // (What the two show at the generator's frame's own moment: it is that frame they are held against.)
    float3 between = lerp(beforeSmall.SampleLevel(lin, (mine - phaseMade * pair.xy) * inv, 0).rgb,
                          afterSmall.SampleLevel(lin, (mine + (1.0 - phaseMade) * pair.xy) * inv, 0).rgb, phaseMade);
    float3 other = abs(held[t] - between);
    calmest[t] = pair.z;
    tries[5][t] = WideAt(mine, pair.xy);
    tries[6][t] = max(other.r, max(other.g, other.b));
    GroupMemoryBarrierWithGroupSync();
    float calm = 1.0, wide = 1.0, apart = 0.0;
    [loop] for (c = 0; c < 64; ++c) { calm = min(calm, calmest[c]); wide = min(wide, tries[5][c]); apart = max(apart, tries[6][c]); }
    theirs = calm > 0.9 && (wide > 0.9 || apart < 0.05);
#else
    GroupMemoryBarrierWithGroupSync();
#endif
    float2 guess = (1.0 - phaseMade) * Prior(centre * inv);

    // The frame after.
    if (!theirs) tries[0][t] = t < 58 ? Apart(afterSmall, centre, FirstTry(t, guess), inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    float best = 1e9;
    float2 where = float2(0.0, 0.0);
    [loop] for (c = 0; c < 58; ++c) if (tries[0][c] < best) { best = tries[0][c]; where = FirstTry(c, guess); }
    // Found badly: a thing moving its own way, not with the picture. Only
    // then is it looked for further out (it was always, and four times in
    // ten the answer was not needed).
    bool badly = !theirs && best > 0.03 * 64.0 * 3.0, far = false;
    if (badly)
    {
        tries[1][t] = Apart(afterSmall, centre, WideTry(t), inv);
        tries[2][t] = t + 64 < 81 ? Apart(afterSmall, centre, WideTry(t + 64), inv) : 1e9;
    }
    GroupMemoryBarrierWithGroupSync();
    if (badly)
    {
        [loop] for (c = 0; c < 64; ++c) if (tries[1][c] < best) { best = tries[1][c]; where = WideTry(c); far = true; }
        [loop] for (c = 0; c < 17; ++c) if (tries[2][c] < best) { best = tries[2][c]; where = WideTry(c + 64); far = true; }
    }
    float2 around = where;
    if (far) tries[3][t] = t < 9 ? Apart(afterSmall, centre, NextTry(t, around), inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    if (far)
        [loop] for (c = 0; c < 9; ++c) if (tries[3][c] < best) { best = tries[3][c]; where = NextTry(c, around); }
    around = where;
    if (!theirs) tries[4][t] = t < 25 ? Apart(afterSmall, centre, FineTry(t, around), inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    [loop] for (c = 0; c < 25; ++c) if (tries[4][c] < best) { best = tries[4][c]; where = FineTry(c, around); }
    float3 after = float3(where, best / (64.0 * 3.0));

    // The frame before.
    float2 back = after.xy * (-phaseMade / max(1.0 - phaseMade, 0.1));
    if (!theirs) tries[5][t] = t == 0 ? Apart(beforeSmall, centre, float2(0.0, 0.0), inv) : t < 10 ? Apart(beforeSmall, centre, NextTry(t - 1, back), inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    best = 1e9;
    where = float2(0.0, 0.0);
    [loop] for (c = 0; c < 10; ++c) if (tries[5][c] < best) { best = tries[5][c]; where = c == 0 ? float2(0.0, 0.0) : NextTry(c - 1, back); }
    around = where;
    if (!theirs) tries[6][t] = t < 25 ? Apart(beforeSmall, centre, FineTry(t, around), inv) : 1e9;
    GroupMemoryBarrierWithGroupSync();
    [loop] for (c = 0; c < 25; ++c) if (tries[6][c] < best) { best = tries[6][c]; where = FineTry(c, around); }
    float3 before = float3(where, best / (64.0 * 3.0));
    if (t != 0) return;
#if REPAIR
    if (theirs)
    {
        float2 goes = SettledAt(centre, inv).xy;
        out0[block.xy] = float4((1.0 - phase) * goes, -phase * goes);
        out1[block.xy] = float4(1.0, 1.0, 0.0, 0.0);
        return;
    }
#endif
    out0[block.xy] = float4(after.xy, before.xy);
    out1[block.xy] = float4(saturate(1.0 - (after.z - 0.012) / 0.05), saturate(1.0 - (before.z - 0.012) / 0.05), 0.0, 0.0);
}

)hlsl" R"hlsl(
// Where a small pixel of the frame in between lies in the two real frames.
//
// What they agree about, moved by the pair's own movement (SettleMain), is
// theirs: the pixel goes by that movement, and is built from them. All
// around a thing that goes its own way - a car crossing: the tiles near it do
// not all agree - only where the generator's frame shows what they show: it
// may have the car here, between the two places they have it at.
//
// Else by the generator's frame: each of the four blocks around the pixel
// says where it went itself, and where a thing ends and the picture behind it
// begins they say different things - the pixel takes the one its own colour
// agrees with (mixed, as between two blocks of one thing, it would be nowhere
// either of them went). A block found badly as a whole still has pixels that
// agree: they count for less. In the real frame before: the blocks' answers
// mixed (it counts for less and is asked for less).
[numthreads(8, 8, 1)]
void ChooseMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)smallW || id.y >= (uint)smallH) return;
    float2 inv = 1.0 / float2(smallW, smallH);
    float2 uv = (float2(id.xy) + 0.5) * inv;
    float3 soft = made.Load(int3(id.xy, 0)).rgb;
#if REPAIR
    float2 at = float2(id.xy) + 0.5;
    float4 pair = SettledAt(at, inv);
    // A thing that goes its own way, here at this frame's moment: built from
    // the two real frames by its own movement.
    float movers_near = 0.0;
    float3 mover = MoverAt(at, pair.xy, inv, movers_near);
    if (mover.z > 0.5)
    {
        out0[id.xy] = float4((1.0 - phase) * mover.xy, -phase * mover.xy);
        out1[id.xy] = float4(1.0, 1.0, 1.0, 0.0);
        return;
    }
    // Where it stood, or will stand: the scene behind it, from the real frame that shows it.
    if (pair.z <= 0.5 && movers_near > 0.5)
    {
        int covers = MoverCovers(at, pair.xy, inv);
        if (covers != 0)
        {
            out0[id.xy] = float4((1.0 - phase) * pair.xy, -phase * pair.xy);
            out1[id.xy] = float4(covers == 1 ? 0.0 : 1.0, covers == 2 ? 0.0 : 1.0, 1.0, 0.0);
            return;
        }
    }
    if (pair.z > 0.5)
    {
        // Does each of the two real frames have this place, or does it lie beyond its edge?
        float2 size = float2(smallW, smallH), in_after = at + (1.0 - phase) * pair.xy, in_before = at - phase * pair.xy;
        bool has_after = all(in_after > 0.5) && all(in_after < size - 0.5), has_before = all(in_before > 0.5) && all(in_before < size - 0.5);
        // Nor does it have the scene there where a mark of the screen covers it.
        float mark_after = settled.SampleLevel(lin, in_after * inv, 0).w, mark_before = settled.SampleLevel(lin, in_before * inv, 0).w;
        if (mark_after > mark_before + 0.06) has_after = false;
        else if (mark_before > mark_after + 0.06) has_before = false;
        float3 a = afterSmall.SampleLevel(lin, in_after * inv, 0).rgb;
        float3 b = beforeSmall.SampleLevel(lin, in_before * inv, 0).rgb;
        float3 shown = has_after && has_before ? lerp(b, a, phase) : has_after ? a : b;
        float wide = WideAt(at, pair.xy);
        // The generator's say, where it has one (near a thing that moves its
        // own way): does its frame show here what the two real frames show -
        // at its frame's own moment, when that is not this frame's. Else the
        // picture all around such a thing was taken from the generator's
        // frame, a sixth of a step from where this frame has it.
        float3 then = shown;
        if (abs(phaseMade - phase) > 0.01)
            then = lerp(beforeSmall.SampleLevel(lin, (at - phaseMade * pair.xy) * inv, 0).rgb,
                        afterSmall.SampleLevel(lin, (at + (1.0 - phaseMade) * pair.xy) * inv, 0).rgb, phaseMade);
        float says = Alike(soft, then);
        float theirs = pair.z * max(wide, says);
        // Calm far around, and the generator's frame shows something else
        // here (round 10). Two things it can be. A thing of the scene it put a
        // little beside its place - a dot on a wall, a floor one row off: then
        // what it shows is in the two real frames too, close by, and they are
        // right. Or a thing on its way across - a lamp post rushing past a
        // car, a palm's trunk: fast enough for its two places in the real
        // frames to lie far to either side, so that all is calm where it is
        // now. Taken for the first, the post's middle was painted over with
        // the sky ("the lower half of the post is gone"). What the generator
        // shows here and the two real frames show nowhere near is such a
        // thing, and stays the generator's.
        if (wide >= 0.5 && says < 0.5)
        {
            bool near_by = false;
            [loop] for (int n = 0; n < 16 && !near_by; ++n)
            {
                float turn = float(n & 7) * 0.7853982;
                float2 o = float2(cos(turn), sin(turn)) * (n < 8 ? 4.0 : 8.0);
                // A stationary feature must exist in BOTH aligned endpoints.
                // Finding a passing pole in only the nearer endpoint does not
                // make it part of the sky being reconstructed here.
                float3 old = beforeSmall.SampleLevel(lin, (at + o - phaseMade * pair.xy) * inv, 0).rgb;
                float3 now = afterSmall.SampleLevel(lin, (at + o + (1.0 - phaseMade) * pair.xy) * inv, 0).rgb;
                float3 off = max(abs(old - soft), abs(now - soft));
                if (max(off.r, max(off.g, off.b)) < 0.08) near_by = true;
            }
            if (!near_by) theirs = 0.0;
        }
        if (theirs > 0.5 && (has_after || has_before))
        {
            out0[id.xy] = float4((1.0 - phase) * pair.xy, -phase * pair.xy);
            out1[id.xy] = float4(has_after ? 1.0 : 0.0, has_before ? 1.0 : 0.0, theirs, 0.0);
            return;
        }
    }
#endif
    float2 grid = uv * float2(gridW, gridH) - 0.5;
    int2 corner = int2(floor(grid)), last = int2(gridW, gridH) - 1;
    float2 part = grid - float2(corner);
    float4 to = float4(0.0, 0.0, 0.0, 0.0);
    float believed = 0.0, agreed = -1.0;
    float2 mixed = whereTo.SampleLevel(lin, uv, 0).zw;
    [unroll] for (int k = 0; k < 4; ++k)
    {
        int2 step = int2(k & 1, k >> 1);
        int2 block = clamp(corner + step, int2(0, 0), last);
        float4 went = whereTo.Load(int3(block, 0));
        float4 well = howWell.Load(int3(block, 0));
        float3 colour = afterSmall.SampleLevel(lin, uv + went.xy * inv, 0).rgb;
        float2 share = lerp(1.0 - part, part, float2(step));
        float near_it = 0.8 + 0.2 * share.x * share.y;
        float trust = lerp(0.35, 1.0, well.x) * near_it;
        float agrees = Alike(colour, soft) * trust;
        if (agrees > agreed) { agreed = agrees; to = float4(went.xy, mixed); believed = trust; }
    }
    out0[id.xy] = to;
    out1[id.xy] = float4(believed, lerp(0.35, 1.0, howWell.SampleLevel(lin, uv, 0).y), 0.0, 0.0);
}

#endif      // DRAW

// The small frame scaled up without its steps showing (Catmull-Rom, nine
// taps): what is shown where no real frame gave its detail.
float3 Smooth(float2 uv)
{
    float2 size = float2(smallW, smallH), inv = 1.0 / size;
    float2 p = uv * size - 0.5;
    float2 b = floor(p), f = p - b, f2 = f * f, f3 = f2 * f;
    float2 a0 = -0.5 * f + f2 - 0.5 * f3;
    float2 a1 = 1.0 - 2.5 * f2 + 1.5 * f3;
    float2 a2 = 0.5 * f + 2.0 * f2 - 1.5 * f3;
    float2 a3 = -0.5 * f2 + 0.5 * f3, a12 = a1 + a2;
    float2 p0 = b - 0.5, p12 = b + 0.5 + a2 / a12, p3 = b + 2.5;
    float xs[3] = {p0.x, p12.x, p3.x}, ys[3] = {p0.y, p12.y, p3.y};
    float wx[3] = {a0.x, a12.x, a3.x}, wy[3] = {a0.y, a12.y, a3.y};
    float3 c = 0.0;
    [unroll] for (int y = 0; y < 3; ++y) [unroll] for (int x = 0; x < 3; ++x)
        c += made.SampleLevel(lin, float2(xs[x], ys[y]) * inv, 0).rgb * wx[x] * wy[y];
    return max(c, 0.0);
}

// A pixel of the frame at the picture's size.
float4 Built(uint2 xy)
{
    float2 uv = (float2(xy) + 0.5) / float2(outW, outH);
    float near = moved.SampleLevel(lin, uv, 0);
    float3 own = afterFull.Load(int3(xy, 0)).rgb;
    float3 soft = made.SampleLevel(lin, uv, 0).rgb;
    // The tile map compares endpoints only: a fast, thin object can cross
    // a calm tile between them. Preserve ChooseMain's transit decision here
    // instead of painting the generated pole over with the real frame's sky.
    if (near < 1.0)
    {
        float3 local = afterSmall.SampleLevel(lin, uv, 0).rgb;
        float3 gap = abs(soft - local);
        int3 cell = int3(min(int2(uv * float2(smallW, smallH)), int2(smallW, smallH) - 1), 0);
        if (max(gap.r, max(gap.g, gap.b)) > 0.04 && howWell.Load(cell).z < 0.5) near = 1.0;
    }
    if (near <= 0.004) return float4(own, 1.0);
    float2 inv = 1.0 / float2(smallW, smallH);
    // Where this pixel lies in the two real frames: what was chosen for the
    // small pixel it lies in (ChooseMain). Does the real frame, there, look
    // like the frame in between here? The nearer real frame first.
    int3 cell = int3(min(int2(uv * float2(smallW, smallH)), int2(smallW, smallH) - 1), 0);
    float4 to = whereTo.Load(cell);
    float4 believed = howWell.Load(cell);
    float2 at_after = uv + to.xy * inv, at_before = uv + to.zw * inv;
    float3 small_after = afterSmall.SampleLevel(lin, at_after, 0).rgb;
    float3 small_before = beforeSmall.SampleLevel(lin, at_before, 0).rgb;
    float3 small_here = afterSmall.SampleLevel(lin, uv, 0).rgb;
    // Built from the real frames alone (ChooseMain): what the two show
    // between them takes the generator's place - from one small pixel to the
    // next without a step, and not where the two differ at this very pixel
    // after all (twelve levels of 255 a colour and more).
    float3 gap = abs(small_after - small_before);
    float agree = saturate(1.0 - (max(gap.r, max(gap.g, gap.b)) - 0.05) / 0.12);
    float3 theirs = lerp(small_before, small_after, phase);
    // Only one of the two has this place (the other's edge lies before it): that one alone.
    if (believed.x < 0.25) { theirs = small_before; agree = 1.0; }
    else if (believed.y < 0.25) { theirs = small_after; agree = 1.0; }
    float from_real = howWell.SampleLevel(lin, uv, 0).z * agree;
    soft = lerp(soft, theirs, from_real);
    float w_after = Alike(small_after, soft) * believed.x * lerp(0.6, 1.0, phase);
    float w_before = Alike(small_before, soft) * believed.y * lerp(1.0, 0.6, phase);
    // And the real frame right here, not moved at all: a HUD's letters over a
    // picture that moves under them lie in a block that went with the picture.
    // Not where the two real frames agree about a movement: there the frame
    // not moved is the wrong place to look (beside a dot on a bare wall its
    // small copy still looks like the wall, and what it "lacks" was drawn
    // around the dot as a pale ring).
    float w_here = Alike(small_here, soft);
    w_here *= w_here * (1.0 - from_real);
    float best = max(w_here, max(w_after, w_before));
    float3 built = soft;
    if (best > 0.02)
    {
        // The detail of the one that agrees best, nearly alone: two that
        // agree about as well still lie a pixel apart, and their details
        // mixed are a thin line twice, each at half strength.
        float3 share = float3(w_here, w_after, w_before) / best;
        share *= share; share *= share; share *= share;
        float3 detail = share.x * (own - small_here);
        if (share.y > 0.001) detail += share.y * (afterFull.SampleLevel(lin, at_after, 0).rgb - small_after);
        if (share.z > 0.001) detail += share.z * (beforeFull.SampleLevel(lin, at_before, 0).rgb - small_before);
        built += detail / (share.x + share.y + share.z) * saturate(best * 1.25);
    }
    // Nothing agreed: at least not in steps.
    if (best < 0.5) built = lerp(lerp(Smooth(uv), soft, from_real), built, best * 2.0);
#if REPAIR
    // A mark that stands on the screen (StillMain), with marks beside it - a
    // line of them, not a pixel that happens to be the same in both frames:
    // as the real frame has it.
    if (stillMark.Load(int3(xy, 0)) > 0.5)
    {
        float2 half_pixel = 0.5 / float2(outW, outH);
        float beside = stillMark.SampleLevel(lin, uv + float2(-half_pixel.x, -half_pixel.y), 0) + stillMark.SampleLevel(lin, uv + float2(half_pixel.x, -half_pixel.y), 0) +
                       stillMark.SampleLevel(lin, uv + float2(-half_pixel.x, half_pixel.y), 0) + stillMark.SampleLevel(lin, uv + float2(half_pixel.x, half_pixel.y), 0);
        if (beside >= 1.9) built = own;
    }
#endif
#if SHOW
    // SM_SHOW_THEIRS=1 (to look at): red where the generator has the say, not the two real frames.
    built = lerp(built, float3(1.0, 0.0, 0.0), 0.6 * (1.0 - howWell.SampleLevel(lin, uv, 0).z));
#endif
    return float4(lerp(own, built, smoothstep(0.0, 1.0, near)), 1.0);
}

#ifndef DRAW
// Into a picture of its own (the presenter copies or scales it onto the back buffer).
[numthreads(8, 8, 1)]
void BuildMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    out0[id.xy] = Built(id.xy);
}
#else
// Straight onto the back buffer, when that is the picture's own size: one
// triangle over all of it. The copy of 3.7 million pixels that followed the
// compute pass, five times a pair at x6, is not made.
float4 DrawVS(uint n : SV_VertexID) : SV_Position
{
    return float4(n == 1 ? 3.0 : -1.0, n == 2 ? -3.0 : 1.0, 0.0, 1.0);
}
float4 DrawPS(float4 at : SV_Position) : SV_Target
{
    return Built(uint2(at.xy));
}
#endif
)hlsl";

static bool RebuildPipelines()
{
    RebuildState &r = g_rebuild;
    if (r.follow && r.choose && r.build && r.pair && r.settle && r.widen && r.still && r.mover && r.spread) return true;
    if (r.failed) return false;
    auto fail = [&](const char *what) { r.failed = true; Trace("[dlssg] small frames in between stay as they are scaled: %s", what); return false; };
    // SM_NO_REPAIR=1 (to compare): everything from the generator's frame, right or wrong.
    static const bool no_repair = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_NO_REPAIR", v, sizeof(v)) > 0 && v[0] == '1'; }();
    r.repairs = !no_repair;
    // Off unless asked for (SM_MOVERS=1), since round 10: on a road in the open - a sign pole against the sky,
    // which the generator's own frame has whole - the second movement took the pole away and tore its plate
    // (testsjelly.ps1 -Mode drive; the report from GTA V: "worse than before"). Why is not found yet.
    static const bool movers = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_MOVERS", v, sizeof(v)) > 0 && v[0] == '1'; }();
    r.movers_on = movers;
    static const bool show = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_SHOW_THEIRS", v, sizeof(v)) > 0 && v[0] == '1'; }();
    const std::string text = std::string(no_repair ? "#define REPAIR 0\n" : "") + (show ? "#define SHOW 1\n" : "") + kRebuildHlsl;
    winrt::com_ptr<ID3DBlob> signature, errors;
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 12, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0}};
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 60;
    for (UINT i = 0; i < 2; ++i)
    {
        params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[i + 1].DescriptorTable = {1, &ranges[i]};
    }
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC desc = {3, params, 1, &sampler};
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
        FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                  IID_PPV_ARGS(r.root.put())))) return fail("its root signature");
    struct Pass { const char *entry; winrt::com_ptr<ID3D12PipelineState> *to; const char *what; };
    const Pass passes[] = {
        {"FollowMain", &r.follow, "the follow pass"}, {"ChooseMain", &r.choose, "the choose pass"}, {"BuildMain", &r.build, "the build pass"},
        {"PairMain", &r.pair, "the pair pass"}, {"SettleMain", &r.settle, "the settle pass"}, {"WidenMain", &r.widen, "the widen pass"},
        {"StillMain", &r.still, "the still pass"}, {"MoverMain", &r.mover, "the mover pass"},
        {"SpreadMain", &r.spread, "the spread pass"}};
    for (const Pass &pass : passes)
    {
        winrt::com_ptr<ID3DBlob> code;
        errors = nullptr;
        if (FAILED(CachedCompile(text.data(), text.size(), "sm-rebuild.hlsl", nullptr, nullptr, pass.entry, "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put())))
            return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : pass.what);
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = r.root.get();
        pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pass.to->put())))) return fail(pass.what);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4 * 9 * 14, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(r.heap.put())))) return fail("its descriptor heap");
    return true;
}

// The build pass drawn straight onto a back buffer of this format. Made by
// the presenter the first time it has such a buffer in hand; false: the
// compute pass and a copy, as before. SM_NO_DIRECT=1 (to compare): never.
static bool RebuildDrawReady(DXGI_FORMAT format)
{
    RebuildState &r = g_rebuild;
    static const bool never = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_NO_DIRECT", v, sizeof(v)) > 0 && v[0] == '1'; }();
    if (never || r.draw_failed || !r.root) return false;
    if (r.draw && r.draw_format == format) return true;
    if (format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_B8G8R8A8_UNORM && format != DXGI_FORMAT_R10G10B10A2_UNORM &&
        format != DXGI_FORMAT_R16G16B16A16_FLOAT) return false;
    auto fail = [&](const char *what) { r.draw_failed = true; r.draw = nullptr; Trace("[dlssg] frames in between are built beside the back buffer and copied onto it: %s", what); return false; };
    const std::string text = std::string("#define DRAW 1\n") + (r.repairs ? "" : "#define REPAIR 0\n") + kRebuildHlsl;
    winrt::com_ptr<ID3DBlob> vs, ps, errors;
    if (FAILED(CachedCompile(text.data(), text.size(), "sm-rebuild-draw.hlsl", nullptr, nullptr, "DrawVS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs.put(), errors.put())))
        return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "its vertex shader");
    errors = nullptr;
    if (FAILED(CachedCompile(text.data(), text.size(), "sm-rebuild-draw.hlsl", nullptr, nullptr, "DrawPS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps.put(), errors.put())))
        return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "its pixel shader");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = r.root.get();
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    r.draw = nullptr;
    if (FAILED(lfCore.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(r.draw.put())))) return fail("its pipeline");
    if (!r.draw_view)
    {
        D3D12_DESCRIPTOR_HEAP_DESC view = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        if (FAILED(lfCore.device->CreateDescriptorHeap(&view, IID_PPV_ARGS(r.draw_view.put())))) return fail("its view");
    }
    r.draw_format = format;
    Trace("[dlssg] frames in between are built straight onto the back buffer");
    return true;
}

// For frames in between of small_w x small_h, a picture of full_w x full_h. Called by EnsureFg with the
// device idle; the textures leave it in the states they rest in.
static bool RebuildReady(UINT small_w, UINT small_h, UINT full_w, UINT full_h)
{
    RebuildState &r = g_rebuild;
    RebuildClose();
    // SM_NO_REBUILD=1 (to compare): small frames in between scaled as they are.
    static const bool plain = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_NO_REBUILD", v, sizeof(v)) > 0 && v[0] == '1'; }();
    if (plain || !RebuildPipelines()) return false;
    // A step of the search is a 640th of the width: a block is the same share
    // of the picture whatever size the frames are made at.
    r.unit = std::max(1.0f, small_w / 640.0f);
    r.grid_w = std::max(8u, static_cast<UINT>(std::ceil(small_w / (16.0f * r.unit))));
    r.grid_h = std::max(4u, static_cast<UINT>(std::ceil(small_h / (16.0f * r.unit))));
    r.small_w = small_w; r.small_h = small_h;
    r.tiles_w = (small_w + 7) / 8; r.tiles_h = (small_h + 7) / 8;
    r.offsets.attach(NewTexture(r.grid_w, r.grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    r.trust.attach(NewTexture(r.grid_w, r.grid_h, DXGI_FORMAT_R8G8B8A8_UNORM, true));
    r.choice.attach(NewTexture(small_w, small_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    r.sure.attach(NewTexture(small_w, small_h, DXGI_FORMAT_R8G8B8A8_UNORM, true));
    r.field.attach(NewTexture(r.grid_w, r.grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    r.settled.attach(NewTexture(small_w, small_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    r.calm.attach(NewTexture(r.tiles_w, r.tiles_h, DXGI_FORMAT_R8_UNORM, true));
    r.wide.attach(NewTexture(r.tiles_w, r.tiles_h, DXGI_FORMAT_R8_UNORM, true));
    r.still_mark.attach(NewTexture(full_w, full_h, DXGI_FORMAT_R8_UNORM, true));
    r.field_last.attach(NewTexture(r.grid_w, r.grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    r.movers.attach(NewTexture(r.grid_w, r.grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    r.movers_found.attach(NewTexture(r.grid_w, r.grid_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
    if (!r.all_moving) r.all_moving.attach(NewTexture(1, 1, DXGI_FORMAT_R8_UNORM, true));
    if (!r.offsets || !r.trust || !r.choice || !r.sure || !r.field || !r.settled || !r.calm || !r.wide || !r.still_mark || !r.field_last || !r.movers || !r.movers_found || !r.all_moving || !CmdOpen())
    { RebuildClose(); return false; }
    D3D12_RESOURCE_BARRIER to[13] = {
        Barrier(r.offsets.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.trust.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.choice.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.sure.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.field.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.settled.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.calm.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.wide.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.still_mark.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.field_last.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.movers.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.movers_found.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(r.all_moving.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    static bool painted = false;
    lfCore.cmd->ResourceBarrier(painted ? 12 : 13, to);
    if (!painted)
    {
        // "Moving" everywhere.
        D3D12_DESCRIPTOR_HEAP_DESC plain = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        winrt::com_ptr<ID3D12DescriptorHeap> cpu_heap;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&plain, IID_PPV_ARGS(cpu_heap.put())))) { CmdSubmit(); RebuildClose(); return false; }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_R8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
        lfCore.device->CreateUnorderedAccessView(r.all_moving.get(), nullptr, &uav, r.heap->GetCPUDescriptorHandleForHeapStart());
        lfCore.device->CreateUnorderedAccessView(r.all_moving.get(), nullptr, &uav, cpu_heap->GetCPUDescriptorHandleForHeapStart());
        ID3D12DescriptorHeap *heaps[] = {r.heap.get()};
        lfCore.cmd->SetDescriptorHeaps(1, heaps);
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        lfCore.cmd->ClearUnorderedAccessViewFloat(gpu, cpu_heap->GetCPUDescriptorHandleForHeapStart(), r.all_moving.get(), one, 0, nullptr);
        auto rest = Barrier(r.all_moving.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &rest);
        if (!AwaitMark(lfCore.timeline, CmdSubmit(), 30000)) { RebuildClose(); return false; }
        painted = true;
    }
    else if (!AwaitMark(lfCore.timeline, CmdSubmit(), 30000)) { RebuildClose(); return false; }
    r.ready = true;
    Trace("[dlssg] frames in between are made at %ux%u and rebuilt at the picture's size from the real frames around them "
          "(%ux%u blocks)", small_w, small_h, r.grid_w, r.grid_h);
    return true;
}

// The presenter: a frame in between (`made`, small, COPY_SOURCE) rebuilt at
// the picture's size into `target` (UNORDERED_ACCESS). after / before: the
// real frames around it at full size (COPY_SOURCE) and small
// (NON_PIXEL_SHADER_RESOURCE); map: the pair's still map or null. pair: which
// pair it is of - the pair's own movement is found with its first frame.
// made_phase: the moment `made` was generated for - the frame's own, or
// another's when the generator makes fewer frames a pair than are shown
// (lf_dlssg.inl, FgMadeFor); `made` may then be after_small itself, the real
// frame after standing in at moment 1. turn (0..3): which of four sets of the
// passes' descriptors to write - the presenter's list before this one may
// still be on the card, reading its own.
// onto: a back buffer of the picture's size (COPY_DEST, left so) the frame is
// drawn straight onto, when RebuildDrawReady said yes for its format; the
// target is not written then.
static void RebuildShow(ID3D12GraphicsCommandList *list, ID3D12Resource *made, ID3D12Resource *after_full,
                        ID3D12Resource *after_small, ID3D12Resource *before_full, ID3D12Resource *before_small,
                        ID3D12Resource *map, ID3D12Resource *target, float phase, float made_phase, const RebuildPrior &prior,
                        UINT64 pair, RebuildMarks *marks = nullptr, ID3D12Resource *onto = nullptr, UINT turn = 0)
{
    RebuildState &r = g_rebuild;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto td = target->GetDesc();
    if (map == nullptr) map = r.all_moving.get();
    auto mark = [&] { if (marks && marks->heap) list->EndQuery(marks->heap, D3D12_QUERY_TYPE_TIMESTAMP, marks->next++); };
    // A pass's twelve inputs by their places (t0..t11; null: not read by it) and its two outputs.
    auto bind = [&](UINT set, std::initializer_list<ID3D12Resource *> inputs, ID3D12Resource *first, ID3D12Resource *second) {
        auto cpu = r.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += SIZE_T((turn % 4) * 9 + set) * 14 * stride;
        UINT filled = 0;
        for (ID3D12Resource *in : inputs)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = in ? in->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
            lfCore.device->CreateShaderResourceView(in, &srv, cpu); cpu.ptr += stride; ++filled;
        }
        for (; filled < 12; ++filled)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
            lfCore.device->CreateShaderResourceView(nullptr, &srv, cpu); cpu.ptr += stride;
        }
        for (ID3D12Resource *out : {first, second})
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = out ? out->GetDesc().Format : DXGI_FORMAT_R8G8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            lfCore.device->CreateUnorderedAccessView(out, nullptr, &uav, cpu); cpu.ptr += stride;
        }
        auto gpu = r.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += UINT64((turn % 4) * 9 + set) * 14 * stride;
        return gpu;
    };
    auto run = [&](ID3D12PipelineState *pass, D3D12_GPU_DESCRIPTOR_HANDLE table, UINT across, UINT down) {
        list->SetPipelineState(pass);
        list->SetComputeRootDescriptorTable(1, table);
        table.ptr += UINT64(12) * stride;
        list->SetComputeRootDescriptorTable(2, table);
        list->Dispatch(across, down, 1);
    };
    struct Numbers
    {
        UINT grid_w, grid_h; float unit, phase;
        float prior[RebuildPrior::kAcross * RebuildPrior::kDown][2];
        UINT out_w, out_h; float small_w, small_h;
        float phase_made, have_prev, unused[2];
    } numbers = {r.grid_w, r.grid_h, r.unit, phase, {}, static_cast<UINT>(td.Width), td.Height, float(r.small_w), float(r.small_h), made_phase,
                 (r.repairs && r.pair_done != ~0ull && pair == r.pair_done + 1) ? 1.0f : 0.0f, {}};
    // The picture's movement in small pixels.
    for (int i = 0; i < RebuildPrior::kAcross * RebuildPrior::kDown; ++i)
    {
        numbers.prior[i][0] = prior.moved[i][0] * r.small_w;
        numbers.prior[i][1] = prior.moved[i][1] * r.small_w;
    }
    static_assert(sizeof(Numbers) == 60 * 4, "the passes' numbers");
    const bool one_real = before_full == after_full;
    // The real frame after in the generator's place: its small copy rests readable.
    const bool made_is_real = made == after_small;
    D3D12_RESOURCE_BARRIER in[3];
    UINT in_count = 0;
    in[in_count++] = Barrier(after_full, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!one_real) in[in_count++] = Barrier(before_full, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!made_is_real) in[in_count++] = Barrier(made, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(in_count, in);
    ID3D12DescriptorHeap *heaps[] = {r.heap.get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(r.root.get());
    list->SetComputeRoot32BitConstants(0, 60, &numbers, 0);
    // Once a pair: its own movement, from the two real frames by themselves.
    if (r.repairs && pair != r.pair_done)
    {
        if (r.field_read)
        {
            D3D12_RESOURCE_BARRIER again[4] = {
                Barrier(r.movers.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Barrier(r.settled.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Barrier(r.wide.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Barrier(r.still_mark.get(), kRebuildRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
            list->ResourceBarrier(4, again);
        }
        // The blocks of the pair before become what this pair's start from; the ones before those are written anew.
        std::swap(r.field, r.field_last);
        std::swap(r.field_is_read, r.last_is_read);
        if (r.field_is_read)
        {
            auto anew = Barrier(r.field.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &anew);
            r.field_is_read = false;
        }
        if (!r.last_is_read)
        {
            auto kept = Barrier(r.field_last.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &kept);
            r.last_is_read = true;
        }
        // The marks that stand on the screen, at the picture's size.
        run(r.still.get(), bind(0, {nullptr, after_full, before_full}, r.still_mark.get(), nullptr),
            (static_cast<UINT>(td.Width) + 7) / 8, (td.Height + 7) / 8);
        auto marked = Barrier(r.still_mark.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRebuildRead);
        list->ResourceBarrier(1, &marked);
        run(r.pair.get(), bind(1, {nullptr, nullptr, nullptr, after_small, before_small, r.field_last.get(), nullptr, nullptr, nullptr, nullptr,
                                   r.still_mark.get()}, r.field.get(), nullptr),
            r.grid_w, r.grid_h);
        auto fielded = Barrier(r.field.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &fielded);
        r.field_is_read = true;
        run(r.settle.get(), bind(2, {nullptr, nullptr, nullptr, after_small, before_small, r.field.get(), nullptr, nullptr, nullptr, nullptr,
                                     r.still_mark.get()}, r.settled.get(), r.calm.get()), r.tiles_w, r.tiles_h);
        D3D12_RESOURCE_BARRIER settled[2] = {
            Barrier(r.settled.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(r.calm.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        list->ResourceBarrier(2, settled);
        run(r.widen.get(), bind(3, {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, r.calm.get()}, r.wide.get(), nullptr),
            (r.tiles_w + 7) / 8, (r.tiles_h + 7) / 8);
        D3D12_RESOURCE_BARRIER widened[2] = {
            Barrier(r.wide.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(r.calm.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list->ResourceBarrier(2, widened);
        // What goes its own way, block by block (only with SM_MOVERS=1).
        if (r.movers_on)
        {
            run(r.mover.get(), bind(4, {nullptr, nullptr, nullptr, after_small, before_small, nullptr, nullptr, nullptr, r.settled.get()},
                                    r.movers_found.get(), nullptr), r.grid_w, r.grid_h);
            auto found_them = Barrier(r.movers_found.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &found_them);
            run(r.spread.get(), bind(8, {nullptr, nullptr, nullptr, nullptr, nullptr, r.movers_found.get(), nullptr, nullptr, r.settled.get()},
                                     r.movers.get(), nullptr), (r.grid_w + 7) / 8, (r.grid_h + 7) / 8);
            auto anew = Barrier(r.movers_found.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &anew);
        }
        auto moved_by = Barrier(r.movers.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &moved_by);
        r.field_read = true;
        r.pair_done = pair;
        if (marks) marks->pair_made = true;
    }
    mark();
    ID3D12Resource *pair_field = r.repairs ? r.settled.get() : nullptr, *pair_wide = r.repairs ? r.wide.get() : nullptr;
    ID3D12Resource *pair_marks = r.repairs ? r.still_mark.get() : nullptr;
    ID3D12Resource *pair_movers = r.repairs && r.movers_on ? r.movers.get() : nullptr;
    // 1. Where each block lies in the two real frames.
    run(r.follow.get(), bind(5, {made, after_full, before_full, after_small, before_small, nullptr, nullptr, nullptr, pair_field, pair_wide},
                             r.offsets.get(), r.trust.get()), r.grid_w, r.grid_h);       // a group of threads a block
    D3D12_RESOURCE_BARRIER found[2] = {
        Barrier(r.offsets.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(r.trust.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(2, found);
    mark();
    // 2. Which of them each small pixel goes by - or by the pair's own movement.
    run(r.choose.get(), bind(6, {made, after_full, before_full, after_small, before_small, r.offsets.get(), r.trust.get(), nullptr,
                                 pair_field, pair_wide, nullptr, pair_movers}, r.choice.get(), r.sure.get()),
        (r.small_w + 7) / 8, (r.small_h + 7) / 8);
    D3D12_RESOURCE_BARRIER chosen[2] = {
        Barrier(r.choice.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(r.sure.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(2, chosen);
    mark();
    // 3. The frame at the picture's size.
    if (onto)
    {
        // The real frames and the small one are read by a pixel shader here.
        D3D12_RESOURCE_BARRIER drawn[4];
        UINT drawn_count = 0;
        drawn[drawn_count++] = Barrier(onto, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        drawn[drawn_count++] = Barrier(after_full, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kRebuildRead);
        if (!one_real) drawn[drawn_count++] = Barrier(before_full, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kRebuildRead);
        if (!made_is_real) drawn[drawn_count++] = Barrier(made, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, kRebuildRead);
        list->ResourceBarrier(drawn_count, drawn);
        D3D12_RESOURCE_BARRIER read[6] = {
            Barrier(after_small, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Barrier(r.choice.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Barrier(r.sure.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Barrier(map, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            Barrier(before_small, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
            {}};
        list->ResourceBarrier(before_small == after_small ? 4 : 5, read);
        const auto od = onto->GetDesc();
        const D3D12_CPU_DESCRIPTOR_HANDLE view = r.draw_view->GetCPUDescriptorHandleForHeapStart();
        lfCore.device->CreateRenderTargetView(onto, nullptr, view);
        list->OMSetRenderTargets(1, &view, FALSE, nullptr);
        const D3D12_VIEWPORT viewport = {0.0f, 0.0f, float(od.Width), float(od.Height), 0.0f, 1.0f};
        const D3D12_RECT all = {0, 0, LONG(od.Width), LONG(od.Height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &all);
        list->SetGraphicsRootSignature(r.root.get());
        list->SetPipelineState(r.draw.get());
        list->SetGraphicsRoot32BitConstants(0, 60, &numbers, 0);
        auto table = bind(7, {made, after_full, before_full, after_small, before_small, r.choice.get(), r.sure.get(), map, nullptr, nullptr,
                              pair_marks}, nullptr, nullptr);
        list->SetGraphicsRootDescriptorTable(1, table);
        table.ptr += UINT64(12) * stride;
        list->SetGraphicsRootDescriptorTable(2, table);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        for (UINT i = 0; i < drawn_count; ++i) std::swap(drawn[i].Transition.StateBefore, drawn[i].Transition.StateAfter);
        for (int i = 0; i < 5; ++i) std::swap(read[i].Transition.StateBefore, read[i].Transition.StateAfter);
        list->ResourceBarrier(drawn_count, drawn);
        list->ResourceBarrier(before_small == after_small ? 4 : 5, read);
    }
    else
        run(r.build.get(), bind(7, {made, after_full, before_full, after_small, before_small, r.choice.get(), r.sure.get(), map, nullptr, nullptr,
                                    pair_marks}, target, nullptr),
            (static_cast<UINT>(td.Width) + 7) / 8, (td.Height + 7) / 8);
    D3D12_RESOURCE_BARRIER out[7];
    UINT out_count = 0;
    out[out_count++] = Barrier(r.offsets.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    out[out_count++] = Barrier(r.trust.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    out[out_count++] = Barrier(r.choice.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    out[out_count++] = Barrier(r.sure.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    out[out_count++] = Barrier(after_full, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!one_real) out[out_count++] = Barrier(before_full, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!made_is_real) out[out_count++] = Barrier(made, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(out_count, out);
    mark();
}
