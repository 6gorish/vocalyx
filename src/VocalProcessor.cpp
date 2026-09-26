#include "VocalProcessor.h"

#include <algorithm>
#include <cassert>

namespace vocalyx {

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
//
// Everything mutable lives here. Right now it holds only configuration and the
// scratch buffers the engine stages will need; the engine itself arrives in the
// next pass. Keeping the member list here rather than in the header is what
// allows Rubber Band to be added without any caller recompiling.

struct VocalProcessor::Impl {
    // --- set by prepare() ---
    double sampleRate    = 0.0;
    int    numChannels   = 0;
    int    maxBlockSize  = 0;
    Mode   mode          = Mode::Offline;
    bool   prepared      = false;

    // --- set by setParameters() ---
    Parameters params;

    // --- scratch ---
    // One working buffer per channel, each maxBlockSize long, allocated once in
    // prepare() and reused forever after. The pass-through does not need these;
    // the equalizer and breathiness stages will, and allocating them now means
    // process() never has to.
    std::vector<std::vector<float>> scratch;

    // Channel pointer array handed to the engine. Held as a member so process()
    // does not build one on every call.
    std::vector<float*> scratchPtrs;

    void allocate() {
        scratch.assign(static_cast<size_t>(numChannels),
                       std::vector<float>(static_cast<size_t>(maxBlockSize), 0.0f));

        scratchPtrs.resize(static_cast<size_t>(numChannels));
        for (size_t c = 0; c < scratch.size(); ++c)
            scratchPtrs[c] = scratch[c].data();
    }

    void clearScratch() {
        for (auto& channel : scratch)
            std::fill(channel.begin(), channel.end(), 0.0f);
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

VocalProcessor::VocalProcessor()
    : impl(std::make_unique<Impl>()) {}

// Defined here, where Impl is complete, rather than in the header.
VocalProcessor::~VocalProcessor() = default;

VocalProcessor::VocalProcessor(VocalProcessor&&) noexcept = default;
VocalProcessor& VocalProcessor::operator=(VocalProcessor&&) noexcept = default;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void VocalProcessor::prepare(double sampleRate, int numChannels, int maxBlockSize, Mode mode) {
    assert(sampleRate > 0.0);
    assert(numChannels > 0);
    assert(maxBlockSize > 0);

    impl->sampleRate   = sampleRate;
    impl->numChannels  = numChannels;
    impl->maxBlockSize = maxBlockSize;
    impl->mode         = mode;

    impl->allocate();

    // The engine will be constructed here once Rubber Band is linked, using
    // sampleRate, numChannels and mode. Any allocation it needs happens now,
    // so that process() can stay allocation-free.

    impl->prepared = true;
}

void VocalProcessor::reset() {
    impl->clearScratch();

    // Engine history is discarded here once there is an engine.
}

void VocalProcessor::setParameters(const Parameters& params) {
    // Plain assignment for now. If a control thread ever calls this while the
    // audio thread is inside process(), the copy needs to become an atomic
    // handoff — a double-buffered Parameters with an atomic index, or a
    // single-producer queue. Worth doing when the interface arrives, and
    // needless before then, since the offline rig renders and plays from
    // buffers rather than calling both at once.
    impl->params = params;
}

Parameters VocalProcessor::getParameters() const {
    return impl->params;
}

// ---------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------

int VocalProcessor::process(const float* const* input,
                            float* const* output,
                            int numChannels,
                            int numFrames) {
    assert(impl->prepared && "prepare() must run before process()");
    assert(input != nullptr && output != nullptr);
    assert(numFrames <= impl->maxBlockSize && "numFrames exceeds the prepared maxBlockSize");

    if (!impl->prepared || numFrames <= 0)
        return 0;

    // Tolerate being handed fewer channels than prepared for; more than
    // prepared for would read past the scratch buffers, so clamp.
    const int channels = std::min(numChannels, impl->numChannels);

    // --- pass-through ---
    //
    // The engine, equalizer and breathiness stages slot in between here and the
    // copy below. Until then the class is a verified-correct piece of plumbing:
    // if the loop sounds wrong now, the problem is in the shell, not in here.
    for (int c = 0; c < channels; ++c) {
        assert(input[c] != nullptr && output[c] != nullptr);

        if (input[c] != output[c])
            std::copy(input[c], input[c] + numFrames, output[c]);
    }

    // Any channel the caller asked for beyond what we prepared gets silence,
    // which is more useful than leaving whatever the host left in the buffer.
    for (int c = channels; c < numChannels; ++c) {
        if (output[c] != nullptr)
            std::fill(output[c], output[c] + numFrames, 0.0f);
    }

    return numFrames;
}

void VocalProcessor::processBuffer(const std::vector<std::vector<float>>& input,
                                   std::vector<std::vector<float>>& output) {
    if (input.empty()) {
        output.clear();
        return;
    }

    const int channels  = static_cast<int>(input.size());
    const int numFrames = static_cast<int>(input[0].size());

    // Every channel must be the same length.
    for (const auto& channel : input)
        assert(static_cast<int>(channel.size()) == numFrames);

    // Offline work renders a whole clip in one call, so the clip length is the
    // block size. Re-prepare when the shape changes.
    if (!impl->prepared
        || impl->numChannels != channels
        || impl->maxBlockSize < numFrames) {
        prepare(impl->sampleRate > 0.0 ? impl->sampleRate : 44100.0,
                channels,
                numFrames,
                impl->mode);
    } else {
        reset();
    }

    output.assign(static_cast<size_t>(channels),
                  std::vector<float>(static_cast<size_t>(numFrames), 0.0f));

    // Build the pointer arrays the raw-pointer overload expects. Allocating
    // here is fine; this function is never called from an audio callback.
    std::vector<const float*> inPtrs(static_cast<size_t>(channels));
    std::vector<float*>       outPtrs(static_cast<size_t>(channels));

    for (size_t c = 0; c < static_cast<size_t>(channels); ++c) {
        inPtrs[c]  = input[c].data();
        outPtrs[c] = output[c].data();
    }

    const int written = process(inPtrs.data(), outPtrs.data(), channels, numFrames);

    // In Offline mode the engine returns the whole clip. Trim if it returns
    // less, so the caller never sees trailing silence it did not ask for.
    if (written < numFrames) {
        for (auto& channel : output)
            channel.resize(static_cast<size_t>(std::max(written, 0)));
    }
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

int VocalProcessor::latencyInSamples() const {
    // Offline processing reports no latency, and the pass-through has none in
    // either mode. Realtime latency comes from the engine's window size once
    // the engine exists.
    return 0;
}

double VocalProcessor::sampleRate() const { return impl->sampleRate; }
int    VocalProcessor::numChannels() const { return impl->numChannels; }
Mode   VocalProcessor::mode() const { return impl->mode; }
bool   VocalProcessor::isPrepared() const { return impl->prepared; }

} // namespace vocalyx
