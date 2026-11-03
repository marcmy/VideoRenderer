// SDR-to-SDR perceptual enhancement: a global tone curve on OKLab lightness
// and vibrance on OKLab chroma. The input is display-referred BT.709 R'G'B'.
// It is decoded once and encoded once with the same display gamma, so the
// neutral settings reproduce the input apart from float rounding.

Texture2D tex : register(t0);
SamplerState samp : register(s0);

cbuffer PS_SDR_ENHANCE : register(b0)
{
    // x: contrast [-0.5, 0.5], y: pivot (OKLab L), z: vibrance [-0.5, 1], w: skin protection [0, 1]
    float4 params;
    // x: A/B split in render-target pixels; pixels left of it are unprocessed.
    // A negative value processes the whole image. y: divider half-width.
    float4 compare;
};

struct PS_INPUT
{
    float4 Pos : SV_POSITION;
    float2 Tex : TEXCOORD;
};

static const float DisplayGamma = 2.2;

// OKLab, Björn Ottosson 2020, for linear BT.709/sRGB primaries.
static const float3x3 LinearToLms = {
    0.4122214708, 0.5363325363, 0.0514459929,
    0.2119034982, 0.6806995451, 0.1073969566,
    0.0883024619, 0.2817188376, 0.6299787005,
};
static const float3x3 LmsToLab = {
    0.2104542553,  0.7936177850, -0.0040720468,
    1.9779984951, -2.4285922050,  0.4505937099,
    0.0259040371,  0.7827717662, -0.8086757660,
};
// OKLab to LMS' is L * (1, 1, 1) plus this matrix applied to (a, b).
static const float3x2 AbToLms = {
     0.3963377774,  0.2158037573,
    -0.1055613458, -0.0638541728,
    -0.0894841775, -1.2914855480,
};
static const float3x3 LmsToLinear = {
     4.0767416621, -3.3077115913,  0.2309699292,
    -1.2684380046,  2.6097574011, -0.3413193965,
    -0.0041960863, -0.7034186147,  1.7076147010,
};
// Typical skin hue in OKLab (about 55 degrees), as a unit (a, b) direction.
static const float2 SkinHue = float2(0.574, 0.819);

float3 LinearToOklab(float3 c)
{
    // In-gamut input keeps LMS non-negative.
    return mul(LmsToLab, pow(max(mul(LinearToLms, c), 0.0), 1.0 / 3.0));
}

// Linear colour of lightness L whose (a, b) has been mapped through AbToLms.
float3 OklabToLinear(float L, float3 abLms)
{
    const float3 lms = L + abLms;
    return mul(LmsToLinear, lms * lms * lms);
}

// Monotonic S-curve with fixed 0, pivot and 1. The base curve has slope 2 at
// the pivot and 0 at the ends; blending by |contrast| <= 0.5 keeps every
// slope within [0.5, 1.5], so neither end clips or crushes.
float ToneCurve(float x, float contrast, float pivot)
{
    const float lo = x / pivot;
    const float hi = (1.0 - x) / (1.0 - pivot);
    const float s = x < pivot ? pivot * lo * lo : 1.0 - (1.0 - pivot) * hi * hi;
    return x + contrast * (s - x);
}

bool InGamut(float3 c)
{
    return all(c >= -1e-6) && all(c <= 1.0 + 1e-6);
}

float4 main(PS_INPUT input) : SV_Target
{
    const float4 pixel = tex.Sample(samp, input.Tex);
    if (input.Pos.x < compare.x) {
        // A/B reference half: the unprocessed value, with a neutral divider.
        return abs(input.Pos.x - compare.x) <= compare.y ? float4(0.5, 0.5, 0.5, 1.0) : pixel;
    }

    const float3 lin = pow(saturate(pixel.rgb), DisplayGamma);
    const float3 lab = LinearToOklab(lin);
    const float L = lab.x;
    const float L2 = ToneCurve(saturate(L), params.x, params.y);
    // OKLab scales as the cube root of linear light, so multiplying linear RGB
    // by (L2/L)^3 sets lightness to exactly L2 and scales chroma with it: hue
    // and saturation are kept. Unlike an OKLab round trip, this is exact for
    // neutral settings, where float error near black would show after encoding.
    const float ratio = L > 1e-4 ? L2 / L : 1.0;
    const float3 toned = lin * (ratio * ratio * ratio);
    const float2 ab = lab.yz * ratio;

    float gain = 1.0;
    const float C = length(ab);
    if (C > 1e-6 && params.z != 0.0) {
        // Vibrance favours muted colours. Near-neutral pixels (chroma noise,
        // slightly tinted greys) and, optionally, skin tones are protected.
        const float sat = saturate(C / max(L2, 0.05) * 2.0);
        float weight = (1.0 - sat) * (1.0 - sat) * smoothstep(0.0, 0.03, C);
        if (params.z > 0.0) {
            const float skin = smoothstep(0.85, 0.97, dot(ab / C, SkinHue));
            weight *= 1.0 - params.w * skin;
        }
        gain = 1.0 + params.z * weight;
    }

    // Chroma changes are applied as linear deltas from the toned colour. Both
    // conversions share their rounding, so it cancels instead of being added.
    float3 rgb = toned;
    if (gain != 1.0 || !InGamut(rgb)) {
        const float3 abLms = mul(AbToLms, ab);
        const float3 offset = toned - OklabToLinear(L2, abLms);
        rgb = offset + OklabToLinear(L2, abLms * gain);
        if (!InGamut(rgb)) {
            // Out of the SDR gamut: lower OKLab chroma at constant lightness
            // and hue rather than clipping channels. Zero chroma is the grey
            // of lightness L2, which is always inside. Eight steps resolve
            // the chroma to 0.4%, always on the in-gamut side.
            float lo = 0.0, hi = gain;
            [unroll] for (int i = 0; i < 8; ++i) {
                const float mid = 0.5 * (lo + hi);
                if (InGamut(offset + OklabToLinear(L2, abLms * mid))) lo = mid;
                else hi = mid;
            }
            rgb = offset + OklabToLinear(L2, abLms * lo);
        }
    }
    // The gamut search leaves at most 1e-6 of overshoot for saturate() to absorb.
    return float4(pow(saturate(rgb), 1.0 / DisplayGamma), pixel.a);
}
