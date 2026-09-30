#pragma once

// The filter engine.
//
// Everything Vocalyx does to the spectral envelope — warping formants, tilting
// the spectrum, adding a nasal pole, blending toward a conversion — can be
// expressed as a gain curve that varies over time. Applying that curve to the
// original signal is a different proposition from rebuilding the signal out of
// analysis data: the excitation, the phase and all the fine structure of the
// recording survive untouched, and at identity settings the output is the
// input.
//
// That is what this does. Short-time Fourier transform, estimate the envelope
// of each frame, work out the envelope we want, divide one by the other, apply
// the ratio to the frame's magnitudes, and overlap-add. The phase is never
// modified.
//
// What it cannot do: change pitch, blend pitch tracks, or add jitter. Those
// require resynthesis, which is what WorldEngine is for. The two are meant to
// be used together — pitch through a pitch shifter, envelope through here.

#include "Physiology.h"

#include <memory>
#include <string>
#include <vector>

namespace vocalyx {

class FilterEngine {
public:
    FilterEngine();
    ~FilterEngine();

    FilterEngine(const FilterEngine&) = delete;
    FilterEngine& operator=(const FilterEngine&) = delete;

    // Allocates the transforms and windows. Call when the sample rate changes.
    void prepare(double sampleRate);

    // Applies the acoustics to input, writing output. Channels are processed
    // independently and keep their own phase, so a stereo recording stays
    // stereo. Returns false only on unusable input.
    bool process(const std::vector<std::vector<float>>& input,
                 const Acoustics& acoustics,
                 std::vector<std::vector<float>>& output,
                 std::string& errorOut);

    // An envelope to blend toward, taken from a WORLD analysis of a second
    // recording of the same performance. Frame rate and resolution are
    // converted internally, so the two analyses need not match.
    void setMorphEnvelope(const std::vector<std::vector<double>>& spectrogram,
                          int fftSize,
                          double sampleRate,
                          double framePeriodMs);

    void clearMorphEnvelope();
    bool hasMorphEnvelope() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace vocalyx
