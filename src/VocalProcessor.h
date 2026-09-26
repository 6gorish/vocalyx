#pragma once

// VocalProcessor — the Vocalyx processing core.
//
// Plain C++. No openFrameworks types, no JUCE types, no file I/O. The caller
// owns the audio and hands it over as buffers; this class transforms samples
// and nothing else. That constraint is what lets an openFrameworks app host it
// today and a JUCE plugin host it later without either shell leaking in here.
//
// Rubber Band's headers are deliberately absent from this file. All engine
// state lives behind an opaque Impl pointer (below), so changing engines, or
// linking a second one for preview, touches only VocalProcessor.cpp.

#include <memory>
#include <vector>

namespace vocalyx {

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

// One parametric equalizer band: a peaking filter at centerHz, boosting or
// cutting by gainDb, with q controlling bandwidth (higher q is narrower).
struct EqBand {
    float centerHz = 1000.0f;
    float gainDb   = 0.0f;
    float q        = 1.0f;
};

// Everything the caller can change. Passed by value: it is small, copying it
// is cheap, and value semantics keep the audio thread from reading a struct
// that another thread is halfway through modifying.
struct Parameters {
    // Pitch shift in semitones. Positive raises.
    float pitchSemitones = 4.0f;

    // Formant shift in semitones, independent of pitch. Zero holds the
    // formants where they were, which preserves the speaker's apparent vocal
    // tract size; a value equal to pitchSemitones moves them with the pitch,
    // which is the chipmunk case.
    float formantSemitones = 1.5f;

    // Post-shift equalization, applied in order. Empty means no equalization.
    std::vector<EqBand> eqBands;

    // Blend of high-passed noise, 0.0 to roughly 0.1. Above that it reads as
    // synthetic. Zero disables the stage entirely.
    float breathiness = 0.0f;

    // Frequency above which the breath noise ramps in.
    float breathinessShelfHz = 3000.0f;
};

// ---------------------------------------------------------------------------
// Processing mode
// ---------------------------------------------------------------------------

// Offline sees the whole signal before committing to output and sounds better.
// Realtime meets a block deadline and introduces latency, reported by
// latencyInSamples(). Chosen once, at prepare() time, because the engine is
// configured differently for each and cannot switch midstream.
//
// The openFrameworks rig renders whole clips on a worker thread, so it uses
// Offline. A JUCE plugin would use Realtime. Both run this same class.
enum class Mode {
    Offline,
    Realtime
};

// ---------------------------------------------------------------------------
// VocalProcessor
// ---------------------------------------------------------------------------

class VocalProcessor {
public:
    VocalProcessor();

    // Declared here and defined in the .cpp because Impl is incomplete in this
    // header; the compiler needs to see Impl's definition to destroy it.
    ~VocalProcessor();

    // Engine state is neither copyable nor cheap to duplicate.
    VocalProcessor(const VocalProcessor&) = delete;
    VocalProcessor& operator=(const VocalProcessor&) = delete;
    VocalProcessor(VocalProcessor&&) noexcept;
    VocalProcessor& operator=(VocalProcessor&&) noexcept;

    // Allocates every buffer the engine needs, so process() can run without
    // allocating. Call before the first process() and again whenever the
    // sample rate, channel count, block size or mode changes.
    //
    // maxBlockSize is the largest numFrames a single process() call will be
    // given. In Offline mode that is the whole clip.
    void prepare(double sampleRate, int numChannels, int maxBlockSize, Mode mode);

    // Discards engine history without reallocating. Call on seek, on loop
    // wrap, or between unrelated clips, so the tail of the last one does not
    // bleed into the next.
    void reset();

    // Parameters take effect on the next process() call. Changing pitch or
    // formant mid-stream in Realtime mode is supported and glitch-free;
    // changing them between Offline renders is the normal case.
    void setParameters(const Parameters& params);
    Parameters getParameters() const;

    // The core call. Non-interleaved: input[c] and output[c] each point at
    // numFrames floats for channel c. In-place is permitted (output may equal
    // input). Neither pointer may be null.
    //
    // Allocates nothing and takes no locks, so this is safe to call from an
    // audio callback once prepare() has run.
    //
    // Returns the number of frames actually written to output, which in
    // Realtime mode is zero until the engine has filled its first window.
    int process(const float* const* input,
                float* const* output,
                int numChannels,
                int numFrames);

    // Convenience wrapper for the offline case: transforms a whole clip and
    // resizes output to match. Allocates, so never call this from an audio
    // callback. channels are non-interleaved, one vector per channel.
    void processBuffer(const std::vector<std::vector<float>>& input,
                       std::vector<std::vector<float>>& output);

    // Frames of delay the engine introduces between input and output. Zero in
    // Offline mode. A plugin host needs this to keep tracks aligned.
    int latencyInSamples() const;

    // What prepare() was last given, for callers that need to check whether a
    // re-prepare is required.
    double sampleRate() const;
    int numChannels() const;
    Mode mode() const;
    bool isPrepared() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace vocalyx
