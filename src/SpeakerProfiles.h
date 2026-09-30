#pragma once

// Speaker profiles from published phonetic data.
//
// A profile is one group's average fundamental and first four formants. On its
// own it says nothing about a transformation; a pair does. Choosing the group
// your recording belongs to as the reference and another as the target gives
// four formant ratios and a pitch ratio, which is exactly what the warp needs.
//
// Working in ratios rather than absolute frequencies is what the published data
// actually supports. Those numbers come from isolated words read aloud, so
// their absolute values do not describe connected speech, while the relation
// between two groups measured the same way transfers.

#include <array>
#include <string>
#include <vector>

namespace vocalyx {

struct SpeakerProfile {
    std::string name;
    float f0Hz = 0.0f;
    std::array<float, 4> formantHz{};
};

// What one profile implies relative to another.
struct ProfileRatios {
    float f0 = 1.0f;
    std::array<float, 4> formant{ 1.0f, 1.0f, 1.0f, 1.0f };
};

// Reads the profile file. Blank lines and lines starting with # are skipped.
// Returns an empty vector if the file is missing or unreadable.
std::vector<SpeakerProfile> loadProfiles(const std::string& path);

ProfileRatios ratiosBetween(const SpeakerProfile& reference, const SpeakerProfile& target);

} // namespace vocalyx
