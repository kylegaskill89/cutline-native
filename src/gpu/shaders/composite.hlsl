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
#define TRANSFER_SCRGB     3  // linear, 1.0 at 80 nits: an HDR nest's picture

// A sequence's working colour space, numbered so that each one's transfer is
// the TRANSFER_ of the same number: a source already in it needs no work.
#define WORKING_REC709 0  // sRGB curve, Rec.709 primaries
#define WORKING_PQ     1  // ST 2084, Rec.2020 primaries
#define WORKING_HLG    2  // hybrid log-gamma, Rec.2020 primaries

#define PRIMARIES_709  0
#define PRIMARIES_2020 1

// A present into scRGB for an HDR display: linear, Rec.709 primaries (and
// beyond them, as negative values), 1.0 at 80 nits. `passA.x` says how many of
// those a unit of the scene is.
#define OUTPUT_SCRGB 3

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
#define PASS_LEVELS    7
#define PASS_BALANCE   8
#define PASS_TINT      9
#define PASS_SHARPEN   10
#define PASS_POSTERIZE 11
#define PASS_THRESHOLD 12
#define PASS_DIRBLUR   13
#define PASS_RADIAL    14
#define PASS_DISTORT   15
#define PASS_NOISE     16
#define PASS_KEEP      17
#define PASS_SHADOW    18
#define PASS_UNDER     19
#define PASS_MATTE     20
#define PASS_LUMETRI   21
#define PASS_SECONDARY 22
#define PASS_LVIGNETTE 23
#define PASS_ULTRAKEY  24
#define PASS_CHOKE     25
#define PASS_SOFTEN    26
#define PASS_ULTRAOUT  27
#define PASS_TRANSFORM 28
#define PASS_WARP      29

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
    float codeScale;  // sampled value to integer code

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

    // How much of the scratch is empty border, per side, as a fraction of it.
    //
    // A blur has to be able to spread *past* the layer, the way Premiere's
    // does, and it can only spread into pixels that exist. So a layer whose
    // stack contains a spreading pass is drawn smaller, into the middle of the
    // scratch, and the composite grows the quad back by the same amount — the
    // picture lands exactly where it would have, with room around it for the
    // blur to reach into. Zero when nothing in the stack spreads, which is
    // almost every stack, and costs the layer nothing.
    float margin;

    // A free-drawn path's corners: where its run starts in the shared point
    // buffer, and how many. A path is the one mask too big for the root
    // constants, which is the whole reason that buffer exists.
    float pathFirst;
    float pathCount;

    // How much the source is softened vertically as it is read, 0 to 1.
    float antiFlicker;
    float bits;       // bits in a code: 8, 10, 12
    int sourcePrimaries;  // PRIMARIES_ of the source

    // Colour management: the sequence's working space, its graphics white in
    // nits, whether HDR sources are tone mapped into an SDR one, and the space
    // a present or an encode is writing out to.
    int working;
    float whiteNits;
    float toneMap;
    int output;

    // This pass's run in the shared pass data, for passes with more to say
    // than eight values: a grade's settings, curves and LUTs.
    float dataFirst;
    float dataCount;
    float2 dataPad;
};


ConstantBuffer<Params> params : register(b0);
/// Scratch coordinates to the layer's own, which is what every pass that means
/// something *geometric* works in: a crop, a vignette, a mask.
///
/// Sampling still uses the scratch's own uv. The two are the same thing without
/// a margin, and the moment there is one they are not.
float2 layerUv(float2 uv) {
    const float span = 1.0 - 2.0 * params.margin;
    return span > 0.0 ? (uv - params.margin) / span : uv;
}

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

/// The corners of every free-drawn mask in the frame, one after another, each in
/// fractions of its layer and measured *from the mask's own centre* — so moving
/// the mask moves the path, and turning it turns the path, through exactly the
/// transform the other two shapes already go through.
StructuredBuffer<float2> maskPath : register(t4);

/// Every pass's own data in the frame, one run after another; a pass reads from
/// `dataFirst` on.
StructuredBuffer<float> passData : register(t5);

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

    // Grown by the margin, so the empty border the scratch was given is drawn
    // rather than cropped away — that border is where a blur has spread to. The
    // layer's own pixels land exactly where they would have without one, since
    // the growth is about the centre and the picture inside the scratch shrank
    // by the same factor.
    const float span = 1.0 - 2.0 * params.margin;
    const float grown = span > 0.0 ? 1.0 / span : 1.0;

    // Canvas y runs downwards, so the ordinary rotation matrix already turns
    // clockwise on screen, matching how rotation is stored.
    const float2 local = corner * params.size * grown;
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

// ---------------------------------------------------------------- colour --
//
// The scene is linear light in the sequence's primaries, with 1.0 meaning the
// sequence's graphics white: SDR white in a Rec.709 sequence, and 203 nits by
// default in an HDR one, which is where BT.2408 puts SDR white inside HDR. An
// HDR sequence holds values above 1.0, up to 10,000 nits' worth.
//
// Every source is brought into the sequence's own coded space as it is read,
// so the effects on a layer all work on the same kind of numbers whatever the
// footage was: SDR in an HDR sequence is mapped up, and HDR in an SDR sequence
// is tone mapped down, both before anything else touches it.

// HLG is scene light; what a display shows is that through the system gamma,
// 1.2 at the nominal 1,000-nit peak. 75% signal lands at about 203 nits.
static const float kHlgPeakNits = 1000.0;
// What tone mapping takes to SDR white: the peak nearly every HDR grade is
// mastered to.
static const float kHdrPeakNits = 1000.0;
static const float3 kLuma2020 = float3(0.2627, 0.6780, 0.0593);
static const float3 kLuma709 = float3(0.2126, 0.7152, 0.0722);

// The forward ST 2084 curve, from light normalised so 1.0 is 10,000 nits.
float3 encodePq(float3 y) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 128.0 * 2523.0 / 4096.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 32.0 * 2413.0 / 4096.0;
    const float c3 = 32.0 * 2392.0 / 4096.0;
    const float3 p = pow(max(y, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// The HLG OETF, from scene light over the unit range.
float3 encodeHlg(float3 e) {
    const float a = 0.17883277;
    const float b = 1.0 - 4.0 * a;
    const float c = 0.5 - a * log(4.0 * a);
    e = max(e, 0.0);
    return select(e <= 1.0 / 12.0, sqrt(3.0 * e), a * log(max(12.0 * e - b, 1e-6)) + c);
}

float3 hlgSceneToNits(float3 scene) {
    const float ys = dot(scene, kLuma2020);
    return kHlgPeakNits * pow(max(ys, 1e-6), 0.2) * scene;
}

float3 nitsToHlgScene(float3 nits) {
    const float yd = dot(nits, kLuma2020);
    const float ys = pow(max(yd / kHlgPeakNits, 1e-6), 1.0 / 1.2);
    return nits / (kHlgPeakNits * pow(ys, 0.2));
}

// Coded to linear light relative to graphics white, in whatever primaries the
// values were coded in.
float3 toRelative(float3 coded, int transfer) {
    if (transfer == TRANSFER_SCRGB) return coded * (80.0 / params.whiteNits);
    if (transfer == TRANSFER_SMPTE2084) return linearizePq(coded) * (10000.0 / params.whiteNits);
    if (transfer == TRANSFER_ARIB_B67) return hlgSceneToNits(linearizeHlg(coded)) / params.whiteNits;
    return linearizeSrgb(coded);
}

// And back, for a space's own curve.
float3 fromRelative(float3 relative, int space) {
    if (space == WORKING_PQ) return encodePq(relative * (params.whiteNits / 10000.0));
    if (space == WORKING_HLG) return saturate(encodeHlg(nitsToHlgScene(max(relative, 0.0) * params.whiteNits)));
    return encodeSrgb(relative);
}

int primariesOf(int space) {
    return space == WORKING_REC709 ? PRIMARIES_709 : PRIMARIES_2020;
}

float3 convertPrimaries(float3 rgb, int from, int to) {
    if (from == to) return rgb;
    if (to == PRIMARIES_2020) {
        return float3(dot(rgb, float3(0.6274, 0.3293, 0.0433)),
                      dot(rgb, float3(0.0691, 0.9195, 0.0114)),
                      dot(rgb, float3(0.0164, 0.0880, 0.8956)));
    }
    return float3(dot(rgb, float3(1.6605, -0.5876, -0.0728)),
                  dot(rgb, float3(-0.1246, 1.1329, -0.0083)),
                  dot(rgb, float3(-0.0182, -0.1006, 1.1187)));
}

// HDR brought into SDR, on luminance so a colour keeps its hue. Up to three
// quarters of SDR white nothing moves; above that an extended Reinhard
// shoulder takes the HDR peak to exactly white, leaving the knee with the same
// slope it arrived with so there is no visible bend.
float3 toneMapToSdr(float3 rgb) {
    rgb = max(rgb, 0.0);
    const float y = dot(rgb, kLuma709);
    const float knee = 0.75;
    if (y > knee) {
        const float room = 1.0 - knee;
        const float u = (y - knee) / room;
        const float most = max((kHdrPeakNits / params.whiteNits - knee) / room, 1e-3);
        const float mapped = knee + room * (u * (1.0 + u / (most * most)) / (1.0 + u));
        rgb *= mapped / y;
    }
    return min(rgb, 1.0);
}

// Linear light relative to graphics white, in `primaries`, into a space's
// coded values: what every source goes through on its way in, and the scene
// on its way out to a display or a file.
float3 intoSpace(float3 relative, int primaries, bool fromHdr, int space) {
    relative = convertPrimaries(relative, primaries, primariesOf(space));
    if (space == WORKING_REC709 && fromHdr && params.toneMap > 0.5) {
        relative = toneMapToSdr(relative);
    }
    return fromRelative(relative, space);
}

// A source's coded values in the sequence's coded space. Nothing to do, and
// nothing done, when the source is already in it, which is SDR footage in an
// SDR sequence: exactly what every frame was before there was colour
// management.
float3 toWorkingCoded(float3 coded) {
    if (params.transfer == params.working &&
        params.sourcePrimaries == primariesOf(params.working)) {
        return coded;
    }
    return intoSpace(toRelative(coded, params.transfer), params.sourcePrimaries,
                     params.transfer != TRANSFER_BT709, params.working);
}

// The sequence's coded values to linear light, and back.
float3 workingLinear(float3 coded) { return toRelative(coded, params.working); }
float3 workingCoded(float3 relative) { return fromRelative(relative, params.working); }

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

/// The layer's own pixels at this uv: coded R'G'B', straight alpha, in the
/// source's own space.
float4 rawCodedSource(float2 uv) {
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
    // it is what keeps blacks from crushing and highlights from clipping. In
    // codes, because studio range is 16 to 235 at eight bits and 64 to 940 at
    // ten, and a sixteen-bit texture holds those wherever its layout puts them.
    y *= params.codeScale;
    chroma *= params.codeScale;
    const float unit = exp2(params.bits - 8.0);
    if (params.fullRange == 0) {
        y = (y - 16.0 * unit) / (219.0 * unit);
        chroma = (chroma - 128.0 * unit) / (224.0 * unit);
    } else {
        const float top = exp2(params.bits) - 1.0;
        y = y / top;
        chroma = chroma / top - 0.5;
    }

    // Chroma reconstruction can push values slightly outside the legal range;
    // clamping before the transfer function keeps pow() away from negatives.
    return float4(saturate(yuvToRgb(float3(y, chroma.x, chroma.y), params.colorSpace)), 1.0);
}

/// The layer's own pixels at this uv in the sequence's coded space: the one
/// place a source changes space, before any effect sees it.
float4 codedSource(float2 uv) {
    const float4 raw = rawCodedSource(uv);
    return float4(toWorkingCoded(raw.rgb), raw.a);
}

/// The source, softened vertically by the anti-flicker filter when one is set.
///
/// The filter belongs here, where the source is *read*, rather than in the pass
/// chain: a still full of one-pixel detail shimmers because which source rows
/// survive the resampling changes from frame to frame, and the only place to
/// prevent that is where the rows are read. An effect that ran afterwards would
/// be softening an image that had already lost the rows.
///
/// A scratch target is exempt. It has already been read through this once, and
/// applying it again on the way out would soften every layer with an effect on
/// it twice.
float4 softenedSource(float2 uv) {
    if (params.antiFlicker <= 0.0 || params.layout == LAYOUT_CODED) return codedSource(uv);

    // One row of the canvas, which is the spacing the resampler is skipping.
    const float2 up = float2(0.0, params.antiFlicker / max(params.canvas.y, 1.0));

    // Three taps, a quarter and a half and a quarter: the smallest filter that
    // is symmetric and sums to one, which is what keeps a flat area exactly as
    // bright as it was.
    return codedSource(uv) * 0.5 + codedSource(saturate(uv + up)) * 0.25 +
           codedSource(saturate(uv - up)) * 0.25;
}

/// The layer in linear light, ready to composite. The transfer function is
/// applied exactly here, once, whether the pixels came straight from a decoder
/// or through a stack of passes.
float4 sourceColor(float2 uv) {
    const float4 coded = softenedSource(uv);
    return float4(workingLinear(coded.rgb), coded.a);
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

/// A blur along the rays out of a point: outward for a zoom, round it for a
/// spin.
///
/// Taps walk from the pixel towards the centre (or round it) rather than in a
/// fixed direction, so the streak is longer the further out it is — which is
/// what makes it read as speed rather than as a smear.
float4 radialBlurPass(float2 uv) {
    const float amount = params.passA.x;
    const float2 centre = params.passA.yz;
    const bool spin = params.passA.w >= 0.5;
    if (amount <= 0.0) return texture0.Sample(linearSampler, uv);

    // In the layer's own coordinates, so the centre means what the panel says
    // whatever margin the scratch was given.
    const float2 here = layerUv(uv);
    const float2 offset = here - centre;

    float4 total = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;

    [unroll]
    for (int i = 0; i <= BLUR_TAPS; ++i) {
        const float step = float(i) / float(BLUR_TAPS) * amount;

        float2 sampled;
        if (spin) {
            // Round the centre: the same radius, a little further along.
            const float angle = step;
            sampled = centre + float2(offset.x * cos(angle) - offset.y * sin(angle),
                                      offset.x * sin(angle) + offset.y * cos(angle));
        } else {
            sampled = centre + offset * (1.0 - step);
        }

        // Back into the scratch, and dropped when it falls off the layer, so
        // the streak fades out at the edge instead of dragging a clamped pixel.
        const float span = 1.0 - 2.0 * params.margin;
        const float2 scratch = sampled * span + params.margin;
        if (sampled.x < 0.0 || sampled.x > 1.0 || sampled.y < 0.0 || sampled.y > 1.0) continue;

        const float4 texel = texture0.Sample(linearSampler, scratch);
        total += float4(texel.rgb * texel.a, texel.a);
        weightSum += 1.0;
    }

    if (weightSum <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    total /= weightSum;
    return float4(total.a > 0.0 ? total.rgb / total.a : float3(0.0, 0.0, 0.0), total.a);
}

/// A lens. Positive barrels the picture outward, negative pinches it inward,
/// and the scale is what puts the corners back inside the frame afterwards.
float4 distortPass(float2 uv) {
    const float amount = params.passA.x;
    const float scale = params.passA.y;

    // Centred and squared: the displacement grows with the square of the
    // distance from the middle, which is what a real lens does and what makes
    // the middle of the picture stay still.
    float2 here = (layerUv(uv) - 0.5) * 2.0;
    const float r2 = dot(here, here);
    here *= (1.0 + amount * r2) / max(scale, 0.01);

    const float2 layer = here * 0.5 + 0.5;
    if (layer.x < 0.0 || layer.x > 1.0 || layer.y < 0.0 || layer.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float span = 1.0 - 2.0 * params.margin;
    return texture0.Sample(linearSampler, layer * span + params.margin);
}

/// A value in 0..1 that looks random and is not: the same inputs give the same
/// number every time, which is what makes a frame reproducible.
float hashNoise(float2 at, float seed) {
    const float3 sown = float3(at, seed);
    return frac(sin(dot(sown, float3(12.9898, 78.233, 37.719))) * 43758.5453);
}

/// Film grain, added in coded values because that is where grain lives — it is
/// a property of the stock rather than of the light.
float4 noisePass(float2 uv, float4 source) {
    const float amount = params.passA.x;
    const float seed = params.passA.y;
    const bool monochrome = params.passA.z >= 0.5;
    if (amount <= 0.0) return source;

    // Quantised to the scratch's own pixels, so the grain is grain rather than
    // a smooth wobble, and it does not crawl when the layer is scaled.
    const float2 at = floor(uv * float2(2048.0, 2048.0));

    float3 grain;
    if (monochrome) {
        grain = (hashNoise(at, seed) - 0.5).xxx;
    } else {
        grain = float3(hashNoise(at, seed) - 0.5, hashNoise(at, seed + 11.0) - 0.5,
                       hashNoise(at, seed + 23.0) - 0.5);
    }

    // Nothing where there is nothing: grain on a transparent corner would be a
    // rectangle of dirt around a layer that has been cropped or masked.
    return float4(saturate(source.rgb + grain * amount), source.a);
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
    if (params.maskShape > 2.5) {
        // A path: the one shape whose description does not fit in the root
        // constants, which is why the point buffer exists at all.
        //
        // Even-odd for what is inside — the rule that makes a shape drawn back
        // over itself cut a hole rather than fill one — and the distance to the
        // nearest edge for the rest, which is the same signed number the other
        // two produce and so feathers and inverts identically.
        const uint count = (uint)max(params.pathCount, 0.0);
        const uint first = (uint)max(params.pathFirst, 0.0);
        // Fewer than three corners encloses nothing, so nothing is covered.
        //
        // This used to answer 1.0 — everything — on the grounds that a broken
        // mask should not hide the effect. There is now a way to be *drawing*
        // one, and during those first two clicks "everything" means the whole
        // frame flashing the effect until the third point lands. Nothing is the
        // better reading of a shape that encloses nothing, and no saved path
        // can reach here: one is seeded with four corners and removing them
        // stops at three.
        if (count < 3u) return 0.0;

        bool inside = false;
        float nearest = 1e9;
        float2 previous = maskPath[first + count - 1u];

        for (uint i = 0u; i < count; ++i) {
            const float2 corner = maskPath[first + i];

            // A ray to the left: an edge that straddles this row and crosses to
            // the left of the point flips the answer.
            if ((corner.y > local.y) != (previous.y > local.y)) {
                const float t = (local.y - corner.y) / (previous.y - corner.y);
                if (local.x < corner.x + t * (previous.x - corner.x)) inside = !inside;
            }

            const float2 edge = previous - corner;
            const float along = saturate(dot(local - corner, edge) / max(dot(edge, edge), 1e-12));
            nearest = min(nearest, length(local - (corner + edge * along)));
            previous = corner;
        }

        distance = inside ? -nearest : nearest;
    } else if (params.maskShape < 1.5) {
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

// ------------------------------------------------------------- the newer few --
//
// Every one of these is a branch here and an entry in the catalogue, and not
// one of them needed a field of its own in the root constants. That is what
// passes bought.

/// Black point, white point, gamma and exposure.
///
/// The points and the gamma work on coded values, where a black point is a
/// coded level and a gamma is the curve everybody means by one. Exposure does
/// not: a stop is a doubling of *light*, so it multiplies in linear and comes
/// back, which is the difference between an exposure and a brightness.
float3 levelsPass(float3 coded) {
    const float black    = params.passA.x;
    const float white    = params.passA.y;
    const float gamma    = params.passA.z;
    const float exposure = params.passA.w;

    float3 c = saturate((coded - black) / max(white - black, 1e-4));
    c = pow(max(c, 0.0), 1.0 / max(gamma, 1e-3));

    if (exposure != 1.0) {
        c = workingCoded(workingLinear(c) * exposure);
    }
    return saturate(c);
}

/// A gain per channel, in linear light, because a colour cast is a cast on the
/// light rather than on the numbers it was written down as.
float3 balancePass(float3 coded) {
    const float3 gain = params.passA.rgb;
    return saturate(workingCoded(workingLinear(coded) * gain));
}

/// Shadows and highlights mapped to two colours, by luma.
float3 tintPass(float3 coded) {
    const float3 shadow = params.passA.rgb;
    const float amount = params.passA.w;
    const float3 highlight = params.passB.rgb;

    const float luma = rgbToYuv601(coded).x;
    return saturate(lerp(coded, lerp(shadow, highlight, saturate(luma)), amount));
}

/// An unsharp mask: the picture minus a blurred copy of it, added back.
///
/// Four taps rather than a full Gaussian. Sharpening is a local operation by
/// definition — the radius is a pixel or two — so a wide kernel would cost
/// noticeably more to produce the same edge.
float3 sharpenPass(float2 uv) {
    const float amount = params.passA.x;
    const float2 step = params.passA.zw;

    const float3 centre = texture0.Sample(linearSampler, uv).rgb;
    float3 around = texture0.Sample(linearSampler, uv + float2(step.x, 0.0)).rgb;
    around += texture0.Sample(linearSampler, uv - float2(step.x, 0.0)).rgb;
    around += texture0.Sample(linearSampler, uv + float2(0.0, step.y)).rgb;
    around += texture0.Sample(linearSampler, uv - float2(0.0, step.y)).rgb;
    around *= 0.25;

    return saturate(centre + (centre - around) * amount);
}

/// One effect over the scratch. Coded in, coded out.
/// How much of a layer a track matte lets through at a point in the layer's
/// scratch: the matte track's picture, sampled where this point of the layer
/// lands on the frame -- the same walk `PSAdjustSource` makes.
float matteFactor(float2 uv) {
    const float2 local = (layerUv(uv) - 0.5) * params.size;
    const float2 rotated = float2(
        local.x * params.rotation.x - local.y * params.rotation.y,
        local.x * params.rotation.y + local.y * params.rotation.x);
    const float2 screen = (params.center + rotated) / params.canvas;
    float factor = 0.0;
    if (all(screen >= 0.0) && all(screen <= 1.0)) {
        const float4 matte = texture1.Sample(linearSampler, screen);
        factor = params.passA.x > 0.5 ? dot(matte.rgb, float3(0.2126, 0.7152, 0.0722)) * matte.a
                                      : matte.a;
    }
    return params.passA.y > 0.5 ? 1.0 - factor : factor;
}

// ----------------------------------------------------------------- lumetri --
//
// Lumetri Color. Its settings, curves and LUTs are the pass's data, laid out as
// `render/lumetri.hpp` says, and every section grades in one space: the
// sequence's own coded values in Rec.709, and in HDR the light scaled so
// graphics white is 1.0 and coded with the same curve, carrying on above 1 on
// a log scale. That is what lets a slider mean the same thing in either.

static const float kGradingLog = 0.2;
static const float kLutHeader = 8.0;

float pd(float at) { return passData[(uint)(params.dataFirst + at)]; }
float3 pd3(float at) { return float3(pd(at), pd(at + 1.0), pd(at + 2.0)); }

float gradingLuma(float3 rgb) {
    return dot(rgb, params.working == WORKING_REC709 ? kLuma709 : kLuma2020);
}

float3 gradingEncode(float3 x) {
    const float3 low = encodeSrgb(saturate(x));
    const float3 high = 1.0 + log2(max(x, 1.0)) * kGradingLog;
    return lerp(low, high, step(1.0, x));
}

float3 gradingDecode(float3 g) {
    const float3 low = linearizeSrgb(saturate(g));
    const float3 high = exp2((max(g, 1.0) - 1.0) / kGradingLog);
    return lerp(low, high, step(1.0, g));
}

float3 toGrading(float3 coded) {
    return params.working == WORKING_REC709 ? coded : gradingEncode(workingLinear(coded));
}

float3 fromGrading(float3 g) {
    return params.working == WORKING_REC709 ? max(g, 0.0) : workingCoded(gradingDecode(max(g, 0.0)));
}

float3 rgbToHsv(float3 c) {
    const float4 k = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    const float4 p = c.g < c.b ? float4(c.bg, k.wz) : float4(c.gb, k.xy);
    const float4 q = c.r < p.x ? float4(p.xyw, c.r) : float4(c.r, p.yzx);
    const float d = q.x - min(q.w, q.y);
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + 1e-10)), d / (q.x + 1e-10), q.x);
}

float3 hsvToRgb(float3 c) {
    const float3 p = abs(frac(c.xxx + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
    return c.z * lerp(float3(1.0, 1.0, 1.0), saturate(p - 1.0), c.y);
}

/// An S about the middle that keeps black and white where they are: steeper
/// through the middle for an exponent above one, flatter below. Past the ends
/// it carries on in a straight line rather than clipping.
float3 sCurve(float3 x, float k) {
    const float3 lowHalf = 0.5 * pow(saturate(2.0 * x), k);
    const float3 highHalf = 1.0 - 0.5 * pow(saturate(2.0 - 2.0 * x), k);
    return lerp(lowHalf, highHalf, step(0.5, x)) + (max(x - 1.0, 0.0) + min(x, 0.0)) * k;
}

float3 lutEntry(float base, int3 i, int size) {
    return pd3(base + kLutHeader + 3.0 * (float)(i.x + size * (i.y + size * i.z)));
}

/// A LUT from the data, at `base`. A cube is read with tetrahedral
/// interpolation, which is what grading applications use: smoother along the
/// grey axis than trilinear, and four reads rather than eight.
float3 applyLut(float base, float3 c) {
    const int size = (int)pd(base);
    const float3 low = pd3(base + 2.0);
    const float3 high = pd3(base + 5.0);
    const float3 x = saturate((c - low) / (high - low)) * (float)(size - 1);
    if (pd(base + 1.0) < 0.5) {
        float3 out1d;
        [unroll] for (int ch = 0; ch < 3; ++ch) {
            const int i0 = min((int)floor(x[ch]), size - 2);
            const float a = pd(base + kLutHeader + 3.0 * (float)i0 + (float)ch);
            const float b = pd(base + kLutHeader + 3.0 * (float)(i0 + 1) + (float)ch);
            out1d[ch] = lerp(a, b, x[ch] - (float)i0);
        }
        return out1d;
    }
    const int3 i0 = min((int3)floor(x), int3(size - 2, size - 2, size - 2));
    const float3 t = x - (float3)i0;
    const float3 c000 = lutEntry(base, i0, size);
    const float3 c111 = lutEntry(base, i0 + int3(1, 1, 1), size);
    if (t.x >= t.y) {
        if (t.y >= t.z) {
            const float3 c100 = lutEntry(base, i0 + int3(1, 0, 0), size);
            const float3 c110 = lutEntry(base, i0 + int3(1, 1, 0), size);
            return c000 + t.x * (c100 - c000) + t.y * (c110 - c100) + t.z * (c111 - c110);
        }
        if (t.x >= t.z) {
            const float3 c100 = lutEntry(base, i0 + int3(1, 0, 0), size);
            const float3 c101 = lutEntry(base, i0 + int3(1, 0, 1), size);
            return c000 + t.x * (c100 - c000) + t.z * (c101 - c100) + t.y * (c111 - c101);
        }
        const float3 c001 = lutEntry(base, i0 + int3(0, 0, 1), size);
        const float3 c101 = lutEntry(base, i0 + int3(1, 0, 1), size);
        return c000 + t.z * (c001 - c000) + t.x * (c101 - c001) + t.y * (c111 - c101);
    }
    if (t.z >= t.y) {
        const float3 c001 = lutEntry(base, i0 + int3(0, 0, 1), size);
        const float3 c011 = lutEntry(base, i0 + int3(0, 1, 1), size);
        return c000 + t.z * (c001 - c000) + t.y * (c011 - c001) + t.x * (c111 - c011);
    }
    if (t.z >= t.x) {
        const float3 c010 = lutEntry(base, i0 + int3(0, 1, 0), size);
        const float3 c011 = lutEntry(base, i0 + int3(0, 1, 1), size);
        return c000 + t.y * (c010 - c000) + t.z * (c011 - c010) + t.x * (c111 - c011);
    }
    const float3 c010 = lutEntry(base, i0 + int3(0, 1, 0), size);
    const float3 c110 = lutEntry(base, i0 + int3(1, 1, 0), size);
    return c000 + t.y * (c010 - c000) + t.x * (c110 - c010) + t.z * (c111 - c110);
}

/// A LUT over what lies between black and white, with anything brighter
/// carried through by how far it was over, so a look does not clip HDR.
float3 applyLutKeepingExcess(float base, float3 c) {
    const float3 inRange = saturate(c);
    return applyLut(base, inRange) + (c - inRange);
}

/// One of the curve tables, read between its samples.
float curveAt(float base, int index, float x) {
    const float f = saturate(x) * 255.0;
    const float i0 = min(floor(f), 254.0);
    const float at = base + (float)index * 256.0 + i0;
    return lerp(pd(at), pd(at + 1.0), f - i0);
}

float3 lumetriBasic(float3 c) {
    if (params.passA.y >= 0.0) c = applyLutKeepingExcess(params.passA.y, c);

    // White balance and exposure are changes to the light.
    c = gradingEncode(gradingDecode(c) * pd3(0.0) * pd(3.0));
    // What is above white, stretched or pulled in.
    c = lerp(c, 1.0 + (c - 1.0) * pd(10.0), step(1.0, c));
    c = sCurve(c, pd(4.0));

    // Highlights and shadows lift or lower a band of tones each, by luma, and
    // leave black and white where they are.
    const float l = saturate(gradingLuma(c));
    const float shadows = 4.0 * l * (1.0 - l) * (1.0 - l);
    const float highlights = 4.0 * l * l * (1.0 - l);
    c += 0.5 * (pd(6.0) * shadows + pd(5.0) * highlights);

    // Whites and blacks move the ends themselves.
    const float3 s = saturate(c);
    c += pd(7.0) * 0.25 * s * s;
    c += pd(8.0) * 0.25 * (1.0 - s) * (1.0 - s);
    c = max(c, 0.0);

    const float luma = gradingLuma(c);
    return max(luma + (c - luma) * pd(9.0), 0.0);
}

float3 lumetriCreative(float3 c) {
    if (params.passA.z >= 0.0) c = lerp(c, applyLutKeepingExcess(params.passA.z, c), pd(11.0));

    // Faded film lifts the blacks the way a print loses its density.
    const float3 s = saturate(c);
    c += pd(12.0) * 0.25 * (1.0 - s) * (1.0 - s) * (1.0 - s);

    // Vibrance does most to what is least saturated, then saturation to all.
    const float luma = gradingLuma(c);
    const float chroma = saturate(max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)));
    c = luma + (c - luma) * (1.0 + pd(13.0) * (1.0 - chroma));
    c = luma + (c - luma) * pd(14.0);

    // The two tints, split by luma at a point the balance moves.
    const float high = pow(saturate(luma), exp2(-2.0 * pd(21.0)));
    c += pd3(15.0) * (1.0 - high) + pd3(18.0) * high;
    return max(c, 0.0);
}

float3 lumetriCurves(float3 c) {
    const float base = params.passA.w;
    if (params.passB.x > 0.5) {
        const float3 inRange = saturate(c);
        float3 x = float3(curveAt(base, 0, inRange.r), curveAt(base, 0, inRange.g),
                          curveAt(base, 0, inRange.b));
        x = float3(curveAt(base, 1, x.r), curveAt(base, 2, x.g), curveAt(base, 3, x.b));
        c = x + (c - inRange);
    }
    if (params.passB.y > 0.5) {
        const float3 inRange = saturate(c);
        const float luma = gradingLuma(inRange);
        float3 hsv = rgbToHsv(inRange);
        const float hue = hsv.x;
        const float saturation = hsv.y;
        hsv.y = curveAt(base, 8, saturation);
        hsv.y *= 2.0 * curveAt(base, 4, hue) * 2.0 * curveAt(base, 7, luma);
        hsv.x = frac(hue + curveAt(base, 5, hue) - 0.5);
        float3 graded = hsvToRgb(float3(hsv.x, saturate(hsv.y), hsv.z));
        // Lighter or darker by hue, weighted by how much of that hue there is,
        // so a grey is never moved by a hue it does not have.
        graded += (curveAt(base, 6, hue) - 0.5) * 0.5 * saturation;
        c = graded + (c - inRange);
    }
    return max(c, 0.0);
}

/// Lift, gamma and gain, each a colour and an amount, from the three wheels.
float3 lumetriWheels(float3 c) {
    c += pd3(22.0) * (1.0 - saturate(c));
    c *= 1.0 + pd3(28.0);
    return pow(max(c, 0.0), 1.0 / max(1.0 + pd3(25.0), 0.05));
}

float3 lumetriPass(float3 coded) {
    const int sections = (int)params.passA.x;
    float3 c = toGrading(coded);
    if ((sections & 1) != 0) c = lumetriBasic(c);
    if ((sections & 2) != 0) c = lumetriCreative(c);
    if ((sections & 4) != 0) c = lumetriCurves(c);
    if ((sections & 8) != 0) c = lumetriWheels(c);
    return fromGrading(c);
}

/// How far into a range a value is: 1 inside it, falling away over the
/// softness either side, and a hard edge when there is none.
float rangeKey(float v, float low, float high, float soft) {
    const float rise = soft > 0.0 ? smoothstep(low - soft, low, v) : step(low, v);
    const float fall = soft > 0.0 ? 1.0 - smoothstep(high, high + soft, v) : step(v, high);
    return rise * fall;
}

float secondaryKeyOf(float3 c) {
    const float3 s = saturate(c);
    const float3 hsv = rgbToHsv(s);
    float hueKey = 1.0;
    const float half = pd(1.0);
    if (half < 0.5) {
        const float distance = abs(frac(hsv.x - pd(0.0) + 0.5) - 0.5);
        const float soft = pd(2.0);
        hueKey = soft > 0.0 ? 1.0 - smoothstep(half, half + soft, distance) : step(distance, half);
    }
    return hueKey * rangeKey(hsv.y, pd(3.0), pd(4.0), pd(5.0)) *
           rangeKey(gradingLuma(s), pd(6.0), pd(7.0), pd(8.0));
}

float3 gradedAt(float2 uv) { return toGrading(texture0.Sample(linearSampler, uv).rgb); }

/// The colour the key is pulled from: averaged with its neighbours when
/// Denoise asks, so grain does not speckle the edge of what is selected.
float3 denoisedAt(float2 uv) {
    const float radius = pd(9.0);
    float3 c = gradedAt(uv);
    if (radius <= 0.0) return c;
    const float2 step = params.passB.zw * radius;
    c += gradedAt(uv + float2(step.x, 0.0)) + gradedAt(uv - float2(step.x, 0.0));
    c += gradedAt(uv + float2(0.0, step.y)) + gradedAt(uv - float2(0.0, step.y));
    return c / 5.0;
}

/// The key, softened by Blur: the key itself averaged round a ring, which is
/// the matte blurred without a pass of its own to hold it.
float secondaryKey(float2 uv) {
    float key = secondaryKeyOf(denoisedAt(uv));
    const float radius = pd(10.0);
    if (radius > 0.0) {
        const float2 step = params.passB.zw * radius;
        float sum = key;
        [unroll] for (int i = 0; i < 8; ++i) {
            const float angle = (float)i * (6.2831853 / 8.0);
            const float2 offset = float2(cos(angle), sin(angle)) * step;
            sum += secondaryKeyOf(denoisedAt(uv + offset));
            sum += secondaryKeyOf(denoisedAt(uv + offset * 0.5));
        }
        key = sum / 17.0;
    }
    return pd(11.0) > 0.5 ? 1.0 - key : key;
}

float3 secondaryCorrect(float3 c) {
    c = gradingEncode(gradingDecode(c) * pd3(16.0));
    c += pd3(13.0);
    c = sCurve(c, pd(19.0));
    const float luma = gradingLuma(c);
    return max(luma + (c - luma) * pd(20.0), 0.0);
}

float4 secondaryPass(float2 uv, float4 before) {
    const float3 c = toGrading(before.rgb);
    const float key = secondaryKey(uv);
    // Show Mask: the selection in colour, everything else grey.
    if (pd(12.0) > 0.5) return float4(fromGrading(lerp(float3(0.5, 0.5, 0.5), c, key)), before.a);

    float3 sharpened = c;
    if (pd(21.0) != 0.0) {
        const float2 step = params.passB.zw;
        const float3 around = 0.25 * (gradedAt(uv + float2(step.x, 0.0)) +
                                      gradedAt(uv - float2(step.x, 0.0)) +
                                      gradedAt(uv + float2(0.0, step.y)) +
                                      gradedAt(uv - float2(0.0, step.y)));
        sharpened = c + (c - around) * pd(21.0);
    }
    return float4(fromGrading(lerp(c, secondaryCorrect(sharpened), key)), before.a);
}

/// Lumetri's vignette: darker or lighter towards the edges, on a shape between
/// the frame's rectangle and a circle.
float3 lumetriVignette(float2 uv, float3 coded) {
    const float amount = params.passA.x;
    const float midpoint = params.passA.y;
    const float roundness = params.passA.z;
    const float feather = params.passA.w;

    float2 p = (layerUv(uv) - 0.5) * 2.0;
    // Toward a circle on the picture rather than an ellipse fitted to it.
    const float aspect = params.size.x / max(params.size.y, 1.0);
    if (roundness > 0.0) p.x *= lerp(1.0, aspect, roundness);
    // Toward the rectangle, as a squarer and squarer superellipse.
    const float n = roundness < 0.0 ? lerp(2.0, 8.0, -roundness) : 2.0;
    const float2 a = abs(p);
    const float d = pow(pow(a.x, n) + pow(a.y, n), 1.0 / n);

    const float centre = lerp(0.2, 1.6, midpoint);
    const float width = max(feather, 0.02) * 1.2;
    const float m = smoothstep(centre - width * 0.5, centre + width * 0.5, d);

    float3 c = toGrading(coded);
    if (amount < 0.0) {
        c = gradingEncode(gradingDecode(c) * (1.0 + amount * m));
    } else {
        c = lerp(c, float3(1.0, 1.0, 1.0), amount * m);
    }
    return fromGrading(c);
}

// ------------------------------------------------------------- ultra key --
//
// The layout of the key pass's data is `render/ultra_key.hpp`'s.

/// A colour less its luma: what is left is which way, and how far, it leans.
float3 leanOf(float3 c) { return c - gradingLuma(c); }

/// How much of the key colour a pixel holds: its lean projected on the key's,
/// 1 for the key itself, and how far it leans off the key's hue besides, as a
/// fraction of the key's own lean.
///
/// The lean is taken relative to the pixel's brightness, so a screen in shade
/// counts as screen: a green half as bright leans half as far, and is no less
/// green. Shadow says how far a darker pixel is brought up to the key's
/// brightness, which is how it keeps dark parts of the subject opaque.
float2 screenOf(float3 c, bool lifted) {
    const float3 k = leanOf(pd3(0.0));
    const float kk = max(dot(k, k), 1e-5);
    const float lumaK = gradingLuma(pd3(0.0));
    const float lift = lifted ? clamp(lumaK / max(gradingLuma(saturate(c)), 1e-3), 0.0,
                                      lerp(4.0, 1.0, pd(5.0)))
                              : 1.0;
    const float3 p = leanOf(c) * lift;
    const float along = dot(p, k) / kk;
    const float off = length(p - along * k) / sqrt(kk);
    return float2(along, off);
}

float4 ultraKeyPass(float4 before) {
    float3 c = toGrading(before.rgb);
    const float3 key = pd3(0.0);
    const float2 screen = screenOf(c, true);

    // Matte Generation. Tolerance is how much leaning off the key's hue still
    // counts as screen; Transparency how much screen it takes to be clear.
    const float amount = screen.x - screen.y * lerp(4.0, 0.5, pd(6.0));
    float alpha = 1.0 - saturate(amount / lerp(1.0, 0.2, pd(3.0)));
    // Highlight keeps what is lighter than the screen, which a reflection on
    // glass is, more opaque. Shadow's part is in `screenOf`.
    const float lumaC = gradingLuma(saturate(c));
    const float lumaK = gradingLuma(key);
    const float lighter = saturate((lumaC - lumaK) / max(1.0 - lumaK, 0.05));
    alpha = max(alpha, pd(4.0) * lighter);
    // Pedestal: the noise left in what is nearly clear is taken to nought.
    const float pedestal = pd(7.0) * 0.5;
    alpha = saturate((alpha - pedestal) / max(1.0 - pedestal, 1e-3));

    // Spill Suppression. The key's lean, where the pixel leans its way within
    // Range of its hue, is taken out; that keeps the luma, so Luma at its
    // middle is the light put back and either side of it less or more.
    // The lean as it is rather than lifted, since that is what is taken out.
    const float2 spill = screenOf(c, false);
    if (spill.x > 0.0) {
        // An edge is screen mixed with the subject, so it leans well off the
        // key's hue and still carries its spill; a yellow leans further off
        // again, and Range at its middle reaches the first and not the second.
        const float near = saturate(1.0 - spill.y / max(spill.x * (0.5 + pd(9.0) * 3.0), 1e-4));
        // Never more than the pixel's own lean, or it would swing to the
        // opposite colour.
        const float taken = spill.x * min(near * pd(10.0), 1.0);
        c -= leanOf(key) * taken;
        c += (pd(11.0) - 1.0) * taken * lumaK;
    }
    // Desaturate: what is almost clear loses its colour, so a fringe that is
    // left carries no green into what it is laid over.
    const float grey = gradingLuma(c);
    c = lerp(c, float3(grey, grey, grey), pd(8.0) * (1.0 - smoothstep(0.0, 0.5, alpha)));

    // Color Correction, of what stays.
    float3 yuv = rgbToYuv601(c);
    const float hc = cos(pd(13.0));
    const float hs = sin(pd(13.0));
    yuv.yz = float2(yuv.y * hc - yuv.z * hs, yuv.y * hs + yuv.z * hc) * pd(12.0);
    c = max(yuv601ToRgb(yuv) * pd(14.0), 0.0);

    return float4(fromGrading(c), before.a * alpha);
}

/// Choke: the matte eroded, the smallest alpha within the radius.
float4 matteChoke(float2 uv, float4 before) {
    const float radius = params.passA.x;
    float alpha = before.a;
    [unroll] for (int y = -4; y <= 4; ++y) {
        [unroll] for (int x = -4; x <= 4; ++x) {
            const float2 o = float2(x, y);
            if (length(o) > radius + 0.25) continue;
            alpha = min(alpha, texture0.Sample(linearSampler, uv + o * params.passB.zw).a);
        }
    }
    return float4(before.rgb, alpha);
}

/// Soften: the matte blurred. The colour is averaged by alpha as well, so the
/// edge it spreads into takes the foreground's colour rather than the clear
/// screen's.
float4 matteSoften(float2 uv) {
    const float radius = max(params.passA.x, 0.5);
    const float sigma = radius / 2.0;
    const float stride = max(radius / 4.0, 1.0);
    float3 colour = 0.0;
    float alpha = 0.0;
    float total = 0.0;
    [unroll] for (int y = -4; y <= 4; ++y) {
        [unroll] for (int x = -4; x <= 4; ++x) {
            const float2 o = float2(x, y) * stride;
            const float w = exp(-dot(o, o) / (2.0 * sigma * sigma));
            const float4 s = texture0.Sample(linearSampler, uv + o * params.passB.zw);
            colour += s.rgb * s.a * w;
            alpha += s.a * w;
            total += w;
        }
    }
    const float3 rgb = alpha > 0.0 ? colour / alpha : texture0.Sample(linearSampler, uv).rgb;
    return float4(rgb, alpha / total);
}

/// Contrast and Mid Point, then what Output shows: the composite, the matte
/// as grey, or the colour with no matte at all.
float4 ultraKeyOutput(float2 uv, float4 before) {
    const float mid = params.passA.y;
    const float alpha = saturate((before.a - mid) * params.passA.x + mid);
    const float2 l = layerUv(uv);
    const float inside = all(l >= 0.0) && all(l <= 1.0) ? 1.0 : 0.0;
    const int output = (int)(params.passA.z + 0.5);
    if (output == 1) return float4(fromGrading(float3(alpha, alpha, alpha)), inside);
    if (output == 2) return float4(before.rgb, inside);
    return float4(before.rgb, alpha);
}

// ------------------------------------------------------------- transform --
//
// The layout of the pass's data is `render/transform_effect.hpp`'s.

float2x2 rotation2(float a) {
    const float c = cos(a);
    const float s = sin(a);
    // Clockwise on the picture, whose y runs down.
    return float2x2(c, -s, s, c);
}

/// A point of the layer's frame, 0 to 1, read from the scratch, or nothing
/// past the frame's edge, which is where Premiere's Transform cuts.
float4 frameAt(float2 f) {
    if (any(f < 0.0) || any(f > 1.0)) return float4(0.0, 0.0, 0.0, 0.0);
    const float2 span = float2(1.0, 1.0) - 2.0 * params.margin;
    return texture0.Sample(linearSampler, f * span + params.margin);
}

float cubicWeight(float x) {
    // Catmull-Rom, which is what a bicubic resample means in practice: sharp,
    // passing through the pixels it is given.
    x = abs(x);
    if (x < 1.0) return 1.5 * x * x * x - 2.5 * x * x + 1.0;
    if (x < 2.0) return -0.5 * x * x * x + 2.5 * x * x - 4.0 * x + 2.0;
    return 0.0;
}

/// Bicubic: sixteen of the frame's pixels about the point, weighted. Read
/// premultiplied, so a transparent neighbour does not darken an edge.
float4 frameBicubic(float2 f) {
    const float2 frameTexel = params.passB.zw / (float2(1.0, 1.0) - 2.0 * params.margin);
    const float2 pixel = f / frameTexel - 0.5;
    const float2 base = floor(pixel);
    const float2 t = pixel - base;
    float4 sum = 0.0;
    float total = 0.0;
    [unroll] for (int j = -1; j <= 2; ++j) {
        [unroll] for (int i = -1; i <= 2; ++i) {
            const float w = cubicWeight(i - t.x) * cubicWeight(j - t.y);
            const float4 s = frameAt((base + float2(i, j) + 0.5) * frameTexel);
            sum += float4(s.rgb * s.a, s.a) * w;
            total += w;
        }
    }
    sum /= max(total, 1e-5);
    const float alpha = saturate(sum.a);
    return float4(alpha > 0.0 ? max(sum.rgb, 0.0) / max(sum.a, 1e-5) : 0.0, alpha);
}

float4 transformPass(float2 uv) {
    const int samples = (int)pd(0.0);
    const bool bicubic = pd(1.0) > 0.5;
    const float2 size = max(params.size, float2(1.0, 1.0));
    // In the frame's pixels, where a rotation is a rotation and not a shear.
    const float2 p = layerUv(uv) * size;

    float4 sum = 0.0;
    for (int n = 0; n < samples; ++n) {
        const float at = 4.0 + 9.0 * (float)n;
        const float2 anchor = float2(pd(at), pd(at + 1.0)) * size;
        const float2 position = float2(pd(at + 2.0), pd(at + 3.0)) * size;
        const float2 scale = float2(pd(at + 4.0), pd(at + 5.0));
        const float skew = pd(at + 6.0);
        const float axis = pd(at + 7.0);
        // Forwards: placed by Position, turned, sheared along the skew axis,
        // scaled about the anchor. Read backwards, from where a pixel lands
        // to where it came from.
        float2 d = mul(rotation2(-pd(at + 8.0)), p - position);
        d = mul(rotation2(axis), d);
        d.x += tan(skew) * d.y;
        d = mul(rotation2(-axis), d);
        if (any(abs(scale) < 1e-5)) continue;
        const float2 f = (anchor + d / scale) / size;
        const float4 s = bicubic ? frameBicubic(f) : frameAt(f);
        sum += float4(s.rgb * s.a, s.a);
    }
    sum /= max((float)samples, 1.0);
    const float3 rgb = sum.a > 0.0 ? sum.rgb / sum.a : float3(0.0, 0.0, 0.0);
    return float4(rgb, sum.a * pd(2.0));
}

// The warp pass's header, as `render/warp_stabilizer.hpp` lays it out.
static const float kWarpHeader = 12.0;
#define SYNTHESIS_COLUMNS 4
#define SYNTHESIS_ROWS 3

/// How far inside the usable part of a frame a point is, as a share of the
/// frame: negative outside. Synthesis Edge Cropping takes its margins off.
float synthesisDepth(float2 s) {
    const float4 crop = float4(pd(6.0), pd(7.0), pd(8.0), pd(9.0));
    return min(min(s.x - crop.x, s.y - crop.y), min(1.0 - crop.z - s.x, 1.0 - crop.w - s.y));
}

/// A neighbouring frame's picture at `s`, from its cell of the atlas Track
/// Matte's slot carries, in the scratch's coded values.
float4 synthesisAt(int k, float2 s) {
    const float2 cell = float2(k % SYNTHESIS_COLUMNS, k / SYNTHESIS_COLUMNS);
    const float2 atlas = (cell + saturate(s)) / float2(SYNTHESIS_COLUMNS, SYNTHESIS_ROWS);
    float4 v = texture1.Sample(linearSampler, atlas);
    if (pd(5.0) > 0.5) v.rgb = fromRelative(toRelative(v.rgb, TRANSFER_SCRGB), params.working);
    return float4(v.rgb, 1.0);
}

/// Warp Stabilizer: each output point drawn from where the mesh says, read
/// from the cell it falls in. Outside what Stabilize, Crop leaves showing,
/// nothing. Where the source is off the frame, nothing either, unless
/// Synthesize Edges has neighbouring frames to fill it from: nearest in time
/// first, each blended in across its feather.
float4 warpPass(float2 uv) {
    const int columns = (int)pd(0.0);
    const int rows = (int)pd(1.0);
    const float visible = pd(2.0);
    const int neighbours = (int)pd(3.0);
    const float2 f = layerUv(uv);
    if (any(abs(f - 0.5) > 0.5 * visible)) return float4(0.0, 0.0, 0.0, 0.0);
    const float2 g = saturate(f) * float2(columns, rows);
    const int2 cell = min(int2(g), int2(columns - 1, rows - 1));
    const float2 t = g - float2(cell);
    const float base = kWarpHeader + 2.0 * (float)(cell.y * (columns + 1) + cell.x);
    const float below = 2.0 * (float)(columns + 1);
    const float2 a = float2(pd(base), pd(base + 1.0));
    const float2 b = float2(pd(base + 2.0), pd(base + 3.0));
    const float2 c = float2(pd(base + below), pd(base + below + 1.0));
    const float2 d = float2(pd(base + below + 2.0), pd(base + below + 3.0));
    const float2 source = lerp(lerp(a, b, t.x), lerp(c, d, t.x), t.y);
    const float4 here = frameAt(source);
    if (neighbours <= 0) return here;
    // Deep inside this frame there is nothing to fill.
    const float feather = pd(4.0);
    const float depth = synthesisDepth(source);
    if (depth >= feather) return here;

    // Front to back: this frame, faded out over the feather towards its edge
    // so the seam with what fills it is soft, then each neighbour, whole
    // wherever it has picture, until the point is covered.
    const float own = feather > 0.0 ? saturate(depth / feather) : step(0.0, depth);
    float4 sum = float4(here.rgb * here.a, here.a) * own;
    const float homographies = kWarpHeader + 2.0 * (float)((columns + 1) * (rows + 1));
    // A pixel of the frame, for an edge that is smooth rather than stepped.
    const float pixel = 1.0 / max(params.size.x, 1.0);
    for (int k = 0; k < neighbours && sum.a < 0.999; ++k) {
        const float at = homographies + 9.0 * (float)k;
        const float3 p = float3(source, 1.0);
        const float w = pd(at + 6.0) * p.x + pd(at + 7.0) * p.y + pd(at + 8.0);
        const float2 s = float2(pd(at) * p.x + pd(at + 1.0) * p.y + pd(at + 2.0),
                                pd(at + 3.0) * p.x + pd(at + 4.0) * p.y + pd(at + 5.0)) /
                         (abs(w) > 1e-6 ? w : 1e-6);
        const float cover = saturate(synthesisDepth(s) / pixel);
        if (cover <= 0.0) continue;
        const float4 there = synthesisAt(k, s);
        sum += (1.0 - sum.a) * cover * float4(there.rgb, 1.0);
    }
    return sum.a > 0.0 ? float4(sum.rgb / sum.a, sum.a) : float4(0.0, 0.0, 0.0, 0.0);
}

float4 PSEffect(VSOutput input) : SV_Target {
    const float4 before = texture0.Sample(linearSampler, input.uv);

    float4 after;
    if (params.passKind == PASS_BLUR || params.passKind == PASS_DIRBLUR) {
        // The same maths either way. A Gaussian is two of these at right
        // angles and a directional blur is one of them aimed somewhere else,
        // and which it is has already been decided by the step the compositor
        // handed down.
        after = blurPass(input.uv);
    } else if (params.passKind == PASS_RADIAL) {
        after = radialBlurPass(input.uv);
    } else if (params.passKind == PASS_DISTORT) {
        after = distortPass(input.uv);
    } else if (params.passKind == PASS_NOISE) {
        after = noisePass(input.uv, before);
    } else if (params.passKind == PASS_SHADOW) {
        // The shape the layer makes, moved and in one colour. Read from where
        // the shadow comes from, so the shape lands where it falls.
        const float shape = texture0.Sample(linearSampler, input.uv - params.passB.xy).a;
        after = float4(params.passA.rgb, shape * params.passA.a);
    } else if (params.passKind == PASS_UNDER) {
        // What was kept, over what the passes since have made of it: the layer
        // back on top of its own shadow.
        const float4 kept = texture2.Sample(linearSampler, input.uv);
        const float alpha = kept.a + before.a * (1.0 - kept.a);
        const float3 rgb = alpha > 0.0
            ? (kept.rgb * kept.a + before.rgb * before.a * (1.0 - kept.a)) / alpha
            : float3(0.0, 0.0, 0.0);
        after = float4(rgb, alpha);
    } else if (params.passKind == PASS_SECONDARY) {
        after = secondaryPass(input.uv, before);
    } else if (params.passKind == PASS_ULTRAKEY) {
        after = ultraKeyPass(before);
    } else if (params.passKind == PASS_CHOKE) {
        after = matteChoke(input.uv, before);
    } else if (params.passKind == PASS_SOFTEN) {
        after = matteSoften(input.uv);
    } else if (params.passKind == PASS_ULTRAOUT) {
        after = ultraKeyOutput(input.uv, before);
    } else if (params.passKind == PASS_TRANSFORM) {
        after = transformPass(input.uv);
    } else if (params.passKind == PASS_WARP) {
        after = warpPass(input.uv);
    } else if (params.passKind == PASS_SHARPEN) {
        after = float4(sharpenPass(input.uv), texture0.Sample(linearSampler, input.uv).a);
    } else if (params.passKind == PASS_FLIP) {
        // A mirror in texture space, which is why it survives the rotation the
        // composite applies afterwards rather than fighting it.
        // Mirrored about the *layer's* centre, not the scratch's, or a margin
        // would slide the picture sideways as well as flipping it.
        const float2 span = float2(1.0, 1.0) - 2.0 * params.margin;
        const float2 mirrored = (layerUv(input.uv) - 0.5) * params.passA.xy * span
                              + 0.5 * span + params.margin;
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
            const float3 linearRgb = workingLinear(after.rgb) * vignetteFactor(layerUv(input.uv));
            after.rgb = workingCoded(linearRgb);
        } else if (params.passKind == PASS_CROP) {
            if (!insideCrop(layerUv(input.uv))) after.a = 0.0;
        } else if (params.passKind == PASS_CHROMAKEY) {
            after.a *= chromaKeyAlpha(after.rgb);
        } else if (params.passKind == PASS_LEVELS) {
            after.rgb = levelsPass(after.rgb);
        } else if (params.passKind == PASS_BALANCE) {
            after.rgb = balancePass(after.rgb);
        } else if (params.passKind == PASS_TINT) {
            after.rgb = tintPass(after.rgb);
        } else if (params.passKind == PASS_POSTERIZE) {
            // Rounded to the nearest step rather than floored, so the darkest
            // and lightest bands are the same width as the ones between them
            // instead of half of one.
            const float levels = max(params.passA.x, 2.0);
            after.rgb = saturate(round(after.rgb * (levels - 1.0)) / (levels - 1.0));
        } else if (params.passKind == PASS_LUMETRI) {
            after.rgb = lumetriPass(after.rgb);
        } else if (params.passKind == PASS_LVIGNETTE) {
            after.rgb = lumetriVignette(input.uv, after.rgb);
        } else if (params.passKind == PASS_MATTE) {
            after.a *= matteFactor(input.uv);
        } else if (params.passKind == PASS_THRESHOLD) {
            const float luma = rgbToYuv601(after.rgb).x;
            after.rgb = luma >= params.passA.x ? float3(1.0, 1.0, 1.0) : float3(0.0, 0.0, 0.0);
        }
    }

    // The mask, the same way for every kind: what the effect did, mixed with
    // what was there, by how much of this pixel the mask covers. An effect
    // never has to know it is masked, which is what makes masking something the
    // whole catalogue gets rather than something each entry implements.
    const float coverage = maskCoverage(layerUv(input.uv));
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
    const float2 layer = layerUv(input.uv);
    // The border is genuinely empty rather than a clamped edge pixel: it is
    // what a blur spreads into, and a smeared copy of the edge would be a
    // border that glows instead of one that fades.
    if (layer.x < 0.0 || layer.x > 1.0 || layer.y < 0.0 || layer.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return softenedSource(layer);
}

/// The same, for an adjustment layer, whose source is whatever is beneath it.
///
/// The backdrop is in canvas space and the scratch is in layer space, so this
/// walks the quad's transform forwards for each scratch texel: exactly what
/// `VSLayer` does to a corner, done per pixel.
float4 PSAdjustSource(VSOutput input) : SV_Target {
    const float2 local = (layerUv(input.uv) - 0.5) * params.size;
    const float2 rotated = float2(
        local.x * params.rotation.x - local.y * params.rotation.y,
        local.x * params.rotation.y + local.y * params.rotation.x);
    const float2 screen = (params.center + rotated) / params.canvas;

    // Linear, and in an HDR sequence above 1.0, which the sequence's own curve
    // keeps; in an SDR one, encoding clamps it.
    const float4 base = backdrop.Sample(linearSampler, screen);
    return float4(workingCoded(base.rgb), 1.0);
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
    const float3 result = workingLinear(coded.rgb);

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
    float4 scene = texture0.Sample(linearSampler, input.uv);

    // Premiere's Display Mode and Transparency Grid, for the monitor alone.
    // Alpha is shown as its own value, as a matte is read, so it skips the
    // encoding below and is only scaled to white on an HDR display.
    if (params.passB.x > 0.5) {
        if (params.output == OUTPUT_SCRGB) return float4(linearizeSrgb(scene.aaa) * params.passA.x, 1.0);
        return float4(scene.aaa, 1.0);
    }
    if (params.passB.y > 0.5) {
        // Squares of sixteen canvas pixels, two greys in linear light, showing
        // through wherever the premultiplied scene does not cover.
        const float2 square = floor(input.uv * params.canvas / 16.0);
        const float shade = fmod(square.x + square.y, 2.0) < 0.5 ? 0.38 : 0.18;
        scene = float4(scene.rgb + shade * (1.0 - scene.a), 1.0);
    }

    if (params.output == OUTPUT_SCRGB) {
        return float4(convertPrimaries(scene.rgb, primariesOf(params.working), PRIMARIES_709) *
                          params.passA.x,
                      scene.a);
    }

    // Out to some other space than the sequence's: an HDR sequence on an SDR
    // display, tone mapped and brought into Rec.709.
    if (params.working != WORKING_REC709 || params.output != WORKING_REC709) {
        return float4(intoSpace(scene.rgb, primariesOf(params.working),
                                params.working != WORKING_REC709, params.output),
                      scene.a);
    }

    const float3 low = scene.rgb * 12.92;
    const float3 high = 1.055 * pow(max(scene.rgb, 0.0), 1.0 / 2.4) - 0.055;
    // `select` rather than `?:` — the condition is a vector, and HLSL 2021
    // refuses to short-circuit one.
    const float3 encoded = select(scene.rgb <= 0.0031308, low, high);

    // Alpha is linear by definition and is not encoded.
    return float4(encoded, scene.a);
}

// ----------------------------------------------------------------- encoding --
//
// The composited frame as an encoder wants it: Y'CbCr, BT.709, studio range,
// 4:2:0 -- a full-size plane of luma and a half-size plane of the two colour
// differences interleaved, which is NV12, or P010 at ten bits. Done here rather
// than by `sws_scale` after a readback because the readback was 33 MB of RGBA a
// 4K frame and the conversion fourteen milliseconds of processor; NV12 is 12 MB
// and this is a fraction of a millisecond.
//
// From the display encoding, exactly as `PSPresent` produces it, so a file and
// the monitor are the same picture. `layout` carries the bit depth.

float3 displayCoded(int2 at) {
    const float3 scene = texture0.Load(int3(at, 0)).rgb;
    if (params.working == WORKING_REC709 && params.output == WORKING_REC709) return encodeSrgb(scene);
    return intoSpace(scene, primariesOf(params.working), params.working != WORKING_REC709,
                     params.output);
}

// The luma weights of the file being written: BT.709 for SDR, BT.2020 for HDR.
float3 outputLuma() { return params.output == WORKING_REC709 ? kLuma709 : kLuma2020; }

// A level on the studio scale, `offset + span * v` in the stored bits, as the
// UNORM value that stores it. Ten bits sit at the top of sixteen, so they are
// rounded to a whole code first: left to the conversion to sixteen bits, a
// fractional code would land in the six bits below it.
float studioLevel(float v, float offset, float span, int bits) {
    if (bits > 8) {
        const float code = round((offset + span * v) * 4.0);
        return code * 64.0 / 65535.0;
    }
    return (offset + span * v) / 255.0;
}

float4 PSEncodeLuma(VSOutput input) : SV_Target {
    const float3 rgb = displayCoded(int2(input.position.xy));
    const float y = dot(rgb, outputLuma());
    return float4(studioLevel(y, 16.0, 219.0, params.layout), 0.0, 0.0, 1.0);
}

float4 PSEncodeChroma(VSOutput input) : SV_Target {
    // The four pixels this chroma sample stands for, averaged in the display
    // encoding the way `sws_scale` did it, rather than in linear light.
    const int2 at = int2(input.position.xy) * 2;
    const float3 rgb = 0.25 * (displayCoded(at) + displayCoded(at + int2(1, 0)) +
                               displayCoded(at + int2(0, 1)) + displayCoded(at + int2(1, 1)));
    const float3 k = outputLuma();
    const float y = dot(rgb, k);
    const float cb = (rgb.b - y) / (2.0 * (1.0 - k.b));
    const float cr = (rgb.r - y) / (2.0 * (1.0 - k.r));
    return float4(studioLevel(cb, 128.0, 224.0, params.layout),
                  studioLevel(cr, 128.0, 224.0, params.layout), 0.0, 1.0);
}
