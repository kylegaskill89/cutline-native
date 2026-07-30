// Compositing a stack of layers in linear light, and encoding the result for
// display.
//
// Getting from a decoded frame to linear light is two steps that are easy to
// conflate: the YUV-to-RGB matrix produces *non-linear* R'G'B' — still
// gamma-encoded — and only applying the transfer function afterwards yields
// linear values. Blending encoded values is the single most common way to get
// compositing subtly wrong.
//
// The scene target is 16-bit float and linear. The present pass writes to an
// _SRGB render target, so the hardware performs the encode back to display
// space on write; that is why it looks like a plain copy.

// Source pixel layouts. A negative layout means the layer has no texture at all
// and draws its solid colour.
#define LAYOUT_SOLID   -1
#define LAYOUT_NV12     0
#define LAYOUT_YUV420P  1
// Already-composited linear RGBA: the blurred copy of a layer, drawn back onto
// the scene. Effects have been applied already, so this path skips them.
#define LAYOUT_TEXTURE  2
// Rasterised sRGB RGBA with premultiplied alpha — a title, or anything else
// drawn rather than decoded. Effects do apply here: it is a source like any
// other, not a finished picture.
#define LAYOUT_RGBA     3

// YUV-to-RGB matrices.
#define SPACE_BT709  0
#define SPACE_BT601  1
#define SPACE_BT2020 2

// Transfer functions, named for what tags them in the file.
#define TRANSFER_BT709     0  // SDR; the sRGB curve stands in for BT.1886
#define TRANSFER_SMPTE2084 1  // PQ, HDR10
#define TRANSFER_ARIB_B67  2  // HLG

// Blend modes. Normal and Add are handled by fixed-function blending and never
// reach the shader's blend path; the rest need to read what is underneath.
#define BLEND_NORMAL     0
#define BLEND_ADD        1
#define BLEND_SCREEN     2
#define BLEND_MULTIPLY   3
#define BLEND_OVERLAY    4
#define BLEND_DARKEN     5
#define BLEND_LIGHTEN    6
#define BLEND_DIFFERENCE 7

// Laid out so no member straddles a 16-byte boundary, because these arrive as
// root constants and HLSL packs a cbuffer in float4 rows.
struct Params {
    float2 canvas;    // canvas size in pixels
    float2 center;    // quad centre in pixels

    float2 size;      // quad width and height in pixels
    float2 rotation;  // cos, sin of the clockwise rotation

    float opacity;
    int layout;
    int colorSpace;
    int transfer;

    int fullRange;
    int blend;
    float2 flip;      // +1 or -1 per axis

    float4 solid;     // linear RGBA, used when layout is LAYOUT_SOLID

    // ---- effects ----
    float brightness; // eq=brightness, a luma offset
    float contrast;   // eq=contrast, about the midpoint
    float saturation; // eq=saturation times hue=s
    float hueRadians; // hue=h

    float invert;     // negate: 0 or 1
    float vignette;   // vignette=a, in radians
    float chromaSimilarity;
    float chromaBlend;

    float4 crop;      // left, top, right, bottom as fractions

    float4 chromaColor;  // rgb of the key colour; w is 1 when keying is on

    float2 blurStep;   // one tap's offset in UV, and the axis it runs along
    float blurSigma;   // in pixels; zero means no blur
    float blurStride;  // pixels between taps, widened when the radius is large

    float4 gradient;     // linear rgb of the far stop; w is 1 when there is one
    float2 gradientDir;  // cos, sin of the gradient angle
    float2 gradientPad;
};

ConstantBuffer<Params> params : register(b0);

// One table of four slots serves every pass, so the bindings are named for
// their position rather than their meaning:
//
//   NV12    t0 = Y,     t1 = UV interleaved
//   planar  t0 = Y,     t1 = U,  t2 = V
//   present t0 = scene
//   t3      = backdrop, the scene as it stood before this layer
//
// Typed views would need a separate declaration per layout at the same
// register, which HLSL does not allow.
Texture2D<float4> texture0 : register(t0);
Texture2D<float4> texture1 : register(t1);
Texture2D<float4> texture2 : register(t2);
Texture2D<float4> backdrop : register(t3);

SamplerState linearSampler : register(s0);

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// ------------------------------------------------------------------ vertex --

// A single oversized triangle covering the viewport. Cheaper than a quad and it
// avoids the diagonal seam two triangles produce under some interpolation.
VSOutput VSFullscreen(uint vertexId : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// A layer's quad, built from the vertex index so there are no vertex buffers to
// manage. Six vertices, two triangles.
VSOutput VSLayer(uint vertexId : SV_VertexID) {
    const float2 corners[6] = {
        float2(-0.5, -0.5), float2( 0.5, -0.5), float2(-0.5,  0.5),
        float2(-0.5,  0.5), float2( 0.5, -0.5), float2( 0.5,  0.5),
    };
    const float2 corner = corners[vertexId];

    // Canvas y runs downwards, so the ordinary rotation matrix already turns
    // clockwise on screen, matching how rotation is stored.
    const float2 local = corner * params.size;
    const float2 rotated = float2(
        local.x * params.rotation.x - local.y * params.rotation.y,
        local.x * params.rotation.y + local.y * params.rotation.x);

    const float2 pixel = params.center + rotated;

    VSOutput output;
    output.position = float4(pixel.x / params.canvas.x * 2.0 - 1.0,
                             1.0 - pixel.y / params.canvas.y * 2.0, 0.0, 1.0);
    // Flipping is a texture-space mirror, so it survives rotation rather than
    // fighting it.
    output.uv = (corner * params.flip) + 0.5;
    return output;
}

// ---------------------------------------------------------------- transfer --

// Inverse of the sRGB/BT.709 encoding: coded value to linear light.
float3 linearizeSrgb(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow(abs(c + 0.055) / 1.055, 2.4));
}

// The forward direction, for the one case that needs it: a solid colour is
// stored linear but effects are defined on coded values.
float3 encodeSrgb(float3 c) {
    c = saturate(c);
    return select(c <= 0.0031308, c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055);
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

// ------------------------------------------------------------------ matrix --

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

// ------------------------------------------------------------------- blend --

float3 blendOverlay(float3 base, float3 src) {
    return select(base <= 0.5, 2.0 * base * src, 1.0 - 2.0 * (1.0 - base) * (1.0 - src));
}

float3 applyBlend(float3 base, float3 src, int mode) {
    if (mode == BLEND_SCREEN)     return 1.0 - (1.0 - base) * (1.0 - src);
    if (mode == BLEND_MULTIPLY)   return base * src;
    if (mode == BLEND_OVERLAY)    return blendOverlay(base, src);
    if (mode == BLEND_DARKEN)     return min(base, src);
    if (mode == BLEND_LIGHTEN)    return max(base, src);
    if (mode == BLEND_DIFFERENCE) return abs(base - src);
    return src;
}

// ----------------------------------------------------------------- effects --
//
// Effects run on *coded* R'G'B' -- after the YUV matrix, before the transfer
// function -- because that is the space FFmpeg's filters are defined in, and
// the spec names those fragments as the authoritative behaviour. Applying a
// 200% contrast to linear light instead would be a large visible difference,
// not a subtle one. Compositing is still linear; only the effect maths is not.

// The BT.601 matrix FFmpeg's eq and hue filters work in, with chroma centred on
// zero rather than 0.5.
float3 rgbToYuv601(float3 rgb) {
    return float3(
         0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b,
        -0.169 * rgb.r - 0.331 * rgb.g + 0.500 * rgb.b,
         0.500 * rgb.r - 0.419 * rgb.g - 0.081 * rgb.b);
}

float3 yuv601ToRgb(float3 yuv) {
    return float3(
        yuv.x + 1.402 * yuv.z,
        yuv.x - 0.344 * yuv.y - 0.714 * yuv.z,
        yuv.x + 1.772 * yuv.y);
}

/// eq (brightness, contrast, saturation) and hue (rotation, saturation), which
/// between them are every colour effect in the registry.
float3 applyColorEffects(float3 coded) {
    float3 yuv = rgbToYuv601(coded);

    // vf_eq: v = (v - 0.5) * contrast + 0.5 + brightness, on luma.
    yuv.x = (yuv.x - 0.5) * params.contrast + 0.5 + params.brightness;

    // vf_hue rotates the chroma plane and scales it; eq's saturation scales it
    // too, and the two are folded into one multiplier before they get here.
    const float c = cos(params.hueRadians);
    const float s = sin(params.hueRadians);
    const float2 chroma = float2(yuv.y * c - yuv.z * s, yuv.y * s + yuv.z * c) * params.saturation;

    float3 rgb = yuv601ToRgb(float3(yuv.x, chroma.x, chroma.y));

    // negate, after everything else, exactly as a trailing filter would run.
    rgb = lerp(rgb, 1.0 - rgb, params.invert);
    return saturate(rgb);
}

/// The chroma keyer: BT.601 chroma distance from the key colour, feathered
/// across `blend`. Deliberately the same approximation the reference used, and
/// like it, not pixel-identical to ffmpeg's chromakey.
float chromaKeyAlpha(float3 coded) {
    if (params.chromaColor.w < 0.5) return 1.0;

    const float3 keyYuv = rgbToYuv601(params.chromaColor.rgb);
    const float3 pixelYuv = rgbToYuv601(coded);

    const float distance = length(pixelYuv.yz - keyYuv.yz);
    if (distance <= params.chromaSimilarity) return 0.0;
    if (params.chromaBlend <= 0.0) return 1.0;
    return saturate((distance - params.chromaSimilarity) / params.chromaBlend);
}

/// vf_vignette: cos of the angle scaled by the normalised distance from the
/// centre, to the fourth power.
float vignetteFactor(float2 uv) {
    if (params.vignette <= 0.0) return 1.0;

    // Normalised so the corner is 1, which is what makes the amount mean the
    // same thing whatever the frame's aspect ratio.
    const float2 offset = (uv - 0.5) * 2.0;
    const float normalised = length(offset) / length(float2(1.0, 1.0));
    const float c = cos(params.vignette * normalised);
    return (c * c) * (c * c);
}

/// Crop cuts fractions off each edge and keeps the frame size, so what is cut
/// becomes transparent rather than shrinking the picture.
bool insideCrop(float2 uv) {
    return uv.x >= params.crop.x && uv.x <= 1.0 - params.crop.z &&
           uv.y >= params.crop.y && uv.y <= 1.0 - params.crop.w;
}

// ------------------------------------------------------------------ layers --

/// The layer's own colour at this pixel, in linear light, before blending.
float4 sourceColor(float2 uv) {
    if (params.layout == LAYOUT_TEXTURE) return texture0.Sample(linearSampler, uv);

    if (!insideCrop(uv)) return float4(0.0, 0.0, 0.0, 0.0);

    if (params.layout == LAYOUT_RGBA) {
        const float4 texel = texture0.Sample(linearSampler, uv);
        // Premultiplied on the way in, because that is what survives bilinear
        // filtering; divided back out here, because every effect below is
        // defined on plain coded values.
        const float3 coded = texel.a > 0.0 ? saturate(texel.rgb / texel.a) : float3(0.0, 0.0, 0.0);

        // The keyer reads the unmodified pixel, as it does for video: a colour
        // correction earlier in the stack must not pull the key colour out from
        // under it.
        const float alpha = texel.a * chromaKeyAlpha(coded);
        const float3 adjusted = applyColorEffects(coded);
        return float4(linearizeSrgb(adjusted) * vignetteFactor(uv), alpha);
    }

    if (params.layout == LAYOUT_SOLID) {
        // A linear gradient runs edge to edge through the quad's centre at the
        // given angle. The half-extent is the rect projected onto the gradient
        // direction, which is what keeps the ramp spanning the whole shape
        // rather than being clipped or leaving flat bands at the corners.
        float3 base = params.solid.rgb;
        if (params.gradient.w > 0.5) {
            const float2 offset = (uv - 0.5) * params.size;
            // Not named `half`: that is a scalar type in HLSL.
            const float extent = abs(params.size.x * 0.5 * params.gradientDir.x) +
                                 abs(params.size.y * 0.5 * params.gradientDir.y);
            const float along = dot(offset, params.gradientDir);
            const float t = extent > 0.0 ? saturate((along + extent) / (2.0 * extent)) : 0.0;

            // Coded, not linear: a gradient between two hex colours is specified
            // the way a canvas draws it, so interpolating in linear light would
            // move the midpoint away from what the author chose.
            base = linearizeSrgb(lerp(encodeSrgb(params.solid.rgb),
                                      encodeSrgb(params.gradient.rgb), t));
        }

        // The solid arrives linear, but effects are defined on coded values, so
        // it makes the same round trip a video frame does -- including the
        // keyer, because an effect belongs to the layer, not to the layer's
        // source. A matte is a degenerate case, but exempting it would be a
        // rule with no reason behind it.
        const float3 coded = encodeSrgb(base);
        const float alpha = params.solid.a * chromaKeyAlpha(coded);
        const float3 adjusted = applyColorEffects(coded);
        return float4(linearizeSrgb(adjusted) * vignetteFactor(uv), alpha);
    }

    float y = texture0.Sample(linearSampler, uv).r;
    float2 chroma;
    if (params.layout == LAYOUT_NV12) {
        chroma = texture1.Sample(linearSampler, uv).rg;
    } else {
        chroma = float2(texture1.Sample(linearSampler, uv).r,
                        texture2.Sample(linearSampler, uv).r);
    }

    // Studio-range video leaves headroom and footroom outside [0,1]; expanding
    // it is what keeps blacks from crushing and highlights from clipping.
    if (params.fullRange == 0) {
        y = (y * 255.0 - 16.0) / 219.0;
        chroma = (chroma * 255.0 - 128.0) / 224.0;
    } else {
        chroma = chroma - 0.5;
    }

    // Chroma reconstruction can push values slightly outside the legal range;
    // clamping before the transfer function keeps pow() away from negatives.
    const float3 coded = saturate(yuvToRgb(float3(y, chroma.x, chroma.y), params.colorSpace));

    // Keying reads the *unmodified* pixel, so a colour correction upstream in
    // the stack cannot pull the key colour out from under the keyer.
    const float alpha = chromaKeyAlpha(coded);
    const float3 adjusted = applyColorEffects(coded);

    return float4(toLinear(adjusted, params.transfer) * vignetteFactor(uv), alpha);
}

// Normal and Add, where fixed-function blending does the combining.
float4 PSLayer(VSOutput input) : SV_Target {
    float4 source = sourceColor(input.uv);
    source.a *= params.opacity;
    return source;
}

// Every other blend mode, which needs the pixel underneath. Fixed-function
// blending is off for this pipeline: the shader produces the final value.
float4 PSLayerBlend(VSOutput input) : SV_Target {
    const float2 screen = input.position.xy / params.canvas;
    const float4 base = backdrop.Sample(linearSampler, screen);
    const float4 source = sourceColor(input.uv);

    const float alpha = source.a * params.opacity;
    const float3 blended = applyBlend(base.rgb, source.rgb, params.blend);
    // Alpha interpolates towards the blended result rather than towards the
    // source, so a half-opaque multiply is half a multiply.
    return float4(lerp(base.rgb, blended, alpha), max(base.a, alpha));
}

// An adjustment layer: everything already composited beneath it, put through
// this layer's effect stack. It draws nothing of its own, which is why it reads
// the backdrop instead of a source, and why opacity means strength here rather
// than transparency.
//
// The backdrop is linear and may exceed 1.0; encoding to coded values clamps it.
// That is correct for the SDR pipeline this ships with and is one of the places
// that will need revisiting when HDR lands.
float4 PSAdjustment(VSOutput input) : SV_Target {
    const float2 screen = input.position.xy / params.canvas;
    const float4 base = backdrop.Sample(linearSampler, screen);

    // Cropping an adjustment layer limits where it reaches, so what falls
    // outside must come through untouched rather than transparent.
    if (!insideCrop(input.uv)) return base;

    const float3 coded = encodeSrgb(base.rgb);
    const float3 adjusted = applyColorEffects(coded);
    const float3 result = linearizeSrgb(adjusted) * vignetteFactor(input.uv);

    return float4(lerp(base.rgb, result, params.opacity), base.a);
}

// One axis of a separable Gaussian. A 2D blur is two of these at right angles,
// which is what makes a large radius affordable: 2n samples rather than n^2.
//
// The tap count is bounded, and the step widens to cover the radius when sigma
// is large. That is an approximation -- above roughly sigma 10 it is sampling a
// Gaussian rather than integrating one -- and it is the reason blur is the one
// registry effect whose maths is not exact. The alternative, hundreds of taps
// per pixel at 4K, is not worth it for an effect used at small radii in
// practice.
#define BLUR_TAPS 24

float4 PSBlur(VSOutput input) : SV_Target {
    if (params.blurSigma <= 0.0) return texture0.Sample(linearSampler, input.uv);

    float4 total = texture0.Sample(linearSampler, input.uv);
    float weightSum = 1.0;

    // Weights come from the true Gaussian at the sampled distance, so widening
    // the step keeps the shape even as it coarsens the sampling.
    const float denominator = 2.0 * params.blurSigma * params.blurSigma;

    [unroll]
    for (int i = 1; i <= BLUR_TAPS; ++i) {
        const float distance = float(i) * params.blurStride;
        const float weight = exp(-(distance * distance) / denominator);

        const float2 offset = params.blurStep * float(i);
        total += texture0.Sample(linearSampler, input.uv + offset) * weight;
        total += texture0.Sample(linearSampler, input.uv - offset) * weight;
        weightSum += 2.0 * weight;
    }

    return total / weightSum;
}

float4 PSPresent(VSOutput input) : SV_Target {
    // Encoded here rather than by an _SRGB render target view.
    //
    // The hardware would do this for nothing, and did, until the display target
    // had to become something Skia can sample. An _SRGB view needs an _SRGB
    // resource, and sampling one decodes back to linear on the way out, so the
    // interface would draw the picture too dark. A typeless resource with two
    // differently-typed views is the usual way out of that, and is not
    // available either: Direct3D refuses to make a shader resource view on a
    // typeless resource, and removes the device rather than failing the call.
    //
    // So the target is plain UNORM holding sRGB-encoded values that any sampler
    // reads back exactly as stored, and the encoding happens here. It costs a
    // handful of instructions once per frame.
    const float4 scene = texture0.Sample(linearSampler, input.uv);

    const float3 low = scene.rgb * 12.92;
    const float3 high = 1.055 * pow(max(scene.rgb, 0.0), 1.0 / 2.4) - 0.055;
    // `select` rather than `?:` — the condition is a vector, and HLSL 2021
    // refuses to short-circuit one.
    const float3 encoded = select(scene.rgb <= 0.0031308, low, high);

    // Alpha is linear by definition and is not encoded.
    return float4(encoded, scene.a);
}
