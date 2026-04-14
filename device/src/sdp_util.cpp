// sdp_util.cpp
// SDP utility functions shared by webrtc_signaling and webrtc_media modules.

#include "webrtc_media.h"
#include <sstream>
#include <unordered_set>
#include <vector>

std::string extract_sdp_summary(const std::string& sdp) {
    if (sdp.empty()) return {};

    std::vector<std::string> codecs;
    std::unordered_set<std::string> seen;

    std::istringstream stream(sdp);
    std::string line;
    while (std::getline(stream, line)) {
        // Strip trailing \r if present (handles \r\n line endings)
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Match lines starting with "a=rtpmap:"
        const std::string prefix = "a=rtpmap:";
        if (line.compare(0, prefix.size(), prefix) != 0) continue;

        // Format: a=rtpmap:<pt> <codec>/<rate>[/<params>]
        auto space_pos = line.find(' ', prefix.size());
        if (space_pos == std::string::npos) continue;

        auto slash_pos = line.find('/', space_pos + 1);
        if (slash_pos == std::string::npos) continue;

        std::string codec = line.substr(space_pos + 1, slash_pos - space_pos - 1);
        if (!codec.empty() && seen.insert(codec).second) {
            codecs.push_back(codec);
        }
    }

    if (codecs.empty()) return {};

    std::string result;
    for (size_t i = 0; i < codecs.size(); ++i) {
        if (i > 0) result += ", ";
        result += codecs[i];
    }
    return result;
}
