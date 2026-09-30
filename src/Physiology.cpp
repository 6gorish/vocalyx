#include "Physiology.h"

#include <algorithm>
#include <cmath>

namespace vocalyx {

namespace {

// The pharynx dominates the lower resonances and the oral cavity the upper
// ones, though no formant belongs to one cavity alone. These two frequencies
// are where each cavity's influence is strongest, and the warp curve is built
// by anchoring the two cavity ratios there and interpolating between.
constexpr float kPharynxAnchorHz = 700.0f;
constexpr float kOralAnchorHz    = 2600.0f;

// How much each formant follows the pharynx rather than the oral cavity. No
// formant belongs to one cavity, and the split varies with the vowel being
// spoken, so these are a fixed approximation of the average tendency: the lower
// resonances track the pharynx, the upper ones the mouth.
constexpr float kPharynxWeight[4] = { 0.70f, 0.50f, 0.30f, 0.20f };

// Protruded lips add roughly this much tube at full rounding.
constexpr float kLipProtrusionCm = 1.0f;

// Rounding pulls the third formant down beyond what the added length explains,
// because it also narrows the opening.
constexpr float kRoundingThirdFormantPull = 0.12f;   // 12 percent at full rounding
constexpr float kThirdFormantHz           = 2800.0f;

float safeDivide(float numerator, float denominator) {
    return (std::fabs(denominator) < 1e-6f) ? 1.0f : numerator / denominator;
}

} // namespace

float warpScaleAt(const std::vector<WarpPoint>& warp, float hz) {
    if (warp.empty())
        return 1.0f;

    if (hz <= warp.front().sourceHz)
        return warp.front().scale;

    for (size_t i = 1; i < warp.size(); ++i) {
        if (hz <= warp[i].sourceHz) {
            const WarpPoint& a = warp[i - 1];
            const WarpPoint& b = warp[i];

            const float span = b.sourceHz - a.sourceHz;
            if (span <= 0.0f)
                return b.scale;

            const float t = (hz - a.sourceHz) / span;
            return a.scale + t * (b.scale - a.scale);
        }
    }

    return warp.back().scale;
}

Acoustics deriveAcoustics(const Physiology& target, const SourceSpeaker& source) {
    Acoustics out;

    // --- tract lengths, split by cavity -------------------------------------
    //
    // Formant frequencies scale inversely with the length of the tube that
    // produces them, so every ratio below is source length over target length.

    const float targetTotal = std::max(8.0f, target.tractLengthCm
                                             + target.lipRounding * kLipProtrusionCm);

    const float targetPharynx = std::clamp(target.pharynxFraction, 0.25f, 0.65f) * targetTotal
                              + target.larynxHeightCm;

    const float targetOral = std::max(3.0f, targetTotal - targetPharynx);

    // The recorded speaker is described only by total length, so a neutral
    // split stands in for the parts. Replacing this with a measured split is
    // the obvious improvement once estimation from the recording exists.
    const float sourceTotal   = std::max(8.0f, source.tractLengthCm);
    const float sourcePharynx = 0.45f * sourceTotal;
    const float sourceOral    = sourceTotal - sourcePharynx;

    const float pharynxRatio = safeDivide(sourcePharynx, std::max(2.0f, targetPharynx));
    const float oralRatio    = safeDivide(sourceOral, targetOral);

    // --- the warp curve ------------------------------------------------------
    //
    // With measured formants, the curve bends at the speaker's own resonances:
    // each one is given the cavity ratio it mostly follows, and the curve is
    // interpolated between them. Without measurements it falls back to fixed
    // anchors at frequencies where an average speaker's cavities dominate.

    const float thirdFormantScale =
        oralRatio * (1.0f - kRoundingThirdFormantPull * std::clamp(target.lipRounding, 0.0f, 1.0f));

    if (target.useFormantTargets && source.formantsMeasured) {
        // Each measured formant goes exactly where it was dragged. The curve is
        // those pairs and nothing else, so what you see is what is applied.
        out.warp.clear();
        out.warp.reserve(6);

        float firstScale = 1.0f;
        float lastScale  = 1.0f;
        float lastHz     = 0.0f;
        bool  any        = false;

        for (int n = 0; n < 4; ++n) {
            const float from = source.formantHz[static_cast<size_t>(n)];
            const float to   = target.formantTargetHz[static_cast<size_t>(n)];

            if (from <= 0.0f || to <= 0.0f)
                continue;

            const float scale = to / from;

            if (!any) {
                firstScale = scale;
                any = true;
            }

            out.warp.push_back({ from, scale });

            lastScale = scale;
            lastHz    = from;
        }

        if (any) {
            out.warp.insert(out.warp.begin(), { 0.0f, firstScale });
            out.warp.push_back({ lastHz * 2.0f, lastScale });
        }
    }

    if (out.warp.empty() && source.formantsMeasured) {
        out.warp.clear();
        out.warp.reserve(6);

        float lowestScale = pharynxRatio;
        float highestScale = oralRatio;
        float highestHz = 0.0f;

        for (int n = 0; n < 4; ++n) {
            const float hz = source.formantHz[static_cast<size_t>(n)];

            if (hz <= 0.0f)
                continue;

            const float weight = kPharynxWeight[n];
            float scale = pharynxRatio * weight + oralRatio * (1.0f - weight);

            // Rounding pulls the third formant down further than length alone
            // explains, because it also narrows the opening.
            if (n == 2)
                scale *= (1.0f - kRoundingThirdFormantPull * std::clamp(target.lipRounding, 0.0f, 1.0f));

            if (out.warp.empty())
                lowestScale = scale;

            out.warp.push_back({ hz, scale });

            highestScale = scale;
            highestHz    = hz;
        }

        if (out.warp.empty()) {
            out.warp = {
                { 0.0f,              pharynxRatio },
                { kPharynxAnchorHz,  pharynxRatio },
                { kThirdFormantHz,   thirdFormantScale },
                { kOralAnchorHz * 2, oralRatio },
            };
        } else if (out.warp.front().sourceHz > 0.0f) {
            // Hold the end values beyond the outermost formants rather than
            // letting the curve run off in either direction.
            out.warp.insert(out.warp.begin(), { 0.0f, lowestScale });
            out.warp.push_back({ highestHz * 2.0f, highestScale });
        }
    } else if (out.warp.empty()) {
        out.warp = {
            { 0.0f,              pharynxRatio },
            { kPharynxAnchorHz,  pharynxRatio },
            { kThirdFormantHz,   thirdFormantScale },
            { kOralAnchorHz * 2, oralRatio },
        };
    }

    // Effort raises the first formant a little. Adding it to the low anchor
    // rather than the whole curve keeps the change where it belongs.
    if (std::fabs(target.effort) > 0.001f) {
        const float firstFormantLift = 1.0f + 0.04f * std::clamp(target.effort, -1.0f, 1.0f);
        out.warp[0].scale *= firstFormantLift;
        out.warp[1].scale *= firstFormantLift;
    }

    // --- source --------------------------------------------------------------

    out.pitchRatio = safeDivide(std::max(40.0f, target.medianF0Hz),
                                std::max(40.0f, source.medianF0Hz));

    // A modal voice falls off near 12 dB per octave. A high open quotient
    // steepens that, a pressed voice flattens it, and effort flattens it
    // further. The numbers here are the shape of the relationship rather than
    // a calibrated fit.
    const float quotientDeviation = std::clamp(target.openQuotient, 0.3f, 0.9f) - 0.6f;
    out.tiltChangeDbPerOctave = -12.0f * quotientDeviation
                              + 3.0f * std::clamp(target.effort, -1.0f, 1.0f);

    // Breathiness arrives two ways: as the tilt above, and as audible noise.
    // A high open quotient implies some of the latter even when aspiration is
    // left at zero.
    out.aspirationAmount = std::clamp(target.aspiration
                                      + std::max(0.0f, quotientDeviation) * 0.5f,
                                      0.0f, 1.0f);

    out.jitterFraction  = 0.02f * std::clamp(target.jitter, 0.0f, 1.0f);
    out.shimmerFraction = 0.15f * std::clamp(target.shimmer, 0.0f, 1.0f);

    out.morphEnvelope     = std::clamp(target.morphEnvelope, 0.0f, 1.0f);
    out.morphAperiodicity = std::clamp(target.morphAperiodicity, 0.0f, 1.0f);
    out.morphF0           = std::clamp(target.morphF0, 0.0f, 1.0f);

    // --- nasal coupling ------------------------------------------------------
    //
    // A separate resonance and antiresonance rather than a change to the
    // existing ones. Both scale with the tract, since a smaller speaker has a
    // smaller nasal cavity too.

    const float nasalScale = safeDivide(sourceTotal, targetTotal);

    out.nasalAmount = std::clamp(target.nasality, 0.0f, 1.0f);
    out.nasalPoleHz = 1000.0f / std::max(0.5f, nasalScale);
    out.nasalZeroHz = 700.0f  / std::max(0.5f, nasalScale);

    return out;
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

Physiology presetNeutralMale() {
    Physiology p;
    p.tractLengthCm   = 17.5f;
    p.pharynxFraction = 0.47f;
    p.medianF0Hz      = 110.0f;
    p.openQuotient    = 0.55f;
    return p;
}

Physiology presetNeutralFemale() {
    Physiology p;
    p.tractLengthCm   = 14.5f;
    p.pharynxFraction = 0.42f;   // the oral cavity differs less than the pharynx
    p.medianF0Hz      = 200.0f;
    p.openQuotient    = 0.65f;   // typically breathier than a modal male voice
    p.aspiration      = 0.05f;
    return p;
}

Physiology presetChild() {
    Physiology p;
    p.tractLengthCm   = 11.5f;
    p.pharynxFraction = 0.38f;   // the pharynx lengthens most during growth
    p.medianF0Hz      = 260.0f;
    p.openQuotient    = 0.6f;
    return p;
}

Physiology presetLarge() {
    Physiology p;
    p.tractLengthCm   = 19.5f;
    p.pharynxFraction = 0.50f;
    p.larynxHeightCm  = -1.0f;
    p.medianF0Hz      = 85.0f;
    p.openQuotient    = 0.5f;
    return p;
}

} // namespace vocalyx
