// lf_dlss5.inl - pure C++ DLSS 5 (DLSSNR / NVSDK_NGX_Feature_Reserved18)
// upscaler / neural enhancer ported from ScreenNative.
//
// - Never bundles or redistributes nvngx_dlssnr.dll: loads it strictly from
//   the user-selected path (sm_options.h: Dlss5GetDll()).
// - Consumes the engine's own motion field (pipe.motion.gpu_tex) directly on
//   the GPU (lf_own_motion.inl / lf_ofa.inl) — zero Python.
// - Enhances pipe.picture.gpu_tex in-place once per real captured frame so
//   every generated intermediate frame (DLSS MFG, XeSS MFG, SMFG 1.4, SMAI)
//   inherits the enhanced result.

namespace dlss5
{

static constexpr unsigned kMaxPasses = 6;
static constexpr int kHaloRadius = 64;
static constexpr int kHaloStep = 4;
static constexpr float kHaloSigmaR = 0.06f;
static constexpr float kHaloShare = 0.5f;

static const char kResizeFineHlsl[] = R"hlsl(
Texture2D<float4> picIn : register(t0);
RWTexture2D<float4> picOut : register(u0);
cbuffer Extent : register(b0) { uint outW; uint outH; uint inW; uint inH; };

float4 PicAt(int2 at, int2 last) { return picIn[clamp(at, int2(0, 0), last)]; }

float4 AreaAverage(uint2 cell, int2 last)
{
    float2 from = float2(cell) * float2(inW, inH) / float2(outW, outH);
    float2 to = float2(cell + 1) * float2(inW, inH) / float2(outW, outH);
    float4 acc = 0;
    [loop] for (int row = int(floor(from.y)); row < int(ceil(to.y)); ++row) {
        float wy = max(0, min(to.y, float(row + 1)) - max(from.y, float(row)));
        [loop] for (int col = int(floor(from.x)); col < int(ceil(to.x)); ++col) {
            float wx = max(0, min(to.x, float(col + 1)) - max(from.x, float(col)));
            acc += PicAt(int2(col, row), last) * wx * wy;
        }
    }
    return acc / ((to.x - from.x) * (to.y - from.y));
}

float4 Bilinear(uint2 cell, int2 last)
{
    float2 pos = (float2(cell) + .5) * float2(inW, inH) / float2(outW, outH) - .5;
    int2 base = int2(floor(pos));
    float2 along = frac(pos);
    float4 upper = lerp(PicAt(base, last), PicAt(base + int2(1, 0), last), along.x);
    float4 lower = lerp(PicAt(base + int2(0, 1), last), PicAt(base + int2(1, 1), last), along.x);
    return lerp(upper, lower, along.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= outW || cell.y >= outH) return;
    int2 last = int2(inW - 1, inH - 1);
    if (inW > outW || inH > outH) picOut[cell.xy] = AreaAverage(cell.xy, last);
    else picOut[cell.xy] = Bilinear(cell.xy, last);
}
)hlsl";

static const char kDetailHlsl[] = R"hlsl(
Texture2D<float4> screenPic : register(t0);
Texture2D<float4> netBefore : register(t1);
Texture2D<float4> netAfter : register(t2);
Texture2D<float4> netChange : register(t3);
RWTexture2D<float4> composed : register(u0);
cbuffer Detail : register(b0) { uint picW; uint picH; float gain; float edgeGuard; float keepColour; float lowLight; };
SamplerState bilinear : register(s0);
static const float3 kLumaWeights = float3(0.299f, 0.587f, 0.114f);

float3 ChangeAt(float2 uv, float3 here)
{
    if (edgeGuard <= 0.0f)
    {
        float3 before = netBefore.SampleLevel(bilinear, uv, 0).rgb;
        float3 after = netAfter.SampleLevel(bilinear, uv, 0).rgb;
        return after - before;
    }
    uint netW, netH;
    netBefore.GetDimensions(netW, netH);
    float2 pos = uv * float2(netW, netH) - 0.5f;
    float2 corner = floor(pos);
    float2 part = pos - corner;
    int2 base = int2(corner);
    int2 last = int2(int(netW) - 1, int(netH) - 1);
    float hereLuma = dot(here, kLumaWeights);
    float3 total = 0.0f;
    float weights = 0.0f;
    [unroll] for (int dy = 0; dy < 2; ++dy)
    [unroll] for (int dx = 0; dx < 2; ++dx)
    {
        int2 at = clamp(base + int2(dx, dy), int2(0, 0), last);
        float spatial = (dx == 0 ? 1.0f - part.x : part.x) * (dy == 0 ? 1.0f - part.y : part.y);
        float lumaGap = dot(netBefore[at].rgb, kLumaWeights) - hereLuma;
        float similar = exp(-lumaGap * lumaGap * 50.0f);
        float weight = spatial * (1.0f - edgeGuard + edgeGuard * similar) + 1e-5f;
        total += netChange[at].rgb * weight;
        weights += weight;
    }
    return total / weights;
}

float3 SmoothChangeAt(float2 uv, float hereLuma)
{
    uint netW, netH;
    netBefore.GetDimensions(netW, netH);
    int2 centre = int2(uv * float2(netW, netH));
    int2 last = int2(int(netW) - 1, int(netH) - 1);
    float3 total = 0.0f;
    float weights = 0.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        int2 at = clamp(centre + int2(dx, dy), int2(0, 0), last);
        float lumaGap = dot(netBefore[at].rgb, kLumaWeights) - hereLuma;
        float weight = exp(-lumaGap * lumaGap * 200.0f) * (dx == 0 && dy == 0 ? 2.0f : 1.0f) + 1e-5f;
        total += netChange[at].rgb * weight;
        weights += weight;
    }
    return total / weights;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= picW || cell.y >= picH) return;
    float2 uv = (float2(cell.xy) + 0.5f) / float2(picW, picH);
    float3 here = screenPic[cell.xy].rgb;
    float dark = 0.0f;
    if (lowLight > 0.0f)
    {
        float lumaHere = dot(here, kLumaWeights);
        dark = (1.0f - smoothstep(0.04f, 0.30f, lumaHere)) * saturate(lowLight);
        if (dark > 0.0f)
        {
            float3 sum = here;
            float sumWeight = 1.0f;
            int2 last = int2(int(picW) - 1, int(picH) - 1);
            [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0) continue;
                float3 there = screenPic[clamp(int2(cell.xy) + int2(dx, dy), int2(0, 0), last)].rgb;
                float lumaGap = dot(there, kLumaWeights) - lumaHere;
                float weight = exp(-lumaGap * lumaGap * 200.0f);
                sum += there * weight;
                sumWeight += weight;
            }
            here = lerp(here, sum / sumWeight, dark);
        }
    }
    float3 change = ChangeAt(uv, here);
    if (dark > 0.0f) change = lerp(change, SmoothChangeAt(uv, dot(here, kLumaWeights)), dark);
    float3 target = max(here + change * gain, 0.0);
    if (keepColour > 0.0f)
    {
        float lumaHere = dot(here, kLumaWeights);
        float lumaTarget = dot(target, kLumaWeights);
        float3 scaled = here * (lumaTarget / max(lumaHere, 1e-4f));
        float3 added = here + (lumaTarget - lumaHere);
        float3 kept = lerp(added, scaled, saturate(lumaHere / 0.06f));
        float top = max(kept.r, max(kept.g, kept.b));
        if (top > 1.0f) kept = lumaTarget + (kept - lumaTarget) * saturate((1.0f - lumaTarget) / max(top - lumaTarget, 1e-4f));
        target = lerp(target, max(kept, 0.0f), saturate(keepColour));
    }
    composed[cell.xy] = float4(target, 1.0);
}
)hlsl";

static const char kHaloBlurHlsl[] = R"hlsl(
Texture2D<float4>   gGuide : register(t0);
Texture2D<float4>   gA     : register(t1);
Texture2D<float4>   gB     : register(t2);
RWTexture2D<float4> gDst   : register(u0);
cbuffer HC : register(b0) { uint gW; uint gH; int gDirX; int gDirY; int gRadius; int gStep; uint gMode; float gSigmaR; uint gMvW; uint gMvH; };
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    int2 hi = int2(int(gW) - 1, int(gH) - 1);
    int2 dir = int2(gDirX, gDirY);
    float lc = dot(gGuide[p].rgb, kLuma);
    float ss = max(1.0f, float(gRadius) * 0.5f);
    float inv_ss = 1.0f / (2.0f * ss * ss);
    float inv_sr = 1.0f / (2.0f * gSigmaR * gSigmaR);
    float4 acc = 0.0f;
    float wsum = 0.0f;
    [loop] for (int k = -gRadius; k <= gRadius; k += gStep)
    {
        int2 q = clamp(p + dir * k, int2(0, 0), hi);
        float dl = dot(gGuide[q].rgb, kLuma) - lc;
        float w = exp(-float(k * k) * inv_ss - dl * dl * inv_sr);
        float4 s = gMode == 0 ? float4(gA[q].rgb - gB[q].rgb, 0.0f) : gA[q];
        acc += s * w;
        wsum += w;
    }
    gDst[p] = acc / max(wsum, 1e-6f);
}
)hlsl";

static const char kHaloFixHlsl[] = R"hlsl(
Texture2D<float4>   gIn  : register(t0);
Texture2D<float4>   gOut : register(t1);
Texture2D<float4>   gBil : register(t2);
Texture2D<float2>   gMv  : register(t3);
RWTexture2D<float4> gDst : register(u0);
cbuffer HC : register(b0) { uint gW; uint gH; int gDirX; int gDirY; int gRadius; int gStep; uint gMode; float gHalo; uint gMvW; uint gMvH; };
float2 MotionAt(int2 q, int2 mhi)
{
    float2 m = gMv[clamp(q, int2(0, 0), mhi)];
    return all(isfinite(m)) ? m : float2(0.0f, 0.0f);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    int2 hi = int2(int(gW) - 1, int(gH) - 1);
    float3 e = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), hi);
        e += (gOut[q].rgb - gIn[q].rgb) - gBil[q].rgb;
    }
    float3 d = gOut[p].rgb - gIn[p].rgb;
    float gain = gHalo;
    if (gMvW > 0 && gMvH > 0)
    {
        float2 toMv = float2(gMvW, gMvH) / float2(gW, gH);
        int2 mhi = int2(int(gMvW) - 1, int(gMvH) - 1);
        int2 q = int2((float2(p) + 0.5f) * toMv);
        float2 m0 = MotionAt(q, mhi);
        float spread = 0.0f;
        [unroll] for (int ring = 0; ring < 2; ++ring)
        {
            float2 r = (ring == 0 ? 5.0f : 12.0f) * toMv;
            int2 rx = int2(max(1.0f, round(r.x)), 0);
            int2 ry = int2(0, max(1.0f, round(r.y)));
            spread = max(spread, length(MotionAt(q + rx, mhi) - m0));
            spread = max(spread, length(MotionAt(q - rx, mhi) - m0));
            spread = max(spread, length(MotionAt(q + ry, mhi) - m0));
            spread = max(spread, length(MotionAt(q - ry, mhi) - m0));
        }
        float border = saturate((spread / max(toMv.x, 1e-4f) - 0.75f) / 3.25f);
        gain = min(1.0f, gHalo * (1.0f + border));
    }
    gDst[p] = float4(d - gain * (e / 9.0f), 0.0f);
}
)hlsl";

static const char kFinishHlsl[] = R"hlsl(
Texture2D<float4>   gSrc  : register(t0);
Texture2D<float2>   gMv   : register(t1);
Texture2D<float4>   gHist : register(t2);
RWTexture2D<float4> gDst  : register(u0);
RWTexture2D<float4> gNext : register(u1);
SamplerState        gLin  : register(s0);
cbuffer PC : register(b0)
{
    uint gW; uint gH; float gSharp; float gEdge;
    uint gMvW; uint gMvH; uint gFlags; float gBright;
    float gContrast; float gSat; float gWarm; float gPad;
};
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);
static const float3 kLuma709 = float3(0.2126f, 0.7152f, 0.0722f);
float L(float3 c) { return dot(c, kLuma); }

float3 Grade(float3 c)
{
    c = saturate(c);
    if (gBright != 0.0f)
        c = pow(max(c, 1e-5f), exp2(-gBright * 0.7f));
    if (gContrast > 0.0f)
        c = lerp(c, c * c * (3.0f - 2.0f * c), gContrast * 0.85f);
    else if (gContrast < 0.0f)
        c = lerp(c, 0.5f + (c - 0.5f) * 0.5f, -gContrast);
    if (gSat != 0.0f)
    {
        float l = dot(c, kLuma709);
        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float gain = gSat > 0.0f ? 1.0f + gSat * (1.0f - 0.6f * saturate(chroma)) : 1.0f + gSat;
        c = l + (c - l) * gain;
    }
    if (gWarm != 0.0f)
    {
        float l0 = dot(c, kLuma709);
        float3 t = c * float3(1.0f + 0.10f * gWarm, 1.0f + 0.015f * gWarm, 1.0f - 0.10f * gWarm);
        float l1 = dot(t, kLuma709);
        c = l1 > 1e-4f ? t * (l0 / l1) : t;
    }
    return saturate(c);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    int2 hi = int2(int(gW) - 1, int(gH) - 1);
    float2 size = float2(gW, gH);
    float3 c  = gSrc[p].rgb;
    float3 n  = gSrc[clamp(p + int2(0, -1), int2(0, 0), hi)].rgb;
    float3 s  = gSrc[clamp(p + int2(0, 1), int2(0, 0), hi)].rgb;
    float3 e  = gSrc[clamp(p + int2(1, 0), int2(0, 0), hi)].rgb;
    float3 w  = gSrc[clamp(p + int2(-1, 0), int2(0, 0), hi)].rgb;
    float3 mn = min(c, min(min(n, s), min(e, w)));
    float3 mx = max(c, max(max(n, s), max(e, w)));

    float2 mv = 0.0f;
    if (gMvW > 0 && gMvH > 0)
    {
        int2 q = clamp(int2((float2(p) + 0.5f) * float2(gMvW, gMvH) / size),
                       int2(0, 0), int2(int(gMvW) - 1, int(gMvH) - 1));
        mv = gMv[q] * size / float2(gMvW, gMvH);
        if (!all(isfinite(mv))) mv = 0.0f;
    }
    float speed = length(mv);

    float3 center = c;
    float temporal = 0.30f * gEdge + 0.15f * gSharp;
    if ((gFlags & 1u) != 0u && temporal > 0.0f)
    {
        float2 prev = float2(p) + 0.5f + mv;
        if (all(prev >= 0.0f) && all(prev <= size))
        {
            float3 h = speed < 0.75f ? gHist[p].rgb : gHist.SampleLevel(gLin, prev / size, 0).rgb;
            float3 held = clamp(h, mn, mx);
            float trust = saturate(1.0f - length(h - held) * 12.0f) *
                          (1.0f - saturate((speed - 6.0f) / 26.0f));
            center = lerp(c, held, temporal * trust);
        }
    }

    float3 aa = center;
    float edgeAmount = 0.0f;
    if (gEdge > 0.0f)
    {
        float lnw = L(gSrc[clamp(p + int2(-1, -1), int2(0, 0), hi)].rgb);
        float lne = L(gSrc[clamp(p + int2(1, -1), int2(0, 0), hi)].rgb);
        float lsw = L(gSrc[clamp(p + int2(-1, 1), int2(0, 0), hi)].rgb);
        float lse = L(gSrc[clamp(p + int2(1, 1), int2(0, 0), hi)].rgb);
        float ln = L(n), ls = L(s), le = L(e), lw = L(w), lc = L(c);
        float gx = (lne + 2.0f * le + lse) - (lnw + 2.0f * lw + lsw);
        float gy = (lsw + 2.0f * ls + lse) - (lnw + 2.0f * ln + lne);
        float grad = sqrt(gx * gx + gy * gy);
        float range = max(max(max(ln, ls), max(le, lw)), lc) - min(min(min(ln, ls), min(le, lw)), lc);
        if (grad > 0.06f && range > 0.04f)
        {
            float2 tangent = float2(-gy, gx) / grad;
            float2 texel = 1.0f / size;
            float2 uv = (float2(p) + 0.5f) * texel;
            float3 a = gSrc.SampleLevel(gLin, uv + tangent * texel * 0.75f, 0).rgb;
            float3 b = gSrc.SampleLevel(gLin, uv - tangent * texel * 0.75f, 0).rgb;
            float3 a2 = gSrc.SampleLevel(gLin, uv + tangent * texel * 1.75f, 0).rgb;
            float3 b2 = gSrc.SampleLevel(gLin, uv - tangent * texel * 1.75f, 0).rgb;
            float3 along = (center * 2.0f + a * 2.0f + b * 2.0f + a2 + b2) / 8.0f;
            edgeAmount = gEdge * saturate((grad - 0.06f) * 6.0f);
            aa = lerp(center, along, edgeAmount);
        }
    }

    gNext[p] = float4(aa, 1.0f);

    float3 result = aa;
    if ((gFlags & 2u) != 0u) result = Grade(result);
    gDst[p] = float4(result, 1.0f);
}
)hlsl";

static const char kSettleHlsl[] = R"hlsl(
Texture2D<float4>   gPic   : register(t0);
Texture2D<float>    gPrevL : register(t1);
RWTexture2D<float2> gMv    : register(u0);
RWTexture2D<float>  gCurL  : register(u1);
SamplerState        gLin   : register(s0);
cbuffer SC : register(b0) { uint gMW; uint gMH; uint gHavePrev; float gFloor; };
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);

[numthreads(8, 8, 1)]
void LumaMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gMW || id.y >= gMH) return;
    float2 uv = (float2(id.xy) + 0.5f) / float2(gMW, gMH);
    gCurL[id.xy] = dot(gPic.SampleLevel(gLin, uv, 0).rgb, kLuma);
}

[numthreads(8, 8, 1)]
void SettleMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gMW || id.y >= gMH) return;
    int2 p = int2(id.xy);
    float2 mv = gMv[p];
    if (!all(isfinite(mv)) || length(mv) < gFloor) { gMv[p] = float2(0.0f, 0.0f); return; }
    if (gHavePrev == 0u) return;
    int2 last = int2(int(gMW) - 1, int(gMH) - 1);
    float2 size = float2(gMW, gMH);
    float missMoved = 0.0f, missStill = 0.0f;
    [unroll] for (int y = -4; y <= 4; y += 2)
    [unroll] for (int x = -4; x <= 4; x += 2)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), last);
        float c = gCurL[q];
        missStill += abs(c - gPrevL.Load(int3(q, 0)));
        missMoved += abs(c - gPrevL.SampleLevel(gLin, (float2(q) + 0.5f + mv) / size, 0));
    }
    if (missMoved >= missStill - 25.0f * (0.5f / 255.0f)) gMv[p] = float2(0.0f, 0.0f);
}
)hlsl";

static const char kDenoiseHlsl[] = R"hlsl(
Texture2D<float4>   gCur   : register(t0);
Texture2D<float4>   gPrev1 : register(t1);
Texture2D<float4>   gPrev2 : register(t2);
RWTexture2D<float4> gDst   : register(u0);
cbuffer PC : register(b0) { uint gW; uint gH; float gAmount; uint gFlags; };
groupshared float3 gC[14][14];
groupshared float3 gP1[14][14];
groupshared float3 gP2[14][14];
groupshared float3 gT[12][12];

float Gap(float3 a, float3 b)
{
    float3 d = abs(a - b);
    return (d.r + d.g + d.b) * (1.0f / 3.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 id : SV_DispatchThreadID)
{
    int2 last = int2(int(gW) - 1, int(gH) - 1);
    int2 origin = int2(gid.xy) * 8 - 3;
    uint self = tid.y * 8 + tid.x;
    bool have1 = (gFlags & 1u) != 0u, have2 = (gFlags & 2u) != 0u;
    for (uint i = self; i < 196; i += 64)
    {
        int2 q = clamp(origin + int2(i % 14, i / 14), int2(0, 0), last);
        float3 c = gCur[q].rgb;
        gC[i / 14][i % 14] = c;
        gP1[i / 14][i % 14] = have1 ? gPrev1[q].rgb : c;
        gP2[i / 14][i % 14] = have2 ? gPrev2[q].rgb : c;
    }
    GroupMemoryBarrierWithGroupSync();

    float sigma = 0.004f + 0.06f * gAmount;
    float inv = 1.0f / (1.2f * sigma);

    for (uint j = self; j < 144; j += 64)
    {
        int ty = int(j / 12) + 1, tx = int(j % 12) + 1;
        float d1 = 0.0f, d2 = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float3 c = gC[ty + y][tx + x];
            d1 += Gap(gP1[ty + y][tx + x], c);
            d2 += Gap(gP2[ty + y][tx + x], c);
        }
        d1 *= (1.0f / 9.0f);
        d2 *= (1.0f / 9.0f);
        float w1 = have1 ? exp(-(d1 * inv) * (d1 * inv)) : 0.0f;
        float w2 = have2 ? exp(-(d2 * inv) * (d2 * inv)) * w1 : 0.0f;
        float3 c0 = gC[ty][tx];
        gT[ty - 1][tx - 1] = (c0 + w1 * gP1[ty][tx] + w2 * gP2[ty][tx]) / (1.0f + w1 + w2);
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= gW || id.y >= gH) return;

    int2 t = int2(tid.xy) + 2;
    float3 avg = gT[t.y][t.x];
    float3 cur = gC[t.y + 1][t.x + 1];
    float still = saturate(1.0f - Gap(avg, cur) / max(sigma, 1e-4f));

    float range = 2.5f * sigma;
    float invRange = 1.0f / (2.0f * range * range);
    const float invSpace = 1.0f / (2.0f * 1.3f * 1.3f);
    float3 acc = 0.0f;
    float wsum = 0.0f;
    [unroll] for (int v = -2; v <= 2; ++v)
    [unroll] for (int u = -2; u <= 2; ++u)
    {
        float3 s = gT[t.y + v][t.x + u];
        float3 d = s - avg;
        float w = exp(-float(u * u + v * v) * invSpace - dot(d, d) * invRange);
        acc += s * w;
        wsum += w;
    }
    float3 spatial = acc / max(wsum, 1e-6f);
    float mix = saturate(gAmount * 2.0f) * lerp(1.0f, 0.5f, still);
    gDst[id.xy] = float4(lerp(avg, spatial, mix), 1.0f);
}
)hlsl";

struct State
{
    // Runtime loaded from user's selected nvngx_dlssnr.dll
    HMODULE module = nullptr;
    NrInitFn init_ext = nullptr;
    NrCreateFn create = nullptr;
    NrEvaluateFn evaluate = nullptr;
    NrReleaseFn release = nullptr;
    unsigned dll_ver = 0;
    std::wstring dll_path;
    bool runtime_ok = false;
    bool runtime_failed = false;

    // NGX feature + multi-pass layers
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    NVSDK_NGX_Handle *extras[kMaxPasses - 1] = {};
    UINT created_nw = 0, created_nh = 0, created_preset = 0, created_passes = 0;
    bool need_warmup = false;

    // Textures
    UINT pic_w = 0, pic_h = 0, nw = 0, nh = 0;
    ID3D12Resource *source_copy = nullptr; // pic_w x pic_h, NPSR at rest (untouched input for still re-eval)
    bool has_source_copy = false;
    ID3D12Resource *result = nullptr;      // pic_w x pic_h, UAV at rest
    ID3D12Resource *net_in = nullptr;      // nw x nh, NPSR at rest (when small)
    ID3D12Resource *net_out = nullptr;     // nw x nh, UAV at rest (when small)
    ID3D12Resource *scratch = nullptr;     // nw x nh, UAV at rest (when passes > 1)
    ID3D12Resource *raw = nullptr;         // pic_w x pic_h, UAV at rest (full-res detail/halo raw)

    // Dedicated nw x nh motion field + still settle (independent of pipe.motion)
    ID3D12DescriptorHeap *mv_heap = nullptr; // 9 descriptors
    ID3D12Resource *mv_prev = nullptr;       // nw x nh, R8G8B8A8_UNORM, NPSR at rest
    ID3D12Resource *mv_pyramid = nullptr;    // UNORDERED_ACCESS at rest
    ID3D12Resource *mv_fields = nullptr;     // UNORDERED_ACCESS at rest
    ID3D12Resource *mv_tex = nullptr;        // nw x nh, R16G16_FLOAT, NPSR at rest
    ID3D12Resource *mv_reverse = nullptr;    // nw x nh, R16G16_FLOAT, NPSR at rest
    ID3D12Resource *mv_zero = nullptr;       // nw x nh, R16G16_FLOAT, COPY_SOURCE at rest
    ID3D12Resource *mv_bound_cur = nullptr;
    UINT mv_w = 0, mv_h = 0, mv_ew = 0, mv_eh = 0, mv_levels = 0, mv_ratio = 1;
    bool mv_have_prev = false;
    LARGE_INTEGER mv_last_qpc = {};

    // MotionSettle pipelines + ping-pong luma
    ID3D12RootSignature *settle_rs = nullptr;
    ID3D12PipelineState *settle_luma_pso = nullptr;
    ID3D12PipelineState *settle_pso = nullptr;
    ID3D12DescriptorHeap *settle_heap = nullptr; // 2 sets x 4
    ID3D12Resource *settle_luma[2] = {};         // [cur] UAV, [1-cur] NPSR
    ID3D12Resource *settle_bound[2][2] = {};
    UINT settle_w = 0, settle_h = 0, settle_cur = 0;
    bool settle_have_prev = false;

    // Denoise targets + pipeline (3-frame temporal + 5x5 spatial)
    ID3D12RootSignature *dn_rs = nullptr;
    ID3D12PipelineState *dn_pso = nullptr;
    ID3D12DescriptorHeap *dn_heap = nullptr; // 3 sets x 4
    ID3D12Resource *dn_ring[3] = {};         // pic_w x pic_h, NPSR at rest
    ID3D12Resource *dn_bound[3][4] = {};
    UINT dn_w = 0, dn_h = 0, dn_head = 0, dn_frames = 0;

    // Halo targets
    ID3D12Resource *halo_th = nullptr;
    ID3D12Resource *halo_tb = nullptr;
    ID3D12Resource *halo_dc = nullptr;
    UINT halo_w = 0, halo_h = 0;

    // Finish targets
    ID3D12Resource *finish_tmp = nullptr;
    ID3D12Resource *finish_hist[2] = {};
    UINT finish_w = 0, finish_h = 0, finish_ping = 0;
    bool finish_hist_valid = false;

    // Compute pipelines + heaps (GpuCore::kRing sets to avoid overwrite while frames are in flight)
    ID3D12RootSignature *resize_rs = nullptr;
    ID3D12PipelineState *resize_pso = nullptr;
    ID3D12DescriptorHeap *resize_heap = nullptr; // kRing * 2

    ID3D12RootSignature *detail_rs = nullptr;
    ID3D12PipelineState *detail_pso = nullptr;
    ID3D12DescriptorHeap *detail_heap = nullptr; // kRing * 5

    ID3D12RootSignature *halo_rs = nullptr;
    ID3D12PipelineState *halo_blur_pso = nullptr;
    ID3D12PipelineState *halo_fix_pso = nullptr;
    ID3D12DescriptorHeap *halo_heap = nullptr;   // kRing * 3 * 5

    ID3D12RootSignature *finish_rs = nullptr;
    ID3D12PipelineState *finish_pso = nullptr;
    ID3D12DescriptorHeap *finish_heap = nullptr; // kRing * 5

    bool shaders_ready = false, shaders_failed = false;
    ULONGLONG last_warn_tick = 0;
};
static State g;

static ID3D12Resource *MakeTex(UINT w, UINT hgt, DXGI_FORMAT fmt, bool uav, D3D12_RESOURCE_STATES initial)
{
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = hgt; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (uav) td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource *r = nullptr;
    if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, initial, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r))))
        return nullptr;
    return r;
}

static void CloseMotionTargets()
{
    for (ID3D12Resource **r : { &g.mv_prev, &g.mv_pyramid, &g.mv_fields, &g.mv_tex, &g.mv_reverse, &g.mv_zero,
                                &g.settle_luma[0], &g.settle_luma[1] })
        if (*r) { (*r)->Release(); *r = nullptr; }
    g.mv_bound_cur = nullptr;
    g.mv_w = g.mv_h = g.mv_ew = g.mv_eh = g.mv_levels = 0;
    g.mv_ratio = 1;
    g.mv_have_prev = false;
    g.mv_last_qpc = {};
    g.settle_w = g.settle_h = g.settle_cur = 0;
    g.settle_have_prev = false;
    memset(g.settle_bound, 0, sizeof(g.settle_bound));
}

static void CloseDenoiseTargets()
{
    for (auto &r : g.dn_ring)
        if (r) { r->Release(); r = nullptr; }
    g.dn_w = g.dn_h = 0;
    g.dn_head = 0;
    g.dn_frames = 0;
    memset(g.dn_bound, 0, sizeof(g.dn_bound));
}

static void ReleaseFeatures()
{
    auto rel = [](NVSDK_NGX_Handle *&h) {
        if (!h) return;
        __try {
            if (g.release) g.release(h);
            else NVSDK_NGX_D3D12_ReleaseFeature(h);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        h = nullptr;
    };
    for (auto &e : g.extras) rel(e);
    rel(g.feature);
    g.created_nw = g.created_nh = g.created_preset = g.created_passes = 0;
    g.need_warmup = false;
}

static void CloseTextures()
{
    for (ID3D12Resource **r : { &g.source_copy, &g.result, &g.net_in, &g.net_out, &g.scratch, &g.raw,
                                &g.halo_th, &g.halo_tb, &g.halo_dc,
                                &g.finish_tmp, &g.finish_hist[0], &g.finish_hist[1] })
        if (*r) { (*r)->Release(); *r = nullptr; }
    g.has_source_copy = false;
    g.pic_w = g.pic_h = g.nw = g.nh = 0;
    g.halo_w = g.halo_h = 0;
    g.finish_w = g.finish_h = 0;
    g.finish_ping = 0;
    g.finish_hist_valid = false;
    CloseDenoiseTargets();
    CloseMotionTargets();
}

static bool EnsureShaders()
{
    if (g.shaders_ready) return true;
    if (g.shaders_failed) return false;
    g.shaders_failed = true;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) { Trace("[dlss5] shader compiler unavailable"); return false; }

    auto build_shader = [&](const char *src, size_t len, const char *name, const char *entry = "CSMain") -> ID3DBlob * {
        ID3DBlob *blob = nullptr, *err = nullptr;
        const HRESULT hr = compile(src, len, name, nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
        if (FAILED(hr) || !blob)
        {
            Trace("[dlss5] %s (%s) failed 0x%08X: %s", name, entry, (unsigned)hr, err ? static_cast<const char *>(err->GetBufferPointer()) : "(no log)");
            if (err) err->Release();
            return nullptr;
        }
        if (err) err->Release();
        return blob;
    };

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    auto make_rs_pso = [&](UINT num_consts, UINT num_srv, UINT num_uav, bool with_sampler,
                           ID3DBlob *cs, ID3D12RootSignature **out_rs, ID3D12PipelineState **out_pso) -> bool {
        if (!cs) return false;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = num_srv;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = num_uav;
        ranges[1].OffsetInDescriptorsFromTableStart = num_srv;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = num_consts;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2; rsd.pParameters = params;
        rsd.NumStaticSamplers = with_sampler ? 1u : 0u;
        rsd.pStaticSamplers = with_sampler ? &samp : nullptr;
        ID3DBlob *blob = nullptr, *err = nullptr;
        if (FAILED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) || !blob)
        {
            if (err) err->Release();
            return false;
        }
        if (err) err->Release();
        HRESULT hr = lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                        __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(out_rs));
        blob->Release();
        if (FAILED(hr)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = *out_rs;
        pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
        hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(out_pso));
        return SUCCEEDED(hr);
    };

    auto make_heap = [&](UINT count, ID3D12DescriptorHeap **out) -> bool {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = count;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        return SUCCEEDED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(out)));
    };

    ID3DBlob *c_resize  = build_shader(kResizeFineHlsl, sizeof(kResizeFineHlsl) - 1, "dlss5_resize.hlsl");
    ID3DBlob *c_detail  = build_shader(kDetailHlsl, sizeof(kDetailHlsl) - 1, "dlss5_detail.hlsl");
    ID3DBlob *c_hblur   = build_shader(kHaloBlurHlsl, sizeof(kHaloBlurHlsl) - 1, "dlss5_haloblur.hlsl");
    ID3DBlob *c_hfix    = build_shader(kHaloFixHlsl, sizeof(kHaloFixHlsl) - 1, "dlss5_halofix.hlsl");
    ID3DBlob *c_finish  = build_shader(kFinishHlsl, sizeof(kFinishHlsl) - 1, "dlss5_finish.hlsl");
    ID3DBlob *c_denoise = build_shader(kDenoiseHlsl, sizeof(kDenoiseHlsl) - 1, "dlss5_denoise.hlsl");
    ID3DBlob *c_sluma   = build_shader(kSettleHlsl, sizeof(kSettleHlsl) - 1, "dlss5_settle.hlsl", "LumaMain");
    ID3DBlob *c_settle  = build_shader(kSettleHlsl, sizeof(kSettleHlsl) - 1, "dlss5_settle.hlsl", "SettleMain");

    bool ok = c_resize && c_detail && c_hblur && c_hfix && c_finish && c_denoise && c_sluma && c_settle;
    if (ok) ok = make_rs_pso(4, 1, 1, true, c_resize, &g.resize_rs, &g.resize_pso) && make_heap(GpuCore::kRing * 2, &g.resize_heap);
    if (ok) ok = make_rs_pso(6, 4, 1, true, c_detail, &g.detail_rs, &g.detail_pso) && make_heap(GpuCore::kRing * 5, &g.detail_heap);
    if (ok)
    {
        ok = make_rs_pso(10, 4, 1, false, c_hblur, &g.halo_rs, &g.halo_blur_pso) && make_heap(GpuCore::kRing * 3 * 5, &g.halo_heap);
        if (ok)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = g.halo_rs;
            pd.CS = { c_hfix->GetBufferPointer(), c_hfix->GetBufferSize() };
            ok = SUCCEEDED(lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g.halo_fix_pso)));
        }
    }
    if (ok) ok = make_rs_pso(12, 3, 2, true, c_finish, &g.finish_rs, &g.finish_pso) && make_heap(GpuCore::kRing * 5, &g.finish_heap);
    if (ok) ok = make_rs_pso(4, 3, 1, false, c_denoise, &g.dn_rs, &g.dn_pso) && make_heap(12, &g.dn_heap);
    if (ok)
    {
        ok = make_rs_pso(4, 2, 2, true, c_sluma, &g.settle_rs, &g.settle_luma_pso) && make_heap(8, &g.settle_heap);
        if (ok)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = g.settle_rs;
            pd.CS = { c_settle->GetBufferPointer(), c_settle->GetBufferSize() };
            ok = SUCCEEDED(lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g.settle_pso)));
        }
    }

    for (ID3DBlob *b : { c_resize, c_detail, c_hblur, c_hfix, c_finish, c_denoise, c_sluma, c_settle }) if (b) b->Release();
    if (!ok) { Trace("[dlss5] pipeline creation failed"); return false; }

    g.shaders_failed = false;
    g.shaders_ready = true;
    return true;
}

static bool InitExtGuarded(NrInitFn fn, const wchar_t *data_dir, const NVSDK_NGX_Parameter *params, NVSDK_NGX_Result *out)
{
    __try { *out = fn(0x1000000ULL, data_dir, lfCore.device, NVSDK_NGX_Version_API, params); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[dlss5] Init_Ext threw 0x%08X", GetExceptionCode()); return false; }
}

static bool CreateFeatureGuarded(NrCreateFn fn, ID3D12GraphicsCommandList *list, const NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out, NVSDK_NGX_Result *res)
{
    __try { *res = fn(list, NVSDK_NGX_Feature_Reserved18, params, out); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[dlss5] CreateFeature threw 0x%08X", GetExceptionCode()); return false; }
}

static bool EvaluateGuarded(NrEvaluateFn fn, ID3D12GraphicsCommandList *list, const NVSDK_NGX_Handle *h, const NVSDK_NGX_Parameter *params, NVSDK_NGX_Result *res)
{
    __try { *res = fn(list, h, params, nullptr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[dlss5] EvaluateFeature threw 0x%08X", GetExceptionCode()); return false; }
}

static bool EnsureRuntime()
{
    const unsigned ver = g_dlss5_dll_ver.load();
    if (g.dll_ver != ver)
    {
        CmdDrain();
        ReleaseFeatures();
        if (g.params) { NVSDK_NGX_D3D12_DestroyParameters(g.params); g.params = nullptr; }
        if (g.module) { FreeLibrary(g.module); g.module = nullptr; }
        g.init_ext = nullptr; g.create = nullptr; g.evaluate = nullptr; g.release = nullptr;
        g.runtime_ok = false;
        g.runtime_failed = false;
        g.dll_ver = ver;
        g.dll_path = Dlss5GetDll();
    }
    if (g.runtime_ok) return true;
    if (g.runtime_failed) return false;

    const std::wstring path = Dlss5GetDll();
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        const ULONGLONG now = GetTickCount64();
        if (now - g.last_warn_tick > 5000)
        {
            g.last_warn_tick = now;
            Trace("[dlss5] nvngx_dlssnr.dll is not selected or not found (%ls)", path.empty() ? L"none" : path.c_str());
        }
        return false;
    }

    DXGI_ADAPTER_DESC1 desc = {};
    if (!lfCard.adapter || FAILED(lfCard.adapter->GetDesc1(&desc)) || desc.VendorId != 0x10DE)
    {
        g.runtime_failed = true;
        Trace("[dlss5] unavailable: DLSS 5 requires an NVIDIA RTX GPU");
        Status("@note DLSS 5 requires an NVIDIA RTX GPU");
        return false;
    }

    LoopBeat();
    if (!lfCore.ngx_ready && !NgxStart())
    {
        g.runtime_failed = true;
        Trace("[dlss5] NGX failed to start for DLSS 5");
        return false;
    }
    LoopBeat();

    g.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.module) g.module = LoadLibraryW(path.c_str());
    if (!g.module)
    {
        g.runtime_failed = true;
        Trace("[dlss5] could not load %ls (%lu)", path.c_str(), GetLastError());
        Status("@note Could not load nvngx_dlssnr.dll (%lu)", GetLastError());
        return false;
    }

    g.init_ext = reinterpret_cast<NrInitFn>(GetProcAddress(g.module, "NVSDK_NGX_D3D12_Init_Ext"));
    g.create   = reinterpret_cast<NrCreateFn>(GetProcAddress(g.module, "NVSDK_NGX_D3D12_CreateFeature"));
    g.evaluate = reinterpret_cast<NrEvaluateFn>(GetProcAddress(g.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    g.release  = reinterpret_cast<NrReleaseFn>(GetProcAddress(g.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!g.init_ext || !g.create || !g.evaluate || !g.release)
    {
        g.runtime_failed = true;
        Trace("[dlss5] %ls lacks DLSSNR exports", path.c_str());
        FreeLibrary(g.module); g.module = nullptr;
        return false;
    }

    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&g.params)) || !g.params)
    {
        g.runtime_failed = true;
        Trace("[dlss5] could not allocate NGX parameter block");
        return false;
    }

    wchar_t data_dir[MAX_PATH] = {};
    EngineDataPath(data_dir);
    NVSDK_NGX_Result ir = NVSDK_NGX_Result_Fail;
    if (!InitExtGuarded(g.init_ext, data_dir, g.params, &ir) || NVSDK_NGX_FAILED(ir))
    {
        g.runtime_failed = true;
        Trace("[dlss5] Init_Ext failed 0x%08X (%s)", (unsigned)ir, NgxCodeText(ir));
        Status("@note DLSS 5 Init failed (%s)", NgxCodeText(ir));
        return false;
    }
    LoopBeat();

    g.runtime_ok = true;
    Trace("[dlss5] runtime ready (%ls)", path.c_str());
    return true;
}

static bool CreateOneFeature(UINT nw, UINT nh, UINT preset, NVSDK_NGX_Handle **out_handle)
{
    g.params->Reset();
    g.params->Set("CreationNodeMask", 1u);
    g.params->Set("VisibilityNodeMask", 1u);
    g.params->Set("DLSSNR.Width", nw);
    g.params->Set("DLSSNR.Height", nh);
    g.params->Set("DLSSNR.InputWidth", nw);
    g.params->Set("DLSSNR.InputHeight", nh);
    g.params->Set("DLSSNR.OutputWidth", nw);
    g.params->Set("DLSSNR.OutputHeight", nh);
    g.params->Set("DLSSNR.Output.Width", nw);
    g.params->Set("DLSSNR.Output.Height", nh);
    g.params->Set("DLSSNR.Upscaling", 0u);
    g.params->Set("DLSSNR.Scale", 1.0f);
    g.params->Set("DLSSNR.ScalingRatio", 1.0f);
    // ScreenNative (preferences.py): 1 = Quality (Model A), 2 = Fast (Model B). (0 falls back to runtime default B.)
    const UINT ngx_preset = (preset == 0u) ? 1u : 2u;
    g.params->Set("DLSSNR.Hint.Render.Preset", ngx_preset);
    g.params->Set("DLSS.Feature.Create.Flags", 0u);

    if (!CmdOpen()) return false;
    NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
    const bool no_exc = CreateFeatureGuarded(g.create, lfCore.cmd, g.params, out_handle, &rf);
    if (!no_exc) { DropCommands(); *out_handle = nullptr; return false; }
    const UINT64 fence = CmdSubmit();
    if (!AwaitMark(lfCore.timeline, fence, 30000) || NVSDK_NGX_FAILED(rf) || !*out_handle)
    {
        Trace("[dlss5] CreateFeature %ux%u preset %u refused 0x%08X (%s)", nw, nh, preset, (unsigned)rf, NgxCodeText(rf));
        *out_handle = nullptr;
        return false;
    }
    return true;
}

static bool EnsureMotionTargets(UINT nw, UINT nh)
{
    if (g.mv_tex && g.mv_zero && g.mv_w == nw && g.mv_h == nh) return true;
    CloseMotionTargets();

    if (!g.mv_heap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g.mv_heap))))
            return false;
    }

    UINT ratio = 1;
    while ((nw + ratio - 1) / ratio > 800 || (nh + ratio - 1) / ratio > 450) ratio *= 2;
    const UINT ew = (nw + ratio - 1) / ratio, eh = (nh + ratio - 1) / ratio;
    UINT levels = 0;
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    while (side(ew, levels + 1) >= 16 && side(eh, levels + 1) >= 16) ++levels;
    const UINT aw = ew + (ew + 1) / 2, band = eh + 16;

    g.mv_prev       = MakeTex(nw, nh, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.mv_pyramid    = MakeTex(aw, 2 * band, DXGI_FORMAT_R32_UINT, true,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.mv_fields     = MakeTex(aw, 4 * band, DXGI_FORMAT_R32_UINT, true,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.mv_tex        = MakeTex(nw, nh, DXGI_FORMAT_R16G16_FLOAT,   true,  D3D12_RESOURCE_STATE_COPY_DEST);
    g.mv_reverse    = MakeTex(nw, nh, DXGI_FORMAT_R16G16_FLOAT,   true,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.mv_zero       = MakeTex(nw, nh, DXGI_FORMAT_R16G16_FLOAT,   false, D3D12_RESOURCE_STATE_COPY_DEST);
    g.settle_luma[0]= MakeTex(nw, nh, DXGI_FORMAT_R32_FLOAT,      true,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.settle_luma[1]= MakeTex(nw, nh, DXGI_FORMAT_R32_FLOAT,      true,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!g.mv_prev || !g.mv_pyramid || !g.mv_fields || !g.mv_tex || !g.mv_reverse || !g.mv_zero ||
        !g.settle_luma[0] || !g.settle_luma[1])
    {
        CloseMotionTargets();
        return false;
    }

    // Zero-initialize mv_zero and mv_tex cleanly so MVec never contains uninitialized values.
    D3D12_RESOURCE_DESC md = g.mv_zero->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 row_bytes = 0, total_bytes = 0;
    lfCore.device->GetCopyableFootprints(&md, 0, 1, 0, &fp, &rows, &row_bytes, &total_bytes);
    D3D12_HEAP_PROPERTIES up = {}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total_bytes; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *upload = nullptr;
    if (FAILED(lfCore.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void **>(&upload))))
    {
        CloseMotionTargets();
        return false;
    }
    BYTE *ptr = nullptr;
    if (SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void **>(&ptr))))
    {
        memset(ptr, 0, static_cast<size_t>(total_bytes));
        D3D12_RANGE none = { 0, 0 };
        upload->Unmap(0, &none);
    }
    bool ok = false;
    if (CmdOpen())
    {
        D3D12_TEXTURE_COPY_LOCATION src = {}, d0 = {}, d1 = {};
        src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
        d0.pResource = g.mv_zero; d0.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        d1.pResource = g.mv_tex;  d1.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        lfCore.cmd->CopyTextureRegion(&d0, 0, 0, 0, &src, nullptr);
        lfCore.cmd->CopyTextureRegion(&d1, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER post[] = {
            Barrier(g.mv_zero, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(g.mv_tex,  D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        lfCore.cmd->ResourceBarrier(2, post);
        const UINT64 fence = CmdSubmit();
        ok = AwaitMark(lfCore.timeline, fence, 10000);
    }
    upload->Release();
    if (!ok) { CloseMotionTargets(); return false; }

    g.mv_w = nw; g.mv_h = nh;
    g.mv_ew = ew; g.mv_eh = eh; g.mv_levels = levels; g.mv_ratio = ratio;
    g.settle_w = nw; g.settle_h = nh;
    g.settle_cur = 0;
    g.settle_have_prev = false;
    return true;
}

static bool EnsurePipelineAndFeatures(UINT pic_w, UINT pic_h, UINT nw, UINT nh, UINT preset, UINT passes)
{
    if (!EnsureShaders()) return false;
    const bool size_changed = (g.pic_w != pic_w || g.pic_h != pic_h || g.nw != nw || g.nh != nh);
    const bool need_features = (!g.feature || g.created_nw != nw || g.created_nh != nh ||
                                g.created_preset != preset || g.created_passes != passes);
    if (size_changed || need_features) CmdDrain();

    if (size_changed)
    {
        for (ID3D12Resource **r : { &g.source_copy, &g.result, &g.net_in, &g.net_out, &g.scratch, &g.raw,
                                    &g.halo_th, &g.halo_tb, &g.halo_dc,
                                    &g.finish_tmp, &g.finish_hist[0], &g.finish_hist[1] })
            if (*r) { (*r)->Release(); *r = nullptr; }
        g.has_source_copy = false;
        g.halo_w = g.halo_h = g.finish_w = g.finish_h = 0;
        g.finish_hist_valid = false;
        CloseDenoiseTargets();
        CloseMotionTargets();

        g.source_copy = MakeTex(pic_w, pic_h, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.result      = MakeTex(pic_w, pic_h, DXGI_FORMAT_R8G8B8A8_UNORM, true,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.raw         = MakeTex(pic_w, pic_h, DXGI_FORMAT_R8G8B8A8_UNORM, true,  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!g.source_copy || !g.result || !g.raw) return false;

        if (nw != pic_w || nh != pic_h)
        {
            g.net_in  = MakeTex(nw, nh, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g.net_out = MakeTex(nw, nh, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (!g.net_in || !g.net_out) return false;
        }
        g.pic_w = pic_w; g.pic_h = pic_h;
        g.nw = nw; g.nh = nh;
        Trace("[dlss5] targets %ux%u (network %ux%u)", pic_w, pic_h, nw, nh);
    }

    if (!EnsureMotionTargets(nw, nh)) return false;

    if (passes > 1 && !g.scratch)
        g.scratch = MakeTex(nw, nh, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (need_features)
    {
        ReleaseFeatures();
        LoopBeat();
        if (!CreateOneFeature(nw, nh, preset, &g.feature)) return false;
        LoopBeat();
        UINT built_passes = 1;
        for (UINT i = 0; i + 1 < passes; ++i)
        {
            if (CreateOneFeature(nw, nh, preset, &g.extras[i])) ++built_passes;
            else break;
            LoopBeat();
        }
        g.created_nw = nw; g.created_nh = nh;
        g.created_preset = preset; g.created_passes = passes;
        g.need_warmup = true;
        Trace("[dlss5] feature up: %ux%u, preset %u, %u pass(es)", nw, nh, preset, built_passes);
    }
    return g.feature != nullptr;
}

static bool EnsureDenoiseTargets(UINT w, UINT hgt)
{
    if (g.dn_ring[0] && g.dn_ring[1] && g.dn_ring[2] && g.dn_w == w && g.dn_h == hgt) return true;
    CloseDenoiseTargets();
    for (auto &r : g.dn_ring)
    {
        r = MakeTex(w, hgt, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!r) { CloseDenoiseTargets(); return false; }
    }
    g.dn_w = w; g.dn_h = hgt;
    g.dn_head = 0; g.dn_frames = 0;
    return true;
}

static bool EnsureHaloTargets(UINT hw, UINT hh)
{
    if (g.halo_dc && g.halo_w == hw && g.halo_h == hh) return true;
    for (ID3D12Resource **r : { &g.halo_th, &g.halo_tb, &g.halo_dc })
        if (*r) { (*r)->Release(); *r = nullptr; }
    g.halo_th = MakeTex(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.halo_tb = MakeTex(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.halo_dc = MakeTex(hw, hh, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!g.halo_th || !g.halo_tb || !g.halo_dc) return false;
    g.halo_w = hw; g.halo_h = hh;
    return true;
}

static bool EnsureFinishTargets(UINT w, UINT hgt)
{
    if (g.finish_tmp && g.finish_hist[0] && g.finish_hist[1] && g.finish_w == w && g.finish_h == hgt) return true;
    for (ID3D12Resource **r : { &g.finish_tmp, &g.finish_hist[0], &g.finish_hist[1] })
        if (*r) { (*r)->Release(); *r = nullptr; }
    g.finish_tmp     = MakeTex(w, hgt, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.finish_hist[0] = MakeTex(w, hgt, DXGI_FORMAT_R8G8B8A8_UNORM, true,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.finish_hist[1] = MakeTex(w, hgt, DXGI_FORMAT_R8G8B8A8_UNORM, true,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!g.finish_tmp || !g.finish_hist[0] || !g.finish_hist[1]) return false;
    g.finish_w = w; g.finish_h = hgt;
    g.finish_ping = 0; g.finish_hist_valid = false;
    return true;
}

static void BindMotionCur(ID3D12Resource *cur)
{
    if (g.mv_bound_cur == cur) return;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.mv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lfCore.device->CreateShaderResourceView(g.mv_prev, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(cur, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    lfCore.device->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateShaderResourceView(g.mv_pyramid, &sd, cpu); cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateUnorderedAccessView(g.mv_pyramid, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(g.mv_fields, nullptr, &ud, cpu); cpu.ptr += stride;
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    lfCore.device->CreateUnorderedAccessView(g.mv_tex, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(g.mv_reverse, nullptr, &ud, cpu);
    g.mv_bound_cur = cur;
}

static void RecordMotionEstimate(ID3D12GraphicsCommandList *list, double interval)
{
    ID3D12DescriptorHeap *heaps[] = { g.mv_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_flow.est_rs);
    list->SetComputeRootDescriptorTable(1, g.mv_heap->GetGPUDescriptorHandleForHeapStart());
    const UINT ew = g.mv_ew, eh = g.mv_eh, levels = g.mv_levels;
    UINT32 k[14] = { g.mv_w, g.mv_h, ew, eh, 0, levels, 0u, EstimateRadius(interval),
                     0, 0, 0, g.mv_w, g.mv_h, g.mv_ratio };
    auto uav = [](ID3D12Resource *r) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        return b;
    };
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    auto pass = [&](int pso, UINT level, UINT from, UINT to, UINT step, UINT x, UINT y, ID3D12Resource *written) {
        k[4] = level; k[8] = step; k[9] = from; k[10] = to;
        list->SetComputeRoot32BitConstants(0, 14, k, 0);
        list->SetPipelineState(g_flow.est_pso[pso]);
        list->Dispatch((x + 7) / 8, (y + 7) / 8, 2);
        D3D12_RESOURCE_BARRIER b = uav(written);
        list->ResourceBarrier(1, &b);
    };
    D3D12_RESOURCE_BARRIER open[] = {
        Barrier(g.mv_tex,     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g.mv_reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(_countof(open), open);
    pass(0, 0, 0, 0, 0, ew, eh, g.mv_pyramid);
    for (UINT l = 1; l <= levels; ++l) pass(1, l, 0, 0, 0, side(ew, l), side(eh, l), g.mv_pyramid);
    D3D12_RESOURCE_BARRIER to_read = Barrier(g.mv_pyramid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);
    UINT set = 0;
    auto spreads = [&](UINT l) {
        for (UINT step : { 4u, 2u, 1u })
        {
            if (l == 0 && step != 1) continue;
            pass(4, l, set, 1 - set, step, side(ew, l), side(eh, l), g.mv_fields);
            set = 1 - set;
        }
    };
    pass(2, levels, 0, 0, 0, side(ew, levels), side(eh, levels), g.mv_fields);
    spreads(levels);
    for (int l = int(levels) - 1; l >= 0; --l)
    {
        pass(3, UINT(l), set, 1 - set, 0, side(ew, UINT(l)), side(eh, UINT(l)), g.mv_fields);
        set = 1 - set;
        spreads(UINT(l));
    }
    pass(5, 0, set, 1 - set, 0, ew, eh, g.mv_fields);
    set = 1 - set;
    pass(6, 0, set, set, 0, ew, eh, g.mv_tex);
    D3D12_RESOURCE_BARRIER close[] = {
        Barrier(g.mv_pyramid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g.mv_tex,     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g.mv_reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(_countof(close), close);
}

static void RecordMotionSettle(ID3D12Resource *nr_color, bool reset)
{
    if (!g.settle_pso || !g.settle_luma[0]) return;
    if (reset) g.settle_have_prev = false;
    const UINT set = g.settle_cur;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (g.settle_bound[set][0] != nr_color || g.settle_bound[set][1] != g.mv_tex)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.settle_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(set) * 4 * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lfCore.device->CreateShaderResourceView(nr_color, &sd, cpu); cpu.ptr += stride;
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        lfCore.device->CreateShaderResourceView(g.settle_luma[1 - set], &sd, cpu); cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ud.Format = DXGI_FORMAT_R16G16_FLOAT;
        lfCore.device->CreateUnorderedAccessView(g.mv_tex, nullptr, &ud, cpu); cpu.ptr += stride;
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        lfCore.device->CreateUnorderedAccessView(g.settle_luma[set], nullptr, &ud, cpu);
        g.settle_bound[set][0] = nr_color;
        g.settle_bound[set][1] = g.mv_tex;
    }

    D3D12_RESOURCE_BARRIER in = Barrier(g.mv_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    lfCore.cmd->ResourceBarrier(1, &in);
    ID3D12DescriptorHeap *heaps[] = { g.settle_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g.settle_rs);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.settle_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(set) * 4 * stride;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    const float floor_px = 0.5f;
    UINT32 c[4] = { g.nw, g.nh, g.settle_have_prev ? 1u : 0u, 0 };
    memcpy(&c[3], &floor_px, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
    lfCore.cmd->SetPipelineState(g.settle_luma_pso);
    lfCore.cmd->Dispatch((g.nw + 7) / 8, (g.nh + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav_b = {};
    uav_b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_b.UAV.pResource = g.settle_luma[set];
    lfCore.cmd->ResourceBarrier(1, &uav_b);

    lfCore.cmd->SetPipelineState(g.settle_pso);
    lfCore.cmd->Dispatch((g.nw + 7) / 8, (g.nh + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER out[] = {
        Barrier(g.mv_tex,                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g.settle_luma[set],       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g.settle_luma[1 - set],   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    lfCore.cmd->ResourceBarrier(_countof(out), out);
    g.settle_cur = 1 - set;
    g.settle_have_prev = true;
}

static ID3D12Resource *PrepareDlss5Motion(ID3D12Resource *nr_color, bool reset, bool from_source_copy)
{
    if (!g.mv_tex || !g.mv_zero) return nullptr;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double interval = g.mv_last_qpc.QuadPart ? double(now.QuadPart - g.mv_last_qpc.QuadPart) / double(freq.QuadPart) : (1.0 / 60.0);
    g.mv_last_qpc = now;

    if (from_source_copy)
    {
        D3D12_RESOURCE_BARRIER to_dst = Barrier(g.mv_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        lfCore.cmd->ResourceBarrier(1, &to_dst);
        lfCore.cmd->CopyResource(g.mv_tex, g.mv_zero);
        D3D12_RESOURCE_BARRIER to_srv = Barrier(g.mv_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
        return g.mv_tex;
    }

    const bool can_est = EnsureFlowEstimate();
    const bool do_est = can_est && g.mv_have_prev && !reset;
    if (do_est)
    {
        BindMotionCur(nr_color);
        RecordMotionEstimate(lfCore.cmd, interval);
        RecordMotionSettle(nr_color, false);
    }
    else
    {
        D3D12_RESOURCE_BARRIER to_dst = Barrier(g.mv_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        lfCore.cmd->ResourceBarrier(1, &to_dst);
        lfCore.cmd->CopyResource(g.mv_tex, g.mv_zero);
        D3D12_RESOURCE_BARRIER to_srv = Barrier(g.mv_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
        RecordMotionSettle(nr_color, true);
    }

    D3D12_RESOURCE_BARRIER keep_in[] = {
        Barrier(nr_color,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(g.mv_prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(_countof(keep_in), keep_in);
    lfCore.cmd->CopyResource(g.mv_prev, nr_color);
    D3D12_RESOURCE_BARRIER keep_out[] = {
        Barrier(nr_color,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g.mv_prev, D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    lfCore.cmd->ResourceBarrier(_countof(keep_out), keep_out);
    g.mv_have_prev = true;
    return g.mv_tex;
}

static void DispatchResize(ID3D12Resource *from, UINT src_w, UINT src_h, ID3D12Resource *into, UINT dst_w, UINT dst_h)
{
    const UINT slot = static_cast<UINT>(lfCore.ring_slot);
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = g.resize_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * 2 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(from, &sd, cpu);
    cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(into, nullptr, &ud, cpu);

    ID3D12DescriptorHeap *heaps[] = { g.resize_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g.resize_rs);
    lfCore.cmd->SetPipelineState(g.resize_pso);
    const UINT sizes[4] = { dst_w, dst_h, src_w, src_h };
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, sizes, 0);
    auto gpu = g.resize_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * 2 * stride;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    lfCore.cmd->Dispatch((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
}

static void DispatchHaloPass(UINT pass_idx, ID3D12PipelineState *pso,
                             ID3D12Resource *t0, ID3D12Resource *t1, ID3D12Resource *t2, ID3D12Resource *mv, ID3D12Resource *u0,
                             UINT w, UINT hgt, int dx, int dy, uint32_t mode, float param, UINT mv_w, UINT mv_h)
{
    const UINT set = static_cast<UINT>(lfCore.ring_slot) * 3 + pass_idx;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = g.halo_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(set) * 5 * stride;
    ID3D12Resource *srvs[4] = { t0, t1, t2, mv };
    for (int i = 0; i < 4; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = srvs[i] ? srvs[i]->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        lfCore.device->CreateShaderResourceView(srvs[i], &sd, cpu);
        cpu.ptr += stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = u0->GetDesc().Format; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(u0, nullptr, &ud, cpu);

    ID3D12DescriptorHeap *heaps[] = { g.halo_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g.halo_rs);
    lfCore.cmd->SetPipelineState(pso);
    UINT32 c[10] = { w, hgt, static_cast<UINT32>(dx), static_cast<UINT32>(dy),
                     static_cast<UINT32>(kHaloRadius), static_cast<UINT32>(kHaloStep), mode, 0,
                     mv ? mv_w : 0u, mv ? mv_h : 0u };
    memcpy(&c[7], &param, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 10, c, 0);
    auto gpu = g.halo_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(set) * 5 * stride;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    lfCore.cmd->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
}

static void RunHaloFilter(ID3D12Resource *nr_in, ID3D12Resource *nr_out, UINT w, UINT hgt, float halo,
                          ID3D12Resource *mv, UINT mv_w, UINT mv_h)
{
    DispatchHaloPass(0, g.halo_blur_pso, nr_in, nr_out, nr_in, mv, g.halo_th, w, hgt, 1, 0, 0u, kHaloSigmaR, mv_w, mv_h);
    D3D12_RESOURCE_BARRIER b1 = Barrier(g.halo_th, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &b1);

    DispatchHaloPass(1, g.halo_blur_pso, nr_in, g.halo_th, nr_in, mv, g.halo_tb, w, hgt, 0, 1, 1u, kHaloSigmaR, mv_w, mv_h);
    D3D12_RESOURCE_BARRIER b2 = Barrier(g.halo_tb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &b2);

    DispatchHaloPass(2, g.halo_fix_pso, nr_in, nr_out, g.halo_tb, mv, g.halo_dc, w, hgt, 0, 0, 2u, halo, mv_w, mv_h);
    D3D12_RESOURCE_BARRIER b3[] = {
        Barrier(g.halo_dc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g.halo_th, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g.halo_tb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    lfCore.cmd->ResourceBarrier(_countof(b3), b3);
}

static void DispatchDetail(ID3D12Resource *native, ID3D12Resource *net_in, ID3D12Resource *net_out,
                           ID3D12Resource *delta, ID3D12Resource *into, UINT pic_w, UINT pic_h,
                           float gain, float edge_guard, float keep_colors, float low_light)
{
    if (!delta) edge_guard = 0.0f;
    const UINT slot = static_cast<UINT>(lfCore.ring_slot);
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = g.detail_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * 5 * stride;

    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(native, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(net_in, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(net_out, &sd, cpu); cpu.ptr += stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC dd = sd;
    dd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lfCore.device->CreateShaderResourceView(delta, &dd, cpu); cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(into, nullptr, &ud, cpu);

    ID3D12DescriptorHeap *heaps[] = { g.detail_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g.detail_rs);
    lfCore.cmd->SetPipelineState(g.detail_pso);
    UINT32 rc[6] = { pic_w, pic_h, 0, 0, 0, 0 };
    memcpy(&rc[2], &gain, sizeof(float));
    memcpy(&rc[3], &edge_guard, sizeof(float));
    memcpy(&rc[4], &keep_colors, sizeof(float));
    memcpy(&rc[5], &low_light, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 6, rc, 0);
    auto gpu = g.detail_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * 5 * stride;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    lfCore.cmd->Dispatch((pic_w + 7) / 8, (pic_h + 7) / 8, 1);
}

static void RunDenoise(ID3D12Resource *output, UINT w, UINT hgt, float amount, bool reset)
{
    if (reset) g.dn_frames = 0;
    const UINT set = g.dn_head;
    ID3D12Resource *slot = g.dn_ring[set];
    D3D12_RESOURCE_BARRIER pre[] = {
        Barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(slot,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(_countof(pre), pre);
    lfCore.cmd->CopyResource(slot, output);
    D3D12_RESOURCE_BARRIER mid[] = {
        Barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(slot,   D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    lfCore.cmd->ResourceBarrier(_countof(mid), mid);

    ID3D12Resource *want[4] = { g.dn_ring[set], g.dn_ring[(set + 2) % 3], g.dn_ring[(set + 1) % 3], output };
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (memcmp(g.dn_bound[set], want, sizeof(want)) != 0)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g.dn_heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(set) * 4 * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int i = 0; i < 3; ++i) { lfCore.device->CreateShaderResourceView(want[i], &sd, cpu); cpu.ptr += stride; }
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(output, nullptr, &ud, cpu);
        memcpy(g.dn_bound[set], want, sizeof(want));
    }

    ID3D12DescriptorHeap *heaps[] = { g.dn_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g.dn_rs);
    lfCore.cmd->SetPipelineState(g.dn_pso);
    const UINT flags = (g.dn_frames >= 1 ? 1u : 0u) | (g.dn_frames >= 2 ? 2u : 0u);
    UINT32 c[4] = { w, hgt, 0, flags };
    memcpy(&c[2], &amount, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.dn_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(set) * 4 * stride;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    lfCore.cmd->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);

    g.dn_head = (set + 1) % 3;
    if (g.dn_frames < 2) ++g.dn_frames;
}

static void RunFinish(ID3D12Resource *output, UINT w, UINT hgt, float edge, ID3D12Resource *mv, UINT mv_w, UINT mv_h,
                      bool reset, float bright, float contrast, float sat, float warm)
{
    const UINT ping = g.finish_ping;
    ID3D12Resource *next = g.finish_hist[1 - ping];
    D3D12_RESOURCE_BARRIER pre[] = {
        Barrier(output,       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,           D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(g.finish_tmp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(_countof(pre), pre);
    lfCore.cmd->CopyResource(g.finish_tmp, output);
    D3D12_RESOURCE_BARRIER mid[] = {
        Barrier(output,       D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g.finish_tmp, D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(next,         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    lfCore.cmd->ResourceBarrier(_countof(mid), mid);

    const UINT slot = static_cast<UINT>(lfCore.ring_slot);
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = g.finish_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(slot) * 5 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lfCore.device->CreateShaderResourceView(g.finish_tmp, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    lfCore.device->CreateShaderResourceView(mv, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lfCore.device->CreateShaderResourceView(g.finish_hist[ping], &sd, cpu); cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(output, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(next, nullptr, &ud, cpu);

    ID3D12DescriptorHeap *heaps[] = { g.finish_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g.finish_rs);
    lfCore.cmd->SetPipelineState(g.finish_pso);
    const bool colors = (bright != 0.0f || contrast != 0.0f || sat != 0.0f || warm != 0.0f);
    const UINT flags = ((g.finish_hist_valid && !reset) ? 1u : 0u) | (colors ? 2u : 0u);
    const float zero = 0.0f;
    UINT32 c[12] = { w, hgt, 0, 0, mv ? mv_w : 0u, mv ? mv_h : 0u, flags, 0, 0, 0, 0, 0 };
    memcpy(&c[2], &zero, sizeof(float));
    memcpy(&c[3], &edge, sizeof(float));
    memcpy(&c[7], &bright, sizeof(float));
    memcpy(&c[8], &contrast, sizeof(float));
    memcpy(&c[9], &sat, sizeof(float));
    memcpy(&c[10], &warm, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 12, c, 0);
    auto gpu = g.finish_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot) * 5 * stride;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    lfCore.cmd->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER post = Barrier(next, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &post);
    g.finish_ping = 1 - ping;
    g.finish_hist_valid = true;
}

} // namespace dlss5

static bool Dlss5Wanted()
{
    return smOpt.upscaler.load() == 7;
}

static bool Dlss5HasSourceCopy()
{
    return dlss5::g.has_source_copy && dlss5::g.source_copy != nullptr;
}

static void Dlss5WarmupStart()
{
    if (!Dlss5Wanted()) return;
    dlss5::EnsureShaders();
    EnsureFlowEstimate();
    dlss5::EnsureRuntime();
}

static bool Dlss5PrepareSession(UINT pic_w, UINT pic_h)
{
    if (!Dlss5Wanted() || pic_w < 64 || pic_h < 64) return true;
    if (!dlss5::EnsureRuntime()) return false;
    const int scale_pct = std::clamp(smOpt.dlss5_scale.load(), 25, 100);
    const UINT nw = std::max(64u, std::min(pic_w, static_cast<UINT>(std::lround(pic_w * (scale_pct / 100.0) / 2.0)) * 2u));
    const UINT nh = std::max(64u, std::min(pic_h, static_cast<UINT>(std::lround(pic_h * (scale_pct / 100.0) / 2.0)) * 2u));
    const UINT preset = static_cast<UINT>(std::clamp(smOpt.dlss5_model.load(), 0, 1));
    const UINT passes = static_cast<UINT>(std::clamp(smOpt.dlss5_passes.load(), 1, static_cast<int>(dlss5::kMaxPasses)));
    return dlss5::EnsurePipelineAndFeatures(pic_w, pic_h, nw, nh, preset, passes);
}

static bool Dlss5RestoreSource(Pipeline &pipe)
{
    if (!dlss5::g.has_source_copy || !dlss5::g.source_copy || !pipe.picture.gpu_tex) return false;
    if (pipe.picture.gpu_tex->GetDesc().Width != dlss5::g.pic_w ||
        pipe.picture.gpu_tex->GetDesc().Height != dlss5::g.pic_h)
        return false;
    if (!CmdOpen()) return false;
    D3D12_RESOURCE_BARRIER in[] = {
        Barrier(dlss5::g.source_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(2, in);
    lfCore.cmd->CopyResource(pipe.picture.gpu_tex, dlss5::g.source_copy);
    D3D12_RESOURCE_BARRIER out[] = {
        Barrier(dlss5::g.source_copy, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    lfCore.cmd->ResourceBarrier(2, out);
    const UINT64 fence = CmdSubmit();
    return fence != 0 && AwaitMark(lfCore.timeline, fence, 5000);
}

static void Dlss5Close()
{
    CmdDrain();
    dlss5::ReleaseFeatures();
    dlss5::CloseTextures();
    auto rel = [](auto *&ptr) { if (ptr) { ptr->Release(); ptr = nullptr; } };
    rel(dlss5::g.mv_heap);
    rel(dlss5::g.settle_luma_pso);
    rel(dlss5::g.settle_pso);
    rel(dlss5::g.settle_rs);
    rel(dlss5::g.settle_heap);
    rel(dlss5::g.dn_pso);
    rel(dlss5::g.dn_rs);
    rel(dlss5::g.dn_heap);
    rel(dlss5::g.resize_pso);
    rel(dlss5::g.resize_rs);
    rel(dlss5::g.resize_heap);
    rel(dlss5::g.detail_pso);
    rel(dlss5::g.detail_rs);
    rel(dlss5::g.detail_heap);
    rel(dlss5::g.halo_blur_pso);
    rel(dlss5::g.halo_fix_pso);
    rel(dlss5::g.halo_rs);
    rel(dlss5::g.halo_heap);
    rel(dlss5::g.finish_pso);
    rel(dlss5::g.finish_rs);
    rel(dlss5::g.finish_heap);
    dlss5::g.shaders_ready = false;
    dlss5::g.shaders_failed = false;
    if (dlss5::g.params) { NVSDK_NGX_D3D12_DestroyParameters(dlss5::g.params); dlss5::g.params = nullptr; }
    if (dlss5::g.module) { FreeLibrary(dlss5::g.module); dlss5::g.module = nullptr; }
    dlss5::g.init_ext = nullptr; dlss5::g.create = nullptr; dlss5::g.evaluate = nullptr; dlss5::g.release = nullptr;
    dlss5::g.runtime_ok = false;
    dlss5::g.runtime_failed = false;
}

static bool Dlss5Run(Pipeline &pipe, bool reset, bool from_source_copy)
{
    if (!Dlss5Wanted() || !pipe.picture.gpu_tex) return true;
    if (!dlss5::EnsureRuntime()) return false;

    const UINT pic_w = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT pic_h = pipe.stretched ? pipe.screen_h : pipe.work_h;
    if (pic_w < 64 || pic_h < 64) return false;

    const int scale_pct = std::clamp(smOpt.dlss5_scale.load(), 25, 100);
    const UINT nw = std::max(64u, std::min(pic_w, static_cast<UINT>(std::lround(pic_w * (scale_pct / 100.0) / 2.0)) * 2u));
    const UINT nh = std::max(64u, std::min(pic_h, static_cast<UINT>(std::lround(pic_h * (scale_pct / 100.0) / 2.0)) * 2u));
    const bool small_nr = (nw != pic_w || nh != pic_h);
    const UINT preset = static_cast<UINT>(std::clamp(smOpt.dlss5_model.load(), 0, 1));
    const UINT passes = static_cast<UINT>(std::clamp(smOpt.dlss5_passes.load(), 1, static_cast<int>(dlss5::kMaxPasses)));

    if (!dlss5::EnsurePipelineAndFeatures(pic_w, pic_h, nw, nh, preset, passes)) return false;
    if (from_source_copy && !dlss5::g.has_source_copy) return true;

    const float raw_intensity = std::clamp(smOpt.dlss5_intensity.load(), 0, 300) / 100.0f;
    const float net_intensity = std::min(raw_intensity, 1.0f);
    const float detail_gain   = std::clamp(raw_intensity, 1.0f, 3.0f);
    const float denoise       = std::clamp(smOpt.dlss5_denoise.load() / 100.0f, 0.0f, 1.0f);
    const float halo          = denoise * dlss5::kHaloShare;
    const float keep_colors   = std::clamp(smOpt.dlss5_keep_colors.load() / 100.0f, 0.0f, 1.0f);
    const float low_light     = std::clamp(smOpt.dlss5_low_light.load() / 100.0f, 0.0f, 1.0f);
    const float edge          = std::clamp(smOpt.dlss5_edge.load() / 100.0f, 0.0f, 1.0f);
    const float bright        = std::clamp(smOpt.dlss5_bright.load() / 100.0f, -1.0f, 1.0f);
    const float contrast      = std::clamp(smOpt.dlss5_contrast.load() / 100.0f, -1.0f, 1.0f);
    const float sat           = std::clamp(smOpt.dlss5_sat.load() / 100.0f, -1.0f, 1.0f);
    const float warm          = std::clamp(smOpt.dlss5_warm.load() / 100.0f, -1.0f, 1.0f);
    const bool has_colors     = (bright != 0.0f || contrast != 0.0f || sat != 0.0f || warm != 0.0f);
    const bool halo_active    = halo > 0.0f && dlss5::EnsureHaloTargets(nw, nh);
    const bool denoise_active = denoise > 0.0f && dlss5::EnsureDenoiseTargets(pic_w, pic_h);
    const bool finish_active  = (edge > 0.0f || has_colors) && dlss5::EnsureFinishTargets(pic_w, pic_h);
    const bool need_detail    = small_nr || halo_active || keep_colors > 0.0f || low_light > 0.0f || detail_gain > 1.0f;

    if (!CmdOpen()) return false;

    if (!from_source_copy)
    {
        D3D12_RESOURCE_BARRIER save_in[] = {
            Barrier(pipe.picture.gpu_tex,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(dlss5::g.source_copy,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        lfCore.cmd->ResourceBarrier(2, save_in);
        lfCore.cmd->CopyResource(dlss5::g.source_copy, pipe.picture.gpu_tex);
        D3D12_RESOURCE_BARRIER save_out[] = {
            Barrier(pipe.picture.gpu_tex,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(dlss5::g.source_copy,  D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        lfCore.cmd->ResourceBarrier(2, save_out);
        dlss5::g.has_source_copy = true;
    }

    if (small_nr)
    {
        D3D12_RESOURCE_BARRIER to_uav = Barrier(dlss5::g.net_in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &to_uav);
        dlss5::DispatchResize(dlss5::g.source_copy, pic_w, pic_h, dlss5::g.net_in, nw, nh);
        D3D12_RESOURCE_BARRIER to_srv = Barrier(dlss5::g.net_in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
    }

    ID3D12Resource *nr_color = small_nr ? dlss5::g.net_in : dlss5::g.source_copy;
    ID3D12Resource *mv_tex   = dlss5::PrepareDlss5Motion(nr_color, reset, from_source_copy);
    ID3D12Resource *nr_result = small_nr ? dlss5::g.net_out : (need_detail ? dlss5::g.raw : dlss5::g.result);

    dlss5::g.params->Reset();
    dlss5::g.params->Set("DLSSNR.Color", nr_color);
    dlss5::g.params->Set("DLSSNR.Output", nr_result);
    dlss5::g.params->Set("DLSSNR.MVec", mv_tex);
    dlss5::g.params->Set("DLSSNR.ColorSubrectBaseX", 0u);
    dlss5::g.params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    dlss5::g.params->Set("DLSSNR.ColorSubrectWidth", nw);
    dlss5::g.params->Set("DLSSNR.ColorSubrectHeight", nh);
    dlss5::g.params->Set("DLSSNR.MVecSubrectBaseX", 0u);
    dlss5::g.params->Set("DLSSNR.MVecSubrectBaseY", 0u);
    dlss5::g.params->Set("DLSSNR.MVecSubrectWidth", nw);
    dlss5::g.params->Set("DLSSNR.MVecSubrectHeight", nh);
    dlss5::g.params->Set("DLSSNR.OutputSubrectBaseX", 0u);
    dlss5::g.params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    dlss5::g.params->Set("DLSSNR.OutputSubrectWidth", nw);
    dlss5::g.params->Set("DLSSNR.OutputSubrectHeight", nh);
    dlss5::g.params->Set("DLSSNR.MVecScaleX", 1.0f);
    dlss5::g.params->Set("DLSSNR.MVecScaleY", 1.0f);
    dlss5::g.params->Set("DLSSNR.Enabled", 1u);
    dlss5::g.params->Set("DLSSNR.Reset", (reset || dlss5::g.need_warmup) ? 1u : 0u);
    dlss5::g.params->Set("DLSSNR.Intensity", net_intensity);
    dlss5::g.params->Set("DLSSNR.LocalToneStrength", std::clamp(smOpt.dlss5_tone.load(), 0, 200) / 100.0f);
    dlss5::g.params->Set("DLSSNR.LocalStructureStrength", std::clamp(smOpt.dlss5_structure.load(), 0, 200) / 100.0f);
    dlss5::g.params->Set("DLSSNR.SkinStructureStrength", std::clamp(smOpt.dlss5_skin.load(), -100, 250) / 100.0f);
    dlss5::g.params->Set("DLSSNR.UseAutoMask", 1u);
    dlss5::g.params->Set("DLSSNR.Style", static_cast<unsigned>(std::clamp(smOpt.dlss5_style.load(), 0, 2)));
    dlss5::g.params->Set("DLSSNR.UICorrection", 1u);

    // Warm up the newly created temporal NR feature (ScreenNative session.py uses <= 4 warmup iterations)
    // so recurrent temporal state settles cleanly before presenting the first output.
    if (dlss5::g.need_warmup)
    {
        dlss5::g.need_warmup = false;
        for (int w = 0; w < 2; ++w)
        {
            dlss5::g.params->Set("DLSSNR.Reset", w == 0 ? 1u : 0u);
            NVSDK_NGX_Result wr = NVSDK_NGX_Result_Fail;
            if (!dlss5::EvaluateGuarded(dlss5::g.evaluate, lfCore.cmd, dlss5::g.feature, dlss5::g.params, &wr))
            {
                DropCommands();
                return false;
            }
            const UINT64 wf = CmdSubmit();
            if (!wf || !AwaitMark(lfCore.timeline, wf, 10000)) return false;
            LoopBeat();
            if (NVSDK_NGX_FAILED(wr))
            {
                Trace("[dlss5] warmup EvaluateFeature failed 0x%08X (%s)", (unsigned)wr, NgxCodeText(wr));
                return false;
            }
            if (!CmdOpen()) return false;
        }
        dlss5::g.params->Set("DLSSNR.Reset", 0u);
    }

    NVSDK_NGX_Result status = NVSDK_NGX_Result_Fail;
    if (!dlss5::EvaluateGuarded(dlss5::g.evaluate, lfCore.cmd, dlss5::g.feature, dlss5::g.params, &status))
    {
        DropCommands();
        return false;
    }

    if (!NVSDK_NGX_FAILED(status) && passes > 1 && dlss5::g.scratch)
    {
        for (UINT i = 0; i + 1 < passes && dlss5::g.extras[i]; ++i)
        {
            D3D12_RESOURCE_BARRIER read = Barrier(nr_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            lfCore.cmd->ResourceBarrier(1, &read);
            dlss5::g.params->Set("DLSSNR.Color", nr_result);
            dlss5::g.params->Set("DLSSNR.Output", dlss5::g.scratch);
            dlss5::g.params->Set("DLSSNR.Reset", reset ? 1u : 0u);
            dlss5::g.params->Set("DLSSNR.LocalToneStrength", 0.0f);
            NVSDK_NGX_Result extra_res = NVSDK_NGX_Result_Fail;
            if (!dlss5::EvaluateGuarded(dlss5::g.evaluate, lfCore.cmd, dlss5::g.extras[i], dlss5::g.params, &extra_res))
            {
                DropCommands();
                return false;
            }
            if (NVSDK_NGX_FAILED(extra_res))
            {
                D3D12_RESOURCE_BARRIER back = Barrier(nr_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                lfCore.cmd->ResourceBarrier(1, &back);
                break;
            }
            D3D12_RESOURCE_BARRIER copy[] = {
                Barrier(nr_result,        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                Barrier(dlss5::g.scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,           D3D12_RESOURCE_STATE_COPY_SOURCE),
            };
            lfCore.cmd->ResourceBarrier(2, copy);
            lfCore.cmd->CopyResource(nr_result, dlss5::g.scratch);
            D3D12_RESOURCE_BARRIER back[] = {
                Barrier(nr_result,        D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                Barrier(dlss5::g.scratch, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            };
            lfCore.cmd->ResourceBarrier(2, back);
        }
    }

    if (NVSDK_NGX_FAILED(status))
    {
        Trace("[dlss5] EvaluateFeature failed 0x%08X (%s)", (unsigned)status, NgxCodeText(status));
        const UINT64 f = CmdSubmit();
        if (f) AwaitMark(lfCore.timeline, f, 5000);
        return false;
    }

    if (nr_result != dlss5::g.result)
    {
        D3D12_RESOURCE_BARRIER to_srv = Barrier(nr_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
        if (halo_active)
        {
            dlss5::RunHaloFilter(nr_color, nr_result, nw, nh, halo, mv_tex, nw, nh);
            dlss5::DispatchDetail(dlss5::g.source_copy, nr_color, nr_result, dlss5::g.halo_dc, dlss5::g.result,
                                  pic_w, pic_h, detail_gain, halo, keep_colors, low_light);
            D3D12_RESOURCE_BARRIER hback = Barrier(dlss5::g.halo_dc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            lfCore.cmd->ResourceBarrier(1, &hback);
        }
        else
        {
            dlss5::DispatchDetail(dlss5::g.source_copy, nr_color, nr_result, nullptr, dlss5::g.result,
                                  pic_w, pic_h, detail_gain, 0.0f, keep_colors, low_light);
        }
        D3D12_RESOURCE_BARRIER back = Barrier(nr_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &back);
    }

    if (denoise_active)
        dlss5::RunDenoise(dlss5::g.result, pic_w, pic_h, denoise, reset || from_source_copy);

    if (finish_active)
        dlss5::RunFinish(dlss5::g.result, pic_w, pic_h, edge, mv_tex, nw, nh,
                         reset || from_source_copy, bright, contrast, sat, warm);
    else
        dlss5::g.finish_hist_valid = false;

    D3D12_RESOURCE_BARRIER copy_in[] = {
        Barrier(dlss5::g.result,       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,           D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(pipe.picture.gpu_tex,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(_countof(copy_in), copy_in);
    lfCore.cmd->CopyResource(pipe.picture.gpu_tex, dlss5::g.result);
    D3D12_RESOURCE_BARRIER copy_out[] = {
        Barrier(dlss5::g.result,       D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(pipe.picture.gpu_tex,  D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    lfCore.cmd->ResourceBarrier(_countof(copy_out), copy_out);

    const UINT64 fence = CmdSubmit();
    return fence != 0 && AwaitMark(lfCore.timeline, fence, 10000);
}
