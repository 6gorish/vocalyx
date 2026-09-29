#include "VocalProcessor.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>

namespace vocalyx {

namespace {

constexpr double kPi = 3.14159265358979323846;

double semitonesToRatio(float semitones) {
    return std::pow(2.0, static_cast<double>(semitones) / 12.0);
}

// --- biquad, peaking form from the RBJ cookbook -----------------------------

struct BiquadCoeffs {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

struct BiquadState {
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    void clear() { x1 = x2 = y1 = y2 = 0.0f; }

    float process(const BiquadCoeffs& c, float x) {
        const float y = c.b0 * x + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }
};

BiquadCoeffs makePeaking(double sampleRate, double centerHz, double gainDb, double q) {
    BiquadCoeffs c;

    // Out-of-range settings pass audio through untouched rather than blowing up.
    if (sampleRate <= 0.0 || centerHz <= 0.0 || centerHz >= sampleRate * 0.5 || q <= 0.0)
        return c;

    const double A     = std::pow(10.0, gainDb / 40.0);
    const double w0    = 2.0 * kPi * centerHz / sampleRate;
    const double cosw0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);

    const double b0 =  1.0 + alpha * A;
    const double b1 = -2.0 * cosw0;
    const double b2 =  1.0 - alpha * A;
    const double a0 =  1.0 + alpha / A;
    const double a1 = -2.0 * cosw0;
    const double a2 =  1.0 - alpha / A;

    c.b0 = static_cast<float>(b0 / a0);
    c.b1 = static_cast<float>(b1 / a0);
    c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0);
    c.a2 = static_cast<float>(a2 / a0);
    return c;
}

// --- noise ------------------------------------------------------------------

// xorshift32: cheap, deterministic, and good enough for breath noise.
struct NoiseSource {
    std::uint32_t state = 0x9E3779B9u;

    float next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<std::int32_t>(state)) * (1.0f / 2147483648.0f);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct VocalProcessor::Impl {
    // --- configuration ---
    double sampleRate   = 0.0;
    int    numChannels  = 0;
    int    maxBlockSize = 0;
    Mode   mode         = Mode::Offline;
    bool   prepared     = false;

    Parameters params;

    // --- engine ---
    std::unique_ptr<RubberBand::RubberBandStretcher> engine;

    // --- post stages ---
    // eqState[channel][band]
    std::vector<std::vector<BiquadState>> eqState;
    std::vector<BiquadCoeffs>             eqCoeffs;

    std::vector<float> breathEnvelope;   // one follower per channel
    std::vector<float> breathHighpass;   // one-pole state per channel
    NoiseSource        noise;

    // --- scratch ---
    std::vector<float*> retrievePtrs;

    void createEngine() {
        if (sampleRate <= 0.0 || numChannels < 1) {
            engine.reset();
            return;
        }

        using RB = RubberBand::RubberBandStretcher;

        int options = RB::OptionEngineFiner          // R3, the better-sounding engine
                    | RB::OptionFormantPreserved     // formants controlled separately
                    | RB::OptionChannelsTogether;    // keep a stereo image coherent

        options |= (mode == Mode::Offline) ? RB::OptionProcessOffline
                                           : RB::OptionProcessRealTime;

        engine = std::make_unique<RB>(static_cast<size_t>(sampleRate),
                                      static_cast<size_t>(numChannels),
                                      options,
                                      1.0,    // time ratio: length unchanged
                                      1.0);   // pitch scale, set per render below

        if (maxBlockSize > 0)
            engine->setMaxProcessSize(static_cast<size_t>(maxBlockSize));

        retrievePtrs.resize(static_cast<size_t>(numChannels));
    }

    // Recomputes filter coefficients and resizes filter state. Allocates, so it
    // is called from setParameters and prepare, never from process.
    void updatePostStages() {
        eqCoeffs.clear();
        eqCoeffs.reserve(params.eqBands.size());

        for (const auto& band : params.eqBands)
            eqCoeffs.push_back(makePeaking(sampleRate, band.centerHz, band.gainDb, band.q));

        eqState.assign(static_cast<size_t>(std::max(numChannels, 0)),
                       std::vector<BiquadState>(eqCoeffs.size()));

        breathEnvelope.assign(static_cast<size_t>(std::max(numChannels, 0)), 0.0f);
        breathHighpass.assign(static_cast<size_t>(std::max(numChannels, 0)), 0.0f);
    }

    void clearState() {
        for (auto& channel : eqState)
            for (auto& band : channel)
                band.clear();

        std::fill(breathEnvelope.begin(), breathEnvelope.end(), 0.0f);
        std::fill(breathHighpass.begin(), breathHighpass.end(), 0.0f);
    }

    // Equalization and breathiness, applied in place after the engine.
    void applyPostStages(float* const* audio, int channels, int frames) {
        if (frames <= 0)
            return;

        const int usable = std::min(channels, static_cast<int>(eqState.size()));

        // --- equalizer ---
        for (int c = 0; c < usable; ++c) {
            auto& states = eqState[static_cast<size_t>(c)];
            float* data  = audio[c];

            for (size_t b = 0; b < eqCoeffs.size(); ++b) {
                const BiquadCoeffs& coeffs = eqCoeffs[b];
                BiquadState& state = states[b];

                for (int f = 0; f < frames; ++f)
                    data[f] = state.process(coeffs, data[f]);
            }
        }

        // --- breathiness ---
        // Noise, high-passed and shaped by the signal's own envelope, so it
        // arrives with the voice instead of hissing underneath silence.
        if (params.breathiness > 0.0001f && sampleRate > 0.0) {
            const float amount = params.breathiness;

            // One-pole highpass coefficient for the shelf frequency.
            const double rc = 1.0 / (2.0 * kPi * std::max(20.0f, params.breathinessShelfHz));
            const double dt = 1.0 / sampleRate;
            const float  hp = static_cast<float>(rc / (rc + dt));

            const float attack  = 0.01f;    // fast enough to follow syllables
            const float release = 0.0005f;

            for (int c = 0; c < usable; ++c) {
                float* data  = audio[c];
                float& env   = breathEnvelope[static_cast<size_t>(c)];
                float& hpOut = breathHighpass[static_cast<size_t>(c)];
                float  prevNoise = 0.0f;

                for (int f = 0; f < frames; ++f) {
                    const float magnitude = std::fabs(data[f]);
                    const float rate = (magnitude > env) ? attack : release;
                    env += rate * (magnitude - env);

                    const float raw = noise.next();
                    hpOut = hp * (hpOut + raw - prevNoise);
                    prevNoise = raw;

                    data[f] += hpOut * env * amount;
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

VocalProcessor::VocalProcessor()
    : impl(std::make_unique<Impl>()) {}

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

    impl->createEngine();
    impl->updatePostStages();

    impl->prepared = (impl->engine != nullptr);
}

void VocalProcessor::reset() {
    if (impl->engine)
        impl->engine->reset();

    impl->clearState();
}

void VocalProcessor::setParameters(const Parameters& params) {
    const bool bandsChanged = params.eqBands.size() != impl->params.eqBands.size();

    impl->params = params;

    // Coefficients depend on the parameters, and the state array on the band
    // count. Recomputing here keeps process() free of both.
    impl->updatePostStages();

    if (!bandsChanged)
        impl->clearState();
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

    if (!impl->prepared || numFrames <= 0)
        return 0;

    const int channels = std::min(numChannels, impl->numChannels);

    // No engine means pass-through, which keeps the application audible even if
    // Rubber Band failed to construct.
    if (!impl->engine) {
        for (int c = 0; c < channels; ++c)
            if (input[c] != output[c])
                std::copy(input[c], input[c] + numFrames, output[c]);
        return numFrames;
    }

    auto& engine = *impl->engine;

    engine.setPitchScale(semitonesToRatio(impl->params.pitchSemitones));
    engine.setFormantScale(semitonesToRatio(impl->params.formantSemitones));

    int written = 0;

    if (impl->mode == Mode::Offline) {
        // Offline: the engine sees the whole clip before committing to output,
        // which is why it sounds better than the streaming path.
        engine.reset();
        engine.setMaxProcessSize(static_cast<size_t>(numFrames));

        engine.study(input, static_cast<size_t>(numFrames), true);
        engine.process(input, static_cast<size_t>(numFrames), true);

        while (written < numFrames) {
            const int available = engine.available();
            if (available <= 0)
                break;                      // -1 means everything has been retrieved

            for (int c = 0; c < channels; ++c)
                impl->retrievePtrs[static_cast<size_t>(c)] = output[c] + written;

            const int wanted = std::min(available, numFrames - written);
            const int got = static_cast<int>(
                engine.retrieve(impl->retrievePtrs.data(), static_cast<size_t>(wanted)));

            if (got <= 0)
                break;

            written += got;
        }
    } else {
        // Realtime: feed a block, take whatever is ready. Output lags input by
        // latencyInSamples() until the first window fills.
        engine.process(input, static_cast<size_t>(numFrames), false);

        const int available = engine.available();

        if (available > 0) {
            for (int c = 0; c < channels; ++c)
                impl->retrievePtrs[static_cast<size_t>(c)] = output[c];

            const int wanted = std::min(available, numFrames);
            written = static_cast<int>(
                engine.retrieve(impl->retrievePtrs.data(), static_cast<size_t>(wanted)));
        }

        // Silence whatever the engine could not fill yet.
        for (int c = 0; c < channels; ++c)
            std::fill(output[c] + written, output[c] + numFrames, 0.0f);
    }

    impl->applyPostStages(output, channels, written);

    // Channels beyond what was prepared get silence.
    for (int c = channels; c < numChannels; ++c)
        if (output[c] != nullptr)
            std::fill(output[c], output[c] + numFrames, 0.0f);

    return (impl->mode == Mode::Offline) ? written : numFrames;
}

void VocalProcessor::processBuffer(const std::vector<std::vector<float>>& input,
                                   std::vector<std::vector<float>>& output) {
    if (input.empty()) {
        output.clear();
        return;
    }

    const int channels  = static_cast<int>(input.size());
    const int numFrames = static_cast<int>(input[0].size());

    for (const auto& channel : input)
        assert(static_cast<int>(channel.size()) == numFrames);

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

    std::vector<const float*> inPtrs(static_cast<size_t>(channels));
    std::vector<float*>       outPtrs(static_cast<size_t>(channels));

    for (size_t c = 0; c < static_cast<size_t>(channels); ++c) {
        inPtrs[c]  = input[c].data();
        outPtrs[c] = output[c].data();
    }

    const int written = process(inPtrs.data(), outPtrs.data(), channels, numFrames);

    if (written < numFrames)
        for (auto& channel : output)
            channel.resize(static_cast<size_t>(std::max(written, 0)));
}

// ---------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------

int VocalProcessor::latencyInSamples() const {
    if (!impl->engine || impl->mode == Mode::Offline)
        return 0;

    return static_cast<int>(impl->engine->getStartDelay());
}

double VocalProcessor::sampleRate() const { return impl->sampleRate; }
int    VocalProcessor::numChannels() const { return impl->numChannels; }
Mode   VocalProcessor::mode() const { return impl->mode; }
bool   VocalProcessor::isPrepared() const { return impl->prepared; }

} // namespace vocalyx
