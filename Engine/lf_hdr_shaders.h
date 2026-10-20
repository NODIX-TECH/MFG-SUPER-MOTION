// lf_hdr_shaders.h - the HDR shaders: the source pass (half float or 10-bit
// to what the network takes) and the output pass (the network's change
// applied to the HDR picture, PQ encoding for an HDR swap chain).
#pragma once

// HDR10 output remains HDR. Optional SDR recorders receive a tone-mapped copy.

static const char kHdrInHlsl[] = R"hlsl(
float ToLinear1(float c) { return c <= .04045 ? c / 12.92 : pow(max((c + .055) / 1.055, 0), 2.4); }
float3 SrgbToLinear(float3 v) { return float3(ToLinear1(v.r), ToLinear1(v.g), ToLinear1(v.b)); }
float ToSrgb1(float c) { return c <= .0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - .055; }
float3 LinearToSrgb(float3 v) { v = max(v, 0); return float3(ToSrgb1(v.r), ToSrgb1(v.g), ToSrgb1(v.b)); }
float Brightest(float3 v) { return max(0, max(v.r, max(v.g, v.b))); }

// An HDR picture as SDR on an HDR screen (the app's HDR switch off). Round 33, measured on a game in HDR
// (tests\hdrgame.exe, hdrcheck.ps1): colours outside sRGB had their negative parts cut off, which made them
// stronger and brighter than the game drew them (a deep red 60 nits shown at 85), and the knee began at 0.8 of
// SDR white, so the game's SDR white itself came out 7% dim. Now a colour outside sRGB goes towards the grey of
// its own brightness until it is inside (brightness kept), and only what is above nine tenths of SDR white is
// bent towards it.
float3 ScRgbToSdr(float3 c, float white)
{
    float3 v = c / max(white, 1e-3);
    const float y = max(dot(v, float3(.2126, .7152, .0722)), 0);
    const float least = min(v.r, min(v.g, v.b));
    if (least < 0) v = y + (v - y) * (y / max(y - least, 1e-6));
    v = max(v, 0);
    float top = Brightest(v);
    if (top > .9) { float kept = .9 + .1 * (1 - exp(-(top - .9) / .1)); v *= kept / top; }
    return LinearToSrgb(saturate(v));
}

Texture2D<float4> source : register(t0);
RWTexture2D<float4> target : register(u0);
cbuffer Source : register(b0) { uint floatMode; float white; uint upsideDown; };

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    uint w, h;
    target.GetDimensions(w, h);
    if (cell.x >= w || cell.y >= h) return;
    int2 from = upsideDown ? int2(w - 1 - cell.x, h - 1 - cell.y) : int2(cell.xy);
    float4 c = source.Load(int3(from, 0));
    if (floatMode == 1) c = float4(LinearToSrgb(max(c.rgb, 0) / (white + Brightest(c.rgb))), 1);
    else if (floatMode == 2) c = float4(ScRgbToSdr(c.rgb, white), 1);
    target[cell.xy] = c;
}
)hlsl";

static const char kHdrOutHlsl[] = R"hlsl(
float ToLinear1(float c) { return c <= .04045 ? c / 12.92 : pow(max((c + .055) / 1.055, 0), 2.4); }
float3 SrgbToLinear(float3 v) { return float3(ToLinear1(v.r), ToLinear1(v.g), ToLinear1(v.b)); }
float ToSrgb1(float c) { return c <= .0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - .055; }
float3 LinearToSrgb(float3 v) { v = max(v, 0); return float3(ToSrgb1(v.r), ToSrgb1(v.g), ToSrgb1(v.b)); }
float Brightest(float3 v) { return max(0, max(v.r, max(v.g, v.b))); }

Texture2D<float4> screenPic : register(t0);
Texture2D<float4> netBefore : register(t1);
Texture2D<float4> netAfter : register(t2);
RWTexture2D<float4> target : register(u0);
cbuffer Output : register(b0) { float white; uint untouched; uint divider; uint displayMode; };

float3 ToPq(float3 rgb709)
{
    float3 rgb2020 = mul(float3x3(.627404, .329283, .043313,
                                  .069097, .919540, .011362,
                                  .016391, .088013, .895595), rgb709);
    float3 y = pow(saturate(rgb2020 * .008), 2610.0 / 16384.0);
    return pow((3424.0 / 4096.0 + (2413.0 / 128.0) * y) / (1 + (2392.0 / 128.0) * y), 2523.0 / 32.0);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    uint w, h;
    target.GetDimensions(w, h);
    if (cell.x >= w || cell.y >= h) return;
    float3 screen = screenPic.Load(int3(cell.xy, 0)).rgb;
    float3 before = SrgbToLinear(netBefore.Load(int3(cell.xy, 0)).rgb);
    float3 after = SrgbToLinear(netAfter.Load(int3(cell.xy, 0)).rgb);
    bool keep = untouched || (divider != 0xffffffff && cell.x < divider);
    float3 outRgb = keep ? screen : screen + (white + Brightest(screen)) * clamp(after - before, -.25, .25);
    if (!untouched && divider != 0xffffffff && cell.x >= divider && cell.x < divider + 2)
        outRgb = white * SrgbToLinear(float3(118.0 / 255.0, 185.0 / 255.0, 0));
    // Not kept as HDR: the SDR picture in linear light. With the linear
    // output picked (bit 4) its white is the screen's SDR white, so on an
    // HDR desktop it is as bright as every other SDR window.
    if (!(displayMode & 1)) outRgb = (keep ? before : after) * ((displayMode & 4) ? white : 1.0);
    if (displayMode & 2) outRgb = ToPq(outRgb);
    target[cell.xy] = float4(clamp(outRgb, -65504, 65504), 1);
}
)hlsl";
