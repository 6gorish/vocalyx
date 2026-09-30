#pragma once

#include "ofMain.h"
#include "ofxGui.h"

#include "FilterEngine.h"
#include "FormantTracker.h"
#include "Physiology.h"
#include "SpeakerProfiles.h"
#include "VocalProcessor.h"
#include "WorldEngine.h"

#include <atomic>
#include <array>
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

    void mouseDragged(int x, int y, int button) override;
    void mousePressed(int x, int y, int button) override;
    void mouseReleased(int x, int y, int button) override;

    void keyReleased(int key) override {}
    void mouseMoved(int x, int y) override {}
    void mouseEntered(int x, int y) override {}
    void mouseExited(int x, int y) override {}
    void windowResized(int w, int h) override {}
    void gotMessage(ofMessage msg) override {}

private:
    void loadFile(const std::string& path);
    void loadMorphFile(const std::string& path);
    void startAudio(double sampleRate, int numChannels);
    void requestRender();
    void renderThreadFunction();

    void publishSource(const std::shared_ptr<Clip>& clip);
    void publishProcessed(const std::shared_ptr<Clip>& clip);
    void retain(const std::shared_ptr<Clip>& clip);

    // Render thread only.
    std::shared_ptr<Clip> renderWorldFull(const vocalyx::Acoustics& acoustics,
                                          const std::shared_ptr<Clip>& source);
    std::shared_ptr<Clip> renderWorldPreview(const vocalyx::Acoustics& acoustics,
                                             const std::shared_ptr<Clip>& source,
                                             const std::shared_ptr<Clip>& base,
                                             int centerSample);

    // Builds the spectrogram image and the formant polylines from the analysis.
    // Runs on the render thread; the texture upload happens in update, because
    // graphics calls belong to the main thread.
    void buildDisplayData();
    void drawTimeView(float x, float y, float w, float h);
    void drawVowelView(float x, float y, float w, float h);
    void drawFormantEditor(float x, float y, float w, float h);

    // Frequency to editor pixel and back.
    float editorHzToX(float hz) const;
    float editorXToHz(float px) const;

    void onFloatChanged(float& value);
    void onSizeChanged(float& value);
    void onBoolChanged(bool& value);

    // Sets the handles and the pitch from the chosen pair of profiles.
    void applyProfilePair();
    void readInterfaceIntoModel();     // call with renderMutex held
    void setStatus(const std::string& text);
    std::string getStatus() const;

    void setMeasurements(const vocalyx::SpeakerMeasurements& measured);
    vocalyx::SpeakerMeasurements getMeasurements() const;

    // --- interface ---
    ofxPanel gui;

    ofParameterGroup tractGroup;
    ofParameter<float> sizeParam;
    ofParameter<float> tractLengthParam;
    ofParameter<float> pharynxFractionParam;
    ofParameter<float> larynxHeightParam;
    ofParameter<float> lipRoundingParam;
    ofParameter<float> nasalityParam;

    ofParameterGroup voiceGroup;
    ofParameter<float> medianF0Param;
    ofParameter<float> openQuotientParam;
    ofParameter<float> aspirationParam;
    ofParameter<float> jitterParam;
    ofParameter<float> shimmerParam;
    ofParameter<float> effortParam;

    ofParameterGroup recordedGroup;
    ofParameter<float> sourceTractLengthParam;

    ofParameterGroup morphGroup;
    ofParameter<float> morphEnvelopeParam;
    ofParameter<float> morphAperiodicityParam;
    ofParameter<float> morphF0Param;

    ofParameter<bool> useWorldParam;

    // --- audio output ---
    ofSoundStream soundStream;
    double streamSampleRate = 0.0;
    int    streamChannels   = 0;

    // --- clips ---
    std::atomic<Clip*> sourcePtr{nullptr};
    std::atomic<Clip*> processedPtr{nullptr};

    std::deque<std::shared_ptr<Clip>> keepAlive;
    std::mutex                        keepAliveMutex;
    static constexpr size_t           keepAliveMax = 8;

    std::atomic<int>  playhead{0};
    std::atomic<bool> playing{false};
    std::atomic<bool> bypass{false};

    // --- processing ---
    // Both engines live on the render thread and are touched nowhere else.
    vocalyx::VocalProcessor rubberBand;
    vocalyx::WorldEngine    world;
    vocalyx::WorldEngine    morphSource;   // the second recording's own analysis
    vocalyx::FilterEngine   filter;

    // Which path the sound takes.
    //   Filter     — the original signal, envelope-filtered. Transparent at
    //                identity, and the right default.
    //   World      — full resynthesis. Needed for pitch-track morphing and
    //                perturbation, and audibly lossy even when doing nothing.
    //   RubberBand — pitch and one uniform formant scale, for comparison.
    enum class Engine { Filter, World, RubberBand };
    Engine engine = Engine::Filter;

    // Guarded by renderMutex: the render thread copies these at the top of each
    // pass rather than reading the interface directly.
    vocalyx::Physiology   physiology;
    vocalyx::SourceSpeaker sourceSpeaker;
    bool                  useWorld        = true;
    Engine                engineChoice    = Engine::Filter;   // copied under the lock
    bool                  analysisPending = false;
    std::shared_ptr<Clip> sourceClip;

    // Set by the main thread when a conversion is chosen, acted on by the
    // render thread, which owns both engines.
    std::shared_ptr<Clip> pendingMorphClip;
    std::string           morphFileName;
    std::atomic<bool>     morphLoaded{false};

    std::thread             renderThread;
    std::mutex              renderMutex;
    std::condition_variable renderCv;
    bool                    renderRequested = false;
    bool                    renderRunning   = true;

    std::atomic<bool>  rendering{false};
    std::atomic<bool>  analyzing{false};
    std::atomic<int>   renderMillis{0};
    std::atomic<float> measuredF0{0.0f};
    std::atomic<bool>  measuredF0IsNew{false};
    std::atomic<float> measuredTractCm{0.0f};
    std::atomic<bool>  measuredTractIsNew{false};

    // Written by the render thread after analysis, read by draw.
    vocalyx::FormantTrack        formantTrack;        // render thread only
    std::shared_ptr<Clip>        lastFullRender;      // render thread only
    vocalyx::SpeakerMeasurements measurements;
    mutable std::mutex           measurementMutex;

    static constexpr double previewSeconds = 3.0;
    std::atomic<bool> previewShowing{false};

    // --- time view ---
    // The pixel buffer and the formant columns are written by the render thread
    // and read by the main thread under displayMutex. The texture belongs to
    // the main thread alone.
    ofPixels                              displayPixels;
    std::vector<std::array<float, 4>>     displayFormants;
    std::mutex                            displayMutex;
    std::atomic<bool>                     displayIsNew{false};
    ofTexture                             spectrogramTexture;
    float                                 displayMaxHz = 5000.0f;

    // The acoustics the sliders currently describe, recomputed on the main
    // thread each frame so the display can show where the formants are being
    // sent without waiting for a render.
    vocalyx::Acoustics displayAcoustics;
    bool               showTransformed = true;

    // --- formant editor ---
    // Where each measured formant is being sent. Dragging a handle writes here,
    // and the warp is built from these pairs directly.
    std::array<float, 4> formantTargets{};
    bool                 targetsActive = false;
    int                  draggingHandle = -1;
    ofRectangle          editorRect;
    bool                 applyingSize = false;

    // --- speaker profiles ---
    std::vector<vocalyx::SpeakerProfile> profiles;
    int referenceProfile = 0;
    int targetProfile    = 0;

    // --- display ---
    std::string         fileName = "no file loaded";
    std::string         status;
    mutable std::mutex  statusMutex;
};
