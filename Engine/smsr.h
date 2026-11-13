// smsr.h - SMSR 1.0 (Super Motion Super Resolution): our own x2 upscaler, a small network trained on game frames
// (ai learning\smsr\code\smsr.py, tiny: 16 channels, three layers in the middle, 9 thousand numbers), run as plain
// compute passes on any card - no ONNX runtime, no DirectML: the very same text is the Windows engine's (D3D12,
// lf_smsr.inl) and the Linux one's (Vulkan, Linux\engine\smsr_vk.h, through Linux\tools\build_smsr_spirv.py).
//
// What it does, at the picture's own (low) size: the picture's 3x3 neighbourhoods into 16 numbers a pixel (head),
// three times 16 into 16 (body), the head's added back, then 12 numbers a pixel (tail) - each low pixel's four high
// ones, three colours each (DepthToSpace) - added to the picture scaled up plainly (bilinear). Zero outside the
// picture for the network, the picture's edge held for the bilinear base, exactly as the model was trained
// (pack_smsr.py checks this arithmetic against ONNX Runtime: worst 0.0007).
//
// Five passes: head (picture t8 -> u0..u3), body 0, 1, 2 (t0..t3 -> u0..u3), tail (t0..t3 + the head's t4..t7 and
// the picture t8 -> u0 at twice the size). Numbers b0: the part of the picture (x, y, w, h) and the body's layer;
// the network's numbers b1 (smsr_weights.h, pack_smsr.py says how they lie).
#pragma once

static const char kSmsrHlsl[] = R"hlsl(
Texture2D<float4> gT0 : register(t0);
Texture2D<float4> gT1 : register(t1);
Texture2D<float4> gT2 : register(t2);
Texture2D<float4> gT3 : register(t3);
Texture2D<float4> gT4 : register(t4);
Texture2D<float4> gT5 : register(t5);
Texture2D<float4> gT6 : register(t6);
Texture2D<float4> gT7 : register(t7);
Texture2D<float4> gPic : register(t8);
RWTexture2D<float4> gU0 : register(u0);
RWTexture2D<float4> gU1 : register(u1);
RWTexture2D<float4> gU2 : register(u2);
RWTexture2D<float4> gU3 : register(u3);
cbuffer SmsrNumbers : register(b0)
{
    int gX; int gY; int gW; int gH; int gLayer; int gN1; int gN2; int gN3;    // gN1: strength, percent (round 88o)
};
cbuffer SmsrWeights : register(b1)
{
    float4 gWt[2324];
};

bool In(int2 p) { return p.x >= 0 && p.y >= 0 && p.x < gW && p.y < gH; }
float4 Pic(int2 p) { return In(p) ? float4(gPic.Load(int3(gX + p.x, gY + p.y, 0)).rgb, 0.0f) : float4(0.0f, 0.0f, 0.0f, 0.0f); }
float3 PicHeld(int2 p)
{
    p = clamp(p, int2(0, 0), int2(gW - 1, gH - 1));
    return gPic.Load(int3(gX + p.x, gY + p.y, 0)).rgb;
}
float4 Feat(Texture2D<float4> t, int2 p) { return In(p) ? t.Load(int3(p, 0)) : float4(0.0f, 0.0f, 0.0f, 0.0f); }

[numthreads(8, 8, 1)]
void SmsrHeadMain(uint3 id : SV_DispatchThreadID)
{
    int2 p = int2(id.xy);
    if (p.x >= gW || p.y >= gH) return;
    float4 acc[4];
    [unroll] for (int g = 0; g < 4; ++g) acc[g] = gWt[144 + g];
    // (Each tap read once and given to all sixteen outputs; the taps in a loop - unrolled, the compiler took a minute and
    // a half over it on the first start.)
    [loop] for (int t = 0; t < 9; ++t)
    {
        float4 x = Pic(p + int2(t % 3 - 1, t / 3 - 1));
        [unroll] for (int o = 0; o < 16; ++o) acc[o / 4][o % 4] += dot(x, gWt[o * 9 + t]);
    }
    gU0[p] = max(acc[0], 0.0f); gU1[p] = max(acc[1], 0.0f); gU2[p] = max(acc[2], 0.0f); gU3[p] = max(acc[3], 0.0f);
}

// (Each body layer its own pass, its numbers' place written in: given as a number at run time, the card took eight
// times as long over each - 3.5 ms against the tail's 0.4.)
void Body(uint3 id, const int base)
{
    int2 p = int2(id.xy);
    if (p.x >= gW || p.y >= gH) return;
    float4 acc[4];
    [unroll] for (int g = 0; g < 4; ++g) acc[g] = gWt[base + 576 + g];
    [loop] for (int t = 0; t < 9; ++t)
    {
        int2 q = p + int2(t % 3 - 1, t / 3 - 1);
        float4 f0 = Feat(gT0, q), f1 = Feat(gT1, q), f2 = Feat(gT2, q), f3 = Feat(gT3, q);
        [unroll] for (int o = 0; o < 16; ++o)
        {
            int w = base + (o * 9 + t) * 4;
            acc[o / 4][o % 4] += dot(f0, gWt[w]) + dot(f1, gWt[w + 1]) + dot(f2, gWt[w + 2]) + dot(f3, gWt[w + 3]);
        }
    }
    gU0[p] = max(acc[0], 0.0f); gU1[p] = max(acc[1], 0.0f); gU2[p] = max(acc[2], 0.0f); gU3[p] = max(acc[3], 0.0f);
}

[numthreads(8, 8, 1)]
void SmsrBody0Main(uint3 id : SV_DispatchThreadID) { Body(id, 148); }
[numthreads(8, 8, 1)]
void SmsrBody1Main(uint3 id : SV_DispatchThreadID) { Body(id, 728); }
[numthreads(8, 8, 1)]
void SmsrBody2Main(uint3 id : SV_DispatchThreadID) { Body(id, 1308); }

float3 Base(int2 hp)
{
    float2 u = max((float2(hp) + 0.5f) * 0.5f - 0.5f, float2(0.0f, 0.0f));
    int2 i0 = int2(floor(u));
    float2 fr = u - float2(i0);
    int2 i1 = i0 + int2(1, 1);
    float3 a = lerp(PicHeld(i0), PicHeld(int2(i1.x, i0.y)), fr.x);
    float3 b = lerp(PicHeld(int2(i0.x, i1.y)), PicHeld(i1), fr.x);
    return lerp(a, b, fr.y);
}

[numthreads(8, 8, 1)]
void SmsrTailMain(uint3 id : SV_DispatchThreadID)
{
    int2 p = int2(id.xy);
    if (p.x >= gW || p.y >= gH) return;
    float4 d[3];
    [unroll] for (int g = 0; g < 3; ++g) d[g] = gWt[2320 + g];
    [loop] for (int t = 0; t < 9; ++t)
    {
        int2 q = p + int2(t % 3 - 1, t / 3 - 1);
        float4 f0 = Feat(gT0, q) + Feat(gT4, q), f1 = Feat(gT1, q) + Feat(gT5, q);
        float4 f2 = Feat(gT2, q) + Feat(gT6, q), f3 = Feat(gT3, q) + Feat(gT7, q);
        [unroll] for (int o = 0; o < 12; ++o)
        {
            int w = 1888 + (o * 9 + t) * 4;
            d[o / 4][o % 4] += dot(f0, gWt[w]) + dot(f1, gWt[w + 1]) + dot(f2, gWt[w + 2]) + dot(f3, gWt[w + 3]);
        }
    }
    // (DepthToSpace, CRD: output o = colour * 4 + dy * 2 + dx.)
    [unroll] for (int dy = 0; dy < 2; ++dy)
    [unroll] for (int dx = 0; dx < 2; ++dx)
    {
        int2 hp = p * 2 + int2(dx, dy);
        // (Round 88o: the network's detail times the strength picked - 100% as it was trained.)
        float3 c = Base(hp) + float(gN1) * 0.01f * float3(d[0][dy * 2 + dx], d[1][dy * 2 + dx], d[2][dy * 2 + dx]);
        gU0[hp] = float4(saturate(c), 1.0f);
    }
}
)hlsl";
