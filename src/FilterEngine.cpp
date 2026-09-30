#include "FilterEngine.h"

#include "world/fft.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vocalyx {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 1024 points at 44.1 kHz is about 23 milliseconds: long enough to resolve the
// formants, short enough not to smear consonants.
constexpr int kFrameSize = 1024;
constexpr int kOverlap   = 4;                        // hop is a quarter of the frame
constexpr int kHopSize   = kFrameSize / kOverlap;

// How many cepstral coefficients to keep when smoothing a frame into an
// envelope. Around forty follows the formants while ignoring the harmonics of
// any adult voice.
constexpr int kCepstralOrder = 40;

// No band is moved by more than this, which stops a division by a near-silent
// region from turning noise into a whistle.
constexpr double kMaxGainDb = 18.0;

double clampDb(double db) {
    return std::clamp(db, -kMaxGainDb, kMaxGainDb);
}

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct FilterEngine::Impl {
    double sampleRate = 0.0;
    bool   prepared   = false;

    int bins = kFrameSize / 2 + 1;

    std::vector<double> window;          // Hann, applied on the way in and out
    double              windowNorm = 1.0;

    // --- transform buffers and plans ---
    // fft_complex is double[2], a raw array type, so these cannot live in a
    // vector: arrays are neither copyable nor assignable. A unique_ptr over a
    // new[] allocation gives the same ownership without that requirement.
    std::vector<double>              timeBuffer;
    std::unique_ptr<fft_complex[]>   spectrum;

    std::vector<double>              cepstrumTime;
    std::unique_ptr<fft_complex[]>   cepstrumSpectrum;

    fft_plan forward{};
    fft_plan inverse{};
    fft_plan cepstrumForward{};
    fft_plan cepstrumInverse{};
    bool     plansMade = false;

    // --- working arrays, sized once ---
    std::vector<double> magnitude;
    std::vector<double> logMagnitude;
    std::vector<double> envelopeDb;
    std::vector<double> targetDb;

    // --- morph envelope ---
    std::vector<std::vector<double>> morphSpectrogram;
    int    morphFftSize      = 0;
    double morphSampleRate   = 0.0;
    double morphFramePeriodMs = 5.0;
    bool   morphReady        = false;

    void destroyPlans() {
        if (!plansMade)
            return;

        fft_destroy_plan(forward);
        fft_destroy_plan(inverse);
        fft_destroy_plan(cepstrumForward);
        fft_destroy_plan(cepstrumInverse);
        plansMade = false;
    }

    ~Impl() { destroyPlans(); }

    void allocate() {
        destroyPlans();

        window.resize(kFrameSize);

        // Hann, applied twice — once analysing and once synthesising — which
        // with a quarter-frame hop sums to a constant.
        for (int n = 0; n < kFrameSize; ++n)
            window[static_cast<size_t>(n)] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / kFrameSize));

        double overlapSum = 0.0;
        for (int n = 0; n < kFrameSize; n += kHopSize)
            overlapSum += window[static_cast<size_t>(n)] * window[static_cast<size_t>(n)];

        windowNorm = (overlapSum > 0.0) ? overlapSum : 1.0;

        timeBuffer.assign(static_cast<size_t>(kFrameSize), 0.0);
        spectrum.reset(new fft_complex[static_cast<size_t>(bins)]());

        cepstrumTime.assign(static_cast<size_t>(kFrameSize), 0.0);
        cepstrumSpectrum.reset(new fft_complex[static_cast<size_t>(bins)]());

        magnitude.assign(static_cast<size_t>(bins), 0.0);
        logMagnitude.assign(static_cast<size_t>(bins), 0.0);
        envelopeDb.assign(static_cast<size_t>(bins), 0.0);
        targetDb.assign(static_cast<size_t>(bins), 0.0);

        forward = fft_plan_dft_r2c_1d(kFrameSize, timeBuffer.data(), spectrum.get(), FFT_ESTIMATE);
        inverse = fft_plan_dft_c2r_1d(kFrameSize, spectrum.get(), timeBuffer.data(), FFT_ESTIMATE);

        cepstrumForward = fft_plan_dft_r2c_1d(kFrameSize, cepstrumTime.data(),
                                              cepstrumSpectrum.get(), FFT_ESTIMATE);
        cepstrumInverse = fft_plan_dft_c2r_1d(kFrameSize, cepstrumSpectrum.get(),
                                              cepstrumTime.data(), FFT_ESTIMATE);

        plansMade = true;
    }

    // Smooths one frame's log magnitude into an envelope, by keeping only the
    // low quefrency part of its cepstrum. The harmonics live high up and the
    // formants low, so cutting there separates them.
    void estimateEnvelope() {
        // Spectrum of the log magnitude, which is the cepstrum.
        for (int k = 0; k < bins; ++k)
            cepstrumSpectrum[static_cast<size_t>(k)][0] = logMagnitude[static_cast<size_t>(k)];

        for (int k = 0; k < bins; ++k)
            cepstrumSpectrum[static_cast<size_t>(k)][1] = 0.0;

        fft_execute(cepstrumInverse);

        // Lifter: keep the first coefficients and their mirror, discard the
        // rest.
        for (int n = 0; n < kFrameSize; ++n) {
            const bool keep = (n < kCepstralOrder) || (n > kFrameSize - kCepstralOrder);

            if (!keep)
                cepstrumTime[static_cast<size_t>(n)] = 0.0;
            else
                cepstrumTime[static_cast<size_t>(n)] /= kFrameSize;   // c2r leaves the scale out
        }

        fft_execute(cepstrumForward);

        for (int k = 0; k < bins; ++k)
            envelopeDb[static_cast<size_t>(k)] =
                cepstrumSpectrum[static_cast<size_t>(k)][0] * (20.0 / std::log(10.0));
    }

    // Reads the morph envelope at a given time and frequency, in decibels.
    double morphDbAt(double seconds, double hz) const {
        if (!morphReady || morphSpectrogram.empty())
            return 0.0;

        const int frames = static_cast<int>(morphSpectrogram.size());
        const int frame = std::clamp(static_cast<int>(std::round(seconds * 1000.0 / morphFramePeriodMs)),
                                     0, frames - 1);

        const double binHz = morphSampleRate / morphFftSize;
        const int morphBins = static_cast<int>(morphSpectrogram[static_cast<size_t>(frame)].size());
        const int bin = std::clamp(static_cast<int>(std::round(hz / binHz)), 0, morphBins - 1);

        // WORLD stores power, so ten rather than twenty.
        return 10.0 * std::log10(std::max(1e-20, morphSpectrogram[static_cast<size_t>(frame)][static_cast<size_t>(bin)]));
    }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

FilterEngine::FilterEngine() : impl(std::make_unique<Impl>()) {}
FilterEngine::~FilterEngine() = default;

void FilterEngine::prepare(double sampleRate) {
    impl->sampleRate = sampleRate;
    impl->allocate();
    impl->prepared = sampleRate > 0.0;
}

void FilterEngine::setMorphEnvelope(const std::vector<std::vector<double>>& spectrogram,
                                    int fftSize,
                                    double sampleRate,
                                    double framePeriodMs) {
    impl->morphSpectrogram   = spectrogram;
    impl->morphFftSize       = fftSize;
    impl->morphSampleRate    = sampleRate;
    impl->morphFramePeriodMs = framePeriodMs;
    impl->morphReady         = !spectrogram.empty() && fftSize > 0 && sampleRate > 0.0;
}

void FilterEngine::clearMorphEnvelope() {
    impl->morphSpectrogram.clear();
    impl->morphReady = false;
}

bool FilterEngine::hasMorphEnvelope() const {
    return impl->morphReady;
}

// ---------------------------------------------------------------------------
// Processing
// ---------------------------------------------------------------------------

bool FilterEngine::process(const std::vector<std::vector<float>>& input,
                           const Acoustics& acoustics,
                           std::vector<std::vector<float>>& output,
                           std::string& errorOut) {
    errorOut.clear();

    if (input.empty() || input[0].empty()) {
        errorOut = "nothing to process";
        return false;
    }

    if (!impl->prepared) {
        errorOut = "prepare before processing";
        return false;
    }

    const int channels = static_cast<int>(input.size());
    const int length   = static_cast<int>(input[0].size());
    const int bins     = impl->bins;
    const double binHz = impl->sampleRate / kFrameSize;

    output.assign(static_cast<size_t>(channels),
                  std::vector<float>(static_cast<size_t>(length), 0.0f));

    // Does anything at all need doing? At identity the ratio is one everywhere
    // and the output is the input, so say so directly rather than passing the
    // signal through a transform for nothing.
    bool warpIsIdentity = true;

    for (const WarpPoint& point : acoustics.warp)
        if (std::fabs(point.scale - 1.0f) > 0.001f)
            warpIsIdentity = false;

    const bool nothingToDo = warpIsIdentity
                          && std::fabs(acoustics.tiltChangeDbPerOctave) < 0.01f
                          && acoustics.nasalAmount < 0.001f
                          && acoustics.morphEnvelope < 0.001f;

    if (nothingToDo) {
        output = input;
        return true;
    }

    const double secondsPerHop = static_cast<double>(kHopSize) / impl->sampleRate;

    for (int c = 0; c < channels; ++c) {
        const std::vector<float>& source = input[static_cast<size_t>(c)];
        std::vector<float>& destination = output[static_cast<size_t>(c)];

        std::fill(destination.begin(), destination.end(), 0.0f);

        for (int start = -kFrameSize; start < length; start += kHopSize) {
            // --- window in ---
            for (int n = 0; n < kFrameSize; ++n) {
                const int index = start + n;
                const double sample = (index >= 0 && index < length)
                                    ? static_cast<double>(source[static_cast<size_t>(index)])
                                    : 0.0;

                impl->timeBuffer[static_cast<size_t>(n)] = sample * impl->window[static_cast<size_t>(n)];
            }

            fft_execute(impl->forward);

            // --- magnitudes and the envelope they imply ---
            for (int k = 0; k < bins; ++k) {
                const double real = impl->spectrum[static_cast<size_t>(k)][0];
                const double imaginary = impl->spectrum[static_cast<size_t>(k)][1];

                impl->magnitude[static_cast<size_t>(k)] = std::sqrt(real * real + imaginary * imaginary);
                impl->logMagnitude[static_cast<size_t>(k)] =
                    std::log(std::max(1e-12, impl->magnitude[static_cast<size_t>(k)]));
            }

            impl->estimateEnvelope();

            // --- the envelope we want instead ---
            const double frameSeconds = (start + kFrameSize * 0.5) / impl->sampleRate;

            // The conversion's envelope sits at its own absolute level, so the
            // two are aligned once per frame, across the band where speech
            // energy lives, before their shapes are blended.
            double morphOffsetDb = 0.0;

            if (acoustics.morphEnvelope > 0.001f && impl->morphReady) {
                double mineTotal = 0.0;
                double theirsTotal = 0.0;
                int    counted = 0;

                for (int k = 0; k < bins; ++k) {
                    const double hz = k * binHz;

                    if (hz < 100.0 || hz > 4000.0)
                        continue;

                    mineTotal += impl->envelopeDb[static_cast<size_t>(k)];
                    theirsTotal += impl->morphDbAt(frameSeconds, hz);
                    ++counted;
                }

                if (counted > 0)
                    morphOffsetDb = (mineTotal - theirsTotal) / counted;
            }

            for (int k = 0; k < bins; ++k) {
                const double hz = k * binHz;

                // Warping: the target at this frequency is the envelope from
                // wherever the warp says it came from.
                const double scale = std::max(0.25,
                    static_cast<double>(warpScaleAt(acoustics.warp, static_cast<float>(hz))));

                const double sourceHz = hz / scale;
                const double position = sourceHz / binHz;
                const int    lower = static_cast<int>(std::floor(position));
                const double fraction = position - lower;

                double target = 0.0;

                if (lower >= 0 && lower + 1 < bins)
                    target = impl->envelopeDb[static_cast<size_t>(lower)] * (1.0 - fraction)
                           + impl->envelopeDb[static_cast<size_t>(lower + 1)] * fraction;
                else if (lower >= 0 && lower < bins)
                    target = impl->envelopeDb[static_cast<size_t>(lower)];
                else
                    target = impl->envelopeDb[static_cast<size_t>(bins - 1)];

                // Tilt, about a one kilohertz hinge.
                if (std::fabs(acoustics.tiltChangeDbPerOctave) > 0.001f && hz > 20.0)
                    target += acoustics.tiltChangeDbPerOctave * std::log2(hz / 1000.0);

                // Nasal resonance and antiresonance.
                if (acoustics.nasalAmount > 0.001f) {
                    const double poleTerm = 1.0 / std::sqrt(1.0 + std::pow((hz - acoustics.nasalPoleHz) / 300.0, 2.0));
                    const double zeroTerm = 1.0 - 0.8 / (1.0 + std::pow((hz - acoustics.nasalZeroHz) / 200.0, 2.0));
                    const double shaped = (1.0 + 1.5 * poleTerm) * std::max(0.05, zeroTerm);

                    target += acoustics.nasalAmount * 20.0 * std::log10(shaped);
                }

                // Blend toward the conversion's envelope, in decibels, which is
                // the same geometric interpolation the vocoder path uses.
                if (acoustics.morphEnvelope > 0.001f && impl->morphReady) {
                    const double theirs = impl->morphDbAt(frameSeconds, hz) + morphOffsetDb;

                    target = (1.0 - acoustics.morphEnvelope) * target
                           + acoustics.morphEnvelope * theirs;
                }

                impl->targetDb[static_cast<size_t>(k)] = target;
            }

            // --- ratio, applied to the magnitudes with the phase untouched ---
            for (int k = 0; k < bins; ++k) {
                const double differenceDb = clampDb(impl->targetDb[static_cast<size_t>(k)]
                                                  - impl->envelopeDb[static_cast<size_t>(k)]);

                const double gain = std::pow(10.0, differenceDb / 20.0);

                impl->spectrum[static_cast<size_t>(k)][0] *= gain;
                impl->spectrum[static_cast<size_t>(k)][1] *= gain;
            }

            fft_execute(impl->inverse);

            // --- window out and overlap-add ---
            for (int n = 0; n < kFrameSize; ++n) {
                const int index = start + n;

                if (index < 0 || index >= length)
                    continue;

                const double value = impl->timeBuffer[static_cast<size_t>(n)] / kFrameSize;

                destination[static_cast<size_t>(index)] +=
                    static_cast<float>(value * impl->window[static_cast<size_t>(n)] / impl->windowNorm);
            }
        }
    }

    return true;
}

} // namespace vocalyx
