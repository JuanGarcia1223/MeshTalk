#pragma once

// Shared helpers used by the TerminalUI*.cpp translation units. Not part of
// the public TerminalUI interface — only included from within src/ui/.

#include "ui/TerminalUI.h"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace tui_detail {

inline uint64_t make_channels(unsigned fg_r, unsigned fg_g, unsigned fg_b, unsigned bg_r,
                               unsigned bg_g, unsigned bg_b) {
    uint64_t ch = 0;
    ncchannels_set_fg_rgb8(&ch, fg_r, fg_g, fg_b);
    ncchannels_set_bg_rgb8(&ch, bg_r, bg_g, bg_b);
    return ch;
}

inline std::string normalize_datetime_display(const std::string& input) {
    if (input.empty()) {
        return TerminalUI::local_datetime_now();
    }

    std::string out = input;
    std::replace(out.begin(), out.end(), 'T', ' ');
    if (!out.empty() && out.back() == 'Z') {
        out.pop_back();
    }

    // Prefer "YYYY-MM-DD HH:MM" when enough data exists.
    if (out.size() >= 16) {
        return out.substr(0, 16);
    }
    return out;
}

// Peers embed a UTC "sent" timestamp in the wire protocol, but the epoch-ms
// value that travels alongside it is timezone-agnostic, so deriving the
// displayed time from timestamp_ms (converted to local time) avoids showing
// the sender's UTC clock on screen. Falls back to the string form for
// pre-migration rows that predate timestamp_ms.
inline std::string format_display_datetime(const std::string& fallback, int64_t timestamp_ms) {
    if (timestamp_ms <= 0) {
        return normalize_datetime_display(fallback);
    }
    const std::time_t tt = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return oss.str();
}

}  // namespace tui_detail
