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

void WorldEngine::clear() {
    *impl = Impl{};
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
    harvestOption.f0_floor     = 60.0;

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
    errorOut.clear();

    if (!hasAnalysis()) {
        errorOut = "analyze before rendering";
        return false;
    }

    const int frames = impl->frames;
    const int bins   = impl->bins;
    const double binHz = impl->sampleRate / impl->fftSize;

    impl->modifiedSpectrogram.assign(static_cast<size_t>(frames),
                                     std::vector<double>(static_cast<size_t>(bins), 0.0));
    impl->modifiedAperiodicity.assign(static_cast<size_t>(frames),
                                      std::vector<double>(static_cast<size_t>(bins), 0.0));
    impl->modifiedF0 = impl->f0;

    Rng rng;

    // --- fundamental frequency: ratio, then perturbation ---
    for (int i = 0; i < frames; ++i) {
        if (impl->modifiedF0[static_cast<size_t>(i)] <= 0.0)
            continue;

        impl->modifiedF0[static_cast<size_t>(i)] *= acoustics.pitchRatio;

        if (acoustics.jitterFraction > 0.0f)
            impl->modifiedF0[static_cast<size_t>(i)] *=
                1.0 + acoustics.jitterFraction * rng.next();
    }

    // --- envelope: warp, tilt, nasal coupling, frame gain ---
    for (int i = 0; i < frames; ++i) {
        const auto& sourceFrame = impl->spectrogram[static_cast<size_t>(i)];
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
    }

    // --- aperiodicity: aspiration raises the noise floor, mostly up high ---
    for (int i = 0; i < frames; ++i) {
        const auto& sourceFrame = impl->aperiodicity[static_cast<size_t>(i)];
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
    }

    // --- synthesis ---
    impl->synthesized.assign(static_cast<size_t>(impl->sourceLength), 0.0);

    {
        auto spectrumPtrs     = impl->pointers(impl->modifiedSpectrogram);
        auto aperiodicityPtrs = impl->pointers(impl->modifiedAperiodicity);

        Synthesis(impl->modifiedF0.data(), frames,
                  spectrumPtrs.data(), aperiodicityPtrs.data(),
                  impl->fftSize, kFramePeriodMs,
                  static_cast<int>(impl->sampleRate),
                  impl->sourceLength, impl->synthesized.data());
    }

    const int channels = std::max(1, outputChannels);
    output.assign(static_cast<size_t>(channels),
                  std::vector<float>(static_cast<size_t>(impl->sourceLength), 0.0f));

    for (int n = 0; n < impl->sourceLength; ++n) {
        const float sample = static_cast<float>(impl->synthesized[static_cast<size_t>(n)]);

        for (int c = 0; c < channels; ++c)
            output[static_cast<size_t>(c)][static_cast<size_t>(n)] = sample;
    }

    return true;
}

} // namespace vocalyx
