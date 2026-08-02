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
//
// ------------------------------------------------------------------ passes --
//
// Effects are **passes**, not fields. A layer with any effect on it is drawn
// once into a scratch target in its own space, each effect runs over that
// scratch in the order the stack lists it, and the result is positioned and
// composited. A layer with no effects skips all of it and draws in one go.
//
// The scratch holds **coded** R'G'B' with straight alpha, not linear light,
// because that is the space FFmpeg's filters are defined in and the spec names
// those fragments as the authoritative behaviour. Compositing is still linear;
// only the effect maths is not, and the transfer function is applied once at
// the end, where the scratch becomes a layer again.
//
// A pass carries eight floats and no more, shared by every kind, because only
// one runs at a time. That is what took the ceiling off: an effect no longer
// needs a permanent field in the root constants, so the catalogue can grow and
// a mask can belong to a single effect rather than to the whole stack.

// Source pixel layouts. A negative layout means the layer has no texture at all
// and draws its solid colour.
#define LAYOUT_SOLID   -1
#define LAYOUT_NV12     0
#define LAYOUT_YUV420P  1
// A scratch target holding coded R'G'B' with straight alpha: a layer that has
// been through its effect passes and is on its way back onto the scene.
#define LAYOUT_CODED    2
// Rasterised sRGB RGBA with premultiplied alpha — a title, or anything else
// drawn rather than decoded.
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

// One pass each, matching `render::EffectPassKind` value for value.
#define PASS_COLOR     0
#define PASS_INVERT    1
#define PASS_VIGNETTE  2
#define PASS_CROP      3
#define PASS_CHROMAKEY 4
#define PASS_FLIP      5
#define PASS_BLUR      6

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
    int passKind;
    float passPad;

    float4 solid;     // linear RGBA, used when layout is LAYOUT_SOLID

    float4 gradient;     // linear rgb of the far stop; w is 1 when there is one
    float2 gradientDir;  // cos, sin of the gradient angle
    float2 gradientPad;

    // The pass, whatever it is. Eight floats, meaning what `passKind` says, and
    // written by the makers in `render/effect_passes.hpp` — which is the one
    // place the packing is stated.
    float4 passA;     // values 0..3
    float4 passB;     // values 4..7

    // Where the pass applies. `maskShape` of 0 is everywhere, which costs one
    // comparison and is what almost every pass says.
    float maskShape;      // 0 none, 1 ellipse, 2 rectangle
    float2 maskCenter;
    float maskFeather;

    float2 maskSize;      // half-extents, fractions of the layer
    float2 maskRotation;  // cos, sin

    float maskOpacity;
    float maskInverted;
    float2 maskPad;
};

ConstantBuffer<Params> params : register(b0);

// One table of four slots serves every pass, so the bindings are named for
// their position rather than their meaning:
//
//   NV12    t0 = Y,     t1 = UV interleaved
//   planar  t0 = Y,     t1 = U,  t2 = V
//   effect  t0 = the scratch this pass reads
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
    output.uv = corner + 0.5;
    return output;
}

// ---------------------------------------------------------------- transfer --

// Inverse of the sRGB/BT.709 encoding: coded value to linear light.
float3 linearizeSrgb(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow(abs(c + 0.055) / 1.055, 2.4));
}

// The forward direction, for the cases that need it: a solid colour is stored
// linear but effects are defined on coded values, and so is the backdrop an
// adjustment layer reads.
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

// ------------------------------------------------------------------ source --
//
// A layer's own pixels, decoded to coded R'G'B' with straight alpha and nothing
// else done to them. Effects are passes and run afterwards.

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

/// The solid or gradient a matte draws, as coded values.
float3 solidCoded(float2 uv) {
    float3 base = params.solid.rgb;
    if (params.gradient.w > 0.5) {
        // A linear gradient runs edge to edge through the quad's centre at the
        // given angle. The half-extent is the rect projected onto the gradient
        // direction, which is what keeps the ramp spanning the whole shape
        // rather than being clipped or leaving flat bands at the corners.
        const float2 offset = (uv - 0.5) * params.size;
        // Not named `half`: that is a scalar type in HLSL.
        const float extent = abs(params.size.x * 0.5 * params.gradientDir.x) +
                             abs(params.size.y * 0.5 * params.gradientDir.y);
        const float along = dot(offset, params.gradientDir);
        const float t = extent > 0.0 ? saturate((along + extent) / (2.0 * extent)) : 0.0;

        // Coded, not linear: a gradient between two hex colours is specified the
        // way a canvas draws it, so interpolating in linear light would move the
        // midpoint away from what the author chose.
        return lerp(encodeSrgb(params.solid.rgb), encodeSrgb(params.gradient.rgb), t);
    }
    return encodeSrgb(base);
}

/// The layer's own pixels at this uv: coded R'G'B', straight alpha.
float4 codedSource(float2 uv) {
    if (params.layout == LAYOUT_CODED) return texture0.Sample(linearSampler, uv);

    if (params.layout == LAYOUT_RGBA) {
        const float4 texel = texture0.Sample(linearSampler, uv);
        // Premultiplied on the way in, because that is what survives bilinear
        // filtering; divided back out here, because every pass is defined on
        // plain coded values.
        const float3 coded =
            texel.a > 0.0 ? saturate(texel.rgb / texel.a) : float3(0.0, 0.0, 0.0);
        return float4(coded, texel.a);
    }

    if (params.layout == LAYOUT_SOLID) {
        return float4(solidCoded(uv), params.solid.a);
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
    return float4(saturate(yuvToRgb(float3(y, chroma.x, chroma.y), params.colorSpace)), 1.0);
}

/// The layer in linear light, ready to composite. The transfer function is
/// applied exactly here, once, whether the pixels came straight from a decoder
/// or through a stack of passes.
float4 sourceColor(float2 uv) {
    const float4 coded = codedSource(uv);
    return float4(toLinear(coded.rgb, params.transfer), coded.a);
}

// ----------------------------------------------------------------- effects --
//
// One branch per kind, and nothing else. Each reads the scratch and writes the
// scratch, both coded with straight alpha.

/// eq (brightness, contrast, saturation) and hue (rotation, saturation), which
/// between them are every colour effect in the registry.
float3 colorPass(float3 coded) {
    const float brightness = params.passA.x;
    const float contrast   = params.passA.y;
    const float saturation = params.passA.z;
    const float hueRadians = params.passA.w;

    float3 yuv = rgbToYuv601(coded);

    // vf_eq: v = (v - 0.5) * contrast + 0.5 + brightness, on luma.
    yuv.x = (yuv.x - 0.5) * contrast + 0.5 + brightness;

    // vf_hue rotates the chroma plane and scales it; eq's saturation scales it
    // too, and one effect may do both.
    const float c = cos(hueRadians);
    const float s = sin(hueRadians);
    const float2 chroma = float2(yuv.y * c - yuv.z * s, yuv.y * s + yuv.z * c) * saturation;

    return saturate(yuv601ToRgb(float3(yuv.x, chroma.x, chroma.y)));
}

/// The chroma keyer: BT.601 chroma distance from the key colour, feathered
/// across `blend`. Deliberately the same approximation the reference used, and
/// like it, not pixel-identical to ffmpeg's chromakey.
float chromaKeyAlpha(float3 coded) {
    const float3 keyRgb = params.passA.rgb;
    const float similarity = params.passA.w;
    const float blend = params.passB.x;

    const float3 keyYuv = rgbToYuv601(keyRgb);
    const float3 pixelYuv = rgbToYuv601(coded);

    const float distance = length(pixelYuv.yz - keyYuv.yz);
    if (distance <= similarity) return 0.0;
    if (blend <= 0.0) return 1.0;
    return saturate((distance - similarity) / blend);
}

/// vf_vignette: cos of the angle scaled by the normalised distance from the
/// centre, to the fourth power.
float vignetteFactor(float2 uv) {
    const float angle = params.passA.x;
    if (angle <= 0.0) return 1.0;

    // Normalised so the corner is 1, which is what makes the amount mean the
    // same thing whatever the frame's aspect ratio.
    const float2 offset = (uv - 0.5) * 2.0;
    const float normalised = length(offset) / length(float2(1.0, 1.0));
    const float c = cos(angle * normalised);
    return (c * c) * (c * c);
}

/// Crop cuts fractions off each edge and keeps the frame size, so what is cut
/// becomes transparent rather than shrinking the picture.
bool insideCrop(float2 uv) {
    return uv.x >= params.passA.x && uv.x <= 1.0 - params.passA.z &&
           uv.y >= params.passA.y && uv.y <= 1.0 - params.passA.w;
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

/// A tap, with everything outside the layer treated as empty.
///
/// The scratch is filled edge to edge by the layer, so a sampler left to clamp
/// would smear the last row of pixels outwards for ever and the layer would
/// come back with a hard edge and a bar of stretched colour along it. Nothing
/// is there, so nothing is what a tap outside should find, and the edge becomes
/// the ramp a blur is supposed to make of it.
float4 blurTap(float2 uv) {
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return texture0.Sample(linearSampler, uv);
}

float4 blurPass(float2 uv) {
    const float sigma  = params.passA.x;
    const float stride = params.passA.y;
    const float2 step  = params.passA.zw;
    if (sigma <= 0.0) return texture0.Sample(linearSampler, uv);

    // Premultiplied while filtering, so a transparent neighbour contributes its
    // coverage and not its colour. Blurring straight alpha pulls whatever
    // happens to be stored in the transparent texels into the visible ones,
    // which reads as a dark fringe round everything.
    float4 centre = blurTap(uv);
    float4 total = float4(centre.rgb * centre.a, centre.a);
    float weightSum = 1.0;

    // Weights come from the true Gaussian at the sampled distance, so widening
    // the step keeps the shape even as it coarsens the sampling.
    const float denominator = 2.0 * sigma * sigma;

    [unroll]
    for (int i = 1; i <= BLUR_TAPS; ++i) {
        const float distance = float(i) * stride;
        const float weight = exp(-(distance * distance) / denominator);
        const float2 offset = step * float(i);

        const float4 a = blurTap(uv + offset);
        const float4 b = blurTap(uv - offset);
        total += float4(a.rgb * a.a, a.a) * weight;
        total += float4(b.rgb * b.a, b.a) * weight;
        weightSum += 2.0 * weight;
    }

    total /= weightSum;
    return float4(total.a > 0.0 ? total.rgb / total.a : float3(0.0, 0.0, 0.0), total.a);
}

// -------------------------------------------------------------------- mask --

/// How much of this pixel the pass applies to: 1 fully, 0 not at all.
///
/// One function for every kind of pass, because a mask has nothing to do with
/// what the effect *is*. That is the whole reason it lives here rather than
/// inside each branch, and it is why adding an effect gets masking for nothing.
float maskCoverage(float2 uv) {
    if (params.maskShape < 0.5) return 1.0;

    // Into the mask's own frame: offset from its centre, turned by its rotation.
    // The inverse turn, because the point is being brought into the shape rather
    // than the shape being drawn.
    const float2 offset = uv - params.maskCenter;
    const float2 local = float2(
         offset.x * params.maskRotation.x + offset.y * params.maskRotation.y,
        -offset.x * params.maskRotation.y + offset.y * params.maskRotation.x);

    const float2 extent = max(params.maskSize, 1e-6);
    const float feather = max(params.maskFeather, 0.0);

    // Distance from the edge in the same units for both shapes: 0 on the edge,
    // negative inside, positive outside, scaled so a feather means the same
    // fraction of the layer whichever shape it is softening.
    float distance;
    if (params.maskShape < 1.5) {
        // An ellipse: the normalised radius, brought back into layer units by
        // the smaller half-extent so a very flat ellipse does not feather far
        // more along one axis than the other.
        const float2 scaled = local / extent;
        const float radius = length(scaled);
        distance = (radius - 1.0) * min(extent.x, extent.y);
    } else {
        // A rectangle: the usual signed distance, which is exact outside and a
        // good enough approximation within the corner radius a feather covers.
        const float2 away = abs(local) - extent;
        distance = length(max(away, 0.0)) + min(max(away.x, away.y), 0.0);
    }

    // Softened across the feather, centred on the edge, so half the softness
    // falls inside the shape and half outside — which is where the eye expects
    // the edge of a feathered mask to be.
    float inside;
    if (feather <= 0.0) {
        inside = distance <= 0.0 ? 1.0 : 0.0;
    } else {
        inside = saturate(0.5 - distance / feather);
        inside = inside * inside * (3.0 - 2.0 * inside);  // smoothstep
    }

    if (params.maskInverted > 0.5) inside = 1.0 - inside;
    return inside * params.maskOpacity;
}

/// One effect over the scratch. Coded in, coded out.
float4 PSEffect(VSOutput input) : SV_Target {
    const float4 before = texture0.Sample(linearSampler, input.uv);

    float4 after;
    if (params.passKind == PASS_BLUR) {
        after = blurPass(input.uv);
    } else if (params.passKind == PASS_FLIP) {
        // A mirror in texture space, which is why it survives the rotation the
        // composite applies afterwards rather than fighting it.
        const float2 mirrored = (input.uv - 0.5) * params.passA.xy + 0.5;
        after = texture0.Sample(linearSampler, mirrored);
    } else {
        after = before;
        if (params.passKind == PASS_COLOR) {
            after.rgb = colorPass(after.rgb);
        } else if (params.passKind == PASS_INVERT) {
            after.rgb = saturate(1.0 - after.rgb);
        } else if (params.passKind == PASS_VIGNETTE) {
            // In linear light and back, because a vignette is a light falloff.
            // Done on coded values it would darken the midtones far more than
            // the ends, which is not what the filter it is named after does.
            const float3 linearRgb = linearizeSrgb(after.rgb) * vignetteFactor(input.uv);
            after.rgb = encodeSrgb(linearRgb);
        } else if (params.passKind == PASS_CROP) {
            if (!insideCrop(input.uv)) after.a = 0.0;
        } else if (params.passKind == PASS_CHROMAKEY) {
            after.a *= chromaKeyAlpha(after.rgb);
        }
    }

    // The mask, the same way for every kind: what the effect did, mixed with
    // what was there, by how much of this pixel the mask covers. An effect
    // never has to know it is masked, which is what makes masking something the
    // whole catalogue gets rather than something each entry implements.
    const float coverage = maskCoverage(input.uv);
    if (coverage >= 1.0) return after;
    return lerp(before, after, coverage);
}

// ------------------------------------------------------------------ layers --

/// A layer drawn into a scratch target, in its own space, before any effect.
///
/// Its own space rather than the canvas: the scratch is filled edge to edge by
/// the layer, so every pass gets uv running 0..1 across the picture. That is
/// what makes crop and vignette mean what they say however the layer is
/// positioned, and it moves the rotation and the scaling to the very end, where
/// they cost one filtering step instead of one per pass.
float4 PSSource(VSOutput input) : SV_Target {
    return codedSource(input.uv);
}

/// The same, for an adjustment layer, whose source is whatever is beneath it.
///
/// The backdrop is in canvas space and the scratch is in layer space, so this
/// walks the quad's transform forwards for each scratch texel: exactly what
/// `VSLayer` does to a corner, done per pixel.
float4 PSAdjustSource(VSOutput input) : SV_Target {
    const float2 local = (input.uv - 0.5) * params.size;
    const float2 rotated = float2(
        local.x * params.rotation.x - local.y * params.rotation.y,
        local.x * params.rotation.y + local.y * params.rotation.x);
    const float2 screen = (params.center + rotated) / params.canvas;

    // Linear and possibly above 1.0; encoding clamps it. That is correct for
    // the SDR pipeline this ships with and is one of the places that will need
    // revisiting when HDR lands.
    const float4 base = backdrop.Sample(linearSampler, screen);
    return float4(encodeSrgb(base.rgb), 1.0);
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
// this layer's effect stack. It draws nothing of its own, which is why opacity
// means strength here rather than transparency.
//
// The passes have already run into the scratch; what is left is to put the
// result back over the backdrop. Where a pass made the scratch transparent — a
// crop, a key — the backdrop comes through untouched, which is what limiting an
// adjustment's reach has to mean.
float4 PSAdjustment(VSOutput input) : SV_Target {
    const float2 screen = input.position.xy / params.canvas;
    const float4 base = backdrop.Sample(linearSampler, screen);

    const float4 coded = texture0.Sample(linearSampler, input.uv);
    const float3 result = linearizeSrgb(coded.rgb);

    return float4(lerp(base.rgb, result, params.opacity * coded.a), base.a);
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
