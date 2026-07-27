// Video decode output to linear light, and linear light to the display.
//
// The pipeline composites in linear light at 16-bit float. Getting there from a
// decoded frame is two steps that are easy to conflate: the YUV-to-RGB matrix
// produces *non-linear* R'G'B' — still gamma-encoded — and only applying the
// transfer function afterwards yields linear values. Blending encoded values is
// the single most common way to get compositing subtly wrong.
//
// Output goes to an _SRGB render target, so the hardware performs the encode
// back to display space on write. That is why the present pass looks like a
// plain copy.

// Source pixel layouts.
#define LAYOUT_NV12    0
#define LAYOUT_YUV420P 1

// YUV-to-RGB matrices.
#define SPACE_BT709  0
#define SPACE_BT601  1
#define SPACE_BT2020 2

// Transfer functions, named for what tags them in the file.
#define TRANSFER_BT709     0  // SDR; the sRGB curve stands in for BT.1886
#define TRANSFER_SMPTE2084 1  // PQ, HDR10
#define TRANSFER_ARIB_B67  2  // HLG

struct Params {
    int layout;
    int colorSpace;
    int transfer;
    int fullRange;
};

ConstantBuffer<Params> params : register(b0);

// One table of three slots serves every pass, so the bindings are named for
// their position rather than their meaning:
//
//   NV12    t0 = Y,     t1 = UV interleaved
//   planar  t0 = Y,     t1 = U,  t2 = V
//   present t0 = scene
//
// Typed views would need a separate declaration per layout at the same
// register, which HLSL does not allow.
Texture2D<float4> texture0 : register(t0);
Texture2D<float4> texture1 : register(t1);
Texture2D<float4> texture2 : register(t2);

SamplerState linearSampler : register(s0);

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// A single oversized triangle covering the viewport. Cheaper than a quad and it
// avoids the diagonal seam two triangles produce under some interpolation.
VSOutput VSMain(uint vertexId : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// ---------------------------------------------------------------- transfer --

// Inverse of the sRGB/BT.709 encoding: coded value to linear light.
float3 linearizeSrgb(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow(abs(c + 0.055) / 1.055, 2.4));
}

// SMPTE ST 2084. Returns linear light normalised so 1.0 is 10000 nits.
float3 linearizePq(float3 c) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 128.0 * 2523.0 / 4096.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 32.0 * 2413.0 / 4096.0;
    const float c3 = 32.0 * 2392.0 / 4096.0;

    float3 e = pow(max(c, 0.0), 1.0 / m2);
    return pow(max(e - c1, 0.0) / (c2 - c3 * e), 1.0 / m1);
}

// ARIB STD-B67, the HLG inverse OETF over the unit range.
float3 linearizeHlg(float3 c) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float cc = 0.5 - a * log(4.0 * a);
    return select(c <= 0.5, (c * c) / 3.0, (exp((c - cc) / a) + b) / 12.0);
}

float3 toLinear(float3 coded, int transfer) {
    if (transfer == TRANSFER_SMPTE2084) return linearizePq(coded);
    if (transfer == TRANSFER_ARIB_B67) return linearizeHlg(coded);
    return linearizeSrgb(coded);
}

// ------------------------------------------------------------------- matrix --

// Luma coefficients per colour space; the full matrix derives from them.
float3 yuvToRgb(float3 yuv, int space) {
    float kr, kb;
    if (space == SPACE_BT601) {
        kr = 0.299;
        kb = 0.114;
    } else if (space == SPACE_BT2020) {
        kr = 0.2627;
        kb = 0.0593;
    } else {
        kr = 0.2126;  // BT.709
        kb = 0.0722;
    }
    const float kg = 1.0 - kr - kb;

    const float y = yuv.x;
    const float u = yuv.y;
    const float v = yuv.z;

    const float r = y + 2.0 * (1.0 - kr) * v;
    const float b = y + 2.0 * (1.0 - kb) * u;
    const float g = (y - kr * r - kb * b) / kg;
    return float3(r, g, b);
}

// ------------------------------------------------------------------- passes --

float4 PSVideo(VSOutput input) : SV_Target {
    float y = texture0.Sample(linearSampler, input.uv).r;
    float2 uv;
    if (params.layout == LAYOUT_NV12) {
        uv = texture1.Sample(linearSampler, input.uv).rg;
    } else {
        uv = float2(texture1.Sample(linearSampler, input.uv).r,
                    texture2.Sample(linearSampler, input.uv).r);
    }

    // Studio-range video leaves headroom and footroom outside [0,1]; expanding
    // it is what keeps blacks from crushing and highlights from clipping.
    if (params.fullRange == 0) {
        y = (y * 255.0 - 16.0) / 219.0;
        uv = (uv * 255.0 - 128.0) / 224.0;
    } else {
        uv = uv - 0.5;
    }

    const float3 coded = yuvToRgb(float3(y, uv.x, uv.y), params.colorSpace);
    // Chroma reconstruction can push values slightly outside the legal range;
    // clamping before the transfer function keeps pow() away from negatives.
    const float3 linearRgb = toLinear(saturate(coded), params.transfer);
    return float4(linearRgb, 1.0);
}

float4 PSPresent(VSOutput input) : SV_Target {
    // The render target is _SRGB, so the hardware encodes on write.
    return texture0.Sample(linearSampler, input.uv);
}
