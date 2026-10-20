// FLOW MFG's own motion estimate - compute shaders, any GPU.
//
// Lossless Scaling, FSR 3 frame generation and AMD's AFMF all find their motion
// in ordinary compute shaders, which is why they look the same on any card.
// FLOW MFG used to take its motion from outside: NVIDIA Optical Flow (a
// hardware block RTX cards have) or, everywhere else, the CPU (DIS at 320x180,
// one direction only). Measured on the football truth test at 15 FPS, the CPU
// field left the ball 9.6 px off and lost it in 15% of frames where NVOFA had
// 2.75 px and 1.5%, and it cost the CPU 7 ms a frame. So FLOW MFG finds the
// motion itself, both directions, on the GPU - and whatever arrives from
// outside (NVOFA on RTX, the CPU field) is one more candidate, kept only where
// it carries the pixels better.
//
// Coarse to fine on a colour pyramid:
//   EstBase    the two frames at the motion texture's size (level 0)
//   EstHalf    each smaller level from the one above it
//   FlowCoarse the smallest level: every motion within a few texels is tried
//   FlowRefine each finer level: the level below's motion here and beside
//              here (an edge takes its neighbour's), no motion, the outside
//              estimate - then one texel around the best; half a texel at
//              level 0
//   FlowSpread after each level, three passes (4, 2, 1 texels apart; level 0
//              one): a texel takes a neighbour's motion where it carries its
//              own patch better - or as well, and the neighbour had more to go
//              on. A texel that matches well on plenty of texture is settled.
//   FlowFinal  level 0, per texel: the still rule
//   FlowCheck  level 0, both directions against each other: a motion whose
//              way back does not come home (a look-alike further off - on the
//              football match another player of the same team, 73 texels the
//              wrong way) gives way to a neighbour's that does and carries the
//              patch about as well; then the result
//
// No motion from the last pair is used here: tried as a candidate at every
// level it made the football truth test worse (the player with the ball 16.7
// -> 18.4 at 15 FPS) - where the estimate is unsure, an old motion is as
// likely to be wrong as right. BlockMain, after this, has its own temporal
// candidate and judges it on a whole block.
// Costs are colour differences over a sparse 5x5 patch, each sample capped:
// past the cap a sample simply does not match.
//
// FLAT PLACES. A patch with no texture matches any motion, so a match there
// proves nothing: on the football pitch the smallest level picked a random
// motion on 38% of the grass. So a motion is only changed where the new one
// is better by more than the noise, and every motion carries how much
// texture it was found on; the spread passes carry the motion of the textured
// places (a stripe's edge, a line, a player) across the flat ones beside them.
//
// STILL AGAINST MOVING. A place where nothing moved matches itself with no
// motion. Where there is enough texture for that to mean something, no motion
// wins outright - the pitch beside a running player, the static background
// beside a moving window, text.
//
// Storage: every level of both frames in one R32_UINT texture (colour at 10
// bits a channel), every level's motion in another - two sets (a pass reads
// one and writes the other) of two directions, each texel a motion at 1/8
// texel and how much texture it was found on. Typed UAV loads of R32_UINT work
// on every D3D12 card. Once built, the pyramid is read through a texture view
// (t4), which is what the texture cache is for.
//
// Size: level 0 is FLOW MFG's motion texture or, where that is large (a 1080p
// network), a half or a quarter of it - no wider than 800 - and FlowCheck
// writes the result back at the motion texture's size. At 640x360 the whole
// estimate is about 2 ms a pair on an RTX 3060.
//
// Bindings: t0/t1 the previous/current frame (output size), t2/t3 the outside
// estimate (current -> previous, previous -> current; motion texels), t4 the
// pyramid (read), u0 the pyramid (while it is built), u1 the motion, u2/u3 the
// result (current -> previous, previous -> current; motion texels). Direction
// z: 0 current -> previous, 1 previous -> current.

#pragma once

static const char kFlowEstimateHlsl[] = R"hlsl(
Texture2D<float4> gPrev : register(t0);
Texture2D<float4> gCur : register(t1);
Texture2D<float2> gHintMv : register(t2);
Texture2D<float2> gHintReverse : register(t3);
Texture2D<uint> gPyramidRead : register(t4);
RWTexture2D<uint> gPyramid : register(u0);
RWTexture2D<uint> gField : register(u1);
RWTexture2D<float2> gEstMv : register(u2);
RWTexture2D<float2> gEstReverse : register(u3);
SamplerState gLin : register(s0);
// gFlags: 1 an outside estimate is bound, 2 it has the previous -> current
// half too. gFrom/gTo: which set of motion a pass reads and writes. gStep: how
// far apart FlowSpread looks. gMW x gMH is level 0, gOutW x gOutH the motion
// texture the result (and the outside estimate) is at, gRatio texels of it to
// one of level 0 (1, 2 or 4). gRadius: how far the smallest level looks - the
// host sets it by how long the pair is.
cbuffer EC : register(b0) {
    uint gW; uint gH; uint gMW; uint gMH; uint gLevel; uint gLevels; uint gFlags; uint gRadius;
    uint gStep; uint gFrom; uint gTo; uint gOutW; uint gOutH; uint gRatio;
};
uint Ratio() { return max(1u, gRatio); }

// ---- where things are --------------------------------------------------------
uint2 LevelSize(uint l) { return uint2((gMW + (1u << l) - 1u) >> l, (gMH + (1u << l) - 1u) >> l); }
// Level 0 at the left, every smaller one stacked in the column beside it; one
// band per frame (or per set and direction), a little taller than level 0 so
// the rounded-up sizes always fit.
int2 LevelOrigin(uint l) {
    if (l == 0) return int2(0, 0);
    int y = 0;
    [loop] for (uint k = 1; k < l; ++k) y += int(LevelSize(k).y);
    return int2(int(gMW), y);
}
int Band() { return int(gMH) + 16; }
int2 At(uint band, uint l, int2 p) {
    int2 s = int2(LevelSize(l));
    return LevelOrigin(l) + clamp(p, int2(0, 0), s - 1) + int2(0, int(band) * Band());
}
uint Pack(float3 c) { uint3 q = uint3(round(saturate(c) * 1023.0)); return q.x | (q.y << 10) | (q.z << 20); }
float3 Unpack(uint v) { return float3(v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u) * (1.0 / 1023.0); }
// While the pyramid is built (EstHalf), and once it stands.
float3 Building(uint frame, uint l, int2 p) { return Unpack(gPyramid[At(frame, l, p)]); }
float3 Texel(uint frame, uint l, int2 p) { return Unpack(gPyramidRead.Load(int3(At(frame, l, p), 0))); }
float3 Between(uint frame, uint l, float2 p) {
    float2 f = p - floor(p);
    int2 i = int2(floor(p));
    return lerp(lerp(Texel(frame, l, i), Texel(frame, l, i + int2(1, 0)), f.x),
                lerp(Texel(frame, l, i + int2(0, 1)), Texel(frame, l, i + int2(1, 1)), f.x), f.y);
}
// A motion: x and y at 1/8 texel in 12 bits each (+-256 texels), and how much
// texture it was found on, 0..255 for 0...05.
struct Move { float2 v; float sure; };
uint PackMove(float2 v, float sure) {
    int2 q = clamp(int2(round(v * 8.0)), -2047, 2047) + 2048;
    return uint(q.x) | (uint(q.y) << 12) | (uint(round(saturate(sure / .05) * 255.0)) << 24);
}
Move UnpackMove(uint u) {
    Move m;
    m.v = float2(int(u & 4095u) - 2048, int((u >> 12) & 4095u) - 2048) * .125;
    m.sure = float(u >> 24) * (.05 / 255.0);
    return m;
}
Move FieldAt(uint set, uint dir, uint l, int2 p) { return UnpackMove(gField[At(set * 2 + dir, l, p)]); }
void Keep(uint dir, uint l, int2 p, float2 v, float sure) { gField[At(gTo * 2 + dir, l, p)] = PackMove(v, sure); }

// ---- how well a motion carries a patch -----------------------------------------
// A sparse 5x5 patch: nine samples two texels apart.
static const int2 kPatch[9] = {int2(-2,-2),int2(0,-2),int2(2,-2),int2(-2,0),int2(0,0),int2(2,0),int2(-2,2),int2(0,2),int2(2,2)};
static const int2 kRing[8] = {int2(-1,-1),int2(0,-1),int2(1,-1),int2(-1,0),int2(1,0),int2(-1,1),int2(0,1),int2(1,1)};
static const float2 kHalf[4] = {float2(.5,0),float2(-.5,0),float2(0,.5),float2(0,-.5)};
static const float kCap = .25;
// The median of nine (Devillard's network): 19 compare-and-swaps.
void Order(inout float a, inout float b) { float lo = min(a, b); b = max(a, b); a = lo; }
float Median9(float p[9]) {
    Order(p[1], p[2]); Order(p[4], p[5]); Order(p[7], p[8]); Order(p[0], p[1]); Order(p[3], p[4]);
    Order(p[6], p[7]); Order(p[1], p[2]); Order(p[4], p[5]); Order(p[7], p[8]); Order(p[0], p[3]);
    Order(p[5], p[8]); Order(p[4], p[7]); Order(p[3], p[6]); Order(p[1], p[4]); Order(p[2], p[5]);
    Order(p[4], p[7]); Order(p[4], p[2]); Order(p[6], p[4]); Order(p[4], p[2]);
    return p[4];
}
// What a change of motion has to win by: a little over what noise and 10-bit
// colour make of the same match.
static const float kMargin = .004;
float Miss(float3 a, float3 b) { float3 d = abs(a - b); return min((d.x + d.y + d.z) * (1.0 / 3.0), kCap); }
// Direction 0 matches the current frame into the previous one, 1 the other way.
uint FromOf(uint dir) { return dir == 0 ? 1u : 0u; }
uint IntoOf(uint dir) { return dir == 0 ? 0u : 1u; }
float CostWhole(uint dir, uint l, int2 p, float3 here[9], int2 v) {
    float total = 0;
    [unroll] for (int k = 0; k < 9; ++k) total += Miss(here[k], Texel(IntoOf(dir), l, p + kPatch[k] + v));
    return total * (1.0 / 9.0);
}
float CostPart(uint dir, uint l, int2 p, float3 here[9], float2 v) {
    float total = 0;
    [unroll] for (int k = 0; k < 9; ++k) total += Miss(here[k], Between(IntoOf(dir), l, float2(p + kPatch[k]) + v));
    return total * (1.0 / 9.0);
}
float Cost(uint dir, uint l, int2 p, float3 here[9], float2 v) {
    return all(frac(v) == 0) ? CostWhole(dir, l, p, here, int2(v)) : CostPart(dir, l, p, here, v);
}
void Here(uint dir, uint l, int2 p, out float3 here[9]) {
    [unroll] for (int k = 0; k < 9; ++k) here[k] = Texel(FromOf(dir), l, p + kPatch[k]);
}
// How much there is to match on: the mean distance of the patch from its own
// mean. A flat patch matches any motion, so its match says nothing.
float Texture(float3 here[9]) {
    float3 mean = 0;
    [unroll] for (int k = 0; k < 9; ++k) mean += here[k];
    mean *= 1.0 / 9.0;
    float spread = 0;
    [unroll] for (int j = 0; j < 9; ++j) { float3 d = abs(here[j] - mean); spread += (d.x + d.y + d.z) * (1.0 / 3.0); }
    return spread * (1.0 / 9.0);
}
// The outside estimate at level l, in that level's texels; zero without one.
// It comes at the motion texture's size, in its texels.
float2 Hint(uint dir, uint l, int2 p) {
    if ((gFlags & 1u) == 0) return 0;
    int2 q = clamp((p << l) * int(Ratio()), int2(0, 0), int2(gOutW, gOutH) - 1);
    float2 v;
    if (dir == 0) v = gHintMv.Load(int3(q, 0));
    else v = (gFlags & 2u) != 0 ? gHintReverse.Load(int3(q, 0)) : -gHintMv.Load(int3(q, 0));
    return all(isfinite(v)) ? v / float(Ratio() << l) : 0;
}

)hlsl"
// MSVC takes at most 16 KB in one string literal: two, joined by the compiler.
R"hlsl(// ---- the pyramid -----------------------------------------------------------------
// Level 0: the frame averaged into each motion texel. At the usual 2:1 one
// bilinear sample at the centre is exactly the 2x2 mean; wider, four.
[numthreads(8, 8, 1)]
void EstBase(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gMW || id.y >= gMH) return;
    float2 texel = 1.0 / float2(gMW, gMH), uv = (float2(id.xy) + .5) * texel;
    float3 c;
    if (gW <= 2 * gMW && gH <= 2 * gMH)
        c = id.z == 0 ? gPrev.SampleLevel(gLin, uv, 0).rgb : gCur.SampleLevel(gLin, uv, 0).rgb;
    else {
        float2 d = .25 * texel;
        c = 0;
        [unroll] for (int k = 0; k < 4; ++k) {
            float2 s = uv + float2((k & 1) ? d.x : -d.x, (k & 2) ? d.y : -d.y);
            c += id.z == 0 ? gPrev.SampleLevel(gLin, s, 0).rgb : gCur.SampleLevel(gLin, s, 0).rgb;
        }
        c *= .25;
    }
    gPyramid[At(id.z, 0, int2(id.xy))] = Pack(c);
}

[numthreads(8, 8, 1)]
void EstHalf(uint3 id : SV_DispatchThreadID) {
    uint2 s = LevelSize(gLevel);
    if (id.x >= s.x || id.y >= s.y) return;
    int2 p = int2(id.xy) * 2;
    float3 c = Building(id.z, gLevel - 1, p) + Building(id.z, gLevel - 1, p + int2(1, 0)) +
               Building(id.z, gLevel - 1, p + int2(0, 1)) + Building(id.z, gLevel - 1, p + int2(1, 1));
    gPyramid[At(id.z, gLevel, int2(id.xy))] = Pack(c * .25);
}

// ---- the smallest level: everything near is tried ------------------------------
[numthreads(8, 8, 1)]
void FlowCoarse(uint3 id : SV_DispatchThreadID) {
    uint2 s = LevelSize(gLevel);
    if (id.x >= s.x || id.y >= s.y) return;
    uint dir = id.z;
    int2 p = int2(id.xy);
    float3 here[9];
    Here(dir, gLevel, p, here);
    // Nothing below to predict from: the outside estimate if there is one,
    // standing still otherwise - and the search is around it. Anything else
    // must win by the margin, and by a margin for every texel it goes further:
    // at this size a player is a texel or two, and a look-alike further off (a
    // team-mate) matches as well as he does - on the football match whole
    // players were carried to one 100 texels away.
    float2 predicted = round(Hint(dir, gLevel, p));
    float best = CostWhole(dir, gLevel, p, here, int2(predicted));
    float2 move = predicted;
    if (any(predicted != 0)) {
        float cost = CostWhole(dir, gLevel, p, here, int2(0, 0)) + kMargin;
        if (cost < best) { best = cost; move = 0; }
    }
    int r = int(gRadius);
    [loop] for (int y = -r; y <= r; ++y)
    [loop] for (int x = -r; x <= r; ++x) {
        float2 v = predicted + float2(x, y);
        float cost = CostWhole(dir, gLevel, p, here, int2(v)) + kMargin + .004 * length(v - predicted);
        if (cost < best) { best = cost; move = v; }
    }
    Keep(dir, gLevel, p, move, Texture(here));
}

// ---- every finer level ----------------------------------------------------------
[numthreads(8, 8, 1)]
void FlowRefine(uint3 id : SV_DispatchThreadID) {
    uint2 s = LevelSize(gLevel);
    if (id.x >= s.x || id.y >= s.y) return;
    uint dir = id.z;
    int2 p = int2(id.xy);
    float3 here[9];
    Here(dir, gLevel, p, here);
    // The level below, here and beside here: at an edge the neighbour's motion
    // is the other object's.
    int2 below = p / 2;
    float2 predicted = 2.0 * FieldAt(gFrom, dir, gLevel + 1, below).v;
    float2 candidates[6];
    candidates[0] = 2.0 * FieldAt(gFrom, dir, gLevel + 1, below + int2(1, 0)).v;
    candidates[1] = 2.0 * FieldAt(gFrom, dir, gLevel + 1, below + int2(-1, 0)).v;
    candidates[2] = 2.0 * FieldAt(gFrom, dir, gLevel + 1, below + int2(0, 1)).v;
    candidates[3] = 2.0 * FieldAt(gFrom, dir, gLevel + 1, below + int2(0, -1)).v;
    candidates[4] = 0;
    candidates[5] = Hint(dir, gLevel, p);
    uint count = (gFlags & 1u) != 0 ? 6u : 5u;
    float2 move = round(predicted);
    float best = CostWhole(dir, gLevel, p, here, int2(move));
    [loop] for (uint i = 0; i < count; ++i) {
        float2 v = round(candidates[i]);
        float cost = CostWhole(dir, gLevel, p, here, int2(v)) + kMargin;
        if (cost < best) { best = cost; move = v; }
    }
    // One texel around the best.
    float2 start = move;
    [loop] for (int k = 0; k < 8; ++k) {
        float2 v = start + float2(kRing[k]);
        float cost = CostWhole(dir, gLevel, p, here, int2(v)) + kMargin;
        if (cost < best) { best = cost; move = v; }
    }
    // Still no match: something small and fast the smaller levels could not
    // see - a player running across the pitch is two texels there - or they
    // took it for a look-alike further off (another player of the same team:
    // on the football match the player with the ball was carried 73 texels the
    // wrong way). Search around the motion of the scene here and around no
    // motion, as far as the smallest level does.
    if (best > .04 && gLevel >= 1) {
        int r = gLevel >= 2 ? int(gRadius) : min(int(gRadius), 4);   // wide where it is cheap
        [loop] for (uint centre = 0; centre < 2; ++centre) {
            start = centre == 0 ? round(predicted) : float2(0, 0);
            if (centre == 1 && all(start == round(predicted))) break;
            [loop] for (int y = -r; y <= r; ++y)
            [loop] for (int x = -r; x <= r; ++x) {
                float2 v = start + float2(x, y);
                float cost = CostWhole(dir, gLevel, p, here, int2(v)) + kMargin + .0005 * length(v);
                if (cost < best) { best = cost; move = v; }
            }
        }
    }
    float sure = Texture(here);
    // Level 0: half a texel around it, where there is texture to tell.
    if (gLevel == 0 && sure > .01) {
        start = move;
        [loop] for (int j = 0; j < 4; ++j) {
            float2 v = start + kHalf[j];
            float cost = CostPart(dir, 0, p, here, v) + kMargin * .5;
            if (cost < best) { best = cost; move = v; }
        }
    }
    Keep(dir, gLevel, p, move, sure);
}

// ---- carrying motion across a level ---------------------------------------------
// A neighbour gStep texels away gives its motion where it carries this patch
// better by the margin - or where this patch cannot tell (as well, within the
// margin) and the neighbour's motion was found on more texture. What is taken
// keeps a little less than the texture it came from, so it never outranks the
// place it was found.
[numthreads(8, 8, 1)]
void FlowSpread(uint3 id : SV_DispatchThreadID) {
    uint2 s = LevelSize(gLevel);
    if (id.x >= s.x || id.y >= s.y) return;
    uint dir = id.z;
    int2 p = int2(id.xy);
    float3 here[9];
    Here(dir, gLevel, p, here);
    Move own = FieldAt(gFrom, dir, gLevel, p);
    // Judged at the nearest whole texel: what is compared is which motion,
    // not its fraction - and at four texture reads a sample, the fraction was
    // most of the estimate's time.
    float ownCost = CostWhole(dir, gLevel, p, here, int2(round(own.v)));
    float2 move = own.v;
    float sure = own.sure, best = ownCost;
    if (ownCost < .01 && own.sure > .02) { Keep(dir, gLevel, p, move, sure); return; }
    [loop] for (int k = 0; k < 8; ++k) {
        Move near = FieldAt(gFrom, dir, gLevel, p + kRing[k] * int(gStep));
        if (all(near.v == move)) continue;
        float cost = CostWhole(dir, gLevel, p, here, int2(round(near.v)));
        bool better = cost + kMargin < best;
        bool surer = cost <= ownCost + kMargin && near.sure * .9 > sure;
        if (better || surer) { best = min(best, cost); move = near.v; sure = better ? max(own.sure, near.sure * .9) : near.sure * .9; }
    }
    Keep(dir, gLevel, p, move, sure);
}

// ---- level 0: the still rule, and the result -------------------------------------
[numthreads(8, 8, 1)]
void FlowFinal(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gMW || id.y >= gMH) return;
    uint dir = id.z;
    int2 p = int2(id.xy);
    float3 here[9];
    Here(dir, 0, p, here);
    float2 move = FieldAt(gFrom, dir, 0, p).v;
    float best = Cost(dir, 0, p, here, move);
    // The outside estimate as it came - NVOFA's quarter texels - wherever it
    // carries this patch better: fusing must never lose what it had.
    if ((gFlags & 1u) != 0) {
        float2 hint = Hint(dir, 0, p);
        float cost = CostPart(dir, 0, p, here, hint);
        if (cost + .001 < best) { best = cost; move = hint; }
    }
    // The middle of the motions here and around (each coordinate's median of
    // nine), wherever the patch cannot tell it from its own: noise a texel or
    // two wide is outvoted, an edge - four or five on each side - is not. On a
    // streamed video's grass the field was a quilt of directions without it.
    float xs[9], ys[9];
    [unroll] for (int k = 0; k < 8; ++k) {
        float2 v = FieldAt(gFrom, dir, 0, p + kRing[k]).v;
        xs[k] = v.x; ys[k] = v.y;
    }
    xs[8] = move.x; ys[8] = move.y;
    float2 middle = float2(Median9(xs), Median9(ys));
    if (any(middle != move)) {
        float cost = Cost(dir, 0, p, here, middle);
        if (cost <= best + kMargin) { best = cost; move = middle; }
    }
    // Still against moving, as the CPU field has always judged it: a motion is
    // kept only where it carries the patch better than standing still. An
    // absolute test here ("no motion matches well enough") held single texels
    // of moving grass still - and the HUD verdict took every one of them for a
    // HUD pixel: the speckle around a HUD at a low frame rate.
    float still = CostWhole(dir, 0, p, here, int2(0, 0));
    if (still <= best + .0015) move = 0;
    Keep(dir, 0, p, move, Texture(here));
}

// ---- level 0: the two directions against each other, and the result -------------
// How far a motion's way back misses home, in texels.
float Away(uint dir, int2 p, float2 v) {
    float2 back = FieldAt(gFrom, 1 - dir, 0, p + int2(round(v))).v;
    return length(v + back);
}
static const int2 kFar[24] = {int2(-2,-2),int2(0,-2),int2(2,-2),int2(-2,0),int2(2,0),int2(-2,2),int2(0,2),int2(2,2),
                              int2(-4,-4),int2(0,-4),int2(4,-4),int2(-4,0),int2(4,0),int2(-4,4),int2(0,4),int2(4,4),
                              int2(-8,-8),int2(0,-8),int2(8,-8),int2(-8,0),int2(8,0),int2(-8,8),int2(0,8),int2(8,8)};
// Check each level-0 vector once, then replicate its footprint. The old
// output-sized dispatch repeated the identical 33-candidate search Ratio^2
// times (16 at 1440p/performance mode), consuming the game's GPU budget.
[numthreads(8, 8, 1)]
void FlowCheck(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gMW || id.y >= gMH) return;
    uint dir = id.z;
    int2 p = int2(id.xy);
    float2 move = FieldAt(gFrom, dir, 0, p).v;
    // Coming home within a texel and a half (and a tenth of the motion): kept.
    // Otherwise the best motion near here that does come home, even at a
    // somewhat worse match: a running player's legs never match exactly, and
    // an identical player elsewhere does - which is exactly the wrong answer.
    // No motion is one of them. Nothing that comes home at all: kept as it is
    // (something uncovered has nowhere to come home to).
    if (Away(dir, p, move) > 1.5 + .1 * length(move)) {
        float3 here[9];
        Here(dir, 0, p, here);
        float limit = Cost(dir, 0, p, here, move) + .06;
        float best = 1e30;
        float2 found = move;
        [loop] for (int k = 0; k < 33; ++k) {
            float2 v = k == 32 ? float2(0, 0) : FieldAt(gFrom, dir, 0, p + (k < 8 ? kRing[min(k, 7)] : kFar[clamp(k - 8, 0, 23)])).v;
            if (Away(dir, p, v) > 1.5 + .1 * length(v)) continue;
            float cost = Cost(dir, 0, p, here, v);
            if (cost < limit && cost < best) { best = cost; found = v; }
        }
        move = found;
    }
    move *= float(Ratio());
    [loop] for (uint y = 0; y < Ratio(); ++y)
    [loop] for (uint x = 0; x < Ratio(); ++x) {
        uint2 q = id.xy * Ratio() + uint2(x, y);
        if (q.x < gOutW && q.y < gOutH) {
            if (dir == 0) gEstMv[q] = move; else gEstReverse[q] = move;
        }
    }
}
)hlsl";
