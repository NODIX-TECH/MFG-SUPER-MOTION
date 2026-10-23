// msm_fg.h - MSM FG 1.1 (SMFG 1.1 in the app): frames in between of the engine's own making.
//
// DLSS MFG and XeSS MFG are other people's generators: each runs on the cards
// its maker lets it run on, costs what it costs, and is closed. This one is
// plain D3D12 compute - any card that runs the engine runs it - and every
// step of it is in this file.
//
// What it is given: the real frame before and the real frame after, at the
// picture's own size. What it makes: the picture at any moment between them,
// at that size, from those two frames' own pixels - nothing is made small and
// scaled up, so nothing has to be rebuilt afterwards (sm_rebuild.inl).
//
// Once a pair:
//   1. Brightness (LumaMain, HalveMain). Each frame's brightness at the size
//      the motion is looked for at ("Flow" in the app, a share of the
//      picture), and four times halved. A frame's is kept: it is the frame
//      before of the next pair.
//   2. Search (SearchMain, RefineMain, RelaxMain). For every block of two by
//      two texels, where its four by four neighbourhood lies in the other
//      frame. At the smallest size every place within four texels is tried -
//      around "not at all", and around where the picture as a whole is
//      expected to have gone when the engine knows (its own count of the
//      picture's movement, or what Adaptive Motion Learning expects of the
//      mouse); each larger size starts from what the blocks around said at
//      the size below (a block takes the word of any of nine) and looks one
//      texel further.
//      A block that still was not found at the second largest size - a thin
//      thing going its own way, a lamp post rushing past a car: too thin to
//      show at the smaller sizes, further off than one texel a size reaches -
//      is looked for far and wide there (FarMain).
//      Relax: a block takes a neighbour's movement where that explains it as
//      well - what is plain (a wall, the sky) moves with what is around it, a
//      thing of its own keeps its own.
//      Both ways: from the frame before to the one after, and back. A piece
//      of picture seen in one frame only (a thing appearing, what another
//      covers) is found by neither. And what one way found, the other is
//      told (CrossSplatMain, CrossMain): a thin thing - a lamp post - is
//      often caught from one frame only, and where it lands in the other
//      frame the block there is offered the same way back.
//   3. Settle (SubpelMain). Parts of a texel, from how the match falls off to
//      either side; whether the block was found at all (it differs from where
//      it was put by no more than its own contrast allows) and whether the
//      way back from there leads home. And the picture at large (LargeMain):
//      for every sixteen by sixteen blocks, how most of the blocks found in
//      and around them move - what a thing of its own stands out from. And
//      what a block that shows little goes by (FillMain): a soft road
//      matches as well a long way off as where it is.
//   4. Marks (MarkMain, at the picture's size, kept from pair to pair). A
//      read-out - a figure, a line, a filled bar - stands on the screen while
//      the picture moves behind it; so does the player's own car seen from
//      behind. That the two real frames have a pixel alike does not tell
//      one: in a soft picture half the pixels are alike from one frame to the
//      next while all of it moves, and a piece of sky is alike before and
//      after a lamp post crosses it. What tells it: had the pixel gone the
//      way its place goes it would be clearly another colour now, and it is
//      the same. Seen in four pairs more than not, the pixel is a mark, and
//      one until it changes or is seen to go with its place again.
// Once a frame in between, for its moment t:
//   5. Carry (SplatMain). Every block of the frame before is carried t of its
//      way, every block of the frame after 1 - t of its way back, and says at
//      the place it reaches how it moves. Two that reach one place: the one
//      found beats the one not found; of two found, the one that moves least
//      like the picture around it does (a lamp post in front of a street, a
//      car on a road: what stands out is in front).
//   6. Resolve (ResolveMain). At every place: both frames say the same - the
//      frame is theirs together. Only one reached it - the other does not
//      show this piece (it is covered there, or not yet there): that frame
//      alone. Neither: what the places around say. And where the two frames
//      do not show the same under the place's word but do under the picture
//      at large's, that is the word taken.
//      Then (EdgeMain) how far each place's word is from the words of the
//      eight around it: where two things moving differently meet.
//   7. Draw (WarpMain, at the picture's size). A mark stays as it is. Every
//      other pixel from where it lies in the frame before and where it lies
//      in the frame after - from the nearer of the two alone unless the
//      moment is half way: two real frames laid over each other are never
//      quite in register, and a frame in between that is softer than the
//      real ones around it is seen. Where it would be taken from a mark, the
//      other frame gives it.
//      Between places that say the same, a pixel goes by their words mixed.
//      Between places that do not - the edge of a lamp post against the sky,
//      of a car against the road - it takes one of their words whole: the
//      one under which the two real frames show the same there. Mixed, the
//      words of two such places are neither's: a pixel between a post that
//      moves two hundred pixels and a sky that moves none was taken from
//      somewhere along those two hundred, and the post came out as a thin
//      outline of itself around nothing (the "squares" and the "glow").
//
// Nothing here waits for the card or reads anything back: a pair's passes go
// into the list they are given.
//
// It leans on four things of the engine's: lfCore.device, CachedCompile,
// Barrier and Trace (tests\msmtruth.cpp brings its own of each).
#pragma once

static const char kMsmHlsl[] = R"hlsl(
cbuffer Numbers : register(b0)
{
    float2 inv_a;       // one texel of what is read first, as a share of it
    float2 inv_out;     // one texel of what is written
    float4 k;           // the pass's own numbers
    float4 m;
    float4 n;
    float4 o;
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

static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);

float Most(float3 v) { return max(v.r, max(v.g, v.b)); }

// ---- 1. brightness

[numthreads(8, 8, 1)]
void LumaMain(uint3 id : SV_DispatchThreadID)
{
    // Nine samples a third of a texel of this size apart, each the mean of four of the picture's, the middle
    // ones counted more: all the picture has under this texel and a little around it. What is finer than this
    // size must be gone from it - left in, it shows as a pattern of its own that moves its own way.
    const float2 uv = (id.xy + 0.5f) * inv_out, q = inv_out / 3.0f;
    float3 sum = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += t0.SampleLevel(lin, uv + float2(x, y) * q, 0).rgb * ((x == 0 ? 2.0f : 1.0f) * (y == 0 ? 2.0f : 1.0f));
    out0[id.xy] = dot(sum * 0.0625f, kLuma);
}

[numthreads(8, 8, 1)]
void HalveMain(uint3 id : SV_DispatchThreadID)
{
    // Four by four texels, weighed 1 3 3 1 each way (four samples between texels), for the same reason.
    const float2 uv = (id.xy + 0.5f) * inv_out, q = 0.375f * inv_out;
    out0[id.xy] = 0.25f * (t0.SampleLevel(lin, uv + float2(-q.x, -q.y), 0).r + t0.SampleLevel(lin, uv + float2(q.x, -q.y), 0).r +
                           t0.SampleLevel(lin, uv + float2(-q.x, q.y), 0).r + t0.SampleLevel(lin, uv + float2(q.x, q.y), 0).r);
}

// ---- 2. search

// A block's neighbourhood: four by four texels around the corner its own four meet at.
struct Patch { float4 a, b, c, d; };

Patch Read(Texture2D<float4> from, float2 corner)
{
    Patch p;
    p.a = from.GatherRed(lin, (corner + float2(-1.0f, -1.0f)) * inv_a);
    p.b = from.GatherRed(lin, (corner + float2(1.0f, -1.0f)) * inv_a);
    p.c = from.GatherRed(lin, (corner + float2(-1.0f, 1.0f)) * inv_a);
    p.d = from.GatherRed(lin, (corner + float2(1.0f, 1.0f)) * inv_a);
    return p;
}

float Apart(Patch x, Patch y)
{
    return dot(abs(x.a - y.a) + abs(x.b - y.b) + abs(x.c - y.c) + abs(x.d - y.d), 1.0f);
}

// A neighbourhood's own contrast: how far its texels are from their mean.
float Contrast(Patch p)
{
    const float mean = dot(p.a + p.b + p.c + p.d, 1.0f) * 0.0625f;
    return dot(abs(p.a - mean) + abs(p.b - mean) + abs(p.c - mean) + abs(p.d - mean), 1.0f);
}

// How much a block's word counts, 0 .. 1: it shows something (k.z: so much contrast is enough) and was found
// (it differs from where it was put by less than its contrast and k.w). What is plain says nothing about how it
// moves: the blocks around say it.
float Sure(float contrast, float apart)
{
    return saturate(contrast * k.z) * saturate(1.0f - apart / (k.w + contrast));
}

// The smallest size: every place within four texels of "not at all" and of where the picture as a whole is
// expected to have gone (n.xy, texels of this size; none: 0). k.x: what a texel further from the nearer of
// the two costs (the nearer of two alike).
[numthreads(8, 8, 1)]
void SearchMain(uint3 id : SV_DispatchThreadID)
{
    const float2 corner = id.xy * 2.0f + 1.0f;
    const Patch here = Read(t0, corner);
    const float2 expected = round(n.xy);
    float least = 1e9f, apart = 0.0f;
    float2 found = 0.0f;
    for (int y = -4; y <= 4; ++y)
        for (int x = -4; x <= 4; ++x)
        {
            const float2 v = float2(x, y);
            const float d = Apart(here, Read(t1, corner + v)), e = d + k.x * min(length(v), length(v - expected));
            if (e < least) { least = e; found = v; apart = d; }
        }
    if (any(abs(expected) > 0.5f))
        for (int j = -4; j <= 4; ++j)
            for (int i = -4; i <= 4; ++i)
            {
                const float2 v = expected + float2(i, j);
                const float d = Apart(here, Read(t1, corner + v)), e = d + k.x * min(length(v), length(float2(i, j)));
                if (e < least) { least = e; found = v; apart = d; }
            }
    out0[id.xy] = float4(found, apart, Sure(Contrast(here), apart));
}

// A larger size: from what the nine blocks around said at the size below, and "not at all"; then one texel
// further. k.x: what a texel away from the block's own word below costs. m.xy: the last block of the size below.
[numthreads(8, 8, 1)]
void RefineMain(uint3 id : SV_DispatchThreadID)
{
    const float2 corner = id.xy * 2.0f + 1.0f;
    const Patch here = Read(t0, corner);
    const int2 below = int2(id.xy >> 1);
    const float4 word = t2.Load(int3(below, 0));
    const float2 own = round(word.xy * 2.0f);
    // Leaving the word of the block below costs as much as that word counts - and the more the less the block
    // itself shows: a soft road, a hillside in haze match as well a long way off as where they are, and a block
    // of them that goes looking by itself comes back with anything (the picture then stood in blots there).
    // The block below sees four times as much, and what it found is the better word.
    const float far = k.x * word.w + k.y * (1.0f - saturate(Contrast(here) * k.z));
    float apart = Apart(here, Read(t1, corner + own));
    float least = apart;
    float2 found = own;
    {
        const float d = Apart(here, Read(t1, corner)), e = d + far * length(own);
        if (e < least) { least = e; found = 0.0f; apart = d; }
    }
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            if (x == 0 && y == 0) continue;
            const float2 v = round(t2.Load(int3(clamp(below + int2(x, y), int2(0, 0), int2(m.xy)), 0)).xy * 2.0f);
            const float d = Apart(here, Read(t1, corner + v)), e = d + far * length(v - own);
            if (e < least) { least = e; found = v; apart = d; }
        }
    const float2 centre = found;
    for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i)
        {
            if (i == 0 && j == 0) continue;
            const float2 v = centre + float2(i, j);
            const float d = Apart(here, Read(t1, corner + v)), e = d + far * length(v - own);
            if (e < least) { least = e; found = v; apart = d; }
        }
    // What the block shows itself, or - plain - most of the word it took over.
    out0[id.xy] = float4(found, apart, max(Sure(Contrast(here), apart), all(found == own) ? 0.7f * word.w : 0.0f));
}

// A block takes a neighbour's movement where that explains it as well. k.x: what a texel away from the eight
// around costs - their mean distance, each counted by how much its word counts. m.zw: the last block of this size.
[numthreads(8, 8, 1)]
void RelaxMain(uint3 id : SV_DispatchThreadID)
{
    const float2 corner = id.xy * 2.0f + 1.0f;
    const Patch here = Read(t0, corner);
    float4 around[9];
    float counts = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const float4 word = t2.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(m.zw)), 0));
            around[(y + 1) * 3 + (x + 1)] = word;
            if (x != 0 || y != 0) counts += word.w;
        }
    float least = 1e9f, apart = 0.0f, backed = 0.0f;
    float2 found = around[4].xy;
    // The block's own word first: it wins a tie.
    static const int order[9] = { 4, 0, 1, 2, 3, 5, 6, 7, 8 };
    for (int i = 0; i < 9; ++i)
    {
        const float2 v = around[order[i]].xy;
        float away = 0.0f, with = 0.0f;
        for (int j = 0; j < 9; ++j)
        {
            if (j == 4) continue;
            const float gap = length(v - around[j].xy);
            away += around[j].w * gap;
            if (gap < 0.5f) with = max(with, around[j].w);
        }
        const float d = Apart(here, Read(t1, corner + v)), e = d + k.x * away / max(counts, 0.5f);
        if (e < least) { least = e; found = v; apart = d; backed = with; }
    }
    // Plain, and moving as a block around it that counts: most of that one's say goes on with it.
    out0[id.xy] = float4(found, apart, max(Sure(Contrast(here), apart), 0.7f * backed));
}

)hlsl" R"hlsl(
// A block that was not found - what lies where its word says is clearly something else - may be a thin thing
// going its own way, faster than what is behind it: a lamp post beside the road, seen from a car. Too thin to
// show at the smaller sizes, it never had a word of its own to start from, and one texel further at each size
// does not reach it. So, at the second largest size, such a block is looked for far and wide: every second
// texel within m.x texels of where its word says, then one texel around the best. Taken where that fits clearly
// better (to n.z of what it is off by now, less n.w). t2: the blocks as they stand. n.x, n.y: a block found
// differs by no more than n.x and n.y for each step of its own contrast; m.y: it shows something from this much
// contrast on.
[numthreads(8, 8, 1)]
void FarMain(uint3 id : SV_DispatchThreadID)
{
    float4 block = t2.Load(int3(id.xy, 0));
    const float2 corner = id.xy * 2.0f + 1.0f;
    const Patch here = Read(t0, corner);
    const float contrast = Contrast(here);
    if (block.z > n.x + n.y * contrast && contrast > m.y)
    {
        float least = block.z;
        float2 found = block.xy;
        const int reach = int(m.x);
        for (int y = -reach; y <= reach; y += 2)
            for (int x = -reach; x <= reach; x += 2)
            {
                const float2 v = block.xy + float2(x, y);
                const float d = Apart(here, Read(t1, corner + v));
                if (d < least) { least = d; found = v; }
            }
        const float2 centre = found;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i)
            {
                const float2 v = centre + float2(i, j);
                const float d = Apart(here, Read(t1, corner + v));
                if (d < least) { least = d; found = v; }
            }
        // (And found there outright: a piece of a pattern that repeats - a tower block's windows - fits a little
        // better in many places, and is in none of them.)
        if (least < block.z * n.z - n.w && least <= n.x + n.y * contrast) block = float4(found, least, Sure(contrast, least));
    }
    out0[id.xy] = block;
}

// What a block says at the place it reaches: how sure (8 bits), and its movement from the frame before to the one
// after in sixteenths of a texel (12 bits each way, 2048 for none).
uint Word(uint rank, float2 v)
{
    const int2 q = clamp(int2(round(v * 16.0f)) + 2048, 1, 4095);
    return (rank << 24) | (uint(q.x) << 12) | uint(q.y);
}

void Unword(uint word, out uint rank, out float2 v)
{
    rank = word >> 24;
    v = (float2((word >> 12) & 4095u, word & 4095u) - 2048.0f) * 0.0625f;
}

// One way's blocks, each carried the whole of its way: at the block of the other frame it lands on it leaves its
// movement (as it is, this way's), ranked by how much its word counts. t0: the blocks. m.zw: the last block.
[numthreads(8, 8, 1)]
void CrossSplatMain(uint3 id : SV_DispatchThreadID)
{
    const float4 block = t0.Load(int3(id.xy, 0));
    const int2 to = int2(floor((id.xy * 2.0f + 1.0f + block.xy) * 0.5f));
    if (to.x < 0 || to.y < 0 || to.x > int(m.z) || to.y > int(m.w)) return;
    InterlockedMax(out1[to], Word(1u + uint(saturate(block.w) * 200.0f), block.xy));
}

// A block is offered the way back of the block that landed on it from the other frame, and takes it where it
// fits clearly better than its own (k.x: by this much, k.y: and to this share). t2: this way's blocks;
// landed_a: what the other way's left here.
[numthreads(8, 8, 1)]
void CrossMain(uint3 id : SV_DispatchThreadID)
{
    float4 block = t2.Load(int3(id.xy, 0));
    const uint word = landed_a.Load(int3(id.xy, 0));
    if (word != 0u)
    {
        uint rank;
        float2 theirs;
        Unword(word, rank, theirs);
        const float2 v = -round(theirs);
        if (any(v != block.xy))
        {
            const float2 corner = id.xy * 2.0f + 1.0f;
            const Patch here = Read(t0, corner);
            const float d = Apart(here, Read(t1, corner + v));
            if (d < block.z * k.y - k.x) block = float4(v, d, Sure(Contrast(here), d));
        }
    }
    out0[id.xy] = block;
}

// ---- 3. settle

// Parts of a texel, and whether the block was found. t2: this way's blocks, t3: the other way's.
// k.x: what a block may differ by whatever it shows; k.y: and for each step of its own contrast;
// k.z: how far the way back may miss home, texels; k.w: and for each texel of the way. Written beside it:
// whether the block shows anything at all - its contrast over m.y, in full from 1 / m.x more (a sky shows
// nothing, a road's grain does).
[numthreads(8, 8, 1)]
void SubpelMain(uint3 id : SV_DispatchThreadID)
{
    const float2 corner = id.xy * 2.0f + 1.0f;
    const Patch here = Read(t0, corner);
    const float2 v = t2.Load(int3(id.xy, 0)).xy;
    const float e0 = Apart(here, Read(t1, corner + v));
    const float ex0 = Apart(here, Read(t1, corner + v + float2(-1.0f, 0.0f))), ex1 = Apart(here, Read(t1, corner + v + float2(1.0f, 0.0f)));
    const float ey0 = Apart(here, Read(t1, corner + v + float2(0.0f, -1.0f))), ey1 = Apart(here, Read(t1, corner + v + float2(0.0f, 1.0f)));
    float2 part = 0.0f;
    const float bend_x = ex0 - 2.0f * e0 + ex1, bend_y = ey0 - 2.0f * e0 + ey1;
    if (bend_x > 1e-4f) part.x = clamp(0.5f * (ex0 - ex1) / bend_x, -0.5f, 0.5f);
    if (bend_y > 1e-4f) part.y = clamp(0.5f * (ey0 - ey1) / bend_y, -0.5f, 0.5f);
    const float contrast = Contrast(here);
    // The way back from where it lands.
    const int2 lands = clamp(int2(floor((corner + v) * 0.5f)), int2(0, 0), int2(m.zw));
    const float2 back = t3.Load(int3(lands, 0)).xy;
    const float misses = length(v + back);
    const bool found = e0 <= k.x + k.y * contrast && misses <= k.z + k.w * length(v);
    out0[id.xy] = float4(v + part, found ? 1.0f : 0.0f, saturate((contrast - m.y) * m.x));
}

// How the picture at large moves: for a tile of blocks, the movement most of the blocks in and around it have
// that were found and show something (a plain block's movement is its neighbours' word, not its own) - their
// mean, then twice the mean of those near it (a thing of its own in the tile is left out). Beside it: the share
// of the blocks that had a say. Where hardly any has - a sky with one post in it - there is no picture at large
// to speak of.
// t0: the settled blocks. m.x: blocks a tile; m.zw: the last block. k.x: how near counts as with it, texels;
// k.y: and for each texel of the movement.
[numthreads(8, 8, 1)]
void LargeMain(uint3 id : SV_DispatchThreadID)
{
    const int tile = int(m.x), hop = max(1, tile / 4);
    const int2 first = int2(id.xy) * tile - tile / 2;
    float4 blocks[64];
    float2 sum = 0.0f;
    float count = 0.0f;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            float4 block = t0.Load(int3(clamp(first + int2(x, y) * hop, int2(0, 0), int2(m.zw)), 0));
            block.z *= block.w;
            blocks[y * 8 + x] = block;
            sum += block.xy * block.z;
            count += block.z;
        }
    float2 mean = count > 0.0f ? sum / count : 0.0f;
    for (int round = 0; round < 2; ++round)
    {
        const float near = k.x + k.y * length(mean);
        float2 with = 0.0f;
        float weight = 0.0f;
        for (int i = 0; i < 64; ++i)
        {
            const float gap = length(blocks[i].xy - mean) / near;
            const float w = blocks[i].z / ((1.0f + gap * gap) * (1.0f + gap * gap));
            with += blocks[i].xy * w;
            weight += w;
        }
        if (weight > 0.0f) mean = with / weight;
    }
    out0[id.xy] = float4(mean, count * 0.015625f, 0.0f);
}

)hlsl" R"hlsl(
// A block that shows little - a soft road, a wall in haze, a sky - was found wherever it was looked for: its
// word is as good as none (on the street of the tests the road's blocks were 33 pixels off on average, two in
// three of them more than three), and a picture drawn by such words stands in blots. Such a block goes by how
// the picture at large moves around it, the more the less it shows. t0: the settled blocks, t1: the picture at
// large (this way). k.x: there is a picture at large where this share of the blocks around has a say.
//
// Round 21, what repeats - a tower's face of fine upright lines, a row of balconies, a fence. A block of it is
// explained as well one line on as where it truly went, and each block picks for itself: neighbours one line
// apart, and the face drawn in broken, dotted lines between them. Such a block has nothing to say against the
// picture at large either - so where the two real frames agree under the picture's word about as well as under
// the block's own (k.y times as far apart and k.z more, at most), the block takes the picture's: the whole face
// one word, its lines whole. A thing that moves by itself does not agree under the picture's word at all, and
// keeps its own. t2: this way's own frame, t3: the other one (the block's word leads from the one to the other).
float Apart(float2 uv, float2 word)
{
    float sum = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const float2 at = uv + float2(x, y) * inv_a;
            sum += dot(abs(t2.SampleLevel(lin, at, 0).rgb - t3.SampleLevel(lin, at + word * inv_a, 0).rgb), float3(0.299f, 0.587f, 0.114f));
        }
    return sum;
}

[numthreads(8, 8, 1)]
void FillMain(uint3 id : SV_DispatchThreadID)
{
    float4 block = t0.Load(int3(id.xy, 0));
    const float2 uv = (id.xy * 2.0f + 1.0f) * inv_a;
    const float3 large = t1.SampleLevel(lin, uv, 0).xyz;
    if (large.z >= k.x)
    {
        const float2 off = large.xy - block.xy;
        if (k.y > 0.0f && dot(off, off) > 1.0f && Apart(uv, large.xy) <= Apart(uv, block.xy) * k.y + k.z) block.xy = large.xy;
        else block.xy = lerp(large.xy, block.xy, block.w);
    }
    out0[id.xy] = block;
}

// ---- 4. marks

// A mark on the screen - a read-out, the player's own car seen from behind - is a pixel that stays while its
// place moves. That the two real frames have a pixel alike says nothing by itself: in a soft picture (grass, a
// hillside, a road under motion blur) half the pixels are alike from one frame to the next while all of it
// moves, and held still they stand in the moving picture as blots. What does say it: drawn the way its place
// goes, the pixel would come out clearly another colour - and both real frames show it the same. So for every
// pixel alike in both frames, the ways it may be drawn by are tried on it - its place's own word, each way
// (found or not: it is what the pixel is drawn by), and how the picture at large moves there - at the end of
// the way and half way along it:
//   - where any of them would make it clearly different, the pixel stays while its place moves: counted up
//     by two;
//   - where none would, it goes with its place, or is a piece of something plain that looks the same wherever
//     it is taken from: counted down by one (a sky a lamp post crosses now and then never gets anywhere);
//   - where its place does not move, nothing is learnt this pair.
// A pixel counted up to k.x is a mark, and one for as long as the count holds; one that changed starts at
// nothing. (Tried and left out: a plain pixel taken for a mark's own when a mark stands close by along the way
// the picture moves - the inside of a filled read-out. It also took the plain wall a lamp post was crossing:
// posts 9% of their pixels off without it, 14% with.)
// Round 20, the read-outs learnt for the game (Adaptive Motion Learning, sm_learn.inl: what stays on the screen
// while the picture moves, kept from one evening to the next): a pixel in one of their cells carries a flag (the
// seventh bit, put there by MsmHudSeed and passed on here). It starts as a mark, and a change does not take its
// count away: a map that turns, a number that counts, a panel one sees through are marks from the first pair
// on and stay marks - shown as the real frame has them, not pulled about with the picture under them. What
// goes with its place inside such a cell is still counted down, and is drawn moving again.
// out1: the count (6 bits), the flag, and whether it is a mark (the eighth); landed_a: as the pair before left it
// (two maps, written in turn: a byte a pixel, and none read while it is written). t0: the frame before, t1: the
// frame after, t2, t3: the settled blocks, the way forward and the way back, t4: the picture at large (the way
// forward). k.yz: a texel of the size the motion is looked for at, in pixels; k.w: the count goes no higher
// than this. n.x: two pixels this near are the same; n.y: and this far apart, clearly not. m.x: a place moves
// from so many pixels on.

// Drawn by this word (pixels, from the frame before to the frame after), would the pixel at uv - `seen` in
// both frames - come out clearly different?
bool Changed(float2 uv, float2 word, float3 seen)
{
    const float2 way = word * inv_out;
    return Most(abs(t1.SampleLevel(lin, uv + way, 0).rgb - seen)) >= n.y || Most(abs(t0.SampleLevel(lin, uv - way, 0).rgb - seen)) >= n.y ||
           Most(abs(t1.SampleLevel(lin, uv + 0.5f * way, 0).rgb - seen)) >= n.y || Most(abs(t0.SampleLevel(lin, uv - 0.5f * way, 0).rgb - seen)) >= n.y;
}

// Round 53: a read-out one sees through - GTA V's wanted stars, a thin crosshair, a panel - is not the same colour in
// both frames when the picture behind it moves: its pixel is part read-out, part picture. What is the same is its
// shape: the pixel against the four around it (its edge, its outline) stays where it is, in both frames, while the
// moving picture's own edges move. (Users' footage: the stars broke into pieces and dragged with the picture, the
// counter beside them left a trace.) A pixel whose edge is clear and is the same in both frames counts as staying,
// as one of the same colour does; it is drawn as both frames have it, where it is (WarpMain).
float Edge(Texture2D<float4> t, int2 p)
{
    // (The four beside it: the eight cost the pair a fifth of a millisecond more at 1440p, for the same answer.)
    const float around = dot(t.Load(int3(p + int2(-1, 0), 0)).rgb + t.Load(int3(p + int2(1, 0), 0)).rgb +
                             t.Load(int3(p + int2(0, -1), 0)).rgb + t.Load(int3(p + int2(0, 1), 0)).rgb, kLuma);
    return dot(t.Load(int3(p, 0)).rgb, kLuma) - around * 0.25f;
}
bool ShapeStays(int2 p)
{
    const float e0 = Edge(t0, p), e1 = Edge(t1, p);
    return abs(e0) > 0.1f && abs(e1) > 0.1f && abs(e0 - e1) < 0.15f * max(abs(e0), abs(e1));
}

[numthreads(8, 8, 1)]
void MarkMain(uint3 id : SV_DispatchThreadID)
{
    const float3 a = t0.Load(int3(id.xy, 0)).rgb, b = t1.Load(int3(id.xy, 0)).rgb;
    const uint was = landed_a.Load(int3(id.xy, 0));
    const uint learnt = was & 64u;
    uint count = learnt != 0u ? (was & 63u) : 0u;
    if (Most(abs(a - b)) < n.x || ShapeStays(int2(id.xy)))
    {
        count = was & 63u;
        const float2 uv = (id.xy + 0.5f) * inv_out;
        // (The way back's word says where the place came from: the other way round, it is a word forward.)
        const float2 forth = t2.SampleLevel(lin, uv, 0).xy * k.yz, back = -t3.SampleLevel(lin, uv, 0).xy * k.yz;
        const float2 large = t4.SampleLevel(lin, uv, 0).xy * k.yz;
        const bool tries_forth = dot(forth, forth) >= m.x * m.x;
        const bool tries_back = dot(back, back) >= m.x * m.x && dot(back - forth, back - forth) > 1.0f;
        const bool tries_large = dot(large, large) >= m.x * m.x && dot(large - forth, large - forth) > 1.0f && dot(large - back, large - back) > 1.0f;
        if (tries_forth || tries_back || tries_large)
        {
            const bool stays = (tries_forth && Changed(uv, forth, a)) || (tries_back && Changed(uv, back, a)) || (tries_large && Changed(uv, large, a));
            // (Counted down only when its own place moves and it goes along: that the picture at large would not
            // change it either says little - the inside of a filled read-out is the same a long way round.)
            if (stays) count = min(count + 2u, uint(k.w));
            else if (tries_forth || tries_back) count = max(count, 1u) - 1u;
        }
    }
    out1[id.xy] = count | learnt | (count >= uint(k.x) ? 128u : 0u);
}

// ---- 5. carry

[numthreads(8, 8, 1)]
void ClearMain(uint3 id : SV_DispatchThreadID)
{
    out1[id.xy] = 0u;
}

// t0: this frame's blocks; t1: how the picture at large moves, this way (LargeMain).
// k.x: how much of its way a block is carried; k.y: 1 the frame before (its way is forward), -1 the frame after
// (its way is back: the movement it says is the other way round). m.zw: the last block.
[numthreads(8, 8, 1)]
void SplatMain(uint3 id : SV_DispatchThreadID)
{
    const float4 block = t0.Load(int3(id.xy, 0));
    const float2 corner = id.xy * 2.0f + 1.0f;
    const float2 large = t1.SampleLevel(lin, corner * inv_a, 0).xy;
    // Its rank: the place's own block before a neighbour's; found before not found; then the one that stands
    // out most from the picture at large (half texels).
    const uint stands_out = min(63u, uint(length(block.xy - large) * 2.0f));
    const uint found = block.z > 0.5f ? 1u : 0u;
    const float2 at = (corner + k.x * block.xy - 1.0f) * 0.5f;      // in blocks: block i has its middle at i
    const int2 first = int2(floor(at));
    const int2 nearest = first + int2(step(0.5f, at - first));
    const float2 says = block.xy * k.y;
    for (int y = 0; y <= 1; ++y)
        for (int x = 0; x <= 1; ++x)
        {
            const int2 to = first + int2(x, y);
            if (to.x < 0 || to.y < 0 || to.x > int(m.z) || to.y > int(m.w)) continue;
            const uint rank = ((all(to == nearest) ? 2u : 0u) + found) * 64u + stands_out;
            InterlockedMax(out1[to], Word(rank, says));
        }
}

)hlsl" R"hlsl(
// ---- 6. resolve

// A place nothing reached: the best word of the eight around, as a neighbour's (never as the place's own).
uint Around(Texture2D<uint> from, int2 at, int2 last)
{
    uint best = 0u;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            best = max(best, from.Load(int3(clamp(at + int2(x, y), int2(0, 0), last), 0)));
    return best & 0x7FFFFFFFu;
}

// k.x: the moment, 0 the frame before .. 1 the frame after. m.zw: the last block. t0, t1: the two frames
// themselves (inv_a: a texel of the size the motion is looked for at, as a share of the picture); t3: how the
// picture at large moves (the way forward). m.x, m.y: the picture at large's movement
// is taken for a place where the two frames are nearer under it than m.y of what they are under the place's
// own, less m.x (o.z: 1 it is asked at all; o.w: where the two words are further apart than this, texels,
// squared).
// Written: the movement (texels of the size the motion is looked for at), the frame after's share of the
// pixel.
[numthreads(8, 8, 1)]
void ResolveMain(uint3 id : SV_DispatchThreadID)
{
    if (k.y > 0.5f)
    {
        // Round 40, direct (as Lossless Scaling draws): the place's word is the movement found at the place itself
        // - the frame before's search forward (t4) and the frame after's back (t5), the mean of the two where they
        // agree, the one found where only one was - and both frames show it, mixed by the moment. Nothing is
        // carried and nothing is chosen between: a word that is wrong comes out as a soft trace where it is, never
        // as a piece of picture landed somewhere else - the "puzzle" of the frames carried and resolved.
        const float4 f = t4.Load(int3(id.xy, 0)), b = t5.Load(int3(id.xy, 0));
        const bool fa = f.z > 0.5f, fb = b.z > 0.5f;
        float2 v;
        if (fa && fb && length(f.xy + b.xy) <= 1.0f + 0.2f * length(f.xy)) v = 0.5f * (f.xy - b.xy);
        else if (fa && !fb) v = f.xy;
        else if (fb && !fa) v = -b.xy;
        else v = 0.5f * (f.xy - b.xy);
        out0[id.xy] = float4(v, k.x, 0.0f);
        return;
    }
    uint a = landed_a.Load(int3(id.xy, 0)), b = landed_b.Load(int3(id.xy, 0));
    if (a == 0u && b == 0u)
    {
        a = Around(landed_a, int2(id.xy), int2(m.zw));
        b = Around(landed_b, int2(id.xy), int2(m.zw));
    }
    uint rank_a, rank_b;
    float2 va, vb;
    Unword(a, rank_a, va);
    Unword(b, rank_b, vb);
    float2 v = 0.0f;
    float share = k.x;
    if (a != 0u && b != 0u)
    {
        if (length(va - vb) <= 1.0f + 0.1f * max(length(va), length(vb)))
        {
            v = lerp(va, vb, k.x);
        }
        else
        {
            const bool first = rank_a >= rank_b;
            v = first ? va : vb;
            // The other frame shows something else here. If that is only a neighbour's word, or a block that was
            // not found, it does not show this piece at all.
            if (((first ? rank_b : rank_a) >> 6) < 3u) share = first ? 0.0f : 1.0f;
        }
    }
    else if (a != 0u) { v = va; share = 0.0f; }
    else if (b != 0u) { v = vb; share = 1.0f; }
    if (o.z > 0.5f)
    {
        // What a place says is one movement for all of it - and a place holds the thing and what is behind it
        // (a post and the wall beside it, a mark of the HUD and the street around it). The other word a place
        // may go by: how the picture at large moves there - where the two frames then show the same, and under
        // the place's own word clearly do not. Asked for the place as a whole - at the middles of its four
        // texels, in the two frames' own colours - so that the pixels of a place get one answer (asked of each
        // pixel by itself, one in a few gets the other, and stands alone among them). Colours, not brightness:
        // two things of different colour and the same brightness are what the search itself takes for each
        // other, and here is where that is put right.
        const float2 middle = id.xy * 2.0f + 1.0f;
        const float2 large = t3.SampleLevel(lin, middle * inv_a, 0).xy, off = large - v;
        if (dot(off, off) > o.w)
        {
            float own_apart = 0.0f, large_apart = 0.0f;
            for (int j = 0; j < 2; ++j)
                for (int i = 0; i < 2; ++i)
                {
                    const float2 at = middle + float2(i, j) - 0.5f;
                    own_apart += dot(abs(t0.SampleLevel(lin, (at - k.x * v) * inv_a, 0).rgb - t1.SampleLevel(lin, (at + (1.0f - k.x) * v) * inv_a, 0).rgb), kLuma);
                    large_apart += dot(abs(t0.SampleLevel(lin, (at - k.x * large) * inv_a, 0).rgb - t1.SampleLevel(lin, (at + (1.0f - k.x) * large) * inv_a, 0).rgb), kLuma);
                }
            if (large_apart * 0.25f < own_apart * 0.25f * m.y - m.x) { v = large; share = k.x; }
        }
    }
    out0[id.xy] = float4(v, share, 0.0f);
}

// How far a place's word is from the words of the eight places around it, texels - the largest - or 8 where one
// of them is shown by one frame only and the place by both. t0: what Resolve wrote; written: the same, with that
// beside it. m.zw: the last block.
// SMFG 1.1: a place whose word stands alone - seven or more of the eight around agree with each other (within
// k.x texels and k.y of their size) and it is further than that from them - takes theirs: one block that found
// the wrong place (a piece of tree in the sky) was drawn as a square of picture where it does not belong. A lamp
// post one place wide keeps its own: the places above and below it say the same as it does. k.x 0: not asked.
[numthreads(8, 8, 1)]
void EdgeMain(uint3 id : SV_DispatchThreadID)
{
    float4 own = t0.Load(int3(id.xy, 0));
    if (k.x > 0.0f)
    {
        float2 around[8];
        int count = 0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x)
                if (x != 0 || y != 0)
                    around[count++] = t0.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(m.zw)), 0)).xy;
        // The middle of the eight, each way (the fourth and fifth of eight, sorted, averaged).
        float2 middle = 0.0f;
        for (int way = 0; way < 2; ++way)
        {
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < 8; ++i)
            {
                const float v = way == 0 ? around[i].x : around[i].y;
                int below = 0, same = 0;
                for (int j = 0; j < 8; ++j) { const float w = way == 0 ? around[j].x : around[j].y; below += w < v ? 1 : 0; same += w == v ? 1 : 0; }
                if (below <= 3 && below + same > 3) lo = min(lo, v);
                if (below <= 4 && below + same > 4) hi = max(hi, v);
            }
            const float mid = 0.5f * (lo + hi);
            if (way == 0) middle.x = mid; else middle.y = mid;
        }
        const float near = k.x + k.y * length(middle);
        int agree = 0;
        for (int i = 0; i < 8; ++i) agree += length(around[i] - middle) <= near ? 1 : 0;
        if (agree >= 7 && length(own.xy - middle) > near) own.xy = middle;
    }
    if (k.z > 0.0f)
    {
        // Round 39, fluid: every place's word the mean of the words of the twenty-five places around it, each
        // counted by how near its word is to the place's own (within k.z texels and k.w of its size: a car's word
        // beside the road's counts for next to nothing, so an edge stays one) and by whether both frames show it
        // (a word one frame alone shows counts less). Words that went their own way by a place or two - the
        // squares - come out as the field around them; the steps between places are gone.
        const float2 self = own.xy;
        const float spread = k.z + k.w * length(self);
        float2 sum = 0.0f;
        float weight = 0.0f;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x)
            {
                const float4 other = t0.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(m.zw)), 0));
                const float2 d = other.xy - self;
                const float both = other.z > 0.001f && other.z < 0.999f ? 1.0f : 0.35f;
                const float w = exp(-dot(d, d) / (2.0f * spread * spread)) * both * ((abs(x) == 2 || abs(y) == 2) ? 0.5f : 1.0f);
                sum += w * other.xy;
                weight += w;
            }
        own.xy = sum / max(weight, 1e-6f);
    }
    float most = 0.0f;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const float4 other = t0.Load(int3(clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(m.zw)), 0));
            most = max(most, max(length(other.xy - own.xy), abs(other.z - own.z) > 0.5f ? 8.0f : 0.0f));
        }
    out0[id.xy] = float4(own.xyz, most);
}

)hlsl" R"hlsl(
// ---- 7. draw

// Round 56: a frame read between its pixels - by Catmull-Rom (nine reads of the card's own filtering) where inv_a
// is its texel, plainly between the four nearest where it is 0 (as before). Read plainly, a frame moved by half a
// pixel loses its finest detail: SMFG's frames kept 42 to 86% of a real frame's (the finest changes' energy, the
// user's footage), the real frames between them all of theirs - detail coming and going four times out of five at
// x6, "waves" over the world.
float3 Between(Texture2D<float4> t, float2 uv)
{
    if (inv_a.x <= 0.0f) return t.SampleLevel(lin, uv, 0).rgb;
    const float2 size = 1.0f / inv_a;
    const float2 at = uv * size;
    const float2 one = floor(at - 0.5f) + 0.5f;
    const float2 f = at - one;
    const float2 w0 = f * (-0.5f + f * (1.0f - 0.5f * f));
    const float2 w1 = 1.0f + f * f * (-2.5f + 1.5f * f);
    const float2 w2 = f * (0.5f + f * (2.0f - 1.5f * f));
    const float2 w3 = f * f * (-0.5f + 0.5f * f);
    const float2 w12 = w1 + w2;
    const float2 p0 = (one - 1.0f) * inv_a, p3 = (one + 2.0f) * inv_a, p12 = (one + w2 / w12) * inv_a;
    float3 c = 0.0f;
    c += t.SampleLevel(lin, float2(p0.x, p0.y), 0).rgb * w0.x * w0.y;
    c += t.SampleLevel(lin, float2(p12.x, p0.y), 0).rgb * w12.x * w0.y;
    c += t.SampleLevel(lin, float2(p3.x, p0.y), 0).rgb * w3.x * w0.y;
    c += t.SampleLevel(lin, float2(p0.x, p12.y), 0).rgb * w0.x * w12.y;
    c += t.SampleLevel(lin, float2(p12.x, p12.y), 0).rgb * w12.x * w12.y;
    c += t.SampleLevel(lin, float2(p3.x, p12.y), 0).rgb * w3.x * w12.y;
    c += t.SampleLevel(lin, float2(p0.x, p3.y), 0).rgb * w0.x * w3.y;
    c += t.SampleLevel(lin, float2(p12.x, p3.y), 0).rgb * w12.x * w3.y;
    c += t.SampleLevel(lin, float2(p3.x, p3.y), 0).rgb * w3.x * w3.y;
    return saturate(c);
}

// A pixel of the frame in between under one word r (the movement, the frame after's share): from the two frames,
// a mark never taken for the picture sought. The rest of WarpMain's own numbers as it has them.
float3 Draw(uint2 id, float2 uv, float4 r)
{
    const float2 pixels = r.xy * k.yz;                    // the movement, pixels of the picture
    const float2 uv_a = uv - k.x * pixels * inv_out, uv_b = uv + (1.0f - k.x) * pixels * inv_out;
    // Both frames show it: the nearer one's own pixels, unless the moment is half way. (Two real frames
    // laid over each other are never quite in register - and one frame read is half the work of two.)
    float share = o.y > o.x ? smoothstep(o.x, o.y, r.z) : r.z;
    bool rests = false;
    if (dot(pixels, pixels) > 0.5f)
    {
        // Taken from a mark: that is the read-out, not the piece of picture sought - the other frame gives it.
        const float2 size = 1.0f / inv_out;
        bool mark_a = false, mark_b = false;
        if (share < 1.0f) mark_a = (landed_a.Load(int3(int2(uv_a * size), 0)) & 128u) != 0u;
        if (share > 0.0f || mark_a) mark_b = (landed_a.Load(int3(int2(uv_b * size), 0)) & 128u) != 0u;
        if (mark_a && mark_b) rests = true;
        else if (mark_a) share = 1.0f;
        else if (mark_b) share = 0.0f;
    }
    if (rests) return lerp(t0.Load(int3(id, 0)).rgb, t1.Load(int3(id, 0)).rgb, k.x);
    // Round 53: is what each frame shows there going this way at all? A thing standing on the screen while the
    // picture behind it moves - the player's head in a turn - covers, in one of the frames, the piece of picture
    // that goes past it: the movement carried that frame's pixel of the head into the moving picture beside it
    // (users' "ghosting on heads", seen in their own GTA V footage: blots of the head's colour beside it). The
    // frame before's own word where the pixel is taken from (t4: its blocks, found) says where that pixel goes; if
    // it is found going clearly another way, it is not this piece of picture, and the frame after's is taken - and
    // the other way round (t5: the frame after's blocks, back).
    if (dot(pixels, pixels) > 0.5f)
    {
        const float2 blocks = m.xy;                     // the blocks across and down (the same grid as t4, t5)
        const float4 from_a = t4.Load(int3(clamp(int2(uv_a * blocks), int2(0, 0), int2(blocks) - 1), 0));
        const float4 from_b = t5.Load(int3(clamp(int2(uv_b * blocks), int2(0, 0), int2(blocks) - 1), 0));
        const float reach = max(1.5f, 0.3f * length(r.xy));
        // (What is wrong is a thing going clearly slower than the place - standing on the screen, as a head, a hand,
        // a weapon, the car from behind; in a turn too fast for the words they say anything, and switching frames
        // there only made the pieces sharper.)
        const float pace = length(r.xy);
        const bool wrong_a = from_a.z > 0.5f && length(from_a.xy - r.xy) > reach && length(from_a.xy) < 0.5f * pace;
        const bool wrong_b = from_b.z > 0.5f && length(from_b.xy + r.xy) > reach && length(from_b.xy) < 0.5f * pace;
        if (wrong_a && !wrong_b) share = 1.0f;
        else if (wrong_b && !wrong_a) share = 0.0f;
    }
    if (share <= 0.0f) return Between(t0, uv_a);
    if (share >= 1.0f) return Between(t1, uv_b);
    const float3 from_a = Between(t0, uv_a), from_b = Between(t1, uv_b);
    // They do not show the same: nearer to the nearer one, not a double picture.
    share = lerp(share, step(0.5f, share), saturate((dot(abs(from_a - from_b), kLuma) - n.z) * n.w));
    return lerp(from_a, from_b, share);
}

// t0: the frame before, t1: the frame after, t2: what Resolve wrote, and how far each place's word is from its
// neighbours' (EdgeMain); landed_a: the marks. k.x: the moment; k.yz: a texel of the size the motion is looked
// for at, in pixels. n.z, n.w: the two frames this far apart at a pixel (n.z) lean to the nearer one, wholly
// from n.z + 1 / n.w on. o.x, o.y: the frame after's share of a pixel both show is none up to o.x of the way
// and all from o.y on. m.xy: the places across and down; o.z: places whose words are further apart than this
// (texels) are not mixed; o.w: what a word shown by one frame only counts as, the two frames' difference under
// the others being what they count as; m.z: by how much the nearer place's word is taken first; m.w: the two
// frames within this of each other under the words mixed, and mixed they stay.
[numthreads(8, 8, 1)]
void WarpMain(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = (id.xy + 0.5f) * inv_out;
    float3 colour;
    // Round 41: a mark - a read-out, learnt or seen - is held as the frame before has it only where the frame
    // after has it the same (k.w: how near, brightness). A read-out stands still from one frame to the next; a
    // piece of a wall or a tree taken for one (Adaptive Motion Learning's read-outs, learnt wrongly) does not, and
    // is drawn as the rest of the picture is - not a square standing while everything around it moves.
    bool held = (landed_a.Load(int3(id.xy, 0)) & 128u) != 0u;
    if (held && k.w > 0.0f) held = dot(abs(t0.Load(int3(id.xy, 0)).rgb - t1.Load(int3(id.xy, 0)).rgb), kLuma) <= k.w || ShapeStays(int2(id.xy));
    if (held)
    {
        // (Both frames where it is, by the moment: a read-out one sees through shows the picture behind it changing.)
        colour = lerp(t0.Load(int3(id.xy, 0)).rgb, t1.Load(int3(id.xy, 0)).rgb, k.x);
    }
    else
    {
        float4 r = t2.SampleLevel(lin, uv, 0);
        if (n.y > 0.0f)
        {
            // Round 39, fluid: no word picked whole at an edge, no frame taken alone where they differ - the field
            // (made smooth by EdgeMain) as it is, between places as well, and the two frames mixed by the moment
            // and by which shows the place (n.w is 0 then: no step to the nearer frame). What it cannot tell comes
            // out as a soft trace, as Lossless Scaling's does - never a square.
            out0[id.xy] = float4(Draw(id.xy, uv, r), 1.0f);
            return;
        }
        if (r.w > o.z)
        {
            // The places around do not say the same. Where the two real frames show the same under their words
            // mixed, mixed they stay: a soft road, a hillside, where the words differ because nothing there
            // tells them apart - taken one by one such a place came out as a patchwork of little squares.
            // Where they do not - an edge - one of the four words, whole: the one under which the two frames
            // show the same here (the nearer place's where they fit alike).
            const float2 mixed = r.xy * k.yz * inv_out;
            const float fits_mixed = dot(abs(t0.SampleLevel(lin, uv - k.x * mixed, 0).rgb - t1.SampleLevel(lin, uv + (1.0f - k.x) * mixed, 0).rgb), kLuma);
            if (fits_mixed > m.w && n.x > 0.0f)
            {
                // SMFG 1.1. Not one of the four words whole - that drew the edge of a thing in steps of a place,
                // and where none of them fitted, a square of picture from somewhere else. Each word's own pixel,
                // weighed by how near the place is and how well the two frames agree under it: where one fits
                // clearly best it is that one's, where they fit alike they are mixed - soft, as an eye takes a
                // fast edge, and never a square. n.x: how fast a word's weight falls off with how badly it fits.
                const float2 at = uv * m.xy - 0.5f;
                const int2 first = int2(floor(at));
                const float2 part = at - first;
                float4 words[4];
                float fits[4], least = 1e9f;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i)
                    {
                        const int c = j * 2 + i;
                        words[c] = t2.Load(int3(clamp(first + int2(i, j), int2(0, 0), int2(m.xy) - 1), 0));
                        const float2 way = words[c].xy * k.yz * inv_out;
                        fits[c] = o.w;
                        if (words[c].z > 0.001f && words[c].z < 0.999f)
                            fits[c] = dot(abs(t0.SampleLevel(lin, uv - k.x * way, 0).rgb - t1.SampleLevel(lin, uv + (1.0f - k.x) * way, 0).rgb), kLuma);
                        least = min(least, fits[c]);
                    }
                float3 sum = 0.0f;
                float weight = 0.0f;
                for (int c2 = 0; c2 < 4; ++c2)
                {
                    const float near = ((c2 & 1) ? part.x : 1.0f - part.x) * ((c2 & 2) ? part.y : 1.0f - part.y);
                    const float w = (near + 0.05f) * exp(-(fits[c2] - least) * n.x);
                    sum += w * Draw(id.xy, uv, words[c2]);
                    weight += w;
                }
                out0[id.xy] = float4(sum / max(weight, 1e-6f), 1.0f);
                return;
            }
            if (fits_mixed > m.w)
            {
                const float2 at = uv * m.xy - 0.5f;
                const int2 first = int2(floor(at));
                const float2 part = at - first;
                float least = fits_mixed;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i)
                    {
                        const float4 word = t2.Load(int3(clamp(first + int2(i, j), int2(0, 0), int2(m.xy) - 1), 0));
                        const float2 way = word.xy * k.yz * inv_out;
                        float fits = o.w;
                        if (word.z > 0.001f && word.z < 0.999f)
                            fits = dot(abs(t0.SampleLevel(lin, uv - k.x * way, 0).rgb - t1.SampleLevel(lin, uv + (1.0f - k.x) * way, 0).rgb), kLuma);
                        fits -= m.z * (i ? part.x : 1.0f - part.x) * (j ? part.y : 1.0f - part.y);
                        if (fits < least) { least = fits; r = word; }
                    }
            }
        }
        colour = Draw(id.xy, uv, r);
        if (n.x > 0.0f && r.z > 0.001f && r.z < 0.999f)
        {
            // SMFG 1.1. A group of places can agree on a wrong word - leaves that found the wrong leaves, and
            // the square of tree they carry lands in the sky; the road's word over the player's own car. Where
            // the two frames do not show the same under the place's word, the picture at large's word there
            // (t3: what most around do) is asked too, and the two pixels are mixed by how well each fits.
            const float2 own = r.xy * k.yz * inv_out;
            const float fits_own = dot(abs(t0.SampleLevel(lin, uv - k.x * own, 0).rgb - t1.SampleLevel(lin, uv + (1.0f - k.x) * own, 0).rgb), kLuma);
            if (fits_own > m.w)
            {
                const float4 large = float4(t3.SampleLevel(lin, uv, 0).xy, r.z, 0.0f);
                const float2 way = large.xy * k.yz * inv_out;
                const float fits_large = dot(abs(t0.SampleLevel(lin, uv - k.x * way, 0).rgb - t1.SampleLevel(lin, uv + (1.0f - k.x) * way, 0).rgb), kLuma);
                if (fits_large < fits_own)
                {
                    const float w = exp(-(fits_own - fits_large) * n.x);       // 1: alike, 0: the picture's clearly
                    colour = lerp(Draw(id.xy, uv, large), colour, w);
                }
            }
        }
    }
    out0[id.xy] = float4(colour, 1.0f);
}
)hlsl";

// The numbers that are tried and tuned (SM_MSM_TUNE=<name>=<value>,... to try others; tests\msmtruth.exe too).
struct MsmTune
{
    float search_far = 0.05f;       // SearchMain: what a texel further costs
    float refine_far = 0.01f;       // (round 55, tuned on five games' footage: 0.02) RefineMain: what a texel away from the block's own word below costs
    float refine_plain = 0.03f;     // (0.06) ... and what more it costs a block that shows nothing itself
    float relax_far = 0.1f;         // RelaxMain: what a texel away from the blocks around costs
    int relax_small = 4;            // RelaxMain: how often, the two smallest sizes
    int relax_mid = 2;              // ... the two between
    int relax_large = 2;            // ... the largest
    float sure_gain = 8.0f;         // (4) a block with this much contrast (1 / it) counts in full
    float sure_floor = 0.05f;       // ... and found: it differs by less than its contrast and this
    float found_floor = 0.12f;      // SubpelMain: a block found differs by no more than this ...
    float found_contrast = 0.35f;   // ... and this for each step of its own contrast
    float back_floor = 1.5f;        // ... and its way back misses home by no more than this, texels
    float back_far = 0.2f;          // (0.1) ... and this for each texel of the way
    float far_reach = 14.0f;        // FarMain: a block not found is looked for within so many texels of the second largest size ...
    float far_share = 0.35f;        // ... and takes what fits to this share of what it is off by now ...
    float far_floor = 0.02f;        // ... less this
    float cross = 1.0f;             // CrossMain: 1 what one way found, the other is told
    float cross_floor = 0.02f;      // ... and takes where that fits better by this ...
    float cross_share = 0.7f;       // ... and to this share of how far its own is off
    float shows_floor = 0.01f;      // SubpelMain: a block shows something from this much contrast on ...
    float shows_gain = 12.0f;       // (6) ... in full from 1 / this more
    float fill_say = 0.05f;         // FillMain: a block that shows little goes by the picture at large where this share of the blocks around has a say
    float alias_ratio = 1.15f;      // ... and one that the picture's word explains nearly as well as its own takes the picture's: so many times as far apart
    float alias_floor = 0.03f;      // ... and this much more (nine places' brightness, summed), at most. alias_ratio 0: not asked
    float large_near = 1.5f;        // LargeMain: a block moves with the picture at large within this, texels ...
    float large_far = 0.1f;         // ... and this for each texel of the movement

    float same = 1.5f / 255.0f;     // MarkMain: two pixels this near are the same
    float differs = 24.0f / 255.0f; // ... and this far apart, clearly not
    float mark_pairs = 8.0f;        // ... a pixel is a mark from this count on: two for each pair it was seen to stay, less one for each it was seen to go
    float mark_most = 24.0f;        // ... and the count goes no higher than this
    float mark_moves = 0.5f;        // ... a place moves from so many pixels on

    float apart = 0.08f;            // WarpMain: the two frames this far apart at a pixel lean to the nearer one
    float apart_steep = 6.0f;
    float large_floor = 0.003f;     // ResolveMain: the picture at large's movement for a place: the two frames nearer by this ...
    float large_share = 0.85f;      // ... and to this share of what the place's own leaves (0: never asked)
    // (Both frames show a pixel: the frame before's alone up to nearer_from of the way, the frame after's alone
    // from nearer_to on - round 17's rule, for a frame as sharp as a real one. Off since round 19, both 0: at x6
    // it left what moved by itself standing where it was for two frames and coming on from where it will be for
    // two, and the frames were no nearer the true ones for it - mean error 1.54 against 1.31 on the moved
    // photograph.)
    float nearer_from = 0.0f;
    float nearer_to = 0.0f;
    float edge = 0.5f;              // ... places whose words are further apart than this, texels, are not mixed
    float one_sided = 0.08f;        // ... there, a word one frame only shows counts as the two frames this far apart
    float nearer_word = 0.03f;      // ... and the nearer place's word is taken first by this much
    float mixed_fits = 0.03f;       // ... and no word is picked at all where the two frames are within this of each other under the words mixed
    float soft = 0.0f;              // ... round 38: the four words mixed by how well each fits, a word's weight falling off this fast (0: one of them whole, as 1.0) - off since round 39
    float fluid = 1.0f;             // round 39: 1 the field made smooth and the two frames mixed softly (EdgeMain, WarpMain) - as Lossless Scaling's look; 0: as 1.0
    float direct = 1.0f;            // round 40: 1 each place's word is what was found at it, both ways (ResolveMain), nothing carried; 0: carried and resolved, as 1.0
    float mark_holds = 0.06f;       // round 41, WarpMain: a mark is held still only where the two frames are within this of each other there (0: always, as before)
    float fluid_near = 2.0f;        // ... words within this, texels ...
    float fluid_share = 0.25f;      // ... and this share of their size count as the same movement when the field is made smooth
    float alone_near = 1.5f;        // EdgeMain, SMFG 1.1: a word alone among seven that agree (within this, texels ...
    float alone_share = 0.15f;      // ... and this share of their movement) takes theirs (alone_near 0: not asked, as 1.0)
    float fluid_apart = 0.2f;       // round 56, fluid: the two frames this far apart at a pixel under its word lean to the nearer one ...
    float fluid_steep = 0.0f;       // ... wholly from fluid_apart + 1 / this on (0: never - two pictures laid over each other, as round 39 to 55)
    float sharp_read = 1.0f;        // round 56, Draw: 1 the frames read between their pixels by Catmull-Rom (Between), 0 plainly (detail kept 57 -> 70%, 42 -> 57%, 62 -> 75% on the user's footage; error the same; 0.01 ms a pair)
};

static struct MsmState
{
    static constexpr UINT kLevels = 5;          // the brightness: the size the motion is looked for at, and four halvings
    static constexpr UINT kSlots = 10;          // a pass's table: eight pictures read, two written
    static constexpr UINT kPasses = 128;        // a pair's passes at most (x6: some 60 for the pair, 6 for each of its five frames)
    static constexpr UINT kPairs = 8;           // tables kept for so many pairs: a list still on its way keeps its own
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> luma, halve, search, refine, relax, far_away, cross_splat, cross, subpel, at_large, fill, mark, clear, splat, resolve, edge, warp;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    UINT pair = 0, pass = 0;
    // For pictures of one size.
    UINT full_w = 0, full_h = 0, w = 0, h = 0;              // the picture; the size the motion is looked for at
    UINT lw[kLevels] = {}, lh[kLevels] = {};                // the brightness
    UINT bw[kLevels] = {}, bh[kLevels] = {};                // the blocks
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    winrt::com_ptr<ID3D12Resource> bright[2][kLevels];      // two frames' brightness
    winrt::com_ptr<ID3D12Resource> blocks[2][kLevels][2];   // each way, each size: the blocks, twice (one to relax into)
    ID3D12Resource *settled[2][kLevels] = {};               // of the two, the one that holds the blocks now
    winrt::com_ptr<ID3D12Resource> flow[2];                 // each way, the largest size: settled
    winrt::com_ptr<ID3D12Resource> rough[2];                // ... as SubpelMain left them, before FillMain
    winrt::com_ptr<ID3D12Resource> large[2];                // each way: how the picture at large moves (a texel sixteen blocks)
    winrt::com_ptr<ID3D12Resource> landed[2];               // what reached each place, from the frame before and from the one after
    winrt::com_ptr<ID3D12Resource> resolving, resolved;     // what Resolve says of each place; and that with EdgeMain's word beside it
    winrt::com_ptr<ID3D12Resource> marks[2];                // a byte a pixel of the picture: MarkMain, written in turn
    int marks_now = 0;                                      // which of the two holds the marks
    int marks_hold = 0;                                     // 0 whatever the card left there, 1 nothing, 2 the marks
    // The read-outs learnt for the game: cells of a small picture (1: a read-out), and what carries them to the card.
    std::vector<unsigned char> hud_cells;
    int hud_w = 0, hud_h = 0;
    bool hud_pending = false;                               // they are to be put into the marks before the next pair
    winrt::com_ptr<ID3D12Resource> hud_upload;
    // Whose brightness the two sets hold: the frame (never looked into, only compared).
    ID3D12Resource *holds[2] = {};
    int before = 0;                 // which of the two sets is the frame before of the pair at hand
    bool chained = false;           // the frame before of the next pair is the frame after of this one
    bool fast = false;              // the pair at hand was asked for with the lighter search
    MsmTune tune;
    bool failed = false;
} g_msm;

static void MsmClose()
{
    MsmState &s = g_msm;
    for (auto &frame : s.bright) for (auto &texture : frame) texture = nullptr;
    for (auto &way : s.blocks) for (auto &level : way) for (auto &texture : level) texture = nullptr;

    for (auto &texture : s.flow) texture = nullptr;
    for (auto &texture : s.rough) texture = nullptr;
    for (auto &texture : s.large) texture = nullptr;
    for (auto &texture : s.landed) texture = nullptr;
    s.resolving = nullptr;
    s.resolved = nullptr;
    for (auto &texture : s.marks) texture = nullptr;
    s.hud_upload = nullptr;
    s.hud_pending = !s.hud_cells.empty();
    s.marks_hold = 0;
    s.holds[0] = s.holds[1] = nullptr;
    s.chained = false;
    s.full_w = s.full_h = s.w = s.h = 0;
}

// The size the motion is looked for at: this share of the picture, in whole thirty-seconds (five sizes, each
// half the one before, and blocks of two texels at the smallest), 320 x 192 at least.
static void MsmSize(UINT full_w, UINT full_h, int share, UINT &w, UINT &h)
{
    const double part = std::clamp(share, 10, 100) / 100.0;
    w = std::max(320u, static_cast<UINT>(std::lround(full_w * part / 32.0)) * 32u);
    h = std::max(192u, static_cast<UINT>(std::lround(full_h * part / 32.0)) * 32u);
}

// Is everything there for pictures of this size, this format and this "Flow"?
static bool MsmLive(UINT full_w, UINT full_h, DXGI_FORMAT format, int share)
{
    UINT w = 0, h = 0;
    MsmSize(full_w, full_h, share, w, h);
    return g_msm.resolved && g_msm.full_w == full_w && g_msm.full_h == full_h && g_msm.format == format && g_msm.w == w && g_msm.h == h;
}

// The frame before of the next pair is not the frame after of the last one (a cut, a frame let go).
static void MsmForget()
{
    g_msm.chained = false;
}

// SM_MSM_TUNE's text (name=value,...) into the numbers - also tests\msmdata.exe's tunes=, one line a try.
static void MsmTuneRead(MsmTune &t, char *text)
{
    struct { const char *name; float *value; int *whole; } names[] = {
        {"search_far", &t.search_far}, {"refine_far", &t.refine_far}, {"refine_plain", &t.refine_plain}, {"relax_far", &t.relax_far},
        {"relax_small", nullptr, &t.relax_small}, {"relax_mid", nullptr, &t.relax_mid}, {"relax_large", nullptr, &t.relax_large},
        {"sure_gain", &t.sure_gain}, {"sure_floor", &t.sure_floor},
        {"found_floor", &t.found_floor}, {"found_contrast", &t.found_contrast},
        {"back_floor", &t.back_floor}, {"back_far", &t.back_far},
        {"cross", &t.cross}, {"cross_floor", &t.cross_floor}, {"cross_share", &t.cross_share},
        {"far_reach", &t.far_reach}, {"far_share", &t.far_share}, {"far_floor", &t.far_floor},
        {"shows_floor", &t.shows_floor}, {"shows_gain", &t.shows_gain}, {"fill_say", &t.fill_say}, {"alias_ratio", &t.alias_ratio}, {"alias_floor", &t.alias_floor},
        {"large_near", &t.large_near}, {"large_far", &t.large_far},
        {"same", &t.same}, {"differs", &t.differs}, {"mark_pairs", &t.mark_pairs},
        {"mark_most", &t.mark_most}, {"mark_moves", &t.mark_moves}, {"apart", &t.apart}, {"apart_steep", &t.apart_steep},
        {"large_floor", &t.large_floor}, {"large_share", &t.large_share},
        {"nearer_from", &t.nearer_from}, {"nearer_to", &t.nearer_to}, {"edge", &t.edge},
        {"soft", &t.soft}, {"alone_near", &t.alone_near}, {"alone_share", &t.alone_share},
        {"fluid", &t.fluid}, {"fluid_near", &t.fluid_near}, {"fluid_share", &t.fluid_share}, {"direct", &t.direct}, {"mark_holds", &t.mark_holds},
        {"one_sided", &t.one_sided}, {"nearer_word", &t.nearer_word}, {"mixed_fits", &t.mixed_fits},
        {"fluid_apart", &t.fluid_apart}, {"fluid_steep", &t.fluid_steep}, {"sharp_read", &t.sharp_read} };
    char *next = nullptr;
    for (char *part = strtok_s(text, ",", &next); part; part = strtok_s(nullptr, ",", &next))
    {
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = 0;
        for (auto &name : names)
            if (_stricmp(part, name.name) == 0)
            {
                if (name.value) *name.value = static_cast<float>(atof(eq + 1));
                else *name.whole = std::clamp(atoi(eq + 1), 0, 8);
            }
    }
}

// The passes (made once) and what they work in, for pictures of this size and frames in between written in
// this format. share: "Flow", percent. False: there are no frames in between of this generator's.
static bool MsmReady(UINT full_w, UINT full_h, DXGI_FORMAT format, int share)
{
    MsmState &s = g_msm;
    if (s.failed) return false;
    auto fail = [&](const char *what) {
        s.failed = true;
        MsmClose();
        Trace("[msm] unavailable: %s", what);
        return false;
    };
    if (!s.warp)
    {
        // SM_MSM_TUNE=name=value,...: other numbers than the ones above.
        char tune[512] = {};
        if (GetEnvironmentVariableA("SM_MSM_TUNE", tune, sizeof(tune)) > 0)
        {
            MsmTuneRead(s.tune, tune);
        }
        winrt::com_ptr<ID3DBlob> signature, errors;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0}};
        ranges[1].OffsetInDescriptorsFromTableStart = 8;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 20;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 1, &sampler};
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(s.root.put()))))
            return fail("its root signature");
        struct { const char *entry; winrt::com_ptr<ID3D12PipelineState> *pass; } passes[] = {
            {"LumaMain", &s.luma}, {"HalveMain", &s.halve}, {"SearchMain", &s.search}, {"RefineMain", &s.refine}, {"RelaxMain", &s.relax},
            {"FarMain", &s.far_away}, {"FillMain", &s.fill},
            {"CrossSplatMain", &s.cross_splat}, {"CrossMain", &s.cross}, {"SubpelMain", &s.subpel}, {"LargeMain", &s.at_large},
            {"MarkMain", &s.mark}, {"ClearMain", &s.clear}, {"SplatMain", &s.splat}, {"ResolveMain", &s.resolve}, {"EdgeMain", &s.edge},
            {"WarpMain", &s.warp} };
        for (auto &pass : passes)
        {
            winrt::com_ptr<ID3DBlob> code;
            errors = nullptr;
            if (FAILED(CachedCompile(kMsmHlsl, sizeof(kMsmHlsl) - 1, "msm-fg.hlsl", nullptr, nullptr, pass.entry, "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put())))
            {
                s.warp = nullptr;
                return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "a pass did not compile");
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = s.root.get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            if (FAILED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pass.pass->put())))) { s.warp = nullptr; return fail("a pass's pipeline"); }
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MsmState::kPasses * MsmState::kPairs * MsmState::kSlots,
                                           D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(s.heap.put())))) { s.warp = nullptr; return fail("its descriptor heap"); }
    }
    UINT w = 0, h = 0;
    MsmSize(full_w, full_h, share, w, h);
    if (s.full_w == full_w && s.full_h == full_h && s.w == w && s.h == h && s.format == format && s.resolved) return true;
    MsmClose();
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto make = [&](UINT tw, UINT th, DXGI_FORMAT tf, winrt::com_ptr<ID3D12Resource> &texture) {
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = tw; d.Height = th; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = tf; d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                                nullptr, IID_PPV_ARGS(texture.put())));
    };
    for (UINT level = 0; level < MsmState::kLevels; ++level)
    {
        s.lw[level] = w >> level; s.lh[level] = h >> level;
        s.bw[level] = s.lw[level] / 2; s.bh[level] = s.lh[level] / 2;
        for (int frame = 0; frame < 2; ++frame)
            if (!make(s.lw[level], s.lh[level], DXGI_FORMAT_R16_FLOAT, s.bright[frame][level])) return fail("its pictures");
        for (int way = 0; way < 2; ++way)
            for (int turn = 0; turn < 2; ++turn)
                if (!make(s.bw[level], s.bh[level], DXGI_FORMAT_R16G16B16A16_FLOAT, s.blocks[way][level][turn])) return fail("its pictures");
    }
    for (int way = 0; way < 2; ++way)
        if (!make(s.bw[0], s.bh[0], DXGI_FORMAT_R16G16B16A16_FLOAT, s.flow[way]) || !make(s.bw[0], s.bh[0], DXGI_FORMAT_R16G16B16A16_FLOAT, s.rough[way]) ||
            !make(s.bw[0], s.bh[0], DXGI_FORMAT_R32_UINT, s.landed[way]) ||
            !make(s.bw[MsmState::kLevels - 1], s.bh[MsmState::kLevels - 1], DXGI_FORMAT_R16G16B16A16_FLOAT, s.large[way]))
            return fail("its pictures");
    if (!make(s.bw[0], s.bh[0], DXGI_FORMAT_R16G16B16A16_FLOAT, s.resolved) || !make(s.bw[0], s.bh[0], DXGI_FORMAT_R16G16B16A16_FLOAT, s.resolving) ||
        !make(full_w, full_h, DXGI_FORMAT_R8_UINT, s.marks[0]) ||
        !make(full_w, full_h, DXGI_FORMAT_R8_UINT, s.marks[1]))
        return fail("its pictures");
    s.full_w = full_w; s.full_h = full_h; s.w = w; s.h = h; s.format = format;
    Trace("[msm] MSM FG 1.1: the motion is looked for at %ux%u (%u x %u blocks), frames in between drawn at %ux%u", w, h, s.bw[0], s.bh[0], full_w, full_h);
    return true;
}

// One pass, recorded. read: up to six pictures (t0..t5) and what reached each place (t6, t7); write: a picture
// (u0) or what reaches each place (u1). A picture of this file's own that is written is opened for it and put
// back to rest.
struct MsmPass
{
    ID3D12PipelineState *state = nullptr;
    ID3D12Resource *read[6] = {}, *reached[2] = {};
    ID3D12Resource *write = nullptr, *reach = nullptr;
    bool own = true;                    // the picture written is this file's own (at rest readable)
    UINT w = 0, h = 0;                  // what is written
    float inv_a[2] = {}, k[4] = {}, m[4] = {}, n[4] = {}, o[4] = {};
};

static void MsmRecord(ID3D12GraphicsCommandList *list, const MsmPass &p)
{
    MsmState &s = g_msm;
    if (s.pass >= MsmState::kPasses) return;
    const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT first = (s.pair * MsmState::kPasses + s.pass) * MsmState::kSlots;
    ++s.pass;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(first) * each;
    gpu.ptr += static_cast<UINT64>(first) * each;
    // A place of the table nothing is put in gets an empty picture of its kind.
    D3D12_SHADER_RESOURCE_VIEW_DESC none = {};
    none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    none.Texture2D.MipLevels = 1;
    D3D12_UNORDERED_ACCESS_VIEW_DESC none_written = {};
    none_written.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
    for (int i = 0; i < 6; ++i, at.ptr += each)
    {
        none.Format = DXGI_FORMAT_R8_UNORM;
        lfCore.device->CreateShaderResourceView(p.read[i], p.read[i] ? nullptr : &none, at);
    }
    for (int i = 0; i < 2; ++i, at.ptr += each)
    {
        none.Format = DXGI_FORMAT_R32_UINT;
        lfCore.device->CreateShaderResourceView(p.reached[i], p.reached[i] ? nullptr : &none, at);
    }
    none_written.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lfCore.device->CreateUnorderedAccessView(p.write, nullptr, p.write ? nullptr : &none_written, at);
    at.ptr += each;
    none_written.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateUnorderedAccessView(p.reach, nullptr, p.reach ? nullptr : &none_written, at);
    ID3D12Resource *const written = p.write ? p.write : p.reach;
    if (p.own)
    {
        auto open = Barrier(written, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &open);
    }
    const float numbers[20] = { p.inv_a[0], p.inv_a[1], 1.0f / p.w, 1.0f / p.h, p.k[0], p.k[1], p.k[2], p.k[3],
                                p.m[0], p.m[1], p.m[2], p.m[3], p.n[0], p.n[1], p.n[2], p.n[3], p.o[0], p.o[1], p.o[2], p.o[3] };
    list->SetPipelineState(p.state);
    list->SetComputeRoot32BitConstants(0, 20, numbers, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((p.w + 7) / 8, (p.h + 7) / 8, 1);
    if (p.own)
    {
        auto shut = Barrier(written, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &shut);
    }
}

static void MsmBegin(ID3D12GraphicsCommandList *list)
{
    ID3D12DescriptorHeap *heaps[] = { g_msm.heap.get() };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_msm.root.get());
}

// The read-outs learnt for the game being played (or none: w 0). They go into the marks before the next pair.
static void MsmHud(const std::vector<unsigned char> &cells, int w, int h)
{
    MsmState &s = g_msm;
    if (w <= 0 || h <= 0 || cells.size() != static_cast<size_t>(w) * h) { s.hud_cells.clear(); s.hud_w = s.hud_h = 0; }
    else { s.hud_cells = cells; s.hud_w = w; s.hud_h = h; }
    s.hud_pending = true;
    size_t count = 0;
    for (unsigned char c : s.hud_cells) count += c != 0;
    Trace(count ? "[msm] read-outs from Adaptive Motion Learning: %zu cells of the small picture held as marks" :
                  "[msm] no read-outs from Adaptive Motion Learning (off, or none learnt) - the marks it gave are cleared", count);
}

// The marks made anew from the learnt read-outs: a pixel in one of their cells flagged, a mark, its count full;
// every other pixel nothing (it is a mark again four pairs on, if it is one). False: left as they were.
static bool MsmHudSeed(ID3D12GraphicsCommandList *list)
{
    MsmState &s = g_msm;
    s.hud_pending = false;
    ID3D12Resource *const target = s.marks[s.marks_now].get();
    if (!target) return false;
    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT print = {};
    UINT64 total = 0;
    lfCore.device->GetCopyableFootprints(&desc, 0, 1, 0, &print, nullptr, nullptr, &total);
    if (!s.hud_upload)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(s.hud_upload.put()))))
            return false;
    }
    unsigned char *bytes = nullptr;
    if (FAILED(s.hud_upload->Map(0, nullptr, reinterpret_cast<void **>(&bytes))) || !bytes) return false;
    const bool any = !s.hud_cells.empty();
    for (UINT y = 0; y < s.full_h; ++y)
    {
        unsigned char *row = bytes + print.Offset + static_cast<size_t>(y) * print.Footprint.RowPitch;
        if (!any) { memset(row, 0, s.full_w); continue; }
        const unsigned char *cells = s.hud_cells.data() + static_cast<size_t>(y * static_cast<UINT>(s.hud_h) / s.full_h) * s.hud_w;
        for (UINT x = 0; x < s.full_w; ++x) row[x] = cells[x * static_cast<UINT>(s.hud_w) / s.full_w] ? static_cast<unsigned char>(128u | 64u | 24u) : 0;
    }
    s.hud_upload->Unmap(0, nullptr);
    auto open = Barrier(target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &open);
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = target; to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = s.hud_upload.get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = print;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto shut = Barrier(target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &shut);
    return true;
}

// A frame's brightness, all five sizes, into one of the two sets. frame: readable by a compute shader.
static void MsmBrightness(ID3D12GraphicsCommandList *list, ID3D12Resource *frame, int set)
{
    MsmState &s = g_msm;
    MsmPass p;
    p.state = s.luma.get();
    p.read[0] = frame; p.write = s.bright[set][0].get();
    p.w = s.lw[0]; p.h = s.lh[0];
    p.inv_a[0] = 1.0f / s.full_w; p.inv_a[1] = 1.0f / s.full_h;
    MsmRecord(list, p);
    for (UINT level = 1; level < MsmState::kLevels; ++level)
    {
        MsmPass q;
        q.state = s.halve.get();
        q.read[0] = s.bright[set][level - 1].get(); q.write = s.bright[set][level].get();
        q.w = s.lw[level]; q.h = s.lh[level];
        q.inv_a[0] = 1.0f / s.lw[level - 1]; q.inv_a[1] = 1.0f / s.lh[level - 1];
        MsmRecord(list, q);
    }
    s.holds[set] = frame;
}

// What a pair may be told and asked beside its two frames.
struct MsmAsk
{
    // Where the picture as a whole is expected to have gone from the frame before to the frame after, pixels
    // (the engine's own count of it, or what the mouse leads one to expect); both 0: not known.
    float expected_x = 0.0f, expected_y = 0.0f;
    bool marks = true;      // read-outs are kept still ("Keep still pixels" in the app)
    bool fast = false;      // the lighter search: relaxed half as often, the two ways not told of each other,
                            // and no place asked whether the picture at large's movement suits it better (ResolveMain)
};

// A pair's own work: the two frames' brightness (the frame before's is there already when the pair follows
// the last one), how every block moves, each way, and which pixels are marks on the screen. before, after: the
// two real frames, both at `rest` as they are handed over (any state a compute shader does not read them in;
// they are put back to it).
static void MsmPair(ID3D12GraphicsCommandList *list, ID3D12Resource *before, ID3D12Resource *after, D3D12_RESOURCE_STATES rest,
                    const MsmAsk &ask = MsmAsk())
{
    MsmState &s = g_msm;
    s.pair = (s.pair + 1) % MsmState::kPairs;
    s.pass = 0;
    MsmBegin(list);
    const bool readable = (rest & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (!readable)
    {
        D3D12_RESOURCE_BARRIER open[] = { Barrier(before, rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Barrier(after, rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
        list->ResourceBarrier(2, open);
    }
    // Which set is the frame before's: the one that took the frame after last time, if this pair follows that one.
    int set_before = -1;
    if (s.chained) for (int set = 0; set < 2; ++set) if (s.holds[set] == before) set_before = set;
    if (set_before < 0) { set_before = 0; MsmBrightness(list, before, 0); }
    const int set_after = 1 - set_before;
    MsmBrightness(list, after, set_after);
    s.before = set_before;
    s.chained = true;
    s.fast = ask.fast;
    // Each way: way 0 from the frame before to the one after, way 1 back.
    const UINT last = MsmState::kLevels - 1;
    const float per_texel_x = static_cast<float>(s.full_w) / s.w, per_texel_y = static_cast<float>(s.full_h) / s.h;
    for (int way = 0; way < 2; ++way)
    {
        const int from = way == 0 ? set_before : set_after, to = way == 0 ? set_after : set_before;
        for (int level = static_cast<int>(last); level >= 0; --level)
        {
            MsmPass p;
            p.read[0] = s.bright[from][level].get(); p.read[1] = s.bright[to][level].get();
            p.inv_a[0] = 1.0f / s.lw[level]; p.inv_a[1] = 1.0f / s.lh[level];
            p.w = s.bw[level]; p.h = s.bh[level];
            p.m[2] = static_cast<float>(s.bw[level] - 1); p.m[3] = static_cast<float>(s.bh[level] - 1);
            p.write = s.blocks[way][level][0].get();
            p.k[2] = s.tune.sure_gain; p.k[3] = s.tune.sure_floor;
            if (level == static_cast<int>(last))
            {
                p.state = s.search.get();
                p.k[0] = s.tune.search_far;
                const float sign = way == 0 ? 1.0f : -1.0f, texel = static_cast<float>(1u << last);
                p.n[0] = sign * ask.expected_x / (per_texel_x * texel); p.n[1] = sign * ask.expected_y / (per_texel_y * texel);
            }
            else
            {
                p.state = s.refine.get();
                p.k[0] = s.tune.refine_far; p.k[1] = s.tune.refine_plain;
                p.read[2] = s.settled[way][level + 1];
                p.m[0] = static_cast<float>(s.bw[level + 1] - 1); p.m[1] = static_cast<float>(s.bh[level + 1] - 1);
            }
            MsmRecord(list, p);
            int holds = 0;
            const int times = ask.fast ? (level >= static_cast<int>(last) - 1 ? 2 : 1)
                                       : (level == 0 ? s.tune.relax_large : level >= static_cast<int>(last) - 1 ? s.tune.relax_small : s.tune.relax_mid);
            for (int i = 0; i < times; ++i)
            {
                MsmPass q = p;
                q.state = s.relax.get();
                q.k[0] = s.tune.relax_far;
                q.read[2] = s.blocks[way][level][holds].get();
                q.write = s.blocks[way][level][1 - holds].get();
                MsmRecord(list, q);
                holds = 1 - holds;
            }
            if (level == 1 && s.tune.far_reach >= 2.0f)
            {
                // What was not found by now: far and wide.
                MsmPass q = p;
                q.state = s.far_away.get();
                q.read[2] = s.blocks[way][level][holds].get();
                q.write = s.blocks[way][level][1 - holds].get();
                q.n[0] = s.tune.found_floor; q.n[1] = s.tune.found_contrast; q.n[2] = s.tune.far_share; q.n[3] = s.tune.far_floor;
                q.m[0] = s.tune.far_reach; q.m[1] = s.tune.shows_floor;
                MsmRecord(list, q);
                holds = 1 - holds;
            }
            s.settled[way][level] = s.blocks[way][level][holds].get();
        }
    }
    // What one way found, the other is told (the largest size).
    if (s.tune.cross > 0.5f && !ask.fast)
    {
        for (int way = 0; way < 2; ++way)
        {
            MsmPass wipe;
            wipe.state = s.clear.get();
            wipe.reach = s.landed[way].get();
            wipe.w = s.bw[0]; wipe.h = s.bh[0];
            MsmRecord(list, wipe);
            MsmPass p;
            p.state = s.cross_splat.get();
            p.read[0] = s.settled[way][0];
            p.reach = s.landed[way].get();
            p.w = s.bw[0]; p.h = s.bh[0];
            p.m[2] = static_cast<float>(s.bw[0] - 1); p.m[3] = static_cast<float>(s.bh[0] - 1);
            MsmRecord(list, p);
        }
        int told[2] = {};
        for (int way = 0; way < 2; ++way)
        {
            const int from = way == 0 ? set_before : set_after, to = way == 0 ? set_after : set_before;
            // Into the one of the two that does not hold the blocks now.
            const int other = s.settled[way][0] == s.blocks[way][0][0].get() ? 1 : 0;
            MsmPass p;
            p.state = s.cross.get();
            p.read[0] = s.bright[from][0].get(); p.read[1] = s.bright[to][0].get();
            p.read[2] = s.settled[way][0];
            p.reached[0] = s.landed[1 - way].get();
            told[way] = other;
            p.write = s.blocks[way][0][other].get();
            p.inv_a[0] = 1.0f / s.lw[0]; p.inv_a[1] = 1.0f / s.lh[0];
            p.w = s.bw[0]; p.h = s.bh[0];
            p.k[0] = s.tune.cross_floor; p.k[1] = s.tune.cross_share; p.k[2] = s.tune.sure_gain; p.k[3] = s.tune.sure_floor;
            MsmRecord(list, p);
        }
        for (int way = 0; way < 2; ++way) s.settled[way][0] = s.blocks[way][0][told[way]].get();
    }
    for (int way = 0; way < 2; ++way)
    {
        const int from = way == 0 ? set_before : set_after, to = way == 0 ? set_after : set_before;
        MsmPass p;
        p.state = s.subpel.get();
        p.read[0] = s.bright[from][0].get(); p.read[1] = s.bright[to][0].get();
        p.read[2] = s.settled[way][0]; p.read[3] = s.settled[1 - way][0];
        p.write = s.rough[way].get();
        p.inv_a[0] = 1.0f / s.lw[0]; p.inv_a[1] = 1.0f / s.lh[0];
        p.w = s.bw[0]; p.h = s.bh[0];
        p.k[0] = s.tune.found_floor; p.k[1] = s.tune.found_contrast; p.k[2] = s.tune.back_floor; p.k[3] = s.tune.back_far;
        p.m[0] = s.tune.shows_gain; p.m[1] = s.tune.shows_floor;
        p.m[2] = static_cast<float>(s.bw[0] - 1); p.m[3] = static_cast<float>(s.bh[0] - 1);
        MsmRecord(list, p);
    }
    for (int way = 0; way < 2; ++way)
    {
        MsmPass p;
        p.state = s.at_large.get();
        p.read[0] = s.rough[way].get();
        p.write = s.large[way].get();
        p.w = s.bw[last]; p.h = s.bh[last];
        p.k[0] = s.tune.large_near; p.k[1] = s.tune.large_far;
        p.m[0] = static_cast<float>(s.bw[0] / s.bw[last]);
        p.m[2] = static_cast<float>(s.bw[0] - 1); p.m[3] = static_cast<float>(s.bh[0] - 1);
        MsmRecord(list, p);
    }
    for (int way = 0; way < 2; ++way)
    {
        MsmPass p;
        p.state = s.fill.get();
        p.read[0] = s.rough[way].get(); p.read[1] = s.large[way].get();
        p.read[2] = way == 0 ? before : after; p.read[3] = way == 0 ? after : before;
        p.write = s.flow[way].get();
        p.inv_a[0] = 1.0f / s.lw[0]; p.inv_a[1] = 1.0f / s.lh[0];
        p.w = s.bw[0]; p.h = s.bh[0];
        p.k[0] = s.tune.fill_say; p.k[1] = s.tune.alias_ratio; p.k[2] = s.tune.alias_floor;
        MsmRecord(list, p);
    }
    // The marks: counted on from the pair before - also over a pair that was let go (nothing moved, and what
    // stood still then is as much a mark after it).
    if (s.marks_hold == 0 || (!ask.marks && s.marks_hold != 1))
    {
        MsmPass wipe;
        wipe.state = s.clear.get();
        wipe.reach = s.marks[s.marks_now].get();
        wipe.w = s.full_w; wipe.h = s.full_h;
        MsmRecord(list, wipe);
        s.marks_hold = 1;
        s.hud_pending = !s.hud_cells.empty();
    }
    if (ask.marks && s.hud_pending && MsmHudSeed(list)) s.marks_hold = 2;
    if (ask.marks)
    {
        MsmPass p;
        p.state = s.mark.get();
        p.read[0] = before; p.read[1] = after; p.read[2] = s.flow[0].get(); p.read[3] = s.flow[1].get(); p.read[4] = s.large[0].get();
        p.reached[0] = s.marks[s.marks_now].get();
        s.marks_now = 1 - s.marks_now;
        p.reach = s.marks[s.marks_now].get();
        p.inv_a[0] = 1.0f / s.full_w; p.inv_a[1] = 1.0f / s.full_h;
        p.w = s.full_w; p.h = s.full_h;
        p.k[0] = s.tune.mark_pairs; p.k[1] = per_texel_x; p.k[2] = per_texel_y; p.k[3] = s.tune.mark_most;
        p.n[0] = s.tune.same; p.n[1] = s.tune.differs;
        p.m[0] = s.tune.mark_moves;
        MsmRecord(list, p);
        s.marks_hold = 2;
    }
    if (!readable)
    {
        D3D12_RESOURCE_BARRIER shut[] = { Barrier(before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest),
                                          Barrier(after, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest) };
        list->ResourceBarrier(2, shut);
    }
}

// A frame in between of the pair MsmPair was last called for, at the moment `moment` (0 the frame before .. 1
// the frame after), into `into` - a picture of the frames' size and format that can be written by a compute
// shader, at `into_rest` as handed over and put back to it. before, after, rest: as for MsmPair.
static void MsmFrame(ID3D12GraphicsCommandList *list, ID3D12Resource *before, ID3D12Resource *after, D3D12_RESOURCE_STATES rest,
                     ID3D12Resource *into, D3D12_RESOURCE_STATES into_rest, float moment)
{
    MsmState &s = g_msm;
    MsmBegin(list);
    // (Direct, round 40: nothing is carried - the two ways' carrying is not done at all.)
    for (int way = 0; way < 2 && s.tune.direct <= 0.0f; ++way)
    {
        MsmPass wipe;
        wipe.state = s.clear.get();
        wipe.reach = s.landed[way].get();
        wipe.w = s.bw[0]; wipe.h = s.bh[0];
        MsmRecord(list, wipe);
        MsmPass p;
        p.state = s.splat.get();
        p.read[0] = s.flow[way].get(); p.read[1] = s.large[way].get();
        p.reach = s.landed[way].get();
        p.inv_a[0] = 1.0f / s.lw[0]; p.inv_a[1] = 1.0f / s.lh[0];
        p.w = s.bw[0]; p.h = s.bh[0];
        p.k[0] = way == 0 ? moment : 1.0f - moment;
        p.k[1] = way == 0 ? 1.0f : -1.0f;
        p.m[2] = static_cast<float>(s.bw[0] - 1); p.m[3] = static_cast<float>(s.bh[0] - 1);
        MsmRecord(list, p);
    }
    // (The two frames are read from here on.)
    const bool readable = (rest & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
    if (!readable)
    {
        D3D12_RESOURCE_BARRIER open[2] = { Barrier(before, rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                           Barrier(after, rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) };
        list->ResourceBarrier(2, open);
    }
    {
        MsmPass p;
        p.state = s.resolve.get();
        p.reached[0] = s.landed[0].get(); p.reached[1] = s.landed[1].get();
        p.read[0] = before; p.read[1] = after; p.read[3] = s.large[0].get();
        p.read[4] = s.flow[0].get(); p.read[5] = s.flow[1].get();
        p.k[1] = s.tune.direct;
        p.write = s.resolving.get();
        p.inv_a[0] = 1.0f / s.lw[0]; p.inv_a[1] = 1.0f / s.lh[0];
        p.w = s.bw[0]; p.h = s.bh[0];
        p.k[0] = moment;
        p.m[0] = s.tune.large_floor; p.m[1] = s.tune.large_share;
        p.m[2] = static_cast<float>(s.bw[0] - 1); p.m[3] = static_cast<float>(s.bh[0] - 1);
        p.o[2] = s.tune.large_share > 0.0f && !s.fast ? 1.0f : 0.0f;         // (the lighter search goes without: it is a sixth of a frame's work on a weak card)
        p.o[3] = (static_cast<float>(s.w) / s.full_w) * (static_cast<float>(s.w) / s.full_w);       // a pixel of the picture
        MsmRecord(list, p);
    }
    {
        MsmPass p;
        p.state = s.edge.get();
        p.k[0] = s.tune.alone_near; p.k[1] = s.tune.alone_share;
        p.k[2] = s.tune.fluid > 0.0f ? s.tune.fluid_near : 0.0f; p.k[3] = s.tune.fluid_share;
        p.read[0] = s.resolving.get();
        p.write = s.resolved.get();
        p.w = s.bw[0]; p.h = s.bh[0];
        p.m[2] = static_cast<float>(s.bw[0] - 1); p.m[3] = static_cast<float>(s.bh[0] - 1);
        MsmRecord(list, p);
    }
    D3D12_RESOURCE_BARRIER open = Barrier(into, into_rest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &open);
    {
        MsmPass p;
        p.state = s.warp.get();
        p.read[0] = before; p.read[1] = after; p.read[2] = s.resolved.get(); p.read[3] = s.large[0].get();
        p.read[4] = s.flow[0].get(); p.read[5] = s.flow[1].get();     // (round 53: whether what each frame shows goes this way)
        p.reached[0] = s.marks[s.marks_now].get();
        p.write = into; p.own = false;
        // (inv_a: the frames' texel for Draw's reading between pixels - Between - or 0, read plainly.)
        p.inv_a[0] = s.tune.sharp_read > 0.0f ? 1.0f / s.full_w : 0.0f; p.inv_a[1] = s.tune.sharp_read > 0.0f ? 1.0f / s.full_h : 0.0f;
        p.w = s.full_w; p.h = s.full_h;
        p.k[0] = moment;
        p.k[1] = static_cast<float>(s.full_w) / s.w; p.k[2] = static_cast<float>(s.full_h) / s.h;
        p.k[3] = s.tune.mark_holds;
        p.n[0] = s.tune.soft;
        p.n[1] = s.tune.fluid;
        p.n[2] = s.tune.fluid > 0.0f ? s.tune.fluid_apart : s.tune.apart; p.n[3] = s.tune.fluid > 0.0f ? s.tune.fluid_steep : s.tune.apart_steep;
        p.o[0] = s.tune.nearer_from; p.o[1] = s.tune.nearer_to; p.o[2] = s.tune.edge; p.o[3] = s.tune.one_sided;
        p.m[0] = static_cast<float>(s.bw[0]); p.m[1] = static_cast<float>(s.bh[0]); p.m[2] = s.tune.nearer_word; p.m[3] = s.tune.mixed_fits;
        MsmRecord(list, p);
    }
    D3D12_RESOURCE_BARRIER shut[3] = { Barrier(into, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, into_rest),
                                       Barrier(before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest),
                                       Barrier(after, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest) };
    list->ResourceBarrier(readable ? 1 : 3, shut);
}
