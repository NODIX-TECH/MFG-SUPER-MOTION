// msm_tri.h - rounds 99 and 100, the owner's "triangles": SMFG's frames in between made another way, to be chosen in its
// place ("Triangle Mesh" in the app, the engine's key msm_tri; SM_MSM_TUNE=tri=1; tests\msmdata.exe and msmtruth.exe tri=1).
//
// SMFG finds how every block of two texels moves by trying places around it at five sizes, and draws a frame in
// between by asking, for every pixel of it, where that pixel lies in the two real frames. This one:
//
//   1. Motion, 3DRS (TriSearchMain). A mesh lies over the picture, a corner every sixteen pixels. Only the corners'
//      motion is looked for, and each corner tries six answers, no more: the one it had (the pair before's - a turn
//      of the camera is followed from pair to pair), its neighbour's across, its neighbour's up or down, the better
//      neighbour's with a small step added, none at all (a read-out, what stands), and where the picture as a whole
//      went if the engine knows. Judged by how alike the two frames' brightness is around the corner (sixteen
//      samples). Corners take turns as the squares of a chessboard do, so each reads neighbours just settled.
//      From small sizes of the brightness to larger ones (SMFG's own, MsmBrightness). Both ways.
//      Then, finer and finer: a texel to either side and between texels (TriFineMain), on SMFG's brightness and on
//      a brightness of half the picture's size made for it; to a part of a pixel by how the brightness slopes
//      (TriExactMain, Lucas and Kanade's step); and what is plain - a wall, the sky - takes the word of the corners
//      around it where that explains it as well (TriRelaxMain).
//   2. The mesh (TriMeshMain). A square of four corners a side whose corners move alike (within two pixels) and that
//      shows no edge stays two large triangles - its inner corners lie on them; else its four squares of two a side
//      are asked the same; what is left are single squares. Every square is cut along the diagonal whose two ends are
//      more alike (brightness and motion - the specification's "smart edge flipping"): the edge of a thing then lies
//      ALONG a triangle's side, not across it. A corner whose way does not lead back home from the other frame (it is
//      covered there) is not believed. A corner standing on a read-out goes the way the picture at large goes
//      (TriLargeMain): the read-out is not the mesh's to draw, the picture behind and beside it is.
//   3. Drawing (TriVS, TriPS): by the card's own rasteriser, not a compute pass. Every corner is moved by its
//      motion times the moment and the triangles are drawn with the frame as their texture - the frame before
//      moved forward to the moment, the frame after moved back to it. A triangle that grew or shrank on the way
//      (something came out from behind something) says so (the stretch); one that turned over is not drawn (culled);
//      where two land on one pixel the one that kept its shape is in front (the depth test); a mark is not carried.
//   4. Together (TriBlendMain): each pixel from the two drawn frames, the nearer in time and the less stretched
//      counting for more; what neither drew, from the real frames as they are; a mark (SMFG's own: MarkMain, told
//      the mesh's motion and the picture at large's) stays as the real frame has it, and so does a pixel beside marks
//      that the two real frames have alike.
//
// Measured (FIX PACING.md 1n, 1o): 59 real pairs of six recordings, mean error SMFG 5.51, this 5.61; the scenes whose
// every moment is known (tests\smooth\tritruth.ps1) 0.93 against 0.62, the read-outs 0.36 against 0.20. On an RTX 3060
// at 2560x1440: a pair's own work 1.6 ms (SMFG 1.7), a frame in between 0.5 ms (0.85).
//
// Not used, and why: mesh shaders (the mesh is 29,000 triangles at 1440p - its corners cost the card nothing to
// speak of; what a frame costs is the pixels, which a mesh shader does not touch); a network on top (ai learning\
// smfg-net-t: +0.1 dB for 5 - 10 ms a frame).
//
// It leans on msm_fg.h's root signature, descriptor tables, brightness and marks: it is included at its end.#pragma once

static const char kTriHlsl[] = R"hlsl(
cbuffer Numbers : register(b0)
{
    float2 inv_a;
    float2 inv_out;
    float4 k;
    float4 m;
    float4 n;
    float4 o;
    float4 q;
    float4 r;
};
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2);
Texture2D<float4> t3 : register(t3);
Texture2D<float4> t4 : register(t4);
Texture2D<float4> t5 : register(t5);
Texture2D<uint> landed_a : register(t6);
Texture2D<uint> landed_b : register(t7);
RWTexture2D<float4> out0 : register(u0);
RWTexture2D<uint> out1 : register(u1);
SamplerState lin : register(s0);

// ---- 1. motion
// t0: the brightness of the frame the corners are of, t1: of the other frame (one size of it; inv_a: its texel).
// t2: the corners' motion so far (xy, pixels of the picture). n.xy: a pixel of the picture as a share of it;
// n.z: pixels between corners. m.zw: the last corner. o.x 1: there is nothing so far (the first pair).

static const float2 kStep[8] = { float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1), float2(2, 0), float2(-2, 0), float2(0, 2), float2(0, -2) };

float2 Had(int2 p)
{
    return o.x > 0.5f ? float2(0.0f, 0.0f) : t2.Load(int3(clamp(p, int2(0, 0), int2(m.zw)), 0)).xy;
}

// (q.x 1: t0 and t1 are the frames themselves, not their brightness - TriFineMain's last rounds, to a pixel.)
float Seen(Texture2D<float4> t, float2 uv)
{
    const float4 c = t.SampleLevel(lin, uv, 0);
    return q.x > 0.5f ? dot(c.rgb, float3(0.299f, 0.587f, 0.114f)) : c.r;
}

void Own(float2 uv, out float own[16])
{
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            own[y * 4 + x] = Seen(t0, uv + (float2(x, y) - 1.5f) * 2.0f * inv_a);
}

float Apart(float2 uv, float own[16], float2 v)
{
    const float2 there = uv + v * n.xy;
    float sum = 0.0f;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            sum += abs(own[y * 4 + x] - Seen(t1, there + (float2(x, y) - 1.5f) * 2.0f * inv_a));
    return sum * 0.0625f;
}

// k.x: whose turn (0, 1: the chessboard's two colours); k.y: which round; k.z: 1 the neighbours before, -1 the ones
// after; k.w: a step, pixels. m.xy: where the picture as a whole went (pixels; 0 0: not known).
[numthreads(8, 8, 1)]
void TriSearchMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    float2 best = Had(p);
    if (((id.x + id.y) & 1u) != uint(k.x)) { out0[id.xy] = float4(best, 0.0f, 0.0f); return; }
    const float2 uv = min(float2(p) * n.z * n.xy, 1.0f);
    float own[16];
    Own(uv, own);
    float cost = Apart(uv, own, best);
    const int d = k.z > 0.0f ? 1 : -1;
    const float2 across = Had(p - int2(d, 0)), down = Had(p - int2(0, d));
    const float ca = Apart(uv, own, across), cd = Apart(uv, own, down);
    const float2 tried = (ca <= cd ? across : down) + kStep[(id.x + 3u * id.y + uint(k.y) * 5u + uint(k.x)) & 7u] * k.w;
    // (A neighbour's answer costs nothing more; a step tried, and the picture at large's, a little: what is plain
    // matches as well a step off, and would wander.)
    if (ca < cost) { cost = ca; best = across; }
    if (cd < cost) { cost = cd; best = down; }
    float c = Apart(uv, own, tried) + 0.001f;
    if (c < cost) { cost = c; best = tried; }
    c = Apart(uv, own, float2(0.0f, 0.0f));
    if (c < cost) { cost = c; best = float2(0.0f, 0.0f); }
    if (any(m.xy != 0.0f))
    {
        c = Apart(uv, own, m.xy) + 0.0005f;
        if (c < cost) { cost = c; best = m.xy; }
    }
    out0[id.xy] = float4(best, cost, 0.0f);
}

// A texel to either side, then parts of a texel. o.zw: a texel of this size, pixels of the picture.
[numthreads(8, 8, 1)]
void TriFineMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    float2 v = Had(p);
    const float2 uv = min(float2(p) * n.z * n.xy, 1.0f);
    float own[16];
    Own(uv, own);
    const float c0 = Apart(uv, own, v);
    const float xm = Apart(uv, own, v - float2(o.z, 0.0f)), xp = Apart(uv, own, v + float2(o.z, 0.0f));
    const float ym = Apart(uv, own, v - float2(0.0f, o.w)), yp = Apart(uv, own, v + float2(0.0f, o.w));
    const float least = min(min(xm, xp), min(ym, yp));
    if (least + 0.0005f < c0)
    {
        // (A whole texel better: gone there; the next round looks between texels.)
        v += least == xm ? float2(-o.z, 0.0f) : least == xp ? float2(o.z, 0.0f) : least == ym ? float2(0.0f, -o.w) : float2(0.0f, o.w);
    }
    else
    {
        const float bx = xm - 2.0f * c0 + xp, by = ym - 2.0f * c0 + yp;
        if (bx > 1e-4f) v.x += clamp(0.5f * (xm - xp) / bx, -0.5f, 0.5f) * o.z;
        if (by > 1e-4f) v.y += clamp(0.5f * (ym - yp) / by, -0.5f, 0.5f) * o.w;
    }
    out0[id.xy] = float4(v, c0, 0.0f);
}

// Relax (round 100, as SMFG's RelaxMain): a corner takes what the corners around it say where that explains what it
// shows as well as its own answer does. What is plain - a wall, a road, the sky - matches as well a pixel off as where
// it is: its corners' answers are each a little wrong their own way, and a fine thing on it (the dots of the bare
// wall, tests\jelly: 0.2 of a pixel from their place, SMFG's 0.03) is drawn between them, off and soft. A thing of its
// own - a pole against the sky - is not explained by the sky's motion, and keeps its own. "What the corners around
// say": the mean of those of the eight that go the way most of them go (within k.y pixels of the one nearest to all).
// On the frames themselves (q.x 1). k.x: as well - this much worse still counts; k.z: and this share of its own worse
// (round 101: where the picture is busy - leaves, grass - no answer explains it to within k.x, a corner's own is one
// of several as good, and each corner kept its own: a tree's corners went each a little their own way).
[numthreads(8, 8, 1)]
void TriRelaxMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    const float2 v = Had(p);
    float2 around[8];
    int i = 0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            if (x != 0 || y != 0) around[i++] = Had(p + int2(x, y));
    float least = 1e30f;
    float2 most = v;
    for (i = 0; i < 8; ++i)
    {
        float sum = 0.0f;
        for (int j = 0; j < 8; ++j) sum += length(around[i] - around[j]);
        if (sum < least) { least = sum; most = around[i]; }
    }
    float2 mean = 0.0f;
    float count = 0.0f;
    for (i = 0; i < 8; ++i)
        if (length(around[i] - most) <= k.y) { mean += around[i]; count += 1.0f; }
    mean /= max(count, 1.0f);
    const float2 uv = min(float2(p) * n.z * n.xy, 1.0f);
    float own[16];
    Own(uv, own);
    const float mine = Apart(uv, own, v), theirs = Apart(uv, own, mean);
    out0[id.xy] = float4(theirs <= mine * (1.0f + k.z) + k.x ? mean : v, min(mine, theirs), 0.0f);
}

// To a part of a pixel, on the frames themselves (q.x 1): the step that brings the other frame's picture around the
// corner onto this frame's, from how the brightness slopes there (Lucas and Kanade's). inv_a: between its samples
// (five by five). A place that shows nothing - no slope to go by - stays as it is. Round 100: the search and
// TriFineMain left the dots of the bare wall (tests\jelly) 0.36 of a pixel from their place, SMFG 0.03 - and a
// texture a third of a pixel off is the same texture, softer and ghosted: most of what was worse than SMFG's.
[numthreads(8, 8, 1)]
void TriExactMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    float2 v = Had(p);
    const float2 uv = min(float2(p) * n.z * n.xy, 1.0f);
    const float2 px = float2(o.z * n.x, 0.0f), py = float2(0.0f, o.w * n.y);      // (o.zw: a texel of what is read, pixels of the picture)
    float xx = 0.0f, xy = 0.0f, yy = 0.0f, xt = 0.0f, yt = 0.0f;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            const float2 here = uv + float2(x, y) * inv_a, there = here + v * n.xy;
            // (The slope: the two frames' together - either one's alone leans the step its way.)
            const float gx = 0.25f * (Seen(t1, there + px) - Seen(t1, there - px) + Seen(t0, here + px) - Seen(t0, here - px));
            const float gy = 0.25f * (Seen(t1, there + py) - Seen(t1, there - py) + Seen(t0, here + py) - Seen(t0, here - py));
            const float dt = Seen(t1, there) - Seen(t0, here);
            xx += gx * gx; xy += gx * gy; yy += gy * gy; xt += gx * dt; yt += gy * dt;
        }
    // (k.x: what counts as a slope at all - under it the step fades to nothing.)
    xx += k.x; yy += k.x;
    const float det = xx * yy - xy * xy;
    if (det > 1e-12f) v -= clamp(float2(yy * xt - xy * yt, xx * yt - xy * xt) / det, -1.0f, 1.0f) * o.zw;
    // (z: how far apart the two frames still were at this corner's answer - TriRelaxMain's, passed on for TriMeshMain.)
    out0[id.xy] = float4(v, o.x > 0.5f ? 0.0f : t2.Load(int3(p, 0)).z, 0.0f);
}

)hlsl" R"hlsl(
// ---- 2. the mesh
// t0: the corners' motion; t1: the frame's brightness (inv_a unused: read by place). k.x: corners of a square moving
// further apart than this (pixels) cut it; k.y: so does this much difference of brightness in it; k.z: what a
// pixel of motion counts beside brightness in choosing the diagonal. Writes xy: the motion of the mesh at this corner
// (inside a square left whole: as its triangles have it), z: the diagonal of the 8-pixel square this corner is the
// top left of (1: top left to bottom right), w: the size of the square it lies in (4, 2, 1 corners; to show).

float2 Mv(int2 g) { return t0.Load(int3(clamp(g, int2(0, 0), int2(m.zw)), 0)).xy; }
float Lu(float2 g) { return t1.SampleLevel(lin, min(g * n.z * n.xy, 1.0f), 0).r; }

bool Whole(int2 c, int size)
{
    if (any(c < 0) || any(c + size > int2(m.zw))) return false;
    const float2 tl = Mv(c), tr = Mv(c + int2(size, 0)), bl = Mv(c + int2(0, size)), br = Mv(c + size);
    const float far_apart = max(max(max(length(tl - tr), length(tl - bl)), max(length(tl - br), length(tr - bl))), max(length(tr - br), length(bl - br)));
    if (far_apart > k.x) return false;
    float lo = 1e9f, hi = -1e9f;
    for (int y = 0; y <= 2; ++y)
        for (int x = 0; x <= 2; ++x)
        {
            const float l = Lu(float2(c) + float2(x, y) * 0.5f * size);
            lo = min(lo, l); hi = max(hi, l);
        }
    return hi - lo <= k.y;
}

bool Diagonal(int2 c, int size)
{
    const float2 tl = Mv(c), tr = Mv(c + int2(size, 0)), bl = Mv(c + int2(0, size)), br = Mv(c + size);
    const float one = abs(Lu(c) - Lu(c + size)) + k.z * length(tl - br);
    const float two = abs(Lu(c + int2(size, 0)) - Lu(c + int2(0, size))) + k.z * length(tr - bl);
    return one < two;
}

float2 OnTriangles(int2 c, int size, int2 p)
{
    const float2 tl = Mv(c), tr = Mv(c + int2(size, 0)), bl = Mv(c + int2(0, size)), br = Mv(c + size);
    const float u = float(p.x - c.x) / size, v = float(p.y - c.y) / size;
    if (Diagonal(c, size)) return u >= v ? tl + u * (tr - tl) + v * (br - tr) : tl + v * (bl - tl) + u * (br - bl);
    return u + v <= 1.0f ? tl + u * (tr - tl) + v * (bl - tl) : br + (1.0f - u) * (bl - br) + (1.0f - v) * (tr - br);
}

[numthreads(8, 8, 1)]
void TriMeshMain(uint3 id : SV_DispatchThreadID)
{
    const int2 p = int2(id.xy);
    float2 v = Mv(p);
    bool placed = false;
    // (A corner on the side of a square belongs to the squares on both sides of it: any of them left whole has it.)
    for (int size = 4; size >= 2 && !placed; size >>= 1)
        for (int e = 0; e < 4 && !placed; ++e)
        {
            const int2 c = ((p - int2(e & 1, e >> 1)) / size) * size;
            if (any(p - int2(e & 1, e >> 1) < 0)) continue;
            if (Whole(c, size)) { v = OnTriangles(c, size, p); placed = true; }
        }
    const int2 c4 = (p / 4) * 4, c2 = (p / 2) * 2;
    const bool cut = Whole(c4, 4) ? Diagonal(c4, 4) : Whole(c2, 2) ? Diagonal(c2, 2) : Diagonal(p, 1);
    // Does the way back from where this corner goes lead home? (t2: the other frame's corners, their own way.) Where it
    // does not - what this frame shows there the other does not: it is covered, or left the picture - the corner is
    // not to be believed (w: 0 it is .. 1 not at all), and what the other frame draws there is taken (TriVS).
    // Round 100, the read-outs. A corner that stands on marks (landed_a: MarkMain's, the pair before's; q.y 1: there are
    // any) goes the way the picture at large goes there (t3; q.zw: its texel in pixels, o.y: -1 this is the way back):
    // the read-out's own pixels are not the mesh's to draw (TriPS leaves them out, TriBlendMain shows them as the real
    // frame has them), and the picture beside and behind it is - left standing with the read-out, a corner there held
    // the moving picture back along the whole of its triangles (the picture beside the read-outs 4.96 against SMFG's 2.95).
    bool under = false;
    if (q.y > 0.5f)
    {
        const int2 last = int2(1.0f / n.xy) - 1, at = int2(float2(p) * n.z), reach = max(1, int(n.z) / 3);
        uint marks = 0u;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                marks += (landed_a.Load(int3(clamp(at + int2(x, y) * reach, int2(0, 0), last), 0)) & 128u) != 0u ? 1u : 0u;
        if (marks >= 5u)
        {
            v = t3.SampleLevel(lin, min(float2(p) * n.z * n.xy, 1.0f), 0).xy * q.zw * o.y;
            under = true;
        }
    }
    const float2 there = (float2(p) + v / n.z + 0.5f) / (m.zw + 1.0f);
    const float2 home = v + t2.SampleLevel(lin, there, 0).xy;
    float lost = k.w > 0.0f && !under ? saturate((length(home) - k.w) / (2.0f * k.w)) : 0.0f;
    // (Round 100: nor one that was not found at all - the two frames do not show the same around it under its answer
    //  (t0's z, brightness; q.x: from so far apart on, wholly at twice that). What entered the picture at its edge in a
    //  fast turn: drawn by such corners it came out as swirls.)
    if (q.x > 0.0f && !under) lost = max(lost, saturate((t0.Load(int3(p, 0)).z - q.x) / q.x));
    out0[id.xy] = float4(v, cut ? 1.0f : 0.0f, lost);
}

)hlsl" R"hlsl(
// How the picture at large moves around each place (round 100, for the read-outs): of twenty-five corners spread wide
// around it, the one whose motion is nearest to all the others' - what most of them say, never a mix of two things.
// A read-out's panel is larger than the corners' own neighbourhood: its corners find that it stands, rightly - and
// nothing then said that the picture around it moves. MarkMain asks this (a pixel that would be another colour had it
// gone the way the picture at large goes, and is the same, stays), and TriMeshMain moves the mesh under a read-out
// by it. t0: the corners' motion, the way forward. k.xy: a texel of the search, pixels (it is written in those, as
// SMFG's LargeMain writes it); k.z: corners between the ones asked. m.zw: the last corner.
[numthreads(8, 8, 1)]
void TriLargeMain(uint3 id : SV_DispatchThreadID)
{
    const float2 centre = (id.xy + 0.5f) * inv_out * m.zw;
    float2 v[25];
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x)
            v[y * 5 + x] = t0.Load(int3(clamp(int2(round(centre + (float2(x, y) - 2.0f) * k.z)), int2(0, 0), int2(m.zw)), 0)).xy;
    float least = 1e30f;
    float2 most = 0.0f;
    for (int i = 0; i < 25; ++i)
    {
        float sum = 0.0f;
        for (int j = 0; j < 25; ++j) sum += length(v[i] - v[j]);
        if (sum < least) { least = sum; most = v[i]; }
    }
    out0[id.xy] = float4(most / k.xy, 0.0f, 0.0f);
}

// The mesh's motion as SMFG's blocks' words (for its marks, MarkMain): t0 the way forward, t1 the way back (corners,
// pixels). k.xy: corners as a share of... a block's middle in corners is read; k.zw: a texel of the search in pixels.
// m.xy: the mesh's size, corners.
[numthreads(8, 8, 1)]
void TriWordsMain(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = (id.xy + 0.5f) * inv_out;
    const float2 at = (uv * k.xy + 0.5f) / m.xy;        // k.xy: the picture in corners' steps (pixels / 8)
    out0[id.xy] = float4(t0.SampleLevel(lin, at, 0).xy / k.zw, t1.SampleLevel(lin, at, 0).xy / k.zw);
}

// Round 101, the triangles' reading of the motion for DLSS MFG and XeSS MFG (TriMotion): the way back - from the
// frame at hand to the frame before, what both ask for - as a motion picture of the size they are given theirs in.
// t0: the way back's corners (pixels of the picture). k.xy: the picture in corners' steps; k.zw: a pixel of the
// picture in texels of what is written; m.xy: the mesh's size, corners.
[numthreads(8, 8, 1)]
void TriMotionMain(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = (id.xy + 0.5f) * inv_out;
    out0[id.xy] = float4(t0.SampleLevel(lin, (uv * k.xy + 0.5f) / m.xy, 0).xy * k.zw, 0.0f, 0.0f);
}

)hlsl" R"hlsl(
// ---- 3. drawing
// t0: the mesh (TriMeshMain's). t1: the frame. k.x: how much of its way a corner goes. m.xy: squares across and down;
// m.zw: the picture, pixels. n.z: pixels between corners.

struct Drawn
{
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float stretch : TEXCOORD1;
};

float2 Corner(int2 g) { return min(float2(g) * n.z, m.zw); }
float2 Moved(int2 g) { return Corner(g) + k.x * t0.Load(int3(g, 0)).xy; }

Drawn TriVS(uint vid : SV_VertexID)
{
    const uint cell = vid / 6u, which = vid % 6u, across = uint(m.x);
    const int2 c = int2(cell % across, cell / across);
    const bool cut = t0.Load(int3(c, 0)).z > 0.5f;
    // The square's two triangles, each clockwise on the screen: cut top left to bottom right, or the other way.
    static const int2 kOne[6] = { int2(0, 0), int2(1, 0), int2(1, 1), int2(0, 0), int2(1, 1), int2(0, 1) };
    static const int2 kTwo[6] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 0), int2(1, 1), int2(0, 1) };
    const uint first = which < 3u ? 0u : 3u;
    int2 g[3];
    for (uint i = 0u; i < 3u; ++i) g[i] = c + (cut ? kOne[first + i] : kTwo[first + i]);
    const float2 a = Moved(g[0]), b = Moved(g[1]), d = Moved(g[2]);
    const float2 a0 = Corner(g[0]), b0 = Corner(g[1]), d0 = Corner(g[2]);
    const float area = (b.x - a.x) * (d.y - a.y) - (d.x - a.x) * (b.y - a.y);
    const float area0 = (b0.x - a0.x) * (d0.y - a0.y) - (d0.x - a0.x) * (b0.y - a0.y);
    const int2 mine = g[which - first];
    const float2 at = Moved(mine);
    Drawn v;
    v.stretch = saturate(abs(area - area0) / max(area0, 1e-3f));
    v.stretch = max(v.stretch, max(t0.Load(int3(g[0], 0)).w, max(t0.Load(int3(g[1], 0)).w, t0.Load(int3(g[2], 0)).w)));
    v.pos = float4(at.x / m.z * 2.0f - 1.0f, 1.0f - at.y / m.w * 2.0f, 0.02f + 0.96f * v.stretch, 1.0f);
    v.uv = Corner(mine) / m.zw;
    return v;
}

// The frame at a place between its pixels, sharply (Catmull and Rom's curve through the four pixels around, each way;
// five reads, each between pixels - the corners of the nine left out). Round 101: read the plain way - each pixel a mix
// of the four around the place - a picture moved half a pixel loses most of what changes from pixel to pixel: leaves,
// grass, gravel came out of every drawn frame softer than the real frames beside them.
float3 Sharp(float2 uv)
{
    const float2 size = m.zw, at = uv * size, mid = floor(at - 0.5f) + 0.5f, f = at - mid;
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f)), w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f)), w3 = f * f * (-0.5f + 0.5f * f);
    const float2 w12 = w1 + w2, o12 = w2 / w12;
    const float2 p0 = (mid - 1.0f) / size, p3 = (mid + 2.0f) / size, p12 = (mid + o12) / size;
    float4 sum = float4(t1.SampleLevel(lin, float2(p12.x, p0.y), 0).rgb, 1.0f) * (w12.x * w0.y);
    sum += float4(t1.SampleLevel(lin, float2(p0.x, p12.y), 0).rgb, 1.0f) * (w0.x * w12.y);
    sum += float4(t1.SampleLevel(lin, float2(p12.x, p12.y), 0).rgb, 1.0f) * (w12.x * w12.y);
    sum += float4(t1.SampleLevel(lin, float2(p3.x, p12.y), 0).rgb, 1.0f) * (w3.x * w12.y);
    sum += float4(t1.SampleLevel(lin, float2(p12.x, p3.y), 0).rgb, 1.0f) * (w12.x * w3.y);
    return max(sum.rgb / sum.a, 0.0f);
}

float4 TriPS(Drawn v) : SV_Target
{
    // (Round 100: a mark - a read-out's pixel - is not carried anywhere; k.y 1: there are marks, landed_a.)
    if (k.y > 0.5f && (landed_a.Load(int3(int2(v.uv * m.zw), 0)) & 128u) != 0u) discard;
    // (Alpha: 0 nothing was drawn here; 1 a triangle of its own size .. 0.5 one that doubled or all but vanished.)
    // (k.z 1: read sharply. Round 112, k.w over 0: only where the triangle kept its shape - stretched less than k.w; the
    //  ones stretched beside a figure that moves are read the plain way.)
    const bool sharp = k.z > 0.5f && (k.w <= 0.0f || v.stretch < k.w);
    return float4(sharp ? Sharp(v.uv) : t1.SampleLevel(lin, v.uv, 0).rgb, 1.0f - 0.5f * v.stretch);
}

// ---- 4. together
// t0: the frame before, t1: the frame after, t2: the frame before drawn at the moment, t3: the frame after drawn at
// it; landed_a: the marks. k.x: the moment; k.y: 1 marks are kept; k.z: 1 what is fine comes from one drawn frame.

// A drawn frame around a pixel, soft: itself and four places a pixel and a half out, each between four pixels - what
// is larger than a leaf. (A place nothing was drawn at, or beside one, is left out.)
float3 Soft(Texture2D<float4> drawn, float2 uv, float4 own)
{
    float4 sum = float4(own.rgb, 1.0f);
    static const float2 kOut[4] = { float2(1.5f, 0.5f), float2(-0.5f, 1.5f), float2(-1.5f, -0.5f), float2(0.5f, -1.5f) };
    for (int i = 0; i < 4; ++i)
    {
        const float4 c = drawn.SampleLevel(lin, uv + kOut[i] * inv_out, 0);
        if (c.a >= 0.5f) sum += float4(c.rgb, 1.0f);
    }
    return sum.rgb / sum.a;
}

[numthreads(8, 8, 1)]
void TriBlendMain(uint3 id : SV_DispatchThreadID)
{
    const int3 p = int3(id.xy, 0);
    const float t = k.x;
    if (k.y > 0.5f && (landed_a.Load(p) & 128u) != 0u)
    {
        out0[id.xy] = float4((t < 0.5f ? t0.Load(p) : t1.Load(p)).rgb, 1.0f);
        return;
    }
    const float4 a = t2.Load(p), b = t3.Load(p);
    const float ga = saturate(a.a * 2.0f - 1.0f), gb = saturate(b.a * 2.0f - 1.0f);
    float wa = a.a > 0.0f ? (1.0f - t) * (ga * ga + 0.02f) : 0.0f;
    float wb = b.a > 0.0f ? t * (gb * gb + 0.02f) : 0.0f;
    float3 colour;
    if (wa + wb <= 0.0f) colour = (t < 0.5f ? t0.Load(p) : t1.Load(p)).rgb;
    else if (k.z > 0.5f && wa > 0.0f && wb > 0.0f)
    {
        // Round 101, "the trees" (the owner: Lossless Scaling keeps trees and the like steadier). The two drawn frames
        // never lie on each other to the pixel - a corner's motion is a part of a pixel off - and leaves, grass,
        // gravel mixed from two pictures a little apart come out soft and doubled: a made frame kept three quarters of
        // what a real one shows there from pixel to pixel (tri_detail.py), so every real frame was sharp and the
        // frames between soft - leaves that boil. Now only what is larger than a leaf is mixed from the two; what is
        // fine is the one drawn frame's that counts for more here - one picture, not two.
        const float2 uv = (id.xy + 0.5f) * inv_out;
        const float3 la = Soft(t2, uv, a), lb = Soft(t3, uv, b);
        // (k.w: how much of what is fine is put back - 1 as it was drawn; a little over makes up what the drawing lost.)
        const float3 fine = wa >= wb ? a.rgb - la : b.rgb - lb;
        colour = saturate((la * wa + lb * wb) / (wa + wb) + fine * k.w);
    }
    else colour = (a.rgb * wa + b.rgb * wb) / (wa + wb);
    // (Tried, round 100, and left out: where both frames' triangles were stretched or not believed, the two real frames
    //  as they are instead - 59 real pairs 5.61 -> 6.40, the thin poles 2.1 -> 9.1: a stretched triangle is still nearer
    //  the truth than two frames laid over each other.)
    // Round 100: beside a mark. A read-out's pixel MarkMain did not count - a row of squares alike, where the square
    // a step along is the same colour: carried along it would not change, so nothing tells it from the picture - is
    // drawn over by the picture moving past. Here it is told by its company: the two real frames have the pixel alike,
    // what was drawn is clearly another colour, and marks stand within a few pixels. (Without the marks the same test
    // took the sky a lamp post crosses for a still thing - and, tried on every pixel, made the frames worse: round 99.)
    if (k.y > 0.5f)
    {
        const float3 fa = t0.Load(p).rgb, fb = t1.Load(p).rgb;
        const float3 same = abs(fa - fb), off = abs(colour - fa);
        if (max(same.r, max(same.g, same.b)) < 0.03f && max(off.r, max(off.g, off.b)) > 0.08f)
        {
            static const int2 kNear[8] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1) };
            const int2 last = int2(m.xy) - 1;
            uint beside = 0u;
            for (int i = 0; i < 8; ++i)
                beside |= landed_a.Load(int3(clamp(p.xy + kNear[i] * 4, int2(0, 0), last), 0)) | landed_a.Load(int3(clamp(p.xy + kNear[i] * 9, int2(0, 0), last), 0));
            if ((beside & 128u) != 0u) colour = lerp(fa, fb, t);
        }
    }
    out0[id.xy] = float4(colour, 1.0f);
}

[numthreads(8, 8, 1)]
void TriZeroMain(uint3 id : SV_DispatchThreadID)
{
    out0[id.xy] = 0.0f;
}
)hlsl";

static struct TriState
{
    UINT pitch = 16;                                        // pixels between the mesh's corners (SM_TRI_PITCH)
    int top = 3, fine_rounds = 2;                                  // the smallest size a pair that follows starts at; rounds of TriFineMain a size (SM_TRI_TOP, SM_TRI_FINE)
    winrt::com_ptr<ID3D12PipelineState> search, fine, exact, relax, mesh, at_large, words, blend, zero, draw[2];   // draw: for 8-bit frames, for HDR ones
    winrt::com_ptr<ID3D12PipelineState> to_motion;          // round 101: the corners' motion as DLSS MFG's and XeSS MFG's motion picture
    winrt::com_ptr<ID3D12Resource> motion;                  // ... that picture (TriMotion)
    UINT motion_w = 0, motion_h = 0;
    bool motion_told = false;
    winrt::com_ptr<ID3D12DescriptorHeap> rtv, dsv;
    UINT full_w = 0, full_h = 0, gw = 0, gh = 0;
    DXGI_FORMAT drawn_format = DXGI_FORMAT_UNKNOWN;
    winrt::com_ptr<ID3D12Resource> field[2][2];             // each way: the corners' motion, twice (one to write into)
    int now[2] = {};                                        // of the two, the one that holds it
    bool kept = false;                                      // they hold the pair before's
    // Round 100: each frame's brightness at half the picture's size - what the last rounds (TriFineMain, TriExactMain,
    // TriRelaxMain) read. On the frames themselves each of those rounds cost 0.2 - 0.7 ms at 2560x1440 for 14,000 corners
    // (a pair 4.4 ms, SMFG's 1.7): a full-size picture of four bytes a pixel read at 14,000 places far apart.
    winrt::com_ptr<ID3D12Resource> detail[2];               // two frames' (as MsmState::bright: by set)
    UINT dw = 0, dh = 0;
    int detail_div = 2;                                     // the picture's size over this (SM_TRI_DETAIL; 0: the frames themselves)
    winrt::com_ptr<ID3D12Resource> mesh_of[2];              // each way: the mesh
    winrt::com_ptr<ID3D12Resource> drawn[2], depth;         // each frame drawn at the moment; the depth both use
    bool failed = false;
    int full_rounds = 2; float full_reach = 1.5f;           // TriFineMain on the frames themselves: rounds, and pixels between its samples (SM_TRI_FULL, SM_TRI_REACH)
    float home = 4.0f;                                      // TriMeshMain: a corner whose way back ends further from home than this is not believed (SM_TRI_HOME, pixels of a picture 1920 across; 0: all are)
    float unfound = 0.0f;                                   // TriMeshMain: a corner's two frames this far apart - not found (SM_TRI_UNFOUND; 0: not asked - 0.15 changed nothing)
    int relax_rounds = 2; float relax_slack = 0.0007f;      // TriRelaxMain: rounds, and how much worse the neighbours' answer may explain a corner (SM_TRI_RELAX, SM_TRI_SLACK)
    int exact_rounds = 2; float exact_floor = 2e-4f;        // TriExactMain: rounds, and what counts as a slope (SM_TRI_EXACT, SM_TRI_FLOOR)
    float rounds = 2.0f;                                    // rounds of the search a size (SM_MSM_TUNE tri_rounds)
    bool split = false;                                     // round 101, TriBlendMain: what is fine from one drawn frame only (SM_TRI_SPLIT=1; measured: detail kept 0.83 -> 0.85, error 8.38 -> 8.44, a blur's reads more a frame - left off)
    bool cubic = true;                                      // round 101, TriPS: the frame read sharply (SM_TRI_CUBIC=0: the plain way)
    float sharp_stretch = 0.05f;                            // round 112, TriPS: sharply only where a triangle stretched less than this (SM_TRI_SHARP_STRETCH; 0: everywhere)
                                                            // (x4, 6 recordings - edges / busy / whole / detail kept: everywhere 16.29 / 14.84 / 7.81 / 0.842,
                                                            //  0.05: 16.21 / 14.74 / 7.76 / 0.811, plain 16.34 / 14.85 / 7.79 / 0.764, SMFG 15.72 / 14.43 / 7.61 / 0.763)
    float crisp = 1.0f;                                     // round 101, TriBlendMain: how much of what is fine is put back (SM_TRI_CRISP)
    float relax_share = 0.0f;                               // round 101, TriRelaxMain: the neighbours' answer may be this share worse than a corner's own (SM_TRI_SHARE)
} g_tri;

static UINT s_pitch();
static void TriClose()
{
    TriState &s = g_tri;
    for (auto &way : s.field) for (auto &texture : way) texture = nullptr;
    for (auto &texture : s.mesh_of) texture = nullptr;
    for (auto &texture : s.detail) texture = nullptr;
    for (auto &texture : s.drawn) texture = nullptr;
    s.depth = nullptr;
    s.motion = nullptr;
    s.motion_w = s.motion_h = 0;
    s.kept = false;
    s.full_w = s.full_h = s.gw = s.gh = 0;
}

static UINT s_pitch() { return g_tri.pitch; }
static bool TriOn() { return g_msm.tune.tri > 0.0f && !g_tri.failed && g_tri.depth; }

// Called by MsmReady when the triangles are asked for: the passes (once) and what they work in, for this size.
static bool TriReady(UINT full_w, UINT full_h, DXGI_FORMAT format)
{
    TriState &s = g_tri;
    if (s.failed) return false;
    auto fail = [&](const char *what) {
        s.failed = true;
        TriClose();
        Trace("[tri] unavailable (SMFG's own frames instead): %s", what);
        return false;
    };
    if (!s.blend)
    {
        winrt::com_ptr<ID3DBlob> errors;
        struct { const char *entry; winrt::com_ptr<ID3D12PipelineState> *pass; } passes[] = {
            {"TriSearchMain", &s.search}, {"TriFineMain", &s.fine}, {"TriExactMain", &s.exact}, {"TriRelaxMain", &s.relax}, {"TriMeshMain", &s.mesh}, {"TriLargeMain", &s.at_large}, {"TriWordsMain", &s.words},
            {"TriZeroMain", &s.zero}, {"TriMotionMain", &s.to_motion}, {"TriBlendMain", &s.blend} };
        for (auto &pass : passes)
        {
            winrt::com_ptr<ID3DBlob> code;
            errors = nullptr;
            if (FAILED(CachedCompile(kTriHlsl, sizeof(kTriHlsl) - 1, "msm-tri.hlsl", nullptr, nullptr, pass.entry, "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put())))
            {
                s.blend = nullptr;
                return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "a pass did not compile");
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = g_msm.root.get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            if (FAILED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pass.pass->put())))) { s.blend = nullptr; return fail("a pass's pipeline"); }
        }
        winrt::com_ptr<ID3DBlob> vs, ps;
        if (FAILED(CachedCompile(kTriHlsl, sizeof(kTriHlsl) - 1, "msm-tri.hlsl", nullptr, nullptr, "TriVS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs.put(), errors.put())) ||
            FAILED(CachedCompile(kTriHlsl, sizeof(kTriHlsl) - 1, "msm-tri.hlsl", nullptr, nullptr, "TriPS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps.put(), errors.put())))
        {
            s.blend = nullptr;
            return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "the drawing did not compile");
        }
        for (int hdr = 0; hdr < 2; ++hdr)
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = g_msm.root.get();
            pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;        // a triangle that turned over is not drawn
            pso.RasterizerState.DepthClipEnable = TRUE;
            pso.DepthStencilState.DepthEnable = TRUE;
            pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            pso.DSVFormat = DXGI_FORMAT_D16_UNORM;
            pso.SampleDesc.Count = 1;
            if (FAILED(lfCore.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(s.draw[hdr].put())))) { s.blend = nullptr; return fail("the drawing's pipeline"); }
        }
        D3D12_DESCRIPTOR_HEAP_DESC rtv = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        D3D12_DESCRIPTOR_HEAP_DESC dsv = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        if (FAILED(lfCore.device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(s.rtv.put()))) ||
            FAILED(lfCore.device->CreateDescriptorHeap(&dsv, IID_PPV_ARGS(s.dsv.put())))) { s.blend = nullptr; return fail("its descriptor heaps"); }
        char rounds[16] = {};
        if (GetEnvironmentVariableA("SM_TRI_ROUNDS", rounds, sizeof(rounds)) > 0) s.rounds = static_cast<float>(std::clamp(atoi(rounds), 1, 8));
        if (GetEnvironmentVariableA("SM_TRI_FULL", rounds, sizeof(rounds)) > 0) s.full_rounds = std::clamp(atoi(rounds), 0, 4);
        if (GetEnvironmentVariableA("SM_TRI_REACH", rounds, sizeof(rounds)) > 0) s.full_reach = static_cast<float>(atof(rounds));
        if (GetEnvironmentVariableA("SM_TRI_HOME", rounds, sizeof(rounds)) > 0) s.home = static_cast<float>(atof(rounds));
        if (GetEnvironmentVariableA("SM_TRI_RELAX", rounds, sizeof(rounds)) > 0) s.relax_rounds = std::clamp(atoi(rounds), 0, 6);
        if (GetEnvironmentVariableA("SM_TRI_SLACK", rounds, sizeof(rounds)) > 0) s.relax_slack = static_cast<float>(atof(rounds));
        if (GetEnvironmentVariableA("SM_TRI_UNFOUND", rounds, sizeof(rounds)) > 0) s.unfound = static_cast<float>(atof(rounds));
        if (GetEnvironmentVariableA("SM_TRI_EXACT", rounds, sizeof(rounds)) > 0) s.exact_rounds = std::clamp(atoi(rounds), 0, 6);
        if (GetEnvironmentVariableA("SM_TRI_FLOOR", rounds, sizeof(rounds)) > 0) s.exact_floor = static_cast<float>(atof(rounds));
        if (GetEnvironmentVariableA("SM_TRI_DETAIL", rounds, sizeof(rounds)) > 0) s.detail_div = std::clamp(atoi(rounds), 0, 4);
        if (GetEnvironmentVariableA("SM_TRI_PITCH", rounds, sizeof(rounds)) > 0) s.pitch = static_cast<UINT>(std::clamp(atoi(rounds), 4, 32));
        if (GetEnvironmentVariableA("SM_TRI_TOP", rounds, sizeof(rounds)) > 0) s.top = std::clamp(atoi(rounds), 1, 4);
        if (GetEnvironmentVariableA("SM_TRI_FINE", rounds, sizeof(rounds)) > 0) s.fine_rounds = std::clamp(atoi(rounds), 0, 4);
        if (GetEnvironmentVariableA("SM_TRI_SPLIT", rounds, sizeof(rounds)) > 0) s.split = rounds[0] != '0';
        if (GetEnvironmentVariableA("SM_TRI_CUBIC", rounds, sizeof(rounds)) > 0) s.cubic = rounds[0] != '0';
        if (GetEnvironmentVariableA("SM_TRI_SHARP_STRETCH", rounds, sizeof(rounds)) > 0) s.sharp_stretch = std::clamp(static_cast<float>(atof(rounds)), 0.0f, 1.0f);
        if (GetEnvironmentVariableA("SM_TRI_SHARE", rounds, sizeof(rounds)) > 0) s.relax_share = std::clamp(static_cast<float>(atof(rounds)), 0.0f, 1.0f);
        if (GetEnvironmentVariableA("SM_TRI_CRISP", rounds, sizeof(rounds)) > 0) s.crisp = std::clamp(static_cast<float>(atof(rounds)), 0.0f, 3.0f);
    }
    const DXGI_FORMAT drawn_format = (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM) ? DXGI_FORMAT_R8G8B8A8_UNORM
                                                                                                                 : DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (s.full_w == full_w && s.full_h == full_h && s.drawn_format == drawn_format && s.depth) return true;
    TriClose();
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](UINT tw, UINT th, DXGI_FORMAT tf, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
                    winrt::com_ptr<ID3D12Resource> &texture) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = tf; d.SampleDesc.Count = 1;
        d.Flags = flags;
        return SUCCEEDED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, clear, IID_PPV_ARGS(texture.put())));
    };
    s.gw = (full_w + s_pitch() - 1) / s_pitch() + 1;
    s.gh = (full_h + s_pitch() - 1) / s_pitch() + 1;
    for (int way = 0; way < 2; ++way)
    {
        for (int turn = 0; turn < 2; ++turn)
            if (!make(s.gw, s.gh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                      nullptr, s.field[way][turn])) return fail("its pictures");
        if (!make(s.gw, s.gh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  nullptr, s.mesh_of[way])) return fail("its pictures");
        D3D12_CLEAR_VALUE none = {};
        none.Format = drawn_format;
        if (!make(full_w, full_h, drawn_format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &none, s.drawn[way]))
            return fail("its pictures");
    }
    s.dw = full_w / static_cast<UINT>(std::max(1, s.detail_div)); s.dh = full_h / static_cast<UINT>(std::max(1, s.detail_div));
    for (int set = 0; set < 2 && s.detail_div > 0; ++set)
        if (!make(s.dw, s.dh, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                  nullptr, s.detail[set])) return fail("its pictures");
    D3D12_CLEAR_VALUE far_away = {};
    far_away.Format = DXGI_FORMAT_D16_UNORM;
    far_away.DepthStencil.Depth = 1.0f;
    if (!make(full_w, full_h, DXGI_FORMAT_D16_UNORM, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_DEPTH_WRITE, &far_away, s.depth)) return fail("its pictures");
    const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE at = s.rtv->GetCPUDescriptorHandleForHeapStart();
    for (int way = 0; way < 2; ++way, at.ptr += each) lfCore.device->CreateRenderTargetView(s.drawn[way].get(), nullptr, at);
    lfCore.device->CreateDepthStencilView(s.depth.get(), nullptr, s.dsv->GetCPUDescriptorHandleForHeapStart());
    s.full_w = full_w; s.full_h = full_h; s.drawn_format = drawn_format;
    Trace("[tri] triangles: a mesh of %u x %u corners (one every %u pixels), %u triangles, drawn by the rasteriser at %ux%u",
          s.gw, s.gh, s_pitch(), (s.gw - 1) * (s.gh - 1) * 2, full_w, full_h);
    return true;
}

// The pair's own work, in place of MsmPairSearch + MsmPairFinish.
// (only: 0 or 1 - that way's motion and nothing else, for TriMotion; -1 all of it.)
static void TriPair(ID3D12GraphicsCommandList *list, ID3D12Resource *before, ID3D12Resource *after, D3D12_RESOURCE_STATES rest, const MsmAsk &ask, int only)
{
    MsmState &s = g_msm;
    TriState &t = g_tri;
    s.pair = (s.pair + 1) % MsmState::kPairs;
    s.pass = 0;
    s.zero_mean_now = 0.0f;
    MsmBegin(list);
    const bool readable = (rest & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (!readable)
    {
        D3D12_RESOURCE_BARRIER open[] = { Barrier(before, rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Barrier(after, rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
        list->ResourceBarrier(2, open);
    }
    int set_before = -1;
    if (s.chained) for (int set = 0; set < 2; ++set) if (s.holds[set] == before) set_before = set;
    const bool follows = set_before >= 0 && t.kept;
    // (Each frame's brightness at half size beside SMFG's own: made once a frame, as that is.)
    auto detail = [&](ID3D12Resource *frame, int set) {
        if (!t.detail[set]) return;
        MsmPass p;
        p.state = s.luma.get();
        p.read[0] = frame; p.write = t.detail[set].get();
        p.w = t.dw; p.h = t.dh;
        p.inv_a[0] = 1.0f / s.full_w; p.inv_a[1] = 1.0f / s.full_h;
        MsmRecord(list, p);
    };
    if (set_before < 0) { set_before = 0; MsmBrightness(list, before, 0); detail(before, 0); }
    const int set_after = 1 - set_before;
    MsmBrightness(list, after, set_after);
    detail(after, set_after);
    s.before = set_before;
    s.chained = true;
    s.fast = ask.fast;
    const float per_x = static_cast<float>(s.full_w) / s.w, per_y = static_cast<float>(s.full_h) / s.h;
    for (int way = 0; way < 2; ++way)
    {
        if (only >= 0 && way != only) continue;
        const int from = way == 0 ? set_before : set_after, to = way == 0 ? set_after : set_before;
        const float sign = way == 0 ? 1.0f : -1.0f;
        bool nothing = !follows;
        auto pass = [&](ID3D12PipelineState *state, UINT level) {
            MsmPass p;
            p.state = state;
            p.read[0] = s.bright[from][level].get(); p.read[1] = s.bright[to][level].get();
            p.read[2] = t.field[way][t.now[way]].get();
            t.now[way] = 1 - t.now[way];
            p.write = t.field[way][t.now[way]].get();
            p.inv_a[0] = 1.0f / s.lw[level]; p.inv_a[1] = 1.0f / s.lh[level];
            p.w = t.gw; p.h = t.gh;
            p.n[0] = 1.0f / s.full_w; p.n[1] = 1.0f / s.full_h; p.n[2] = static_cast<float>(s_pitch());
            p.m[0] = sign * ask.expected_x; p.m[1] = sign * ask.expected_y;
            p.m[2] = static_cast<float>(t.gw - 1); p.m[3] = static_cast<float>(t.gh - 1);
            p.o[0] = nothing ? 1.0f : 0.0f;
            p.o[2] = per_x * static_cast<float>(1u << level); p.o[3] = per_y * static_cast<float>(1u << level);
            nothing = false;
            return p;
        };
        // From the small sizes to the large ones. A pair that follows the one before starts from that pair's answer
        // and needs fewer rounds; one that does not (the first, after a cut) starts from nothing, further down.
        const int rounds = static_cast<int>(t.rounds) + (follows ? 0 : 2);
        for (int level = follows ? t.top : 4; level >= 1; --level)
            for (int round = 0; round < (level >= 3 ? std::max(1, rounds - 1) : rounds); ++round)
                for (int colour = 0; colour < 2; ++colour)
                {
                    MsmPass p = pass(t.search.get(), static_cast<UINT>(level));
                    p.k[0] = static_cast<float>(colour); p.k[1] = static_cast<float>(round * 3 + level);
                    p.k[2] = (round & 1) ? -1.0f : 1.0f;
                    p.k[3] = per_x * static_cast<float>(1u << level);
                    MsmRecord(list, p);
                }
        for (int level = 1; level >= 0; --level)
            for (int round = 0; round < t.fine_rounds; ++round)
                MsmRecord(list, pass(t.fine.get(), static_cast<UINT>(level)));
        // The last rounds, on the brightness at half the picture's size (or the frames themselves): a pixel or two
        // either side; to a part of a pixel; what is plain takes its neighbours' word; to a part of a pixel once more.
        const bool halved = t.detail[0] && t.detail[1];
        const float texel = halved ? static_cast<float>(t.detail_div) : 1.0f;
        auto last = [&](ID3D12PipelineState *state, float k0, float k1, float step, float k2 = 0.0f) {
            MsmPass p = pass(state, 0);
            if (halved)
            {
                p.read[0] = t.detail[from].get(); p.read[1] = t.detail[to].get();
                p.inv_a[0] = 1.0f / t.dw; p.inv_a[1] = 1.0f / t.dh;
            }
            else
            {
                p.read[0] = way == 0 ? before : after; p.read[1] = way == 0 ? after : before;
                p.inv_a[0] = t.full_reach / s.full_w; p.inv_a[1] = t.full_reach / s.full_h;
                p.q[0] = 1.0f;
            }
            p.k[0] = k0; p.k[1] = k1; p.k[2] = k2; p.k[3] = 0.0f;
            p.o[2] = p.o[3] = step;
            MsmRecord(list, p);
        };
        for (int round = 0; round < t.full_rounds; ++round) last(t.fine.get(), 0.0f, 0.0f, round == 0 ? 1.5f : 0.75f);
        for (int round = 0; round < t.exact_rounds; ++round) last(t.exact.get(), t.exact_floor, 0.0f, texel);
        for (int round = 0; round < t.relax_rounds; ++round) last(t.relax.get(), t.relax_slack, 1.5f * s.full_w / 1920.0f, texel, t.relax_share);
        if (t.relax_rounds > 0 && t.exact_rounds > 0) last(t.exact.get(), t.exact_floor, 0.0f, texel);
    }
    if (only >= 0)
    {
        t.kept = true;
        s.marks_hold = 0;
        if (!readable)
        {
            D3D12_RESOURCE_BARRIER shut[] = { Barrier(before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest),
                                              Barrier(after, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest) };
            list->ResourceBarrier(2, shut);
        }
        return;
    }
    // How the picture at large moves (the way forward), into SMFG's own picture for it.
    const UINT top = MsmState::kLevels - 1;
    {
        MsmPass p;
        p.state = t.at_large.get();
        p.read[0] = t.field[0][t.now[0]].get();
        p.write = s.large[0].get();
        p.w = s.bw[top]; p.h = s.bh[top];
        p.k[0] = per_x; p.k[1] = per_y;
        p.k[2] = std::max(1.0f, std::round(static_cast<float>(s.full_w) / s.bw[top] / s_pitch() * 0.6f));
        p.m[2] = static_cast<float>(t.gw - 1); p.m[3] = static_cast<float>(t.gh - 1);
        MsmRecord(list, p);
    }
    // The mesh, each way - after both ways' motion: a corner asks the other way whether its own leads home.
    const bool marked = ask.marks && s.marks_hold == 2;         // (the pair before's marks)
    for (int way = 0; way < 2; ++way)
    {
        MsmPass m;
        m.state = t.mesh.get();
        m.read[0] = t.field[way][t.now[way]].get(); m.read[1] = s.bright[way == 0 ? set_before : set_after][1].get();
        m.read[2] = t.field[1 - way][t.now[1 - way]].get();
        m.write = t.mesh_of[way].get();
        m.w = t.gw; m.h = t.gh;
        m.k[0] = 2.0f * s.full_w / 2560.0f + 0.5f; m.k[1] = 0.05f; m.k[2] = 0.02f; m.k[3] = t.home * s.full_w / 1920.0f;
        m.n[0] = 1.0f / s.full_w; m.n[1] = 1.0f / s.full_h; m.n[2] = static_cast<float>(s_pitch());
        m.m[2] = static_cast<float>(t.gw - 1); m.m[3] = static_cast<float>(t.gh - 1);
        if (marked)
        {
            m.reached[0] = s.marks[s.marks_now].get(); m.read[3] = s.large[0].get();
            m.q[1] = 1.0f; m.q[2] = per_x; m.q[3] = per_y; m.o[1] = way == 0 ? 1.0f : -1.0f;
        }
        m.q[0] = t.relax_rounds > 0 ? t.unfound : 0.0f;
        MsmRecord(list, m);
    }
    t.kept = true;
    // The mesh's motion as SMFG's blocks' words: its marks are found from them (MsmMarksFromWords, as SMAI's are).
    const bool marks = ask.marks && s.net_words;
    if (marks)
    {
        MsmPass w;
        w.state = t.words.get();
        w.read[0] = t.field[0][t.now[0]].get(); w.read[1] = t.field[1][t.now[1]].get();
        w.write = s.net_words.get();
        w.w = s.bw[0]; w.h = s.bh[0];
        w.k[0] = static_cast<float>(s.full_w) / s_pitch(); w.k[1] = static_cast<float>(s.full_h) / s_pitch();
        w.k[2] = per_x; w.k[3] = per_y;
        w.m[0] = static_cast<float>(t.gw); w.m[1] = static_cast<float>(t.gh);
        MsmRecord(list, w);
    }
    if (!readable)
    {
        D3D12_RESOURCE_BARRIER shut[] = { Barrier(before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest),
                                          Barrier(after, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest) };
        list->ResourceBarrier(2, shut);
    }
    if (marks) MsmMarksFromWords(list, before, after, rest, ask, s.large[0].get());
    else s.marks_hold = 0;
}

// Round 101 (the owner: "the triangles for every kind of MFG - they read the motion better and lighter"). DLSS MFG and
// XeSS MFG make their frames themselves; what they can be given is the motion - and left to themselves here they are
// given none (a picture of noughts: the engine has no game's motion vectors to hand over). With "Triangle Mesh" on
// they are handed the mesh's: the way back's search alone (half a pair's work of the triangles' - no mesh, no marks,
// nothing drawn), written as a motion picture of the size theirs is. Null: not now (no room, not ready) - the
// generator finds the motion itself, as it did.
static ID3D12Resource *TriMotion(ID3D12GraphicsCommandList *list, ID3D12Resource *before, ID3D12Resource *after, D3D12_RESOURCE_STATES rest,
                                 UINT full_w, UINT full_h, DXGI_FORMAT format, int share, UINT motion_w, UINT motion_h)
{
    MsmState &s = g_msm;
    TriState &t = g_tri;
    if (s.failed || t.failed || motion_w == 0 || motion_h == 0) return nullptr;
    s.tune.tri = 1.0f;
    if ((s.full_w != full_w || s.full_h != full_h || !TriOn()) && !(MsmReady(full_w, full_h, format, share) && TriOn())) return nullptr;
    if (!t.motion || t.motion_w != motion_w || t.motion_h != motion_h)
    {
        t.motion = nullptr;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = motion_w; d.Height = motion_h; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16_FLOAT; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                                          IID_PPV_ARGS(t.motion.put())))) return nullptr;
        t.motion_w = motion_w; t.motion_h = motion_h;
    }
    if (!t.motion_told)
    {
        t.motion_told = true;
        Trace("[tri] the motion for this generator is read by the triangles: %u x %u corners, handed over as a motion picture of %ux%u", t.gw, t.gh, motion_w, motion_h);
    }
    MsmAsk ask;
    ask.marks = false;
    TriPair(list, before, after, rest, ask, 1);
    MsmPass p;
    p.state = t.to_motion.get();
    p.read[0] = t.field[1][t.now[1]].get();
    p.write = t.motion.get();
    p.w = motion_w; p.h = motion_h;
    p.k[0] = static_cast<float>(s.full_w) / s_pitch(); p.k[1] = static_cast<float>(s.full_h) / s_pitch();
    p.k[2] = static_cast<float>(motion_w) / s.full_w; p.k[3] = static_cast<float>(motion_h) / s.full_h;
    p.m[0] = static_cast<float>(t.gw); p.m[1] = static_cast<float>(t.gh);
    MsmRecord(list, p);
    return t.motion.get();
}

// A frame in between of the pair TriPair was last called for, in place of MsmFrame.
static void TriFrame(ID3D12GraphicsCommandList *list, ID3D12Resource *before, ID3D12Resource *after, D3D12_RESOURCE_STATES rest,
                     ID3D12Resource *into, D3D12_RESOURCE_STATES into_rest, float moment)
{
    MsmState &s = g_msm;
    TriState &t = g_tri;
    MsmBegin(list);
    list->SetGraphicsRootSignature(s.root.get());
    // The frames are read by the drawing (a pixel shader) and by the pass that puts the frame together.
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const bool readable = (rest & read) == read;
    if (!readable)
    {
        D3D12_RESOURCE_BARRIER open[2] = { Barrier(before, rest, read), Barrier(after, rest, read) };
        list->ResourceBarrier(2, open);
    }
    const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_VIEWPORT view = {0.0f, 0.0f, static_cast<float>(s.full_w), static_cast<float>(s.full_h), 0.0f, 1.0f};
    const D3D12_RECT all = {0, 0, static_cast<LONG>(s.full_w), static_cast<LONG>(s.full_h)};
    list->RSSetViewports(1, &view);
    list->RSSetScissorRects(1, &all);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->SetPipelineState(t.draw[t.drawn_format == DXGI_FORMAT_R8G8B8A8_UNORM ? 0 : 1].get());
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = t.dsv->GetCPUDescriptorHandleForHeapStart();
    // (The marks: read by the drawing too - a read-out's pixel is not carried anywhere.)
    ID3D12Resource *const kept = s.marks_hold == 2 ? s.marks[s.marks_now].get() : nullptr;
    if (kept)
    {
        auto seen = Barrier(kept, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, read);
        list->ResourceBarrier(1, &seen);
    }
    for (int way = 0; way < 2 && s.pass < MsmState::kPasses; ++way)
    {
        // The pass's table: the mesh (t0) and the frame (t1); the rest empty.
        const UINT first = (s.pair * MsmState::kPasses + s.pass) * MsmState::kSlots;
        ++s.pass;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = s.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(first) * each;
        gpu.ptr += static_cast<UINT64>(first) * each;
        D3D12_SHADER_RESOURCE_VIEW_DESC none = {};
        none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        none.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC none_written = {};
        none_written.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ID3D12Resource *const reads[2] = { t.mesh_of[way].get(), way == 0 ? before : after };
        D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
        for (int i = 0; i < 8; ++i, at.ptr += each)
        {
            none.Format = i < 6 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R32_UINT;
            ID3D12Resource *const one = i < 2 ? reads[i] : i == 6 ? kept : nullptr;
            lfCore.device->CreateShaderResourceView(one, one ? nullptr : &none, at);
        }
        none_written.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        lfCore.device->CreateUnorderedAccessView(nullptr, nullptr, &none_written, at);
        at.ptr += each;
        none_written.Format = DXGI_FORMAT_R32_UINT;
        lfCore.device->CreateUnorderedAccessView(nullptr, nullptr, &none_written, at);
        float numbers[28] = {};
        numbers[4] = way == 0 ? moment : 1.0f - moment;                                                 // k.x
        numbers[5] = kept ? 1.0f : 0.0f;                                                                // k.y
        numbers[6] = t.cubic ? 1.0f : 0.0f;                                                             // k.z
        numbers[7] = t.sharp_stretch;                                                                   // k.w
        numbers[8] = static_cast<float>(t.gw - 1); numbers[9] = static_cast<float>(t.gh - 1);           // m.xy
        numbers[10] = static_cast<float>(s.full_w); numbers[11] = static_cast<float>(s.full_h);         // m.zw
        numbers[14] = static_cast<float>(s_pitch());                                             // n.z
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = t.rtv->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(way) * rtv_each;
        auto open = Barrier(t.drawn[way].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &open);
        const float nothing[4] = {};
        list->ClearRenderTargetView(rtv, nothing, 0, nullptr);
        list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        list->SetGraphicsRoot32BitConstants(0, 28, numbers, 0);
        list->SetGraphicsRootDescriptorTable(1, gpu);
        list->DrawInstanced((t.gw - 1) * (t.gh - 1) * 6, 1, 0, 0);
        auto shut = Barrier(t.drawn[way].get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &shut);
    }
    if (g_msm_mark) g_msm_mark(list, "triangles drawn");
    D3D12_RESOURCE_BARRIER open = Barrier(into, into_rest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &open);
    {
        MsmPass p;
        p.state = t.blend.get();
        p.read[0] = before; p.read[1] = after; p.read[2] = t.drawn[0].get(); p.read[3] = t.drawn[1].get();
        const bool marks = kept != nullptr;
        if (marks) p.reached[0] = kept;
        p.write = into; p.own = false;
        p.w = s.full_w; p.h = s.full_h;
        p.k[0] = moment; p.k[1] = marks ? 1.0f : 0.0f; p.k[2] = t.split ? 1.0f : 0.0f; p.k[3] = t.crisp;
        p.m[0] = static_cast<float>(s.full_w); p.m[1] = static_cast<float>(s.full_h);
        MsmRecord(list, p);
    }
    D3D12_RESOURCE_BARRIER shut[3] = { Barrier(into, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, into_rest), Barrier(before, read, rest), Barrier(after, read, rest) };
    list->ResourceBarrier(readable ? 1 : 3, shut);
    if (kept)
    {
        auto rests = Barrier(kept, read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &rests);
    }
}
