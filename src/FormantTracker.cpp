#include "FormantTracker.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace vocalyx {

namespace {

constexpr double kSpeedOfSoundCmPerSec = 35000.0;

// Formant analysis is conventionally done on a band-limited signal: four
// formants live below about 5 kHz, and modelling the octave above them wastes
// poles on detail that does not matter. Treating the spectrum up to this
// frequency as if it were the whole band is equivalent to downsampling.
constexpr double kAnalysisCeilingHz = 5500.0;

// Two poles per resonance, plus a few for the overall spectral shape.
constexpr int kLpcOrder = 12;

// Points at which the all-pole response is evaluated for peak picking.
constexpr int kResponsePoints = 700;

// At most this many candidate peaks per frame feed the path search.
constexpr int kMaxCandidates = 7;

struct Candidate {
    float frequencyHz = 0.0f;
    float bandwidthHz = 0.0f;
    float amplitudeDb = 0.0f;
};

// One possible assignment of four candidates to F1 through F4, in order.
struct Assignment {
    std::array<int, kFormantCount> index{};   // indices into the frame's candidates
};

double powerToDb(double power) {
    return 10.0 * std::log10(std::max(1e-20, power));
}

// --- all-pole fitting -------------------------------------------------------

// Autocorrelation from a power spectrum, by direct summation. The spectrum is
// treated as covering zero to kAnalysisCeilingHz, so lag k corresponds to the
// band-limited signal rather than the original sample rate.
void autocorrelationFromSpectrum(const std::vector<double>& frame,
                                 double binHz,
                                 std::vector<double>& out) {
    const int lags = kLpcOrder + 1;
    out.assign(static_cast<size_t>(lags), 0.0);

    const int lastBin = std::min(static_cast<int>(frame.size()) - 1,
                                 static_cast<int>(kAnalysisCeilingHz / binHz));

    for (int k = 0; k < lags; ++k) {
        double total = 0.0;

        for (int b = 0; b <= lastBin; ++b) {
            // Normalized angular frequency, where the ceiling maps to pi.
            const double theta = M_PI * (b * binHz) / kAnalysisCeilingHz;
            total += frame[static_cast<size_t>(b)] * std::cos(theta * k);
        }

        out[static_cast<size_t>(k)] = total;
    }
}

// Levinson-Durbin recursion: solves the autocorrelation for the coefficients of
// the all-pole filter that best predicts the signal.
bool levinsonDurbin(const std::vector<double>& r, std::vector<double>& coefficients) {
    const int order = kLpcOrder;

    if (r.size() < static_cast<size_t>(order + 1) || r[0] <= 1e-20)
        return false;

    coefficients.assign(static_cast<size_t>(order + 1), 0.0);
    coefficients[0] = 1.0;

    std::vector<double> previous(static_cast<size_t>(order + 1), 0.0);
    double error = r[0];

    for (int i = 1; i <= order; ++i) {
        double accumulator = r[static_cast<size_t>(i)];

        for (int j = 1; j < i; ++j)
            accumulator -= coefficients[static_cast<size_t>(j)] * r[static_cast<size_t>(i - j)];

        const double reflection = accumulator / error;

        if (!std::isfinite(reflection) || std::fabs(reflection) >= 1.0)
            return false;   // the recursion has gone unstable; this frame is unusable

        previous = coefficients;
        coefficients[static_cast<size_t>(i)] = reflection;

        for (int j = 1; j < i; ++j)
            coefficients[static_cast<size_t>(j)] = previous[static_cast<size_t>(j)]
                                                 - reflection * previous[static_cast<size_t>(i - j)];

        error *= (1.0 - reflection * reflection);

        if (error <= 1e-20)
            return false;
    }

    return true;
}

// The peaks of the all-pole response. Far fewer and far cleaner than the peaks
// of the raw envelope, which is the point of fitting a model at all.
std::vector<Candidate> findCandidates(const std::vector<double>& frame, double binHz) {
    std::vector<double> r;
    autocorrelationFromSpectrum(frame, binHz, r);

    std::vector<double> coefficients;
    if (!levinsonDurbin(r, coefficients))
        return {};

    // Evaluate one over the magnitude of A, in decibels.
    std::vector<double> response(static_cast<size_t>(kResponsePoints), 0.0);

    for (int p = 0; p < kResponsePoints; ++p) {
        const double theta = M_PI * p / (kResponsePoints - 1);

        double real = 1.0;
        double imaginary = 0.0;

        for (int k = 1; k <= kLpcOrder; ++k) {
            // The recursion above stores prediction coefficients with a sign
            // convention where A(z) = 1 - sum a_k z^-k.
            real      -= coefficients[static_cast<size_t>(k)] * std::cos(theta * k);
            imaginary += coefficients[static_cast<size_t>(k)] * std::sin(theta * k);
        }

        const double magnitude = real * real + imaginary * imaginary;
        response[static_cast<size_t>(p)] = -powerToDb(magnitude);
    }

    const double hzPerPoint = kAnalysisCeilingHz / (kResponsePoints - 1);

    std::vector<Candidate> candidates;

    for (int p = 1; p < kResponsePoints - 1; ++p) {
        const double previous = response[static_cast<size_t>(p - 1)];
        const double current  = response[static_cast<size_t>(p)];
        const double next     = response[static_cast<size_t>(p + 1)];

        if (current <= previous || current <= next)
            continue;

        const double denominator = previous - 2.0 * current + next;
        const double offset = (std::fabs(denominator) < 1e-12)
                            ? 0.0
                            : 0.5 * (previous - next) / denominator;

        Candidate candidate;
        candidate.frequencyHz = static_cast<float>((p + offset) * hzPerPoint);
        candidate.amplitudeDb = static_cast<float>(current);

        if (candidate.frequencyHz < 150.0f || candidate.frequencyHz > kAnalysisCeilingHz - 200.0)
            continue;

        // Bandwidth from the three decibel points on the modelled response.
        const double target = current - 3.0;

        int lower = p;
        while (lower > 0 && response[static_cast<size_t>(lower)] > target)
            --lower;

        int upper = p;
        while (upper < kResponsePoints - 1 && response[static_cast<size_t>(upper)] > target)
            ++upper;

        candidate.bandwidthHz = static_cast<float>((upper - lower) * hzPerPoint);
        candidate.bandwidthHz = std::clamp(candidate.bandwidthHz, 20.0f, 1500.0f);

        candidates.push_back(candidate);
    }

    // Keep the strongest few, in frequency order.
    if (static_cast<int>(candidates.size()) > kMaxCandidates) {
        std::partial_sort(candidates.begin(), candidates.begin() + kMaxCandidates, candidates.end(),
                          [](const Candidate& a, const Candidate& b) {
                              return a.amplitudeDb > b.amplitudeDb;
                          });
        candidates.resize(kMaxCandidates);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.frequencyHz < b.frequencyHz;
              });

    return candidates;
}

// --- path search ------------------------------------------------------------

// Every way of choosing four candidates in increasing frequency order.
std::vector<Assignment> enumerateAssignments(int candidateCount) {
    std::vector<Assignment> out;

    if (candidateCount < kFormantCount)
        return out;

    for (int a = 0; a < candidateCount - 3; ++a)
        for (int b = a + 1; b < candidateCount - 2; ++b)
            for (int c = b + 1; c < candidateCount - 1; ++c)
                for (int d = c + 1; d < candidateCount; ++d)
                    out.push_back(Assignment{ { a, b, c, d } });

    return out;
}

double octavesBetween(float a, float b) {
    if (a <= 0.0f || b <= 0.0f)
        return 4.0;

    return std::fabs(std::log2(a / b));
}

// How far an assignment sits from where this speaker's formants usually are,
// plus a penalty for choosing weak peaks.
double localCost(const Assignment& assignment,
                 const std::vector<Candidate>& candidates,
                 const std::array<float, kFormantCount>& nominal,
                 double strongestDb) {
    double cost = 0.0;

    for (int n = 0; n < kFormantCount; ++n) {
        const Candidate& candidate = candidates[static_cast<size_t>(assignment.index[static_cast<size_t>(n)])];

        cost += 1.0 * octavesBetween(candidate.frequencyHz, nominal[static_cast<size_t>(n)]);
        cost += 0.02 * (strongestDb - candidate.amplitudeDb);

        // A very wide pole is usually spectral shape rather than a resonance.
        if (candidate.bandwidthHz > 700.0f)
            cost += 0.3;
    }

    return cost;
}

double transitionCost(const Assignment& from,
                      const std::vector<Candidate>& fromCandidates,
                      const Assignment& to,
                      const std::vector<Candidate>& toCandidates) {
    double cost = 0.0;

    for (int n = 0; n < kFormantCount; ++n) {
        const float a = fromCandidates[static_cast<size_t>(from.index[static_cast<size_t>(n)])].frequencyHz;
        const float b = toCandidates[static_cast<size_t>(to.index[static_cast<size_t>(n)])].frequencyHz;

        cost += 3.0 * octavesBetween(a, b);
    }

    return cost;
}

// Chooses the assignment for every frame at once, so a single ambiguous moment
// cannot swap two formants for the rest of the utterance.
void solvePath(const std::vector<std::vector<Candidate>>& candidates,
               const std::array<float, kFormantCount>& nominal,
               std::vector<int>& chosen) {
    const size_t frames = candidates.size();
    chosen.assign(frames, -1);

    std::vector<std::vector<Assignment>> assignments(frames);
    std::vector<std::vector<double>>     cost(frames);
    std::vector<std::vector<int>>        backPointer(frames);

    for (size_t i = 0; i < frames; ++i) {
        assignments[i] = enumerateAssignments(static_cast<int>(candidates[i].size()));

        cost[i].assign(assignments[i].size(), std::numeric_limits<double>::infinity());
        backPointer[i].assign(assignments[i].size(), -1);
    }

    for (size_t i = 0; i < frames; ++i) {
        if (assignments[i].empty())
            continue;

        double strongest = -1000.0;
        for (const Candidate& candidate : candidates[i])
            strongest = std::max(strongest, static_cast<double>(candidate.amplitudeDb));

        for (size_t s = 0; s < assignments[i].size(); ++s) {
            const double local = localCost(assignments[i][s], candidates[i], nominal, strongest);

            // The previous frame, if it had any assignment at all. A gap resets
            // the path rather than bridging it.
            double best = std::numeric_limits<double>::infinity();
            int    bestFrom = -1;

            if (i > 0 && !assignments[i - 1].empty()) {
                for (size_t p = 0; p < assignments[i - 1].size(); ++p) {
                    if (!std::isfinite(cost[i - 1][p]))
                        continue;

                    const double total = cost[i - 1][p]
                                       + transitionCost(assignments[i - 1][p], candidates[i - 1],
                                                        assignments[i][s], candidates[i]);

                    if (total < best) {
                        best = total;
                        bestFrom = static_cast<int>(p);
                    }
                }
            }

            cost[i][s] = local + (std::isfinite(best) ? best : 0.0);
            backPointer[i][s] = bestFrom;
        }
    }

    // Walk back from the end of each continuous run.
    for (size_t end = frames; end-- > 0; ) {
        if (assignments[end].empty() || chosen[end] >= 0)
            continue;

        size_t bestIndex = 0;
        double bestCost = std::numeric_limits<double>::infinity();

        for (size_t s = 0; s < cost[end].size(); ++s) {
            if (cost[end][s] < bestCost) {
                bestCost = cost[end][s];
                bestIndex = s;
            }
        }

        if (!std::isfinite(bestCost))
            continue;

        size_t at = end;
        int state = static_cast<int>(bestIndex);

        while (state >= 0) {
            chosen[at] = state;

            const int previous = backPointer[at][static_cast<size_t>(state)];

            if (at == 0 || previous < 0)
                break;

            --at;
            state = previous;
        }
    }

    // Store the chosen assignment index per frame; the caller reads the
    // candidates back out.
    for (size_t i = 0; i < frames; ++i) {
        if (chosen[i] >= 0 && assignments[i].empty())
            chosen[i] = -1;
    }

    // Replace the state index with a packed assignment so the caller does not
    // need the table: encode by storing the four candidate indices in the
    // frame's own candidate order, which enumerateAssignments can reproduce.
    for (size_t i = 0; i < frames; ++i) {
        if (chosen[i] < 0)
            continue;

        if (static_cast<size_t>(chosen[i]) >= assignments[i].size())
            chosen[i] = -1;
    }

    // Hand back the assignments themselves through the same vector by index;
    // the caller re-enumerates, which is cheap and keeps this function's
    // interface small.
}

float median(std::vector<float>& values) {
    if (values.empty())
        return 0.0f;

    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    return values[middle];
}

} // namespace

// ---------------------------------------------------------------------------
// Tracking
// ---------------------------------------------------------------------------

FormantTrack trackFormants(const std::vector<std::vector<double>>& spectrogram,
                           const std::vector<double>& f0,
                           double sampleRate,
                           int fftSize,
                           double framePeriodMs) {
    FormantTrack track;
    track.framePeriodMs = framePeriodMs;

    if (spectrogram.empty() || fftSize <= 0 || sampleRate <= 0.0)
        return track;

    const double binHz = sampleRate / fftSize;
    const size_t frames = spectrogram.size();

    track.frames.resize(frames);

    // --- candidates, once ---
    std::vector<std::vector<Candidate>> candidates(frames);

    for (size_t i = 0; i < frames; ++i) {
        const bool voiced = (i < f0.size()) && f0[i] > 0.0;

        track.frames[i].f0Hz   = voiced ? static_cast<float>(f0[i]) : 0.0f;
        track.frames[i].voiced = voiced;

        if (voiced)
            candidates[i] = findCandidates(spectrogram[i], binHz);
    }

    // --- two passes: generic expectations, then this speaker's own ---
    std::array<float, kFormantCount> nominal = { 500.0f, 1500.0f, 2500.0f, 3500.0f };

    std::vector<int> chosen;

    for (int pass = 0; pass < 2; ++pass) {
        solvePath(candidates, nominal, chosen);

        std::array<std::vector<float>, kFormantCount> collected;

        for (size_t i = 0; i < frames; ++i) {
            if (chosen[i] < 0)
                continue;

            const std::vector<Assignment> assignments =
                enumerateAssignments(static_cast<int>(candidates[i].size()));

            if (static_cast<size_t>(chosen[i]) >= assignments.size())
                continue;

            const Assignment& assignment = assignments[static_cast<size_t>(chosen[i])];

            for (int n = 0; n < kFormantCount; ++n) {
                const Candidate& candidate =
                    candidates[i][static_cast<size_t>(assignment.index[static_cast<size_t>(n)])];

                collected[static_cast<size_t>(n)].push_back(candidate.frequencyHz);

                if (pass == 1) {
                    track.frames[i].frequencyHz[static_cast<size_t>(n)] = candidate.frequencyHz;
                    track.frames[i].bandwidthHz[static_cast<size_t>(n)] = candidate.bandwidthHz;
                    track.frames[i].amplitudeDb[static_cast<size_t>(n)] = candidate.amplitudeDb;
                }
            }
        }

        if (pass == 0) {
            // Re-centre the expectations on what this speaker actually does, so
            // the second pass is not pulled toward an average speaker.
            for (int n = 0; n < kFormantCount; ++n) {
                const float value = median(collected[static_cast<size_t>(n)]);

                if (value > 0.0f)
                    nominal[static_cast<size_t>(n)] = value;
            }
        }
    }

    return track;
}

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

SpeakerMeasurements measureSpeaker(const FormantTrack& track,
                                   const std::vector<std::vector<double>>& spectrogram,
                                   const std::vector<double>& f0,
                                   double sampleRate,
                                   int fftSize) {
    SpeakerMeasurements out;

    if (track.empty() || spectrogram.empty() || fftSize <= 0)
        return out;

    const double binHz = sampleRate / fftSize;

    std::vector<float> f0Values;
    std::vector<float> dispersions;
    std::vector<float> tilts;
    std::vector<float> energies;
    std::array<std::vector<float>, kFormantCount> formantValues;

    for (size_t i = 0; i < track.frames.size(); ++i) {
        const FormantFrame& frame = track.frames[i];

        if (!frame.voiced)
            continue;

        f0Values.push_back(frame.f0Hz);

        for (int n = 0; n < kFormantCount; ++n)
            if (frame.frequencyHz[static_cast<size_t>(n)] > 0.0f)
                formantValues[static_cast<size_t>(n)].push_back(frame.frequencyHz[static_cast<size_t>(n)]);

        int gaps = 0;
        float gapTotal = 0.0f;

        for (int n = 1; n < kFormantCount; ++n) {
            const float lower = frame.frequencyHz[static_cast<size_t>(n - 1)];
            const float upper = frame.frequencyHz[static_cast<size_t>(n)];

            if (lower > 0.0f && upper > lower) {
                gapTotal += upper - lower;
                ++gaps;
            }
        }

        if (gaps >= 2)
            dispersions.push_back(gapTotal / gaps);

        const int lowBin  = static_cast<int>(500.0 / binHz);
        const int highBin = static_cast<int>(4000.0 / binHz);

        if (highBin < static_cast<int>(spectrogram[i].size()) && lowBin > 0) {
            const double lowDb  = powerToDb(spectrogram[i][static_cast<size_t>(lowBin)]);
            const double highDb = powerToDb(spectrogram[i][static_cast<size_t>(highBin)]);
            const double octaves = std::log2(4000.0 / 500.0);

            tilts.push_back(static_cast<float>((highDb - lowDb) / octaves));
        }

        double energy = 0.0;
        for (double bin : spectrogram[i])
            energy += bin;

        energies.push_back(static_cast<float>(powerToDb(energy)));
    }

    if (f0Values.size() < 4)
        return out;

    {
        double total = 0.0;
        int    count = 0;

        for (size_t i = 1; i < f0Values.size(); ++i) {
            if (f0Values[i] <= 0.0f || f0Values[i - 1] <= 0.0f)
                continue;

            const double previousPeriod = 1.0 / f0Values[i - 1];
            const double currentPeriod  = 1.0 / f0Values[i];

            total += std::fabs(currentPeriod - previousPeriod) / previousPeriod;
            ++count;
        }

        if (count > 0)
            out.jitter = static_cast<float>(total / count);
    }

    {
        double total = 0.0;
        int    count = 0;

        for (size_t i = 1; i < energies.size(); ++i) {
            total += std::fabs(energies[i] - energies[i - 1]);
            ++count;
        }

        if (count > 0)
            out.shimmer = static_cast<float>(total / count / 20.0);
    }

    out.medianF0Hz = median(f0Values);

    for (int n = 0; n < kFormantCount; ++n)
        out.medianFormantHz[static_cast<size_t>(n)] = median(formantValues[static_cast<size_t>(n)]);

    if (!tilts.empty())
        out.spectralTiltDbPerOctave = median(tilts);

    if (!dispersions.empty()) {
        const float dispersion = median(dispersions);

        if (dispersion > 200.0f) {
            const float length = static_cast<float>(kSpeedOfSoundCmPerSec / (2.0 * dispersion));

            if (length > 9.0f && length < 22.0f) {
                out.tractLengthCm = length;
                out.valid = true;
            }
        }
    }

    return out;
}

} // namespace vocalyx
