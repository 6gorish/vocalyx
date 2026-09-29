#pragma once

// The physiological parameter model.
//
// Everything here describes a speaker's anatomy and phonation, in units a
// person can reason about: centimetres of vocal tract, how far the velum is
// open, how breathy the folds are. The engine never sees these. deriveAcoustics
// turns them into the acoustic quantities an engine can apply — a frequency
// warp curve, a pitch ratio, a spectral tilt, a nasal pole and zero.
//
// That separation is the point. The interface and the visualization speak
// physiology; the signal processing speaks frequencies and ratios; each side
// can change without disturbing the other.
//
// A caution about the constants below: they come from the speech literature's
// typical adult values, and every one of them is an approximation over a wide
// distribution. They are a starting point to tune by ear, not measurements of
// any particular person.

#include <vector>

namespace vocalyx {

// ---------------------------------------------------------------------------
// What the recording already is
// ---------------------------------------------------------------------------

// Every transformation is a ratio between the speaker who was recorded and the
// speaker being modelled, so the original has to be described too. These can be
// estimated from the recording later; for now they are what you tell the
// application about your own voice.
struct SourceSpeaker {
    float tractLengthCm = 17.0f;   // adult men average roughly 17-18, women 14-15
    float medianF0Hz    = 110.0f;  // the speaker's habitual pitch
};

// ---------------------------------------------------------------------------
// The speaker being modelled
// ---------------------------------------------------------------------------

struct Physiology {
    // --- tract geometry ---

    // Overall length from glottis to lips. Formant frequencies scale inversely
    // with it: a shorter tract raises every resonance.
    float tractLengthCm = 15.0f;          // useful range roughly 12 to 20

    // How much of that length is pharynx rather than mouth. This is the axis
    // uniform scaling gets wrong: the sexes differ more in pharynx length than
    // in oral cavity length, which is why a voice shifted by one ratio across
    // the whole spectrum still sounds processed.
    float pharynxFraction = 0.45f;        // roughly 0.40 to 0.55

    // Raising or lowering the larynx changes pharynx length alone. Negative
    // lowers it, which is the darkening a singer does deliberately.
    float larynxHeightCm = 0.0f;          // -1.5 to 1.5

    // Rounded, protruded lips extend the tube and pull the third formant down.
    float lipRounding = 0.0f;             // 0 spread, 1 fully rounded

    // Opening the velum couples the nasal cavity, which adds a resonance and an
    // antiresonance rather than moving the existing ones.
    float nasality = 0.0f;                // 0 closed, 1 fully open

    // --- source ---

    // Where the folds sit. Larger, longer folds vibrate more slowly.
    float medianF0Hz = 190.0f;            // 70 to 350 covers most adult speech

    // The fraction of each cycle the glottis stays open. High is breathy and
    // soft, low is pressed and buzzy.
    float openQuotient = 0.6f;            // 0.4 pressed to 0.8 breathy

    // Turbulent noise from air escaping through folds that never fully close.
    float aspiration = 0.0f;              // 0 to 1

    // Cycle-to-cycle irregularity in period and amplitude. Small amounts read
    // as age or fatigue; larger amounts as roughness.
    float jitter  = 0.0f;                 // 0 to 1, scaled inside the mapping
    float shimmer = 0.0f;                 // 0 to 1

    // How hard the speaker is pushing. Raises the first formant slightly and
    // flattens the source's spectral slope, which is most of what makes a
    // loud voice sound loud even at the same playback level.
    float effort = 0.0f;                  // -1 relaxed, 0 conversational, 1 loud
};

// ---------------------------------------------------------------------------
// What an engine can actually apply
// ---------------------------------------------------------------------------

// One point on the frequency warp curve: at sourceHz in the original envelope,
// multiply the frequency by scale. Points are interpolated between, so a
// two-point curve is a uniform shift and more points bend it.
struct WarpPoint {
    float sourceHz = 0.0f;
    float scale    = 1.0f;
};

struct Acoustics {
    // Spectral envelope warping, low frequency to high.
    std::vector<WarpPoint> warp;

    // Multiplier on the fundamental. 1.0 leaves pitch alone.
    float pitchRatio = 1.0f;

    // Change to the source's spectral slope, in decibels per octave. Negative
    // is darker and breathier, positive brighter and more pressed.
    float tiltChangeDbPerOctave = 0.0f;

    // Breath noise, shaped by the signal's own envelope.
    float aspirationAmount = 0.0f;

    // Nasal coupling, as a resonance and the antiresonance below it.
    float nasalAmount   = 0.0f;
    float nasalPoleHz   = 1000.0f;
    float nasalZeroHz   = 700.0f;

    // Period and amplitude perturbation, as fractions.
    float jitterFraction  = 0.0f;
    float shimmerFraction = 0.0f;
};

// Turns a target anatomy into the acoustic quantities that realize it, relative
// to the speaker actually recorded.
Acoustics deriveAcoustics(const Physiology& target, const SourceSpeaker& source);

// Named starting points. Presets are anatomies, so they carry across whatever
// engine is underneath.
Physiology presetNeutralMale();
Physiology presetNeutralFemale();
Physiology presetChild();
Physiology presetLarge();      // long tract, low larynx, heavy folds

} // namespace vocalyx
