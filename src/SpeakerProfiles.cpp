#include "SpeakerProfiles.h"

#include <fstream>
#include <sstream>

namespace vocalyx {

std::vector<SpeakerProfile> loadProfiles(const std::string& path) {
    std::vector<SpeakerProfile> profiles;

    std::ifstream file(path);
    if (!file)
        return profiles;

    std::string line;

    while (std::getline(file, line)) {
        // Trim leading whitespace, then skip blanks and comments.
        const size_t start = line.find_first_not_of(" \t\r\n");

        if (start == std::string::npos)
            continue;

        if (line[start] == '#')
            continue;

        std::istringstream stream(line.substr(start));

        SpeakerProfile profile;
        stream >> profile.name >> profile.f0Hz;

        for (int n = 0; n < 4; ++n)
            stream >> profile.formantHz[static_cast<size_t>(n)];

        // A line that did not parse fully is ignored rather than half-used.
        if (stream.fail() || profile.name.empty() || profile.f0Hz <= 0.0f)
            continue;

        profiles.push_back(profile);
    }

    return profiles;
}

ProfileRatios ratiosBetween(const SpeakerProfile& reference, const SpeakerProfile& target) {
    ProfileRatios out;

    if (reference.f0Hz > 0.0f)
        out.f0 = target.f0Hz / reference.f0Hz;

    for (int n = 0; n < 4; ++n) {
        const float from = reference.formantHz[static_cast<size_t>(n)];
        const float to   = target.formantHz[static_cast<size_t>(n)];

        if (from > 0.0f && to > 0.0f)
            out.formant[static_cast<size_t>(n)] = to / from;
    }

    return out;
}

} // namespace vocalyx
