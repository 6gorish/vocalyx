#include "ofApp.h"
#include "AudioFileLoader.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

// Rubber Band takes two numbers: a pitch shift and a single formant scale. The
// warp curve carries more than that, so feeding it to Rubber Band means picking
// one scale from the middle of the curve and accepting the loss. Which is the
// comparison worth hearing — it is the difference between uniform and
// non-uniform warping.
float averageWarpSemitones(const vocalyx::Acoustics& acoustics) {
    if (acoustics.warp.empty())
        return 0.0f;

    double total = 0.0;
    for (const auto& point : acoustics.warp)
        total += point.scale;

    const double mean = total / acoustics.warp.size();
    return static_cast<float>(12.0 * std::log2(std::max(0.25, mean)));
}

double rootMeanSquare(const std::vector<std::vector<float>>& channels) {
    double total = 0.0;
    size_t count = 0;

    for (const auto& channel : channels) {
        for (float sample : channel)
            total += static_cast<double>(sample) * sample;

        count += channel.size();
    }

    return (count == 0) ? 0.0 : std::sqrt(total / count);
}

// Warping and tilting change the overall level, so an untreated-against-
// processed comparison is partly a comparison of loudness. Matching the two
// makes the judgment about timbre instead.
void matchLoudness(const std::vector<std::vector<float>>& reference,
                   std::vector<std::vector<float>>& target) {
    const double referenceLevel = rootMeanSquare(reference);
    const double targetLevel    = rootMeanSquare(target);

    if (referenceLevel <= 1e-9 || targetLevel <= 1e-9)
        return;

    // Clamped, so a near-silent render cannot be amplified into noise.
    double gain = std::clamp(referenceLevel / targetLevel, 0.25, 4.0);

    // Resynthesis peaks higher than the original at the same average level, so
    // matching loudness alone can push the result past full scale, where the
    // audio path clips it. Back off to keep the highest peak inside.
    double peak = 0.0;

    for (const auto& channel : target)
        for (float sample : channel)
            peak = std::max(peak, std::fabs(static_cast<double>(sample)));

    if (peak * gain > 0.98)
        gain = 0.98 / peak;

    for (auto& channel : target)
        for (float& sample : channel)
            sample = static_cast<float>(sample * gain);
}

} // namespace

// ---------------------------------------------------------------------------
// setup / teardown
// ---------------------------------------------------------------------------

void ofApp::setup() {
    ofSetWindowTitle("Vocalyx");
    ofBackground(18);
    ofSetFrameRate(60);

    gui.setup("vocalyx");
    gui.setPosition(20, 150);

    tractGroup.setName("tract");
    tractGroup.add(sizeParam.set("size", 0.0f, -1.0f, 1.0f));
    tractGroup.add(tractLengthParam.set("length (cm)", 17.0f, 12.0f, 20.0f));
    tractGroup.add(pharynxFractionParam.set("pharynx share", 0.45f, 0.35f, 0.60f));
    tractGroup.add(larynxHeightParam.set("larynx (cm)", 0.0f, -1.5f, 1.5f));
    tractGroup.add(lipRoundingParam.set("lip rounding", 0.0f, 0.0f, 1.0f));
    tractGroup.add(nasalityParam.set("nasality", 0.0f, 0.0f, 1.0f));

    voiceGroup.setName("voice");
    voiceGroup.add(medianF0Param.set("pitch (Hz)", 110.0f, 60.0f, 350.0f));
    voiceGroup.add(openQuotientParam.set("open quotient", 0.6f, 0.35f, 0.85f));
    voiceGroup.add(aspirationParam.set("aspiration", 0.0f, 0.0f, 1.0f));
    voiceGroup.add(jitterParam.set("jitter", 0.0f, 0.0f, 1.0f));
    voiceGroup.add(shimmerParam.set("shimmer", 0.0f, 0.0f, 1.0f));
    voiceGroup.add(effortParam.set("effort", 0.0f, -1.0f, 1.0f));

    recordedGroup.setName("recorded voice");
    recordedGroup.add(sourceTractLengthParam.set("length (cm)", 17.0f, 12.0f, 20.0f));

    morphGroup.setName("morph to conversion");
    morphGroup.add(morphEnvelopeParam.set("timbre", 0.0f, 0.0f, 1.0f));
    morphGroup.add(morphAperiodicityParam.set("breath", 0.0f, 0.0f, 1.0f));
    morphGroup.add(morphF0Param.set("pitch track", 0.0f, 0.0f, 1.0f));

    gui.add(tractGroup);
    gui.add(voiceGroup);
    gui.add(recordedGroup);
    gui.add(morphGroup);
    gui.add(useWorldParam.set("WORLD engine", true));

    tractLengthParam.addListener(this, &ofApp::onFloatChanged);
    sizeParam.addListener(this, &ofApp::onSizeChanged);
    pharynxFractionParam.addListener(this, &ofApp::onFloatChanged);
    larynxHeightParam.addListener(this, &ofApp::onFloatChanged);
    lipRoundingParam.addListener(this, &ofApp::onFloatChanged);
    nasalityParam.addListener(this, &ofApp::onFloatChanged);
    medianF0Param.addListener(this, &ofApp::onFloatChanged);
    openQuotientParam.addListener(this, &ofApp::onFloatChanged);
    aspirationParam.addListener(this, &ofApp::onFloatChanged);
    jitterParam.addListener(this, &ofApp::onFloatChanged);
    shimmerParam.addListener(this, &ofApp::onFloatChanged);
    effortParam.addListener(this, &ofApp::onFloatChanged);
    sourceTractLengthParam.addListener(this, &ofApp::onFloatChanged);
    morphEnvelopeParam.addListener(this, &ofApp::onFloatChanged);
    morphAperiodicityParam.addListener(this, &ofApp::onFloatChanged);
    morphF0Param.addListener(this, &ofApp::onFloatChanged);
    useWorldParam.addListener(this, &ofApp::onBoolChanged);

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        readInterfaceIntoModel();
    }

    renderThread = std::thread(&ofApp::renderThreadFunction, this);

    profiles = vocalyx::loadProfiles(ofToDataPath("profiles.txt", true));

    setStatus(profiles.empty()
              ? "drag an audio file onto the window (no profiles loaded)"
              : "drag an audio file onto the window");
}

void ofApp::exit() {
    tractLengthParam.removeListener(this, &ofApp::onFloatChanged);
    sizeParam.removeListener(this, &ofApp::onSizeChanged);
    pharynxFractionParam.removeListener(this, &ofApp::onFloatChanged);
    larynxHeightParam.removeListener(this, &ofApp::onFloatChanged);
    lipRoundingParam.removeListener(this, &ofApp::onFloatChanged);
    nasalityParam.removeListener(this, &ofApp::onFloatChanged);
    medianF0Param.removeListener(this, &ofApp::onFloatChanged);
    openQuotientParam.removeListener(this, &ofApp::onFloatChanged);
    aspirationParam.removeListener(this, &ofApp::onFloatChanged);
    jitterParam.removeListener(this, &ofApp::onFloatChanged);
    shimmerParam.removeListener(this, &ofApp::onFloatChanged);
    effortParam.removeListener(this, &ofApp::onFloatChanged);
    sourceTractLengthParam.removeListener(this, &ofApp::onFloatChanged);
    morphEnvelopeParam.removeListener(this, &ofApp::onFloatChanged);
    morphAperiodicityParam.removeListener(this, &ofApp::onFloatChanged);
    morphF0Param.removeListener(this, &ofApp::onFloatChanged);
    useWorldParam.removeListener(this, &ofApp::onBoolChanged);

    soundStream.close();

    sourcePtr.store(nullptr);
    processedPtr.store(nullptr);

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        renderRunning = false;
    }
    renderCv.notify_all();

    if (renderThread.joinable())
        renderThread.join();

    std::lock_guard<std::mutex> lock(keepAliveMutex);
    keepAlive.clear();
}

// ---------------------------------------------------------------------------
// interface to model
// ---------------------------------------------------------------------------

void ofApp::readInterfaceIntoModel() {
    physiology.tractLengthCm   = tractLengthParam.get();
    physiology.pharynxFraction = pharynxFractionParam.get();
    physiology.larynxHeightCm  = larynxHeightParam.get();
    physiology.lipRounding     = lipRoundingParam.get();
    physiology.nasality        = nasalityParam.get();
    physiology.medianF0Hz      = medianF0Param.get();
    physiology.openQuotient    = openQuotientParam.get();
    physiology.aspiration      = aspirationParam.get();
    physiology.jitter          = jitterParam.get();
    physiology.shimmer         = shimmerParam.get();
    physiology.effort          = effortParam.get();

    sourceSpeaker.tractLengthCm = sourceTractLengthParam.get();

    // Pitch is measured from the recording rather than guessed, once analysis
    // has run.
    const float measured = measuredF0.load();
    if (measured > 0.0f)
        sourceSpeaker.medianF0Hz = measured;

    // So are the formants, which is what lets the warp curve bend at this
    // speaker's resonances rather than at fixed frequencies.
    {
        const vocalyx::SpeakerMeasurements stats = getMeasurements();
        int found = 0;

        for (int n = 0; n < vocalyx::kFormantCount && n < 4; ++n) {
            const float hz = stats.medianFormantHz[static_cast<size_t>(n)];
            sourceSpeaker.formantHz[static_cast<size_t>(n)] = hz;

            if (hz > 0.0f)
                ++found;
        }

        // Two anchors is the minimum that makes a bend meaningful.
        sourceSpeaker.formantsMeasured = found >= 2;
    }

    useWorld = useWorldParam.get();
    engineChoice = engine;

    // Dragged handles take precedence over the anatomy.
    physiology.useFormantTargets = targetsActive;
    for (int n = 0; n < 4; ++n)
        physiology.formantTargetHz[static_cast<size_t>(n)] = formantTargets[static_cast<size_t>(n)];

    physiology.morphEnvelope     = morphEnvelopeParam.get();
    physiology.morphAperiodicity = morphAperiodicityParam.get();
    physiology.morphF0           = morphF0Param.get();
}

void ofApp::onFloatChanged(float&) {
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        readInterfaceIntoModel();
        renderRequested = true;
    }
    renderCv.notify_one();
}

// One control for how big the speaker is. Pitch and tract length are not free
// of each other in real people: a shorter tract comes with smaller, faster
// folds. The exponent below is fitted to two anchors — an adult male to adult
// female shift, and an adult male to child shift — so the whole range between
// them stays inside the space of voices that exist.
void ofApp::onSizeChanged(float& value) {
    if (applyingSize)
        return;

    applyingSize = true;

    const float baseLength = (measuredTractCm.load() > 0.0f) ? measuredTractCm.load()
                                                             : sourceTractLengthParam.get();
    const float basePitch  = (measuredF0.load() > 0.0f) ? measuredF0.load() : 110.0f;

    // Larger is positive, so the length ratio runs from 0.8 to 1.2.
    const float lengthRatio = 1.0f + 0.20f * std::clamp(value, -1.0f, 1.0f);

    tractLengthParam = std::clamp(baseLength * lengthRatio, 12.0f, 20.0f);
    medianF0Param    = std::clamp(basePitch * std::pow(lengthRatio, -2.8f), 60.0f, 350.0f);

    // The pharynx shortens faster than the mouth in smaller speakers.
    pharynxFractionParam = std::clamp(0.45f + 0.07f * std::clamp(value, -1.0f, 1.0f), 0.35f, 0.60f);

    applyingSize = false;

    float ignored = 0.0f;
    onFloatChanged(ignored);
}

void ofApp::onBoolChanged(bool&) {
    float ignored = 0.0f;
    onFloatChanged(ignored);
}

// Published group averages give ratios, and the ratios move this speaker's own
// measured formants. Nothing here assumes the recording matches the reference
// group's absolute values — only that it differs from the target group the way
// the two published groups differ from each other.
void ofApp::applyProfilePair() {
    if (profiles.empty())
        return;

    const auto& reference = profiles[static_cast<size_t>(referenceProfile)];
    const auto& target    = profiles[static_cast<size_t>(targetProfile)];

    const vocalyx::ProfileRatios ratios = vocalyx::ratiosBetween(reference, target);
    const vocalyx::SpeakerMeasurements stats = getMeasurements();

    bool any = false;

    for (int n = 0; n < 4; ++n) {
        const float measured = stats.medianFormantHz[static_cast<size_t>(n)];

        if (measured <= 0.0f) {
            formantTargets[static_cast<size_t>(n)] = 0.0f;
            continue;
        }

        formantTargets[static_cast<size_t>(n)] = measured * ratios.formant[static_cast<size_t>(n)];
        any = true;
    }

    if (!any)
        return;

    targetsActive = true;

    if (measuredF0.load() > 0.0f)
        medianF0Param = std::clamp(measuredF0.load() * ratios.f0, 60.0f, 350.0f);

    setStatus(reference.name + " to " + target.name);

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        readInterfaceIntoModel();
        renderRequested = true;
    }

    renderCv.notify_one();
}

void ofApp::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex);
    status = text;
}

std::string ofApp::getStatus() const {
    std::lock_guard<std::mutex> lock(statusMutex);
    return status;
}

void ofApp::setMeasurements(const vocalyx::SpeakerMeasurements& measured) {
    std::lock_guard<std::mutex> lock(measurementMutex);
    measurements = measured;
}

vocalyx::SpeakerMeasurements ofApp::getMeasurements() const {
    std::lock_guard<std::mutex> lock(measurementMutex);
    return measurements;
}

// ---------------------------------------------------------------------------
// rendering helpers, render thread only
// ---------------------------------------------------------------------------

std::shared_ptr<Clip> ofApp::renderWorldFull(const vocalyx::Acoustics& acoustics,
                                             const std::shared_ptr<Clip>& source) {
    auto result = std::make_shared<Clip>();
    result->sampleRate = source->sampleRate;

    std::string error;
    if (!world.render(acoustics, source->numChannels(), result->channels, error)) {
        setStatus("render failed: " + error);
        return nullptr;
    }

    matchLoudness(source->channels, result->channels);
    return result;
}

std::shared_ptr<Clip> ofApp::renderWorldPreview(const vocalyx::Acoustics& acoustics,
                                                const std::shared_ptr<Clip>& source,
                                                const std::shared_ptr<Clip>& base,
                                                int centerSample) {
    const int hop = world.samplesPerFrame();
    if (hop <= 0)
        return nullptr;

    const int windowSamples = static_cast<int>(previewSeconds * source->sampleRate);
    const int startSample   = std::clamp(centerSample - windowSamples / 2,
                                         0, std::max(0, source->numFrames() - 1));

    const int startFrame  = startSample / hop;
    const int frameCount  = std::max(1, windowSamples / hop);

    std::vector<std::vector<float>> window;
    std::string error;

    if (!world.renderRange(acoustics, startFrame, frameCount,
                           source->numChannels(), window, error)) {
        setStatus("preview failed: " + error);
        return nullptr;
    }

    // The window replaces part of whatever was playing, so the rest of the clip
    // keeps its previous audio rather than dropping to silence.
    auto result = std::make_shared<Clip>();
    result->sampleRate = source->sampleRate;

    const bool baseUsable = base
                         && base->numFrames() == source->numFrames()
                         && base->numChannels() == source->numChannels();

    result->channels = baseUsable ? base->channels : source->channels;

    // Match the window's level to the same stretch of the original, so a
    // preview does not jump in volume against the surrounding audio.
    {
        const int patchStart = startFrame * hop;
        const int patchLength = std::min(static_cast<int>(window[0].size()),
                                         source->numFrames() - patchStart);

        if (patchLength > 0) {
            std::vector<std::vector<float>> reference(source->channels.size());
            for (size_t c = 0; c < source->channels.size(); ++c)
                reference[c].assign(source->channels[c].begin() + patchStart,
                                    source->channels[c].begin() + patchStart + patchLength);

            matchLoudness(reference, window);
        }
    }

    const int patchStart = startFrame * hop;
    const int ramp = 256;

    for (size_t c = 0; c < result->channels.size() && c < window.size(); ++c) {
        auto& destination = result->channels[c];
        const auto& patch = window[c];

        const int length = std::min(static_cast<int>(patch.size()),
                                    static_cast<int>(destination.size()) - patchStart);

        for (int n = 0; n < length; ++n) {
            // Ramp in and out so the seams do not click.
            float blend = 1.0f;

            if (n < ramp)
                blend = static_cast<float>(n) / ramp;
            else if (n > length - ramp)
                blend = static_cast<float>(length - n) / ramp;

            const size_t index = static_cast<size_t>(patchStart + n);
            destination[index] = destination[index] * (1.0f - blend) + patch[static_cast<size_t>(n)] * blend;
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// the time view
// ---------------------------------------------------------------------------

void ofApp::buildDisplayData() {
    const auto& spectrogram = world.spectrogram();

    if (spectrogram.empty() || world.fftSize() <= 0)
        return;

    const int frames = static_cast<int>(spectrogram.size());
    const int bins   = static_cast<int>(spectrogram[0].size());
    const double binHz = world.sampleRate() / world.fftSize();

    // One column per pixel at most: a minute of speech is twelve thousand
    // frames, and the display is a thousand pixels wide.
    const int columns = std::min(frames, 1200);
    const int rows    = 256;
    const int step    = std::max(1, frames / columns);

    ofPixels pixels;
    pixels.allocate(columns, rows, OF_PIXELS_GRAY);

    std::vector<std::array<float, 4>> formantColumns(static_cast<size_t>(columns));

    // A fixed window below the loudest point in the clip, so quiet passages
    // stay visible without the display swimming. The tilt compensation matters:
    // speech falls off steeply with frequency, so without it the low end blows
    // out to white and the formants above 2 kHz disappear into the floor.
    auto displayDb = [&](double power, double hz) {
        const double db = 10.0 * std::log10(std::max(1e-20, power));
        const double tilt = 6.0 * std::log2(std::max(200.0, hz) / 500.0);
        return db + tilt;
    };

    std::vector<double> levels;
    levels.reserve(static_cast<size_t>(columns) * 64);

    for (int c = 0; c < columns; ++c) {
        const int frame = std::min(frames - 1, c * step);

        for (int k = 1; k < bins; k += 4) {
            const double hz = k * binHz;
            if (hz > displayMaxHz)
                break;

            levels.push_back(displayDb(spectrogram[static_cast<size_t>(frame)][static_cast<size_t>(k)], hz));
        }
    }

    if (levels.empty())
        return;

    // The top of the scale comes from a high percentile rather than the maximum,
    // so one loud frame cannot wash out the rest.
    const size_t percentile = static_cast<size_t>(levels.size() * 0.99);
    std::nth_element(levels.begin(), levels.begin() + percentile, levels.end());

    const double peakDb  = levels[percentile];
    const double floorDb = peakDb - 55.0;

    for (int c = 0; c < columns; ++c) {
        const int frame = std::min(frames - 1, c * step);

        for (int r = 0; r < rows; ++r) {
            // Row zero is the top of the display, so frequency runs upward.
            const double hz = displayMaxHz * (1.0 - static_cast<double>(r) / rows);
            const int    bin = std::min(bins - 1, static_cast<int>(hz / binHz));

            const double db = displayDb(spectrogram[static_cast<size_t>(frame)][static_cast<size_t>(bin)], hz);
            double normalized = std::clamp((db - floorDb) / (peakDb - floorDb), 0.0, 1.0);

            // A gamma below one lifts the mid greys, which is where formant
            // structure lives.
            normalized = std::pow(normalized, 1.4);

            pixels.setColor(c, r, ofColor(static_cast<unsigned char>(normalized * 230.0)));
        }

        // The formants for this column, from the same frame.
        std::array<float, 4> values{};

        if (frame < static_cast<int>(formantTrack.frames.size())) {
            const auto& source = formantTrack.frames[static_cast<size_t>(frame)];

            for (int n = 0; n < 4 && n < vocalyx::kFormantCount; ++n)
                values[static_cast<size_t>(n)] = source.voiced ? source.frequencyHz[static_cast<size_t>(n)] : 0.0f;
        }

        formantColumns[static_cast<size_t>(c)] = values;
    }

    {
        std::lock_guard<std::mutex> lock(displayMutex);
        displayPixels   = pixels;
        displayFormants = std::move(formantColumns);
    }

    displayIsNew.store(true);
}

void ofApp::drawTimeView(float x, float y, float w, float h) {
    ofSetColor(40);
    ofNoFill();
    ofDrawRectangle(x, y, w, h);
    ofFill();

    if (!spectrogramTexture.isAllocated()) {
        ofSetColor(90);
        ofDrawBitmapString("no analysis yet", x + 12, y + 20);
        return;
    }

    ofSetColor(255);
    spectrogramTexture.draw(x, y, w, h);

    // --- formant tracks ---
    std::vector<std::array<float, 4>> columns;
    {
        std::lock_guard<std::mutex> lock(displayMutex);
        columns = displayFormants;
    }

    if (!columns.empty()) {
        const ofColor trackColors[4] = {
            ofColor(255, 120, 90),
            ofColor(120, 200, 255),
            ofColor(150, 255, 160),
            ofColor(230, 200, 110),
        };

        for (int n = 0; n < 4; ++n) {
            ofColor color = trackColors[n];
            color.a = 70;                     // the original, dimmed
            ofSetColor(color);

            // A new polyline wherever the track breaks, so unvoiced gaps stay
            // gaps rather than being drawn across.
            ofPolyline line;

            for (size_t c = 0; c < columns.size(); ++c) {
                const float hz = columns[c][static_cast<size_t>(n)];

                if (hz <= 0.0f) {
                    if (line.size() > 1)
                        line.draw();
                    line.clear();
                    continue;
                }

                const float px = x + w * (static_cast<float>(c) / columns.size());
                const float py = y + h * (1.0f - std::min(hz, displayMaxHz) / displayMaxHz);

                line.addVertex(px, py);
            }

            if (line.size() > 1)
                line.draw();
        }

        // --- where the sliders are sending them ---
        if (showTransformed) {
            for (int n = 0; n < 4; ++n) {
                ofColor color = trackColors[n];
                color.a = 200;
                ofSetColor(color);

                ofPolyline line;

                for (size_t c = 0; c < columns.size(); ++c) {
                    const float hz = columns[c][static_cast<size_t>(n)];

                    if (hz <= 0.0f) {
                        if (line.size() > 1)
                            line.draw();
                        line.clear();
                        continue;
                    }

                    const float moved = hz * vocalyx::warpScaleAt(displayAcoustics.warp, hz);

                    const float px = x + w * (static_cast<float>(c) / columns.size());
                    const float py = y + h * (1.0f - std::min(moved, displayMaxHz) / displayMaxHz);

                    line.addVertex(px, py);
                }

                if (line.size() > 1)
                    line.draw();
            }
        }
    }

    // --- playhead ---
    const Clip* clip = processedPtr.load();
    if (clip != nullptr && clip->numFrames() > 0) {
        const float t = static_cast<float>(playhead.load()) / clip->numFrames();

        ofSetColor(255, 255, 255, 160);
        ofDrawLine(x + w * t, y, x + w * t, y + h);
    }

    // --- frequency scale ---
    ofSetColor(120);
    for (int hz = 1000; hz <= static_cast<int>(displayMaxHz); hz += 1000) {
        const float py = y + h * (1.0f - hz / displayMaxHz);
        ofDrawBitmapString(ofToString(hz / 1000) + "k", x - 24, py + 4);

        ofSetColor(255, 255, 255, 20);
        ofDrawLine(x, py, x + w, py);
        ofSetColor(120);
    }
}

// ---------------------------------------------------------------------------
// the vowel view
// ---------------------------------------------------------------------------

void ofApp::drawVowelView(float x, float y, float w, float h) {
    // The conventional phonetics layout: the second formant runs right to left,
    // the first runs top to bottom, so the plot matches the shape of the mouth.
    // A high front vowel lands top left, a low back vowel bottom right.
    constexpr float f1Low  = 250.0f,  f1High = 1000.0f;
    constexpr float f2Low  = 700.0f,  f2High = 2800.0f;

    auto plotX = [&](float f2) {
        const float t = (std::log(f2High) - std::log(std::clamp(f2, f2Low, f2High)))
                      / (std::log(f2High) - std::log(f2Low));
        return x + w * t;
    };

    auto plotY = [&](float f1) {
        const float t = (std::log(std::clamp(f1, f1Low, f1High)) - std::log(f1Low))
                      / (std::log(f1High) - std::log(f1Low));
        return y + h * t;
    };

    ofSetColor(40);
    ofNoFill();
    ofDrawRectangle(x, y, w, h);
    ofFill();

    ofSetColor(120);
    ofDrawBitmapString("vowel space", x, y - 10);
    ofDrawBitmapString("F2", x + w - 20, y + h + 16);
    ofDrawBitmapString("F1", x - 28, y + 10);

    // --- reference vowels ---
    // Approximate averages for adult male American English, as a rough frame of
    // reference rather than a target to hit. Labels are in ARPABET, since the
    // bitmap font has no phonetic symbols.
    struct Reference { const char* label; float f1; float f2; };
    static const Reference references[] = {
        { "iy", 270.0f, 2300.0f },
        { "ih", 400.0f, 2000.0f },
        { "eh", 530.0f, 1850.0f },
        { "ae", 660.0f, 1700.0f },
        { "aa", 730.0f, 1100.0f },
        { "ao", 570.0f,  850.0f },
        { "uh", 440.0f, 1000.0f },
        { "uw", 300.0f,  870.0f },
    };

    ofSetColor(70);
    for (const Reference& vowel : references) {
        const float px = plotX(vowel.f2);
        const float py = plotY(vowel.f1);

        ofDrawBitmapString(vowel.label, px - 6, py + 4);
    }

    // --- where the voice has been ---
    std::vector<std::array<float, 4>> columns;
    {
        std::lock_guard<std::mutex> lock(displayMutex);
        columns = displayFormants;
    }

    if (columns.empty()) {
        ofSetColor(90);
        ofDrawBitmapString("no analysis yet", x + 12, y + 20);
        return;
    }

    ofSetColor(90, 140, 190, 60);
    for (const auto& column : columns) {
        const float f1 = column[0];
        const float f2 = column[1];

        if (f1 <= 0.0f || f2 <= 0.0f)
            continue;

        ofDrawCircle(plotX(f2), plotY(f1), 1.6f);
    }

    // --- and where the sliders send it ---
    if (showTransformed) {
        ofSetColor(255, 170, 110, 70);

        for (const auto& column : columns) {
            const float f1 = column[0];
            const float f2 = column[1];

            if (f1 <= 0.0f || f2 <= 0.0f)
                continue;

            ofDrawCircle(plotX(f2 * vocalyx::warpScaleAt(displayAcoustics.warp, f2)),
                         plotY(f1 * vocalyx::warpScaleAt(displayAcoustics.warp, f1)),
                         1.6f);
        }
    }

    // --- where it is now ---
    const Clip* clip = processedPtr.load();
    if (clip == nullptr || clip->numFrames() == 0)
        return;

    const float progress = static_cast<float>(playhead.load()) / clip->numFrames();
    const int   current  = std::clamp(static_cast<int>(progress * columns.size()),
                                      0, static_cast<int>(columns.size()) - 1);

    // A short trail behind the playhead, so the movement between vowels reads
    // as motion rather than a jumping dot.
    const int trail = 40;

    for (int i = std::max(0, current - trail); i <= current; ++i) {
        const float f1 = columns[static_cast<size_t>(i)][0];
        const float f2 = columns[static_cast<size_t>(i)][1];

        if (f1 <= 0.0f || f2 <= 0.0f)
            continue;

        const float age = static_cast<float>(current - i) / trail;
        ofSetColor(255, 200, 120, static_cast<int>(200 * (1.0f - age)));
        ofDrawCircle(plotX(f2), plotY(f1), 3.0f);
    }

    const float f1 = columns[static_cast<size_t>(current)][0];
    const float f2 = columns[static_cast<size_t>(current)][1];

    if (f1 > 0.0f && f2 > 0.0f) {
        ofSetColor(140, 190, 240);
        ofDrawCircle(plotX(f2), plotY(f1), 4.0f);

        if (showTransformed) {
            const float movedF1 = f1 * vocalyx::warpScaleAt(displayAcoustics.warp, f1);
            const float movedF2 = f2 * vocalyx::warpScaleAt(displayAcoustics.warp, f2);

            // A line from where the voice is to where it is being sent, which
            // is the transformation made visible.
            ofSetColor(255, 200, 150, 120);
            ofDrawLine(plotX(f2), plotY(f1), plotX(movedF2), plotY(movedF1));

            ofSetColor(255, 220, 170);
            ofDrawCircle(plotX(movedF2), plotY(movedF1), 5.0f);

            ofSetColor(160);
            ofDrawBitmapString(ofToString(static_cast<int>(f1)) + " / " + ofToString(static_cast<int>(f2))
                               + "   to   "
                               + ofToString(static_cast<int>(movedF1)) + " / " + ofToString(static_cast<int>(movedF2)),
                               x + 8, y + h - 10);
        } else {
            ofSetColor(160);
            ofDrawBitmapString(ofToString(static_cast<int>(f1)) + " / " + ofToString(static_cast<int>(f2)),
                               x + 8, y + h - 10);
        }
    }
}

// ---------------------------------------------------------------------------
// the formant editor
// ---------------------------------------------------------------------------

namespace {
constexpr float kEditorLowHz  = 200.0f;
constexpr float kEditorHighHz = 4500.0f;
} // namespace

float ofApp::editorHzToX(float hz) const {
    const float t = (std::log(std::clamp(hz, kEditorLowHz, kEditorHighHz)) - std::log(kEditorLowHz))
                  / (std::log(kEditorHighHz) - std::log(kEditorLowHz));
    return editorRect.x + editorRect.width * t;
}

float ofApp::editorXToHz(float px) const {
    const float t = std::clamp((px - editorRect.x) / editorRect.width, 0.0f, 1.0f);
    return std::exp(std::log(kEditorLowHz) + t * (std::log(kEditorHighHz) - std::log(kEditorLowHz)));
}

void ofApp::drawFormantEditor(float x, float y, float w, float h) {
    editorRect.set(x, y, w, h);

    ofSetColor(40);
    ofNoFill();
    ofDrawRectangle(x, y, w, h);
    ofFill();

    ofSetColor(120);
    ofDrawBitmapString("formant targets" + std::string(targetsActive ? "  (dragged)" : "  (from anatomy)"),
                       x, y - 10);

    // Frequency scale.
    ofSetColor(70);
    for (int hz : { 250, 500, 1000, 2000, 4000 }) {
        const float px = editorHzToX(static_cast<float>(hz));
        ofDrawLine(px, y + h - 14, px, y + h);
        ofDrawBitmapString(ofToString(hz), px - 12, y + h - 18);
    }

    const vocalyx::SpeakerMeasurements stats = getMeasurements();

    if (!stats.valid && stats.medianFormantHz[0] <= 0.0f) {
        ofSetColor(90);
        ofDrawBitmapString("load a file to measure formants", x + 12, y + 24);
        return;
    }

    const ofColor handleColors[4] = {
        ofColor(255, 120, 90),
        ofColor(120, 200, 255),
        ofColor(150, 255, 160),
        ofColor(230, 200, 110),
    };

    const float laneTop = y + 16.0f;
    const float laneGap = (h - 48.0f) / 4.0f;

    for (int n = 0; n < 4; ++n) {
        const float from = stats.medianFormantHz[static_cast<size_t>(n)];

        if (from <= 0.0f)
            continue;

        const float laneY = laneTop + laneGap * n + laneGap * 0.5f;

        // Undragged handles show where the anatomy is currently sending this
        // formant, so a preset visibly moves them.
        const float to = targetsActive && formantTargets[static_cast<size_t>(n)] > 0.0f
                       ? formantTargets[static_cast<size_t>(n)]
                       : from * vocalyx::warpScaleAt(displayAcoustics.warp, from);

        const float fromX = editorHzToX(from);
        const float toX   = editorHzToX(to);

        // The measured position, as a fixed reference.
        ofSetColor(90);
        ofDrawLine(fromX, laneY - 7, fromX, laneY + 7);
        ofDrawBitmapString("F" + ofToString(n + 1), x - 26, laneY + 4);

        // The move.
        ofColor color = handleColors[n];
        color.a = 120;
        ofSetColor(color);
        ofDrawLine(fromX, laneY, toX, laneY);

        // The handle.
        ofSetColor(handleColors[n]);
        ofDrawCircle(toX, laneY, draggingHandle == n ? 8.0f : 6.0f);

        ofSetColor(150);
        ofDrawBitmapString(ofToString(static_cast<int>(to)), toX + 10, laneY + 4);
    }
}

// ---------------------------------------------------------------------------
// mouse
// ---------------------------------------------------------------------------

void ofApp::mousePressed(int x, int y, int button) {
    if (!editorRect.inside(static_cast<float>(x), static_cast<float>(y)))
        return;

    const vocalyx::SpeakerMeasurements stats = getMeasurements();

    const float laneTop = editorRect.y + 16.0f;
    const float laneGap = (editorRect.height - 48.0f) / 4.0f;

    // Whichever handle the click is nearest, within reach.
    float bestDistance = 24.0f;
    draggingHandle = -1;

    for (int n = 0; n < 4; ++n) {
        const float from = stats.medianFormantHz[static_cast<size_t>(n)];

        if (from <= 0.0f)
            continue;

        const float laneY = laneTop + laneGap * n + laneGap * 0.5f;
        const float to = targetsActive && formantTargets[static_cast<size_t>(n)] > 0.0f
                       ? formantTargets[static_cast<size_t>(n)]
                       : from * vocalyx::warpScaleAt(displayAcoustics.warp, from);

        const float distance = ofDist(static_cast<float>(x), static_cast<float>(y),
                                      editorHzToX(to), laneY);

        if (distance < bestDistance) {
            bestDistance = distance;
            draggingHandle = n;
        }
    }

    if (draggingHandle >= 0) {
        // Dragging starts from wherever the anatomy had put the handles, so
        // taking manual control does not jump the sound.
        if (!targetsActive) {
            for (int n = 0; n < 4; ++n) {
                const float from = stats.medianFormantHz[static_cast<size_t>(n)];
                formantTargets[static_cast<size_t>(n)] =
                    (from > 0.0f) ? from * vocalyx::warpScaleAt(displayAcoustics.warp, from) : 0.0f;
            }
        }

        mouseDragged(x, y, button);
    }
}

void ofApp::mouseDragged(int x, int y, int button) {
    if (draggingHandle < 0)
        return;

    const vocalyx::SpeakerMeasurements stats = getMeasurements();

    float hz = editorXToHz(static_cast<float>(x));

    // Keep the formants in order, since a warp curve that doubles back on
    // itself has no meaning.
    const float below = (draggingHandle > 0 && formantTargets[static_cast<size_t>(draggingHandle - 1)] > 0.0f)
                      ? formantTargets[static_cast<size_t>(draggingHandle - 1)] + 60.0f
                      : kEditorLowHz;

    const float above = (draggingHandle < 3 && formantTargets[static_cast<size_t>(draggingHandle + 1)] > 0.0f)
                      ? formantTargets[static_cast<size_t>(draggingHandle + 1)] - 60.0f
                      : kEditorHighHz;

    hz = std::clamp(hz, below, above);

    formantTargets[static_cast<size_t>(draggingHandle)] = hz;
    targetsActive = true;

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        readInterfaceIntoModel();
        renderRequested = true;
    }

    renderCv.notify_one();
}

void ofApp::mouseReleased(int x, int y, int button) {
    draggingHandle = -1;
}

// ---------------------------------------------------------------------------
// clip publication
// ---------------------------------------------------------------------------

void ofApp::retain(const std::shared_ptr<Clip>& clip) {
    std::lock_guard<std::mutex> lock(keepAliveMutex);

    keepAlive.push_back(clip);

    while (keepAlive.size() > keepAliveMax)
        keepAlive.pop_front();
}

void ofApp::publishSource(const std::shared_ptr<Clip>& clip) {
    retain(clip);
    sourcePtr.store(clip.get());
}

void ofApp::publishProcessed(const std::shared_ptr<Clip>& clip) {
    retain(clip);
    processedPtr.store(clip.get());
}

// ---------------------------------------------------------------------------
// loading
// ---------------------------------------------------------------------------

void ofApp::loadFile(const std::string& path) {
    std::vector<std::vector<float>> channels;
    double fileSampleRate = 0.0;
    std::string error;

    if (!vocalyx::loadAudioFile(path, channels, fileSampleRate, error)) {
        setStatus(error);
        return;
    }

    auto clip = std::make_shared<Clip>();
    clip->channels   = std::move(channels);
    clip->sampleRate = fileSampleRate;

    playing.store(false);
    playhead.store(0);
    measuredF0.store(0.0f);

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        sourceClip      = clip;
        analysisPending = true;     // WORLD analyzes once, on the render thread
        renderRequested = true;
    }

    publishSource(clip);
    publishProcessed(clip);   // the untouched clip plays until the first render lands

    fileName = ofFilePath::getFileName(path);

    const int outChannels = std::min(clip->numChannels(), 2);
    if (streamSampleRate != clip->sampleRate || streamChannels != outChannels)
        startAudio(clip->sampleRate, clip->numChannels());

    renderCv.notify_one();

    playing.store(true);
    setStatus(ofToString(clip->numFrames()) + " frames, "
              + ofToString(clip->numChannels()) + " ch, "
              + ofToString(static_cast<int>(clip->sampleRate)) + " Hz");
}

// Loads a conversion of the same performance to morph toward. The analysis
// runs on the render thread, which owns both engines.
void ofApp::loadMorphFile(const std::string& path) {
    std::vector<std::vector<float>> channels;
    double fileSampleRate = 0.0;
    std::string error;

    if (!vocalyx::loadAudioFile(path, channels, fileSampleRate, error)) {
        setStatus(error);
        return;
    }

    auto clip = std::make_shared<Clip>();
    clip->channels   = std::move(channels);
    clip->sampleRate = fileSampleRate;

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        pendingMorphClip = clip;
        morphFileName    = ofFilePath::getFileName(path);
        renderRequested  = true;
    }

    renderCv.notify_one();
    setStatus("analyzing the conversion...");
}

void ofApp::startAudio(double sampleRate, int numChannels) {
    soundStream.close();
    ofSoundStreamSettings settings;
    settings.setOutListener(this);
    settings.sampleRate        = static_cast<int>(sampleRate);
    settings.numOutputChannels = std::min(numChannels, 2);
    settings.numInputChannels  = 0;
    settings.bufferSize        = 512;

    if (!soundStream.setup(settings)) {
        setStatus("could not open the audio output device");
        return;
    }

    streamSampleRate = sampleRate;
    streamChannels   = settings.numOutputChannels;
}

// ---------------------------------------------------------------------------
// render thread
// ---------------------------------------------------------------------------

void ofApp::requestRender() {
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        renderRequested = true;
    }
    renderCv.notify_one();
}

void ofApp::renderThreadFunction() {
    for (;;) {
        vocalyx::Physiology    localPhysiology;
        vocalyx::SourceSpeaker localSource;
        std::shared_ptr<Clip>  localClip;
        std::shared_ptr<Clip>  localMorphClip;
        bool                   localUseWorld = true;
        Engine                 localEngine = Engine::Filter;
        bool                   needsAnalysis = false;

        {
            std::unique_lock<std::mutex> lock(renderMutex);
            renderCv.wait(lock, [this] { return renderRequested || !renderRunning; });

            if (!renderRunning)
                return;

            renderRequested = false;
            localPhysiology = physiology;
            localSource     = sourceSpeaker;
            localClip       = sourceClip;
            localUseWorld   = useWorld;
            localEngine     = engineChoice;
            needsAnalysis   = analysisPending;

            localMorphClip  = pendingMorphClip;
            pendingMorphClip.reset();
        }

        if (!localClip || localClip->numFrames() == 0)
            continue;
        // --- analysis, once per clip -----------------------------------------
        if (needsAnalysis) {
            analyzing.store(true);
            setStatus("analyzing...");

            std::string error;
            const bool ok = world.analyze(localClip->channels, localClip->sampleRate, error);

            analyzing.store(false);

            if (ok) {
                const float median = world.measuredMedianF0Hz();
                measuredF0.store(median);
                measuredF0IsNew.store(true);

                if (median > 0.0f)
                    localSource.medianF0Hz = median;

                // The tracker runs on the analysis WORLD already produced, so
                // this costs peak picking rather than another pass over the
                // audio.
                formantTrack = vocalyx::trackFormants(world.spectrogram(),
                                                      world.fundamentalTrack(),
                                                      world.sampleRate(),
                                                      world.fftSize(),
                                                      world.framePeriodMs());

                const vocalyx::SpeakerMeasurements measured =
                    vocalyx::measureSpeaker(formantTrack, world.spectrogram(),
                                            world.fundamentalTrack(),
                                            world.sampleRate(), world.fftSize());

                setMeasurements(measured);

                if (measured.valid) {
                    measuredTractCm.store(measured.tractLengthCm);
                    measuredTractIsNew.store(true);
                    localSource.tractLengthCm = measured.tractLengthCm;
                    setStatus("analysis complete, tract measured");
                } else {
                    setStatus("analysis complete, tract estimate unreliable");
                }

                // This pass already copied the model before the tracker ran, so
                // give it the formants directly.
                int found = 0;
                for (int n = 0; n < vocalyx::kFormantCount && n < 4; ++n) {
                    const float hz = measured.medianFormantHz[static_cast<size_t>(n)];
                    localSource.formantHz[static_cast<size_t>(n)] = hz;

                    if (hz > 0.0f)
                        ++found;
                }

                localSource.formantsMeasured = found >= 2;

                buildDisplayData();
            } else {
                setStatus("analysis failed: " + error);
            }

            std::lock_guard<std::mutex> lock(renderMutex);
            analysisPending = false;
        }

        // --- physiology to acoustics ------------------------------------------
        // A conversion arrived: analyze it and hand it to the engine to blend
        // against. Done after the source analysis, which it depends on.
        if (localMorphClip && localMorphClip->numFrames() > 0) {
            analyzing.store(true);

            std::string error;

            if (morphSource.analyze(localMorphClip->channels, localMorphClip->sampleRate, error)
                && world.setMorphTarget(morphSource, error)) {
                // The filter path needs the same envelope, in its own terms.
                filter.setMorphEnvelope(morphSource.spectrogram(),
                                        morphSource.fftSize(),
                                        morphSource.sampleRate(),
                                        morphSource.framePeriodMs());

                morphLoaded.store(true);
                setStatus("conversion loaded, morph available");
            } else {
                morphLoaded.store(false);
                setStatus("conversion not usable: " + error);
            }

            analyzing.store(false);
        }

        const vocalyx::Acoustics acoustics = vocalyx::deriveAcoustics(localPhysiology, localSource);

        rendering.store(true);

        if (localEngine == Engine::Filter) {
            // Filter the original rather than rebuilding it. Pitch changes are
            // outside what this can do, so they go through Rubber Band first
            // and the envelope work happens on its output.
            const auto began = std::chrono::steady_clock::now();

            filter.prepare(localClip->sampleRate);

            std::vector<std::vector<float>> staged;
            const bool needsPitch = std::fabs(acoustics.pitchRatio - 1.0f) > 0.005f;

            if (needsPitch) {
                vocalyx::Parameters params;
                params.pitchSemitones = static_cast<float>(
                    12.0 * std::log2(std::max(0.25f, acoustics.pitchRatio)));
                params.formantSemitones = 0.0f;   // formants stay put; the filter moves them

                rubberBand.setParameters(params);
                rubberBand.processBuffer(localClip->channels, staged);
            } else {
                staged = localClip->channels;
            }

            auto result = std::make_shared<Clip>();
            result->sampleRate = localClip->sampleRate;

            std::string error;

            if (filter.process(staged, acoustics, result->channels, error)) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - began);

                renderMillis.store(static_cast<int>(elapsed.count()));

                matchLoudness(localClip->channels, result->channels);
                lastFullRender = result;
                previewShowing.store(false);
                publishProcessed(result);
            } else {
                setStatus("filter failed: " + error);
            }
        } else if (localEngine == Engine::World && world.hasAnalysis()) {
            // Preview: three seconds around the playhead, patched into the last
            // full render. Fast enough that a slider feels connected to the
            // sound.
            const auto previewBegan = std::chrono::steady_clock::now();

            auto preview = renderWorldPreview(acoustics, localClip, lastFullRender,
                                              playhead.load());

            const auto previewElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - previewBegan);

            renderMillis.store(static_cast<int>(previewElapsed.count()));

            if (preview) {
                previewShowing.store(true);
                publishProcessed(preview);
            }

            // Wait a moment. If another change arrives, abandon this pass and
            // preview again rather than spending a second on audio that is
            // already stale.
            {
                std::unique_lock<std::mutex> lock(renderMutex);

                const bool interrupted = renderCv.wait_for(
                    lock, std::chrono::milliseconds(400),
                    [this] { return renderRequested || !renderRunning; });

                if (!renderRunning)
                    return;

                if (interrupted) {
                    rendering.store(false);
                    continue;
                }
            }

            // Nothing else arrived, so render the whole clip.
            const auto fullBegan = std::chrono::steady_clock::now();

            auto full = renderWorldFull(acoustics, localClip);

            const auto fullElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - fullBegan);

            renderMillis.store(static_cast<int>(fullElapsed.count()));

            if (full) {
                lastFullRender = full;
                previewShowing.store(false);
                publishProcessed(full);
            }
        } else {
            // Rubber Band, which can apply the pitch ratio and one uniform
            // formant scale and nothing else in Acoustics.
            const auto began = std::chrono::steady_clock::now();

            auto result = std::make_shared<Clip>();
            result->sampleRate = localClip->sampleRate;

            vocalyx::Parameters params;
            params.pitchSemitones   = static_cast<float>(12.0 * std::log2(std::max(0.25f, acoustics.pitchRatio)));
            params.formantSemitones = averageWarpSemitones(acoustics);
            params.breathiness      = acoustics.aspirationAmount * 0.05f;

            rubberBand.setParameters(params);
            rubberBand.processBuffer(localClip->channels, result->channels);

            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began);

            renderMillis.store(static_cast<int>(elapsed.count()));

            if (!result->channels.empty()) {
                matchLoudness(localClip->channels, result->channels);
                lastFullRender = result;
                previewShowing.store(false);
                publishProcessed(result);
            }
        }

        rendering.store(false);
    }
}

// ---------------------------------------------------------------------------
// audio callback — no allocation, no locks, no file access
// ---------------------------------------------------------------------------

void ofApp::audioOut(ofSoundBuffer& out) {
    const int frames   = static_cast<int>(out.getNumFrames());
    const int channels = static_cast<int>(out.getNumChannels());

    const Clip* clip = bypass.load() ? sourcePtr.load() : processedPtr.load();

    if (clip == nullptr || clip->numFrames() == 0 || !playing.load()) {
        for (int f = 0; f < frames; ++f)
            for (int c = 0; c < channels; ++c)
                out[static_cast<size_t>(f) * static_cast<size_t>(channels) + static_cast<size_t>(c)] = 0.0f;
        return;
    }

    const int clipFrames   = clip->numFrames();
    const int clipChannels = clip->numChannels();

    int pos = playhead.load();
    if (pos >= clipFrames)
        pos = 0;

    for (int f = 0; f < frames; ++f) {
        for (int c = 0; c < channels; ++c) {
            const int sourceChannel = std::min(c, clipChannels - 1);
            out[static_cast<size_t>(f) * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
                clip->channels[static_cast<size_t>(sourceChannel)][static_cast<size_t>(pos)];
        }

        if (++pos >= clipFrames)
            pos = 0;
    }

    playhead.store(pos);
}

// ---------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------

void ofApp::keyPressed(int key) {
    switch (key) {
        case OF_KEY_UP:    medianF0Param    = medianF0Param.get() * 1.03f;    break;
        case OF_KEY_DOWN:  medianF0Param    = medianF0Param.get() / 1.03f;    break;
        case OF_KEY_RIGHT: tractLengthParam = tractLengthParam.get() - 0.25f; break;
        case OF_KEY_LEFT:  tractLengthParam = tractLengthParam.get() + 0.25f; break;

        case '1': sizeParam = 0.35f;  openQuotientParam = 0.55f; break;
        case '2': sizeParam = -0.55f; openQuotientParam = 0.65f; break;
        case '3': sizeParam = -0.95f; openQuotientParam = 0.60f; break;

        case '0':
            // Identity: model the recorded speaker back at themselves.
            targetsActive = false;
            formantTargets = {};
            sizeParam = 0.0f;
            tractLengthParam = sourceTractLengthParam.get();
            pharynxFractionParam = 0.45f;
            larynxHeightParam = 0.0f;
            lipRoundingParam = 0.0f;
            nasalityParam = 0.0f;
            openQuotientParam = 0.6f;
            aspirationParam = 0.0f;
            jitterParam = 0.0f;
            shimmerParam = 0.0f;
            effortParam = 0.0f;
            if (measuredF0.load() > 0.0f)
                medianF0Param = measuredF0.load();
            break;

        case 'h':
            // Hand the handles back to the anatomy sliders.
            targetsActive = false;
            formantTargets = {};
            {
                std::lock_guard<std::mutex> lock(renderMutex);
                readInterfaceIntoModel();
                renderRequested = true;
            }
            renderCv.notify_one();
            break;

        case '[':
            if (!profiles.empty()) {
                targetProfile = (targetProfile + static_cast<int>(profiles.size()) - 1)
                              % static_cast<int>(profiles.size());
                applyProfilePair();
            }
            break;

        case ']':
            if (!profiles.empty()) {
                targetProfile = (targetProfile + 1) % static_cast<int>(profiles.size());
                applyProfilePair();
            }
            break;

        case 'g':
            // Which published group the recording itself belongs to.
            if (!profiles.empty()) {
                referenceProfile = (referenceProfile + 1) % static_cast<int>(profiles.size());
                applyProfilePair();
            }
            break;

        case 'c': {
            // A conversion of the same performance, to morph toward.
            ofFileDialogResult chosen = ofSystemLoadDialog("choose a converted version");

            if (chosen.bSuccess)
                loadMorphFile(chosen.getPath());
            break;
        }

        case 't': showTransformed = !showTransformed; break;
        case 'e': {
            // Filter, then vocoder, then the old pitch shifter.
            engine = (engine == Engine::Filter) ? Engine::World
                   : (engine == Engine::World)  ? Engine::RubberBand
                                                : Engine::Filter;

            float ignored = 0.0f;
            onFloatChanged(ignored);
            break;
        }
        case ' ': playing.store(!playing.load()); break;
        case 'b': bypass.store(!bypass.load());   break;
        case 'r': playhead.store(0);              break;

        case 'm': {
            // Re-adopt the measured tract length, for when a slider has wandered
            // away from it.
            const float measured = measuredTractCm.load();
            if (measured > 0.0f)
                sourceTractLengthParam = std::round(measured * 10.0f) / 10.0f;
            break;
        }

        default: break;
    }
}

void ofApp::dragEvent(ofDragInfo dragInfo) {
    if (!dragInfo.files.empty())
        loadFile(dragInfo.files[0]);
}

// ---------------------------------------------------------------------------
// display
// ---------------------------------------------------------------------------

void ofApp::update() {
    // The analysis measured the recorded pitch; adopt it as the starting target
    // so the first sound you hear is the voice as recorded.
    if (measuredF0IsNew.exchange(false)) {
        const float measured = measuredF0.load();

        if (measured > 0.0f)
            medianF0Param = std::round(measured * 10.0f) / 10.0f;
    }

    // Same for tract length: a measured baseline beats a guessed one, and every
    // transformation is a ratio from it.
    if (measuredTractIsNew.exchange(false)) {
        const float measured = measuredTractCm.load();

        if (measured > 0.0f) {
            const float rounded = std::round(measured * 10.0f) / 10.0f;
            sourceTractLengthParam = rounded;
            tractLengthParam       = rounded;   // start from the speaker as they are
        }
    }

    // The render thread built the spectrogram image; uploading it to the
    // graphics card has to happen here.
    if (displayIsNew.exchange(false)) {
        std::lock_guard<std::mutex> lock(displayMutex);

        if (displayPixels.isAllocated())
            spectrogramTexture.loadData(displayPixels);
    }

    // Where the sliders are currently sending things. Cheap enough to redo
    // every frame, and it means the display never lags the controls.
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        displayAcoustics = vocalyx::deriveAcoustics(physiology, sourceSpeaker);
    }
}

void ofApp::draw() {
    ofSetColor(230);

    int y = 30;
    const int lineHeight = 18;

    ofDrawBitmapString("VOCALYX", 20, y); y += lineHeight;
    ofDrawBitmapString(fileName, 20, y); y += lineHeight;
    ofDrawBitmapString(getStatus(), 20, y); y += lineHeight;

    const float measured = measuredF0.load();
    const char* engineName = (engine == Engine::Filter) ? "filter"
                           : (engine == Engine::World)  ? "WORLD"
                                                        : "Rubber Band";

    std::string engineLine = std::string("engine ") + engineName
                           + "   source " + (bypass.load() ? "untreated" : "processed")
                           + "   " + ofToString(renderMillis.load()) + " ms"
                           + (previewShowing.load() ? " preview" : "");

    if (measured > 0.0f)
        engineLine += "   measured " + ofToString(static_cast<int>(measured)) + " Hz";

    ofDrawBitmapString(engineLine, 20, y);

    gui.draw();

    // --- what the recording measured ---
    const vocalyx::SpeakerMeasurements stats = getMeasurements();

    int right = 300;
    int ry = 150;

    ofSetColor(200);
    ofDrawBitmapString("measured from the recording", right, ry); ry += lineHeight * 2;

    if (stats.valid || stats.medianF0Hz > 0.0f) {
        ofDrawBitmapString("tract      " + (stats.valid ? ofToString(stats.tractLengthCm, 1) + " cm"
                                                        : std::string("unreliable")), right, ry); ry += lineHeight;
        ofDrawBitmapString("pitch      " + ofToString(stats.medianF0Hz, 1) + " Hz", right, ry); ry += lineHeight;
        ofDrawBitmapString("tilt       " + ofToString(stats.spectralTiltDbPerOctave, 1) + " dB/oct", right, ry); ry += lineHeight;
        ofDrawBitmapString("jitter     " + ofToString(stats.jitter * 100.0f, 2) + " %", right, ry); ry += lineHeight;
        ofDrawBitmapString("shimmer    " + ofToString(stats.shimmer * 100.0f, 2) + " %", right, ry); ry += lineHeight * 2;

        for (int n = 0; n < vocalyx::kFormantCount; ++n) {
            const float value = stats.medianFormantHz[static_cast<size_t>(n)];
            ofDrawBitmapString("F" + ofToString(n + 1) + "         "
                               + (value > 0.0f ? ofToString(static_cast<int>(value)) + " Hz"
                                               : std::string("not found")), right, ry);
            ry += lineHeight;
        }
    } else {
        ofDrawBitmapString("(load a file)", right, ry);
    }

    // --- time view ---
    drawTimeView(560.0f, 140.0f, ofGetWidth() - 600.0f, 280.0f);
    drawFormantEditor(560.0f, 450.0f, ofGetWidth() - 600.0f, 150.0f);
    drawVowelView(560.0f, 630.0f, 300.0f, 190.0f);

    ofSetColor(140);
    int help = static_cast<int>(gui.getPosition().y + gui.getHeight()) + 24;
    ofDrawBitmapString("1 2 3   male / female / child     0  back to recorded voice", 20, help); help += lineHeight;
    ofDrawBitmapString("e       cycle engine                b  hear untreated source", 20, help); help += lineHeight;
    ofDrawBitmapString("space   play or pause             m  adopt measured tract", 20, help); help += lineHeight;
    ofDrawBitmapString("r       back to start             t  show transformed formants", 20, help); help += lineHeight;
    ofDrawBitmapString("drag the handles to place each formant;  h  returns them to the anatomy", 20, help); help += lineHeight;

    if (!profiles.empty()) {
        ofDrawBitmapString("g  my group: " + profiles[static_cast<size_t>(referenceProfile)].name
                           + "     [ ]  target: " + profiles[static_cast<size_t>(targetProfile)].name,
                           20, help);
        help += lineHeight;
    }

    ofDrawBitmapString("c  load a conversion to morph toward"
                       + std::string(morphLoaded.load() ? ":  " + morphFileName : ""),
                       20, help);

    const Clip* clip = processedPtr.load();
    if (clip != nullptr && clip->numFrames() > 0) {
        const float w = ofGetWidth() - 40.0f;
        const float t = static_cast<float>(playhead.load()) / static_cast<float>(clip->numFrames());

        ofSetColor(60);
        ofDrawRectangle(20, ofGetHeight() - 40, w, 3);
        ofSetColor(230);
        ofDrawRectangle(20, ofGetHeight() - 40, w * t, 3);
    }
}
