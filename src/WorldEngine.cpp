#include "WorldEngine.h"

#include "world/cheaptrick.h"
#include "world/d4c.h"
#include "world/harvest.h"
#include "world/synthesis.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>

namespace vocalyx {

namespace {

constexpr double kFramePeriodMs = 5.0;
constexpr double kTiltReferenceHz = 1000.0;

// Reads the warp curve at one frequency, interpolating between the points and
// holding the end values beyond them.
float scaleAt(const std::vector<WarpPoint>& warp, float hz) {
    return warpScaleAt(warp, hz);
}

// Magnitude response of one resonance and one antiresonance, used for nasal
// coupling. Both are given generous bandwidths, since a nasal cavity is lossy.
double nasalResponse(double hz, double poleHz, double zeroHz, double amount) {
    if (amount <= 0.0001)
        return 1.0;

    const double poleBandwidth = 300.0;
    const double zeroBandwidth = 200.0;

    const double poleTerm = 1.0 / std::sqrt(1.0 + std::pow((hz - poleHz) / poleBandwidth, 2.0));
    const double zeroTerm = 1.0 - 0.8 / (1.0 + std::pow((hz - zeroHz) / zeroBandwidth, 2.0));

    const double shaped = (1.0 + 1.5 * poleTerm) * zeroTerm;

    // Blend toward the unmodified response so the control is continuous.
    return 1.0 + amount * (shaped - 1.0);
}

// A small deterministic generator, so a given jitter setting always sounds the
// same rather than shimmering differently on every render.
struct Rng {
    std::uint32_t state = 0x2545F491u;

    double next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(static_cast<std::int32_t>(state)) / 2147483648.0;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct WorldEngine::Impl {
    double sampleRate = 0.0;
    int    fftSize    = 0;
    int    frames     = 0;
    int    bins       = 0;
    int    sourceLength = 0;

    std::vector<double> mono;                 // the analyzed signal
    std::vector<double> temporalPositions;
    std::vector<double> f0;

    // [frame][bin], stored flat with a pointer array for WORLD's interface.
    std::vector<std::vector<double>> spectrogram;
    std::vector<std::vector<double>> aperiodicity;

    // Scratch reused by render, so repeated renders do not reallocate.
    std::vector<std::vector<double>> modifiedSpectrogram;
    std::vector<std::vector<double>> modifiedAperiodicity;
    std::vector<double>              modifiedF0;
    std::vector<double>              synthesized;

    // A second analysis of the same performance to blend toward.
    std::vector<std::vector<double>> morphSpectrogram;
    std::vector<std::vector<double>> morphAperiodicity;
    std::vector<double>              morphF0;
    bool                             morphReady = false;

    float medianF0 = 0.0f;

    std::vector<double*> pointers(std::vector<std::vector<double>>& rows) {
        std::vector<double*> ptrs(rows.size());
        for (size_t i = 0; i < rows.size(); ++i)
            ptrs[i] = rows[i].data();
        return ptrs;
    }
};

WorldEngine::WorldEngine() : impl(std::make_unique<Impl>()) {}
WorldEngine::~WorldEngine() = default;

bool WorldEngine::hasAnalysis() const { return impl->frames > 0; }
float WorldEngine::measuredMedianF0Hz() const { return impl->medianF0; }
double WorldEngine::framePeriodMs() const { return kFramePeriodMs; }

const std::vector<std::vector<double>>& WorldEngine::spectrogram() const { return impl->spectrogram; }
const std::vector<double>& WorldEngine::fundamentalTrack() const { return impl->f0; }
int WorldEngine::fftSize() const { return impl->fftSize; }
double WorldEngine::sampleRate() const { return impl->sampleRate; }
int WorldEngine::analysisFrameCount() const { return impl->frames; }

int WorldEngine::samplesPerFrame() const {
    return static_cast<int>(std::round(kFramePeriodMs * impl->sampleRate / 1000.0));
}

void WorldEngine::clear() {
    *impl = Impl{};
}

// ---------------------------------------------------------------------------
// Morph target
// ---------------------------------------------------------------------------

bool WorldEngine::setMorphTarget(const WorldEngine& other, std::string& errorOut) {
    errorOut.clear();

    if (!hasAnalysis()) {
        errorOut = "analyze the source before attaching a morph target";
        return false;
    }

    if (!other.hasAnalysis()) {
        errorOut = "the morph target has no analysis";
        return false;
    }

    if (other.impl->fftSize != impl->fftSize) {
        errorOut = "the two recordings analyzed at different resolutions; "
                   "they need the same sample rate";
        return false;
    }

    impl->morphSpectrogram  = other.impl->spectrogram;
    impl->morphAperiodicity = other.impl->aperiodicity;
    impl->morphF0           = other.impl->f0;
    impl->morphReady        = !impl->morphSpectrogram.empty();

    return impl->morphReady;
}

void WorldEngine::clearMorphTarget() {
    impl->morphSpectrogram.clear();
    impl->morphAperiodicity.clear();
    impl->morphF0.clear();
    impl->morphReady = false;
}

bool WorldEngine::hasMorphTarget() const {
    return impl->morphReady;
}

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

bool WorldEngine::analyze(const std::vector<std::vector<float>>& input,
                          double sampleRate,
                          std::string& errorOut) {
    errorOut.clear();
    clear();

    if (input.empty() || input[0].empty() || sampleRate <= 0.0) {
        errorOut = "nothing to analyze";
        return false;
    }

    const int channels = static_cast<int>(input.size());
    const int length   = static_cast<int>(input[0].size());

    // Mix to mono. WORLD models one voice.
    impl->mono.assign(static_cast<size_t>(length), 0.0);

    for (int c = 0; c < channels; ++c)
        for (int n = 0; n < length; ++n)
            impl->mono[static_cast<size_t>(n)] += static_cast<double>(input[static_cast<size_t>(c)][static_cast<size_t>(n)]);

    if (channels > 1) {
        const double gain = 1.0 / channels;
        for (auto& sample : impl->mono)
            sample *= gain;
    }

    impl->sampleRate   = sampleRate;
    impl->sourceLength = length;

    // --- fundamental frequency ---
    HarvestOption harvestOption{};
    InitializeHarvestOption(&harvestOption);
    harvestOption.frame_period = kFramePeriodMs;

    // The floor sets how long an analysis window has to be. Putting it far
    // below the speaker's actual pitch lengthens the window and smears
    // consonants, so 71 Hz — WORLD's own default — suits adult speech better
    // than the 60 this used before. A very low voice would need it lowered.
    harvestOption.f0_floor = 71.0;
    harvestOption.f0_ceil  = 500.0;

    const int frames = GetSamplesForHarvest(static_cast<int>(sampleRate), length, kFramePeriodMs);
    if (frames < 1) {
        errorOut = "clip too short to analyze";
        return false;
    }

    impl->frames = frames;
    impl->temporalPositions.assign(static_cast<size_t>(frames), 0.0);
    impl->f0.assign(static_cast<size_t>(frames), 0.0);

    Harvest(impl->mono.data(), length, static_cast<int>(sampleRate), &harvestOption,
            impl->temporalPositions.data(), impl->f0.data());

    // --- spectral envelope ---
    CheapTrickOption cheapTrickOption{};
    InitializeCheapTrickOption(static_cast<int>(sampleRate), &cheapTrickOption);
    cheapTrickOption.f0_floor = harvestOption.f0_floor;
    cheapTrickOption.fft_size = GetFFTSizeForCheapTrick(static_cast<int>(sampleRate), &cheapTrickOption);

    impl->fftSize = cheapTrickOption.fft_size;
    impl->bins    = impl->fftSize / 2 + 1;

    impl->spectrogram.assign(static_cast<size_t>(frames),
                             std::vector<double>(static_cast<size_t>(impl->bins), 0.0));

    {
        auto ptrs = impl->pointers(impl->spectrogram);
        CheapTrick(impl->mono.data(), length, static_cast<int>(sampleRate),
                   impl->temporalPositions.data(), impl->f0.data(), frames,
                   &cheapTrickOption, ptrs.data());
    }

    // --- aperiodicity ---
    D4COption d4cOption{};
    InitializeD4COption(&d4cOption);

    impl->aperiodicity.assign(static_cast<size_t>(frames),
                              std::vector<double>(static_cast<size_t>(impl->bins), 0.0));

    {
        auto ptrs = impl->pointers(impl->aperiodicity);
        D4C(impl->mono.data(), length, static_cast<int>(sampleRate),
            impl->temporalPositions.data(), impl->f0.data(), frames,
            impl->fftSize, &d4cOption, ptrs.data());
    }

    // --- median of the voiced frames, for SourceSpeaker ---
    std::vector<double> voiced;
    voiced.reserve(static_cast<size_t>(frames));

    for (double value : impl->f0)
        if (value > 0.0)
            voiced.push_back(value);

    if (!voiced.empty()) {
        const size_t middle = voiced.size() / 2;
        std::nth_element(voiced.begin(), voiced.begin() + middle, voiced.end());
        impl->medianF0 = static_cast<float>(voiced[middle]);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Modification and synthesis
// ---------------------------------------------------------------------------

bool WorldEngine::render(const Acoustics& acoustics,
                         int outputChannels,
                         std::vector<std::vector<float>>& output,
                         std::string& errorOut) {
    return renderRange(acoustics, 0, impl->frames, outputChannels, output, errorOut);
}

bool WorldEngine::renderRange(const Acoustics& acoustics,
                              int startFrame,
                              int requestedFrames,
                              int outputChannels,
                              std::vector<std::vector<float>>& output,
                              std::string& errorOut) {
    errorOut.clear();

    if (!hasAnalysis()) {
        errorOut = "analyze before rendering";
        return false;
    }

    const int first = std::clamp(startFrame, 0, impl->frames - 1);
    const int frames = std::clamp(requestedFrames, 1, impl->frames - first);
    const int bins   = impl->bins;
    const double binHz = impl->sampleRate / impl->fftSize;

    impl->modifiedSpectrogram.assign(static_cast<size_t>(frames),
                                     std::vector<double>(static_cast<size_t>(bins), 0.0));
    impl->modifiedAperiodicity.assign(static_cast<size_t>(frames),
                                      std::vector<double>(static_cast<size_t>(bins), 0.0));

    impl->modifiedF0.assign(static_cast<size_t>(frames), 0.0);
    for (int i = 0; i < frames; ++i)
        impl->modifiedF0[static_cast<size_t>(i)] = impl->f0[static_cast<size_t>(first + i)];

    Rng rng;

    // Morphing maps this render's frames onto the second analysis. The two
    // recordings are the same performance, so the mapping is proportional
    // rather than a search for correspondence.
    const bool morphing = impl->morphReady
                       && (acoustics.morphEnvelope > 0.0001f
                        || acoustics.morphAperiodicity > 0.0001f
                        || acoustics.morphF0 > 0.0001f);

    const int morphFrames = static_cast<int>(impl->morphSpectrogram.size());

    auto morphIndex = [&](int i) {
        if (impl->frames <= 1 || morphFrames <= 0)
            return 0;

        const double position = static_cast<double>(first + i) / (impl->frames - 1);
        return std::clamp(static_cast<int>(std::round(position * (morphFrames - 1))),
                          0, morphFrames - 1);
    };

    // --- fundamental frequency: ratio, then perturbation ---
    for (int i = 0; i < frames; ++i) {
        if (impl->modifiedF0[static_cast<size_t>(i)] <= 0.0)
            continue;

        impl->modifiedF0[static_cast<size_t>(i)] *= acoustics.pitchRatio;

        // Blend toward the other recording's pitch, in the logarithmic domain
        // where the midpoint of two pitches is the note between them. Only
        // where both are voiced; a blend against silence is a glitch.
        if (morphing && acoustics.morphF0 > 0.0001f) {
            const double other = impl->morphF0[static_cast<size_t>(morphIndex(i))];

            if (other > 0.0) {
                const double mine = impl->modifiedF0[static_cast<size_t>(i)];
                const double weight = acoustics.morphF0;

                impl->modifiedF0[static_cast<size_t>(i)] =
                    std::exp((1.0 - weight) * std::log(mine) + weight * std::log(other));
            }
        }

        if (acoustics.jitterFraction > 0.0f)
            impl->modifiedF0[static_cast<size_t>(i)] *=
                1.0 + acoustics.jitterFraction * rng.next();
    }

    // --- envelope: warp, tilt, nasal coupling, frame gain ---
    for (int i = 0; i < frames; ++i) {
        const auto& sourceFrame = impl->spectrogram[static_cast<size_t>(first + i)];
        auto&       targetFrame = impl->modifiedSpectrogram[static_cast<size_t>(i)];

        const double frameGain = (acoustics.shimmerFraction > 0.0f)
                               ? 1.0 + acoustics.shimmerFraction * rng.next()
                               : 1.0;

        for (int k = 0; k < bins; ++k) {
            const double hz = k * binHz;

            // Where in the original envelope this output frequency comes from.
            // Dividing by the scale moves a resonance up when the scale is
            // above one.
            const double scale = std::max(0.25, static_cast<double>(scaleAt(acoustics.warp, static_cast<float>(hz))));
            const double sourceHz = hz / scale;

            // Linear interpolation between the two neighbouring source bins.
            const double position = sourceHz / binHz;
            const int    lower    = static_cast<int>(std::floor(position));
            const double fraction = position - lower;

            double value = 0.0;

            if (lower >= 0 && lower + 1 < bins)
                value = sourceFrame[static_cast<size_t>(lower)] * (1.0 - fraction)
                      + sourceFrame[static_cast<size_t>(lower + 1)] * fraction;
            else if (lower >= 0 && lower < bins)
                value = sourceFrame[static_cast<size_t>(lower)];
            else
                value = sourceFrame[static_cast<size_t>(bins - 1)];

            // Spectral tilt, in decibels per octave about a one kilohertz hinge.
            if (std::fabs(acoustics.tiltChangeDbPerOctave) > 0.001f && hz > 20.0) {
                const double octaves = std::log2(hz / kTiltReferenceHz);
                const double gainDb  = acoustics.tiltChangeDbPerOctave * octaves;
                value *= std::pow(10.0, gainDb / 10.0);   // power spectrum, so 10 not 20
            }

            const double nasal = nasalResponse(hz, acoustics.nasalPoleHz,
                                               acoustics.nasalZeroHz, acoustics.nasalAmount);

            targetFrame[static_cast<size_t>(k)] = std::max(1e-16, value * nasal * nasal * frameGain);
        }

        // Blend the envelope toward the other recording's, geometrically. A
        // straight average of two spectra produces peaks belonging to neither
        // voice; interpolating the logarithms moves each resonance toward its
        // counterpart instead.
        if (morphing && acoustics.morphEnvelope > 0.0001f) {
            const auto& other = impl->morphSpectrogram[static_cast<size_t>(morphIndex(i))];
            const double weight = acoustics.morphEnvelope;

            const int shared = std::min(bins, static_cast<int>(other.size()));

            for (int k = 0; k < shared; ++k) {
                const double mine = std::max(1e-16, targetFrame[static_cast<size_t>(k)]);
                const double theirs = std::max(1e-16, other[static_cast<size_t>(k)]);

                targetFrame[static_cast<size_t>(k)] =
                    std::exp((1.0 - weight) * std::log(mine) + weight * std::log(theirs));
            }
        }
    }

    // --- aperiodicity: aspiration raises the noise floor, mostly up high ---
    for (int i = 0; i < frames; ++i) {
        const auto& sourceFrame = impl->aperiodicity[static_cast<size_t>(first + i)];
        auto&       targetFrame = impl->modifiedAperiodicity[static_cast<size_t>(i)];

        for (int k = 0; k < bins; ++k) {
            double value = sourceFrame[static_cast<size_t>(k)];

            if (acoustics.aspirationAmount > 0.0001f) {
                const double hz = k * binHz;

                // Weight rising through the upper mid range, where breath
                // noise actually lives.
                const double weight = 1.0 / (1.0 + std::exp(-(hz - 2500.0) / 800.0));
                value += acoustics.aspirationAmount * weight * (1.0 - value);
            }

            targetFrame[static_cast<size_t>(k)] = std::clamp(value, 0.0, 1.0);
        }

        // Aperiodicity is already a ratio, so a straight average is correct
        // here in a way it is not for the envelope.
        if (morphing && acoustics.morphAperiodicity > 0.0001f) {
            const auto& other = impl->morphAperiodicity[static_cast<size_t>(morphIndex(i))];
            const double weight = acoustics.morphAperiodicity;

            const int shared = std::min(bins, static_cast<int>(other.size()));

            for (int k = 0; k < shared; ++k) {
                targetFrame[static_cast<size_t>(k)] =
                    std::clamp((1.0 - weight) * targetFrame[static_cast<size_t>(k)]
                               + weight * other[static_cast<size_t>(k)],
                               0.0, 1.0);
            }
        }
    }

    // --- synthesis ---
    // The whole clip's length when rendering everything, otherwise the span the
    // requested frames cover.
    const int hop = samplesPerFrame();
    const int length = (frames == impl->frames)
                     ? impl->sourceLength
                     : std::min(frames * hop, impl->sourceLength - first * hop);

    if (length <= 0) {
        errorOut = "empty render range";
        return false;
    }

    impl->synthesized.assign(static_cast<size_t>(length), 0.0);

    {
        auto spectrumPtrs     = impl->pointers(impl->modifiedSpectrogram);
        auto aperiodicityPtrs = impl->pointers(impl->modifiedAperiodicity);

        Synthesis(impl->modifiedF0.data(), frames,
                  spectrumPtrs.data(), aperiodicityPtrs.data(),
                  impl->fftSize, kFramePeriodMs,
                  static_cast<int>(impl->sampleRate),
                  length, impl->synthesized.data());
    }

    const int channels = std::max(1, outputChannels);
    output.assign(static_cast<size_t>(channels),
                  std::vector<float>(static_cast<size_t>(length), 0.0f));

    for (int n = 0; n < length; ++n) {
        const float sample = static_cast<float>(impl->synthesized[static_cast<size_t>(n)]);

        for (int c = 0; c < channels; ++c)
            output[static_cast<size_t>(c)][static_cast<size_t>(n)] = sample;
    }

    return true;
}

} // namespace vocalyx
