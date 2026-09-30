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

    // The same, over a range of analysis frames rather than the whole clip.
    // This is what makes a slider feel live: synthesizing three seconds around
    // the playhead costs a fraction of synthesizing a minute.
    bool renderRange(const Acoustics& acoustics,
                     int startFrame,
                     int frameCount,
                     int outputChannels,
                     std::vector<std::vector<float>>& output,
                     std::string& errorOut);

    // Frames held, and how many samples each frame advances — the conversion
    // between a playhead position and a frame index.
    int analysisFrameCount() const;
    int samplesPerFrame() const;

    bool hasAnalysis() const;
    void clear();

    // --- morph target ---
    // A second analysis of the same performance, usually a neural conversion of
    // it, to blend toward frame by frame. Copied in rather than referenced, so
    // the engine it came from can be reused.
    //
    // Only meaningful when the two recordings say the same words at the same
    // time. Two unrelated recordings blend into a frame-by-frame average of
    // different words.
    bool setMorphTarget(const WorldEngine& other, std::string& errorOut);
    void clearMorphTarget();
    bool hasMorphTarget() const;

    // The measured median fundamental frequency of the analyzed clip, which is
    // what SourceSpeaker needs so pitch ratios are relative to the real voice
    // rather than a guess.
    float measuredMedianF0Hz() const;

    // Frame spacing in milliseconds, exposed because the display will want it.
    double framePeriodMs() const;

    // The analysis itself, for the formant tracker. Valid only while an
    // analysis is held; the references die with the next analyze or clear.
    const std::vector<std::vector<double>>& spectrogram() const;
    const std::vector<double>& fundamentalTrack() const;
    int fftSize() const;
    double sampleRate() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace vocalyx
