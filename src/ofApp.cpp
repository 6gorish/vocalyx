#include "ofApp.h"
#include "AudioFileLoader.h"

#include <algorithm>
#include <chrono>

// ---------------------------------------------------------------------------
// setup / teardown
// ---------------------------------------------------------------------------

void ofApp::setup() {
    ofSetWindowTitle("Vocalyx");
    ofBackground(18);
    ofSetFrameRate(60);

    gui.setup("vocalyx");
    gui.setPosition(20, 200);

    gui.add(pitchParam.set("pitch (st)", 4.0f, -12.0f, 12.0f));
    gui.add(formantParam.set("formant (st)", 1.5f, -12.0f, 12.0f));
    gui.add(presenceDbParam.set("2.8 kHz (dB)", 0.0f, -12.0f, 12.0f));
    gui.add(airDbParam.set("7 kHz (dB)", 0.0f, -12.0f, 12.0f));
    gui.add(breathinessParam.set("breathiness", 0.0f, 0.0f, 0.1f));

    pitchParam.addListener(this, &ofApp::onParameterChanged);
    formantParam.addListener(this, &ofApp::onParameterChanged);
    presenceDbParam.addListener(this, &ofApp::onParameterChanged);
    airDbParam.addListener(this, &ofApp::onParameterChanged);
    breathinessParam.addListener(this, &ofApp::onParameterChanged);

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        params = currentParameters();
    }

    renderThread = std::thread(&ofApp::renderThreadFunction, this);

    status = "drag an audio file onto the window";
}

void ofApp::exit() {
    pitchParam.removeListener(this, &ofApp::onParameterChanged);
    formantParam.removeListener(this, &ofApp::onParameterChanged);
    presenceDbParam.removeListener(this, &ofApp::onParameterChanged);
    airDbParam.removeListener(this, &ofApp::onParameterChanged);
    breathinessParam.removeListener(this, &ofApp::onParameterChanged);

    // Stop the audio callback first, so nothing is reading the clips while they
    // are released.
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
// parameters
// ---------------------------------------------------------------------------

vocalyx::Parameters ofApp::currentParameters() const {
    vocalyx::Parameters p;

    p.pitchSemitones   = pitchParam.get();
    p.formantSemitones = formantParam.get();
    p.breathiness      = breathinessParam.get();

    // Two fixed bands: presence, where pitch shifting tends to leave harshness,
    // and air, which a raised voice often lacks. Only the gains are exposed.
    p.eqBands.push_back({2800.0f, presenceDbParam.get(), 1.2f});
    p.eqBands.push_back({7000.0f, airDbParam.get(),      0.9f});

    return p;
}

void ofApp::onParameterChanged(float&) {
    {
        std::lock_guard<std::mutex> lock(renderMutex);
        params = currentParameters();
        renderRequested = true;
    }
    renderCv.notify_one();
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
        status = error;
        return;
    }

    auto clip = std::make_shared<Clip>();
    clip->channels   = std::move(channels);
    clip->sampleRate = fileSampleRate;

    playing.store(false);
    playhead.store(0);

    {
        std::lock_guard<std::mutex> lock(renderMutex);
        sourceClip = clip;
    }

    publishSource(clip);
    publishProcessed(clip);   // play the untouched clip until the first render lands

    fileName = ofFilePath::getFileName(path);

    const int outChannels = std::min(clip->numChannels(), 2);
    if (streamSampleRate != clip->sampleRate || streamChannels != outChannels)
        startAudio(clip->sampleRate, clip->numChannels());

    requestRender();

    playing.store(true);
    status = ofToString(clip->numFrames()) + " frames, "
           + ofToString(clip->numChannels()) + " ch, "
           + ofToString(static_cast<int>(clip->sampleRate)) + " Hz";
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
        status = "could not open the audio output device";
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
        vocalyx::Parameters   localParams;
        std::shared_ptr<Clip> localSource;

        {
            std::unique_lock<std::mutex> lock(renderMutex);
            renderCv.wait(lock, [this] { return renderRequested || !renderRunning; });

            if (!renderRunning)
                return;

            renderRequested = false;
            localParams     = params;
            localSource     = sourceClip;
        }

        if (!localSource || localSource->numFrames() == 0)
            continue;

        rendering.store(true);

        const auto began = std::chrono::steady_clock::now();

        auto result = std::make_shared<Clip>();
        result->sampleRate = localSource->sampleRate;

        processor.setParameters(localParams);
        processor.processBuffer(localSource->channels, result->channels);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);

        renderMillis.store(static_cast<int>(elapsed.count()));

        publishProcessed(result);

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
            const int sourceChannel = std::min(c, clipChannels - 1);   // mono feeds both outputs
            out[static_cast<size_t>(f) * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
                clip->channels[static_cast<size_t>(sourceChannel)][static_cast<size_t>(pos)];
        }

        if (++pos >= clipFrames)
            pos = 0;   // loop
    }

    playhead.store(pos);
}

// ---------------------------------------------------------------------------
// keys
// ---------------------------------------------------------------------------

void ofApp::keyPressed(int key) {
    // The sliders hold the values; the keys nudge them, and their listeners do
    // the rest.
    switch (key) {
        case OF_KEY_UP:    pitchParam   = pitchParam.get() + 0.5f;   break;
        case OF_KEY_DOWN:  pitchParam   = pitchParam.get() - 0.5f;   break;
        case OF_KEY_RIGHT: formantParam = formantParam.get() + 0.5f; break;
        case OF_KEY_LEFT:  formantParam = formantParam.get() - 0.5f; break;

        case '0':
            pitchParam   = 0.0f;
            formantParam = 0.0f;
            break;

        case ' ': playing.store(!playing.load()); break;
        case 'b': bypass.store(!bypass.load());   break;
        case 'r': playhead.store(0);              break;

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

void ofApp::update() {}

void ofApp::draw() {
    ofSetColor(230);

    int y = 30;
    const int lineHeight = 18;

    ofDrawBitmapString("VOCALYX", 20, y); y += lineHeight * 2;
    ofDrawBitmapString(fileName, 20, y); y += lineHeight;
    ofDrawBitmapString(status, 20, y); y += lineHeight * 2;

    ofDrawBitmapString(std::string("source   ") + (bypass.load() ? "untreated" : "processed"), 20, y); y += lineHeight;
    ofDrawBitmapString(std::string("render   ")
                       + (rendering.load() ? "working" : "idle")
                       + "  (" + ofToString(renderMillis.load()) + " ms)", 20, y);

    gui.draw();

    ofSetColor(140);
    int help = static_cast<int>(gui.getPosition().y + gui.getHeight()) + 30;
    ofDrawBitmapString("arrows   pitch and formant by half steps", 20, help); help += lineHeight;
    ofDrawBitmapString("0        reset pitch and formant", 20, help); help += lineHeight;
    ofDrawBitmapString("space    play / pause", 20, help); help += lineHeight;
    ofDrawBitmapString("b        toggle untreated source", 20, help); help += lineHeight;
    ofDrawBitmapString("r        back to start", 20, help);

    const Clip* clip = processedPtr.load();
    if (clip != nullptr && clip->numFrames() > 0) {
        const float w = ofGetWidth() - 40.0f;
        const float t = static_cast<float>(playhead.load()) / static_cast<float>(clip->numFrames());

        ofSetColor(60);
        ofDrawRectangle(20, ofGetHeight() - 60, w, 3);
        ofSetColor(230);
        ofDrawRectangle(20, ofGetHeight() - 60, w * t, 3);
    }
}
