#pragma once

#include "ofMain.h"
#include "ofxGui.h"
#include "VocalProcessor.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A decoded clip, non-interleaved: one vector per channel, all the same length.
// Immutable once built, which is what makes it safe to hand to the audio thread
// by pointer.
struct Clip {
    std::vector<std::vector<float>> channels;
    double sampleRate = 0.0;

    int numChannels() const { return static_cast<int>(channels.size()); }
    int numFrames() const { return channels.empty() ? 0 : static_cast<int>(channels[0].size()); }
};

class ofApp : public ofBaseApp {
public:
    void setup() override;
    void update() override;
    void draw() override;
    void exit() override;

    void audioOut(ofSoundBuffer& out) override;

    void keyPressed(int key) override;
    void dragEvent(ofDragInfo dragInfo) override;

    void keyReleased(int key) override {}
    void mouseMoved(int x, int y) override {}
    void mouseDragged(int x, int y, int button) override {}
    void mousePressed(int x, int y, int button) override {}
    void mouseReleased(int x, int y, int button) override {}
    void mouseEntered(int x, int y) override {}
    void mouseExited(int x, int y) override {}
    void windowResized(int w, int h) override {}
    void gotMessage(ofMessage msg) override {}

private:
    void loadFile(const std::string& path);
    void startAudio(double sampleRate, int numChannels);
    void requestRender();
    void renderThreadFunction();

    void publishSource(const std::shared_ptr<Clip>& clip);
    void publishProcessed(const std::shared_ptr<Clip>& clip);
    void retain(const std::shared_ptr<Clip>& clip);

    // Slider changes land here, get copied into the processing parameters, and
    // wake the render thread.
    void onParameterChanged(float& value);
    vocalyx::Parameters currentParameters() const;

    // --- interface ---
    ofxPanel gui;
    ofParameter<float> pitchParam;
    ofParameter<float> formantParam;
    ofParameter<float> presenceDbParam;    // peaking cut or boost around 2.8 kHz
    ofParameter<float> airDbParam;         // peaking cut or boost around 7 kHz
    ofParameter<float> breathinessParam;

    // --- audio output ---
    ofSoundStream soundStream;
    double streamSampleRate = 0.0;
    int    streamChannels   = 0;

    // --- clips ---
    // The audio thread reads these raw pointers and nothing else. Ownership
    // lives in keepAlive, which holds the last several clips so a pointer the
    // callback is mid-way through using cannot be freed underneath it.
    std::atomic<Clip*> sourcePtr{nullptr};
    std::atomic<Clip*> processedPtr{nullptr};

    std::deque<std::shared_ptr<Clip>> keepAlive;
    std::mutex                        keepAliveMutex;
    static constexpr size_t           keepAliveMax = 8;

    std::atomic<int>  playhead{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> bypass{false};

    // --- processing ---
    vocalyx::VocalProcessor processor;
    vocalyx::Parameters     params;        // guarded by renderMutex
    std::shared_ptr<Clip>   sourceClip;    // main and render threads only

    std::thread             renderThread;
    std::mutex              renderMutex;
    std::condition_variable renderCv;
    bool                    renderRequested = false;
    bool                    renderRunning   = true;
    std::atomic<bool>       rendering{false};
    std::atomic<int>        renderMillis{0};

    // --- display ---
    std::string fileName = "no file loaded";
    std::string status;
};
