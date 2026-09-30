#pragma once

// Formant tracking and speaker measurement.
//
// WORLD's analysis already produces a smooth spectral envelope for every frame,
// with the harmonics removed. The resonances of the vocal tract are the peaks
// of that envelope, so finding them needs no new analysis — only peak picking
// on data we already have.
//
// What the measurements are for:
//   - tract length, so transformations are ratios from a measured baseline
//     rather than from a slider the user guessed at
//   - formant positions, so the warp curve can be anchored where the speaker's
//     resonances actually are
//   - a formant display, which is the whole of the pronunciation-feedback idea
//
// The peak-picking approach here is the simple one. Fitting an all-pole model
// and solving for its roots is more accurate, particularly when two formants
// converge, and is the upgrade path if these numbers prove too jumpy.

#include <array>
#include <cstddef>
#include <vector>

namespace vocalyx {

constexpr int kFormantCount = 4;

struct FormantFrame {
    bool  voiced = false;
    float f0Hz   = 0.0f;

    // Zero means the formant was not found in this frame.
    std::array<float, kFormantCount> frequencyHz{};
    std::array<float, kFormantCount> bandwidthHz{};
    std::array<float, kFormantCount> amplitudeDb{};
};

struct FormantTrack {
    std::vector<FormantFrame> frames;
    double framePeriodMs = 5.0;

    bool empty() const { return frames.empty(); }
};

// What the recording says about the person who made it.
struct SpeakerMeasurements {
    bool  valid = false;

    // From the average spacing between successive formants, which for a tube
    // closed at the glottis is the speed of sound over twice the length.
    float tractLengthCm = 0.0f;

    float medianF0Hz = 0.0f;

    // Median position of each formant across voiced frames, which is roughly
    // the centre of the speaker's vowel space.
    std::array<float, kFormantCount> medianFormantHz{};

    // Slope of the envelope between 500 Hz and 4 kHz. A breathier voice falls
    // off faster. This stands in for open quotient, which cannot be measured
    // from an envelope with the harmonics already removed.
    float spectralTiltDbPerOctave = 0.0f;

    // Cycle-to-cycle variation, as fractions.
    float jitter  = 0.0f;
    float shimmer = 0.0f;
};

// spectrogram is WORLD's output: one power spectrum per frame, each with
// fftSize / 2 + 1 bins. f0 is one value per frame, zero where unvoiced.
FormantTrack trackFormants(const std::vector<std::vector<double>>& spectrogram,
                           const std::vector<double>& f0,
                           double sampleRate,
                           int fftSize,
                           double framePeriodMs);

SpeakerMeasurements measureSpeaker(const FormantTrack& track,
                                   const std::vector<std::vector<double>>& spectrogram,
                                   const std::vector<double>& f0,
                                   double sampleRate,
                                   int fftSize);

} // namespace vocalyx
