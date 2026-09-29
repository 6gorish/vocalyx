#pragma once

// Audio file loading through Apple's AudioToolbox, which openFrameworks
// already links. openFrameworks 0.12.1 has no file reader of its own: the core
// ships ofSoundBuffer, ofSoundStream and ofSoundPlayer, and nothing that turns
// a path into samples.
//
// Handles WAV, AIFF, CAF, MP3 and M4A, and hands back non-interleaved float
// channels at the file's own sample rate.

#include <string>
#include <vector>

namespace vocalyx {

// Returns true on success. On failure, errorOut explains why and the other
// outputs are left empty.
bool loadAudioFile(const std::string& path,
                   std::vector<std::vector<float>>& channelsOut,
                   double& sampleRateOut,
                   std::string& errorOut);

} // namespace vocalyx
