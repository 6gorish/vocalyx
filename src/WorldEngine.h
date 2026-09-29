#pragma once

// The WORLD engine.
//
// WORLD (Masanori Morise) decomposes speech into three things: a fundamental
// frequency track, a spectral envelope per frame, and an aperiodicity measure
// per frame that says how much of each band is noise rather than periodic
// vibration. Those three resynthesize back into speech, and modifying them in
// between is where every physiological parameter takes effect.
//
// The split that matters for interactivity: analysis is slow and depends only
// on the recording, while synthesis is faster and depends on the parameters.
// So analysis runs once when a clip loads, and dragging a slider re-runs only
// the modification and synthesis.
//
// WORLD is monophonic. A stereo file is mixed down before analysis and the
// result is duplicated across output channels. For a voice that is the right
// trade; for anything stereo-critical it is not.

#include "Physiology.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace vocalyx {

class WorldEngine {
public:
    WorldEngine();
    ~WorldEngine();

    WorldEngine(const WorldEngine&) = delete;
    WorldEngine& operator=(const WorldEngine&) = delete;

    // Runs fundamental frequency estimation, envelope estimation and
    // aperiodicity estimation, and keeps the results. Slow: expect a second or
    // more for a few seconds of audio. Call once per clip, off the audio
    // thread.
    //
    // Returns false and fills errorOut if the input is unusable.
    bool analyze(const std::vector<std::vector<float>>& input,
                 double sampleRate,
                 std::string& errorOut);

    // Applies the acoustics to the stored analysis and resynthesizes. Fast
    // enough to sit behind a slider on short clips. Requires a successful
    // analyze first.
    bool render(const Acoustics& acoustics,
                int outputChannels,
                std::vector<std::vector<float>>& output,
                std::string& errorOut);

    bool hasAnalysis() const;
    void clear();

    // The measured median fundamental frequency of the analyzed clip, which is
    // what SourceSpeaker needs so pitch ratios are relative to the real voice
    // rather than a guess.
    float measuredMedianF0Hz() const;

    // Frame spacing in milliseconds, exposed because the display will want it.
    double framePeriodMs() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace vocalyx
