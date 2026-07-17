#include "ui/TerminalUI.h"
#include "ui/TerminalUIInternal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

using tui_detail::make_channels;
using tui_detail::resolve_local_path;

void TerminalUI::render() {
    if (!nc_) {
        return;
    }

    ncplane* stdp = notcurses_stdplane(nc_);
    if (!stdp) {
        return;
    }

    if (!ensure_layout()) {
        ncplane_erase(stdp);
        const uint64_t ch = make_channels(0xff, 0xff, 0xff, 0x14, 0x14, 0x14);
        ncplane_set_base(stdp, " ", 0, ch);
        ncplane_putstr_yx(stdp, 0, 0, "terminal too small");
        notcurses_render(nc_);
        return;
    }

    draw_contacts();
    draw_chat();
    if (debug_mode_ && debug_plane_) {
        draw_debug();
    }

    // Draw trust modal on top if showing
    if (showing_trust_modal_) {
        draw_trust_modal();
    }

    // Draw identity popup on top if showing
    if (showing_identity_popup_) {
        draw_identity_popup();
    }

    // Draw clear modal on top if showing
    if (showing_clear_modal_) {
        draw_clear_modal();
    }

    // Draw upload popup on top if showing
    if (showing_upload_popup_) {
        draw_upload_popup();
    }

    // Draw download popup on top if showing
    if (showing_download_popup_) {
        draw_download_popup();
    }

    // Draw download result popup on top if showing
    if (showing_download_result_popup_) {
        draw_download_result_popup();
    }

    // Draw alert popup on top if showing
    if (showing_alert_popup_) {
        draw_alert_popup();
    }

    notcurses_render(nc_);
}


bool TerminalUI::ensure_layout() {
    ncplane* stdp = notcurses_stdplane(nc_);
    if (!stdp) {
        return false;
    }

    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(stdp, &rows, &cols);
    if (rows < 10 || cols < 50) {
        destroy_layout();
        return false;
    }

    if (rows != last_rows_ || cols != last_cols_ || !people_plane_ || !chat_plane_ ||
        (debug_mode_ && !debug_plane_)) {
        rebuild_layout(rows, cols);
    }
    return people_plane_ && chat_plane_ && (!debug_mode_ || debug_plane_);
}


void TerminalUI::rebuild_layout(unsigned rows, unsigned cols) {
    destroy_layout();

    last_rows_ = rows;
    last_cols_ = cols;

    int app_x = 0;
    int app_w = static_cast<int>(cols);

    if (debug_mode_) {
        app_w = static_cast<int>(cols / 2);
        const int debug_x = app_w;
        const int debug_w = static_cast<int>(cols) - app_w;
        if (debug_w >= 20) {
            debug_plane_ = make_plane(0, debug_x, static_cast<int>(rows), debug_w, "debug");
        }
    }

    if (app_w < 24) {
        return;
    }

    int people_w = std::max(14, (app_w * 20) / 100);
    int chat_w = app_w - people_w;
    if (chat_w < 12) {
        chat_w = 12;
        people_w = app_w - chat_w;
    }

    people_plane_ = make_plane(0, app_x, static_cast<int>(rows), people_w, "people");
    chat_plane_ = make_plane(0, app_x + people_w, static_cast<int>(rows), chat_w, "chat");
}


void TerminalUI::destroy_layout() {
    if (people_plane_) {
        ncplane_destroy(people_plane_);
        people_plane_ = nullptr;
    }
    if (chat_plane_) {
        ncplane_destroy(chat_plane_);
        chat_plane_ = nullptr;
    }
    if (debug_plane_) {
        ncplane_destroy(debug_plane_);
        debug_plane_ = nullptr;
    }
    last_rows_ = 0;
    last_cols_ = 0;
}


ncplane* TerminalUI::make_plane(int y, int x, int h, int w, const char* name) {
    if (h < 2 || w < 2) {
        return nullptr;
    }
    ncplane_options opts{};
    opts.y = y;
    opts.x = x;
    opts.rows = h;
    opts.cols = w;
    opts.name = name;
    return ncplane_create(notcurses_stdplane(nc_), &opts);
}


void TerminalUI::draw_panel(ncplane* plane, const std::string& title, uint64_t border_channels,
                            uint64_t text_channels, uint32_t bg_ul, uint32_t bg_ur,
                            uint32_t bg_ll, uint32_t bg_lr) {
    if (!plane) {
        return;
    }

    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(plane, &rows, &cols);
    if (rows < 2 || cols < 2) {
        return;
    }

    ncplane_erase(plane);

    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_ul);
    ncchannels_set_bg_rgb(&ur, bg_ur);
    ncchannels_set_bg_rgb(&ll, bg_ll);
    ncchannels_set_bg_rgb(&lr, bg_lr);
    ncchannels_set_fg_rgb(&ul, bg_ul);
    ncchannels_set_fg_rgb(&ur, bg_ur);
    ncchannels_set_fg_rgb(&ll, bg_ll);
    ncchannels_set_fg_rgb(&lr, bg_lr);
    ncplane_gradient(plane, 0, 0, rows, cols, " ", 0, ul, ur, ll, lr);

    ncplane_rounded_box_sized(plane, 0, border_channels, static_cast<int>(rows),
                              static_cast<int>(cols), 0);

    if (!title.empty() && cols > 4) {
        ncplane_set_channels(plane, text_channels);
        ncplane_on_styles(plane, NCSTYLE_BOLD);
        const std::string capped = title.substr(0, static_cast<size_t>(cols - 4));
        ncplane_putstr_yx(plane, 0, 2, capped.c_str());
        ncplane_off_styles(plane, NCSTYLE_BOLD);
    }
}


void TerminalUI::draw_debug() {
    const uint64_t border_ch = make_channels(0xf5, 0xd0, 0xfe, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xf8, 0xfa, 0xfc, 0x22, 0x12, 0x29);

    draw_panel(debug_plane_, " Debug ", border_ch, text_ch, 0x221229, 0x2a1633, 0x150b1f,
               0x1a1026);

    if (!debug_plane_) {
        return;
    }

    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(debug_plane_, &rows, &cols);
    if (rows < 4 || cols < 4) {
        return;
    }

    const int max_width = static_cast<int>(cols) - 2;
    const int max_rows = static_cast<int>(rows) - 2;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        lines = debug_lines_;
    }

    const int total = static_cast<int>(lines.size());

    // Work backwards from the end to find the first line that fits in max_rows, accounting for text wrapping.
    int start = total;
    int consumed_rows = 0;
    for (int i = total - 1; i >= 0; --i) {
        int line_rows = (static_cast<int>(lines[i].size()) + max_width - 1) / max_width;
        if (line_rows < 1) line_rows = 1;
        consumed_rows += line_rows;
        if (consumed_rows > max_rows) {
            start = i + 1;
            break;
        }
        start = i;
    }

    ncplane_set_channels(debug_plane_, text_ch);
    int row = 1;
    for (int i = start; i < total && row < static_cast<int>(rows) - 1; ++i) {
        const std::string& line = lines[i];
        if (static_cast<int>(line.size()) <= max_width) {
            ncplane_putstr_yx(debug_plane_, row, 1, line.c_str());
            ++row;
        } else {
            for (size_t pos = 0; pos < line.size() && row < static_cast<int>(rows) - 1; pos += max_width, ++row) {
                ncplane_putstr_yx(debug_plane_, row, 1, line.substr(pos, max_width).c_str());
            }
        }
    }
}


void TerminalUI::draw_contacts() {
    const uint64_t border_ch = make_channels(0x86, 0xef, 0xac, 0x0b, 0x1c, 0x16);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0b, 0x1c, 0x16);
    const uint64_t selected_ch = make_channels(0xff, 0xff, 0xff, 0x1f, 0x60, 0x48);
    const uint64_t online_ch = make_channels(0x22, 0xc5, 0x5e, 0x0b, 0x1c, 0x16);  // Green for online dot
    const uint64_t dim_ch = make_channels(0x80, 0x80, 0x80, 0x0b, 0x1c, 0x16);  // Grey for pending
    const uint64_t warning_ch = make_channels(0xff, 0x44, 0x44, 0x0b, 0x1c, 0x16);  // Red for mismatch
    const uint64_t unread_ch = make_channels(0xff, 0xaa, 0x00, 0x0b, 0x1c, 0x16);  // Orange for unread

    draw_panel(people_plane_, " People ", border_ch, text_ch, 0x0b1c16, 0x0b1c16, 0x10271f,
               0x10271f);

    if (!people_plane_) {
        return;
    }

    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(people_plane_, &rows, &cols);
    if (rows < 4 || cols < 4) {
        return;
    }

    // Build separate lists for trusted and pending peers
    std::vector<PeerInfo> trusted_list;
    std::vector<PeerInfo> pending_list;
    std::set<std::string> online_set;
    std::set<std::string> trusted_set;
    std::set<std::string> mismatch_set;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        // Self always first in trusted list
        trusted_list.push_back(PeerInfo{"self", "", 0});
        for (const auto& kv : peers_) {
            const auto& name = kv.first;
            if (trusted_peers_.count(name) > 0) {
                trusted_list.push_back(kv.second);
            } else {
                pending_list.push_back(kv.second);
            }
        }
        online_set = online_peers_;
        trusted_set = trusted_peers_;
        mismatch_set = mismatch_peers_;
    }

    // Combine for selection tracking
    people_rows_.clear();
    people_rows_.reserve(1 + trusted_list.size() + pending_list.size());
    for (const auto& p : trusted_list) {
        people_rows_.push_back(p);
    }
    for (const auto& p : pending_list) {
        people_rows_.push_back(p);
    }

    if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        selected_peer_index_ = 0;
    }

    int y = 2;

    // Draw trusted section
    for (size_t i = 0; i < trusted_list.size() && y < static_cast<int>(rows) - 1; ++i) {
        const bool selected = static_cast<int>(i) == selected_peer_index_;
        const bool is_self = trusted_list[i].username == "self";
        const bool is_online = !is_self && online_set.count(trusted_list[i].username) > 0;
        int unread = 0;
        if (!is_self) {
            auto uit = unread_counts_.find(trusted_list[i].username);
            if (uit != unread_counts_.end()) unread = uit->second;
        }

        ncplane_set_channels(people_plane_, selected ? selected_ch : text_ch);
        if (selected) {
            ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
        }

        // Build line: "  ● username" or "  ○ username"
        std::string line;
        if (is_self) {
            line = "  " + trusted_list[i].username;
        } else {
            line = is_online ? "  ● " : "  ○ ";
            line += trusted_list[i].username;
        }

        if (line.size() > cols - 2) {
            line.resize(cols - 2);
        }

        // Draw the dot in green if online
        if (!is_self && is_online && cols > 4) {
            ncplane_set_channels(people_plane_, online_ch);
            ncplane_putstr_yx(people_plane_, y, 1, " ●");
            ncplane_set_channels(people_plane_, selected ? selected_ch : text_ch);
            if (selected) {
                ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
            }
            std::string name_part = " " + trusted_list[i].username;
            if (name_part.size() > cols - 4) {
                name_part.resize(cols - 4);
            }
            int x = 3 + static_cast<int>(name_part.size());
            ncplane_putstr_yx(people_plane_, y, 3, name_part.c_str());
            if (unread > 0 && x < static_cast<int>(cols) - 2) {
                ncplane_set_channels(people_plane_, unread_ch);
                ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
                std::string unread_str = " +" + std::to_string(unread);
                if (x + static_cast<int>(unread_str.size()) > static_cast<int>(cols) - 2) {
                    unread_str = " +";
                }
                ncplane_putstr_yx(people_plane_, y, x, unread_str.c_str());
                ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
            }
        } else {
            ncplane_putstr_yx(people_plane_, y, 1, line.c_str());
            if (!is_self && unread > 0) {
                int x = 1 + static_cast<int>(line.size());
                if (x < static_cast<int>(cols) - 2) {
                    ncplane_set_channels(people_plane_, unread_ch);
                    ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
                    std::string unread_str = " +" + std::to_string(unread);
                    if (x + static_cast<int>(unread_str.size()) > static_cast<int>(cols) - 2) {
                        unread_str = " +";
                    }
                    ncplane_putstr_yx(people_plane_, y, x, unread_str.c_str());
                    ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
                }
            }
        }

        if (selected) {
            ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
        }
        ++y;
    }

    // Draw separator if there are pending peers
    if (!pending_list.empty() && y < static_cast<int>(rows) - 1) {
        ncplane_set_channels(people_plane_, dim_ch);
        std::string sep = " ── unknown ── ";
        int padding = (static_cast<int>(cols) - 2 - static_cast<int>(sep.size())) / 2;
        if (padding < 0) padding = 0;
        std::string line(padding, ' ');
        line += sep;
        if (line.size() > cols - 2) {
            line.resize(cols - 2);
        }
        ncplane_putstr_yx(people_plane_, y, 1, line.c_str());
        ++y;
    }

    // Draw pending section (starting after trusted section)
    size_t pending_offset = trusted_list.size();
    for (size_t i = 0; i < pending_list.size() && y < static_cast<int>(rows) - 1; ++i) {
        const size_t global_idx = pending_offset + i;
        const bool selected = static_cast<int>(global_idx) == selected_peer_index_;
        const bool is_online = online_set.count(pending_list[i].username) > 0;
        const bool is_mismatch = mismatch_set.count(pending_list[i].username) > 0;
        int unread = 0;
        auto uit = unread_counts_.find(pending_list[i].username);
        if (uit != unread_counts_.end()) unread = uit->second;

        // Use dim color for pending peers
        uint64_t peer_ch = selected ? selected_ch : dim_ch;
        if (is_mismatch) {
            peer_ch = selected ? selected_ch : warning_ch;
        }
        ncplane_set_channels(people_plane_, peer_ch);
        if (selected) {
            ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
        }

        // Build line: "  ● username" or "  ○ username" or "  ! username" for mismatch
        std::string line;
        if (is_mismatch) {
            line = "  ! ";
        } else {
            line = is_online ? "  ● " : "  ○ ";
        }
        line += pending_list[i].username;

        if (line.size() > cols - 2) {
            line.resize(cols - 2);
        }

        // Draw the dot in appropriate color
        if (cols > 4) {
            if (is_mismatch) {
                ncplane_set_channels(people_plane_, warning_ch);
                ncplane_putstr_yx(people_plane_, y, 1, " !");
            } else if (is_online) {
                ncplane_set_channels(people_plane_, online_ch);
                ncplane_putstr_yx(people_plane_, y, 1, " ●");
            } else {
                ncplane_set_channels(people_plane_, dim_ch);
                ncplane_putstr_yx(people_plane_, y, 1, " ○");
            }
            ncplane_set_channels(people_plane_, peer_ch);
            if (selected) {
                ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
            }
            std::string name_part = " " + pending_list[i].username;
            if (name_part.size() > cols - 4) {
                name_part.resize(cols - 4);
            }
            int x = 3 + static_cast<int>(name_part.size());
            ncplane_putstr_yx(people_plane_, y, 3, name_part.c_str());
            if (unread > 0 && x < static_cast<int>(cols) - 2) {
                ncplane_set_channels(people_plane_, unread_ch);
                ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
                std::string unread_str = " +" + std::to_string(unread);
                if (x + static_cast<int>(unread_str.size()) > static_cast<int>(cols) - 2) {
                    unread_str = " +";
                }
                ncplane_putstr_yx(people_plane_, y, x, unread_str.c_str());
                ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
            }
        } else {
            ncplane_putstr_yx(people_plane_, y, 1, line.c_str());
        }

        if (selected) {
            ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
        }
        ++y;
    }
}


bool TerminalUI::handle_people_click(int abs_y, int abs_x) {
    if (!people_plane_) {
        return false;
    }

    int plane_y = 0;
    int plane_x = 0;
    ncplane_abs_yx(people_plane_, &plane_y, &plane_x);

    unsigned h = 0;
    unsigned w = 0;
    ncplane_dim_yx(people_plane_, &h, &w);

    if (abs_y < plane_y || abs_x < plane_x) {
        return false;
    }

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;
    if (rel_y < 2 || rel_y >= static_cast<int>(h) - 1 || rel_x < 1 || rel_x >= static_cast<int>(w) - 1) {
        return false;
    }

    // Check if we have pending peers (separator shown)
    int trusted_count = 0;
    int pending_count = 0;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        // Count trusted peers (including self: 1 + trusted_peers_.size())
        trusted_count = 1 + static_cast<int>(trusted_peers_.size());
        pending_count = static_cast<int>(pending_peers_.size());
    }

    const int visual_idx = rel_y - 2;  // Row within the list (0 = first item)
    int idx = visual_idx;

    // Separator sits at index trusted_count; if shown and click is at/below it, adjust the index.
    if (pending_count > 0 && visual_idx >= trusted_count) {
        // Click is on separator or in pending section
        if (visual_idx == trusted_count) {
            // Click is on the separator itself - ignore
            return false;
        }
        // Click is in pending section, subtract 1 for the separator
        idx = visual_idx - 1;
    }

    if (idx < 0 || idx >= static_cast<int>(people_rows_.size())) {
        return false;
    }

    selected_peer_index_ = idx;
    chat_scroll_offset_ = 0;  // Reset scroll when switching peers
    return activate_selected_peer();
}


bool TerminalUI::activate_selected_peer() {
    if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        return false;
    }

    const PeerInfo peer = people_rows_[selected_peer_index_];
    if (peer.username == "self") {
        return false;
    }

    // Check if peer is trusted
    bool is_trusted = false;
    bool is_pending = false;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        is_trusted = trusted_peers_.count(peer.username) > 0;
        is_pending = pending_peers_.count(peer.username) > 0;
    }

    // If pending, show trust modal and request info
    if (is_pending) {
        std::string fp = peer.fingerprint.empty() ? "UNKNOWN" : peer.fingerprint;
        std::vector<std::pair<std::string, std::string>> cached_info;
        if (db_manager_) {
            auto entries = db_manager_->loadPeerInfo(peer.username);
            for (const auto& e : entries) {
                cached_info.emplace_back(e.key, e.value);
            }
        }
        show_trust_modal(peer.username, fp, cached_info);
        if (peer.ip.empty() || peer.tcp_port == 0) {
            return true;
        }
        if (on_request_info_) {
            on_request_info_(peer.username);
        }
        return true;
    }

    // If not trusted (legacy or mismatch), request info instead of activating
    if (!is_trusted) {
        if (peer.ip.empty() || peer.tcp_port == 0) {
            return false;
        }
        if (on_request_info_) {
            on_request_info_(peer.username);
        }
        return true;
    }

    if (peer.ip.empty() || peer.tcp_port == 0) {
        return false;
    }

    // Clear unread count when selecting a peer
    clear_unread(peer.username);

    if (on_peer_activate_) {
        on_peer_activate_(peer);
        return true;
    }
    return false;
}


bool TerminalUI::is_selected_peer_online() {
    if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        return true;  // Default to true if no selection
    }
    const std::string& name = people_rows_[selected_peer_index_].username;
    if (name == "self") {
        return true;  // Self is always "online"
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return online_peers_.count(name) > 0;
}


bool TerminalUI::is_selected_peer_trusted() {
    if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        return true;  // Default to true if no selection
    }
    const std::string& name = people_rows_[selected_peer_index_].username;
    if (name == "self") {
        return true;  // Self is always trusted
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return trusted_peers_.count(name) > 0;
}


namespace {
std::vector<std::string> wrap_text(const std::string& text, int width) {
    std::vector<std::string> out;
    if (width <= 0) {
        return out;
    }
    if (text.empty()) {
        out.push_back("");
        return out;
    }
    for (size_t i = 0; i < text.size(); i += static_cast<size_t>(width)) {
        out.push_back(text.substr(i, static_cast<size_t>(width)));
    }
    return out;
}
}  // namespace

void TerminalUI::draw_chat() {
    const uint64_t border_ch = make_channels(0x93, 0xc5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t base_text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
    const uint64_t sender_ch = make_channels(0x86, 0xef, 0xac, 0x0f, 0x17, 0x2a);
    const uint64_t receiver_ch = make_channels(0x93, 0xc5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t attachment_ch = make_channels(0xf5, 0xd0, 0x9e, 0x0f, 0x17, 0x2a);  // Amber/gold for attachments
    const uint64_t dt_ch = make_channels(0x94, 0xa3, 0xb8, 0x0f, 0x17, 0x2a);
    const uint64_t input_border_ch = make_channels(0xc4, 0xb5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t offline_ch = make_channels(0x94, 0xa3, 0xb8, 0x0f, 0x17, 0x2a);
    const uint64_t warning_ch = make_channels(0xff, 0x44, 0x44, 0x0f, 0x17, 0x2a);  // Red for untrusted

    std::string active_name = "self";
    if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
        active_name = people_rows_[selected_peer_index_].username;
    }

    const bool peer_online = is_selected_peer_online();
    const bool peer_trusted = is_selected_peer_trusted();

    // Show online/offline status and trust state in the title
    std::string title = " Chat: " + active_name + " ";
    uint64_t title_text_ch = base_text_ch;
    if (active_name != "self") {
        std::string status_tag = peer_online ? "(online)" : "(offline)";
        if (!peer_trusted) {
            title = " Chat: " + active_name + status_tag + "(untrusted) ";
            title_text_ch = warning_ch;
        } else {
            title = " Chat: " + active_name + status_tag + " ";
        }
    }
    draw_panel(chat_plane_, title, border_ch, title_text_ch, 0x0f172a,
               0x111c33, 0x0f172a, 0x111c33);

    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(chat_plane_, &rows, &cols);
    if (rows < 8 || cols < 16) {
        return;
    }

    const int input_w = static_cast<int>(cols) - 2;
    const int input_inner_w = std::max(1, input_w - 2);

    const std::string input_text = "> " + input_buffer_;
    const int input_text_lines =
        std::max(1, static_cast<int>((input_text.size() + static_cast<size_t>(input_inner_w) - 1) /
                                     static_cast<size_t>(input_inner_w)));

    const int max_input_h = std::max(3, static_cast<int>(rows) - 4);
    const int input_h = std::min(max_input_h, input_text_lines + 2);
    const int input_y = static_cast<int>(rows) - 1 - input_h;

    struct VisualLine {
        bool sender{false};
        std::string left;
        std::string right;
        bool pending{false};
    };

    std::vector<ChatItem> items;
    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        auto it = chats_by_peer_.find(active_name);
        if (it != chats_by_peer_.end()) {
            items = it->second;
        }
    }

    std::vector<VisualLine> visual;
    const int content_w = static_cast<int>(cols) - 2;

    for (const ChatItem& msg : items) {
        // Add warning symbol for untrusted peers
        const std::string warning_prefix = peer_trusted ? "" : "⚠";
        const std::string prefix = warning_prefix + (msg.sender ? "S: " : "R: ");
        const std::string text = prefix + msg.content;
        const int dtw = static_cast<int>(msg.datetime.size());

        bool inline_dt = false;
        int first_w = content_w;
        if (!msg.datetime.empty() && content_w >= dtw + 8) {
            inline_dt = true;
            first_w = content_w - dtw - 1;
        }

        std::vector<std::string> text_lines;
        if (first_w > 0) {
            if (static_cast<int>(text.size()) <= first_w) {
                text_lines.push_back(text);
            } else {
                text_lines.push_back(text.substr(0, static_cast<size_t>(first_w)));
                const auto more = wrap_text(text.substr(static_cast<size_t>(first_w)), content_w);
                text_lines.insert(text_lines.end(), more.begin(), more.end());
            }
        }

        if (text_lines.empty()) {
            text_lines.push_back(prefix);
        }

        bool is_pending = msg.sender && msg.status == "pending";
        for (size_t i = 0; i < text_lines.size(); ++i) {
            VisualLine line;
            line.sender = msg.sender;
            line.left = text_lines[i];
            line.pending = is_pending;
            if (i == 0 && inline_dt) {
                line.right = msg.datetime;
            }
            visual.push_back(std::move(line));
        }

        if (!inline_dt && !msg.datetime.empty()) {
            visual.push_back(VisualLine{msg.sender, "", msg.datetime, is_pending});
        }
    }

    const int chat_top = 1;
    const int chat_bottom = input_y - 1;
    const int visible_lines = std::max(0, chat_bottom - chat_top + 1);

    int start = 0;
    if (static_cast<int>(visual.size()) > visible_lines) {
        // Calculate start based on scroll offset (0 = at bottom)
        start = static_cast<int>(visual.size()) - visible_lines - chat_scroll_offset_;
        // Clamp start to valid range
        if (start < 0) {
            start = 0;
            chat_scroll_offset_ = static_cast<int>(visual.size()) - visible_lines;
        }
    } else {
        chat_scroll_offset_ = 0;  // Reset scroll when all content fits
    }

    const uint64_t warning_yellow_ch = make_channels(0xff, 0xff, 0x00, 0x0f, 0x17, 0x2a);  // Yellow for warning
    const uint64_t pending_red_ch = make_channels(0xff, 0xff, 0xff, 0xff, 0x44, 0x44);  // White text on red bg for pending S:

    int y = chat_top;
    for (int i = start; i < static_cast<int>(visual.size()) && y <= chat_bottom; ++i, ++y) {
        const VisualLine& vl = visual[i];
        
        // Check if this is an attachment message (contains 📎)
        bool is_attachment = (vl.left.find("📎") != std::string::npos);
        
        // Use attachment color for attachment messages, otherwise sender/receiver color
        if (is_attachment) {
            ncplane_set_channels(chat_plane_, attachment_ch);
        } else {
            ncplane_set_channels(chat_plane_, vl.sender ? sender_ch : receiver_ch);
        }

        int max_left = content_w;
        if (!vl.right.empty()) {
            max_left = std::max(0, content_w - static_cast<int>(vl.right.size()) - 1);
        }

        std::string left = vl.left;
        if (static_cast<int>(left.size()) > max_left) {
            left.resize(static_cast<size_t>(max_left));
        }

        if (!left.empty()) {
            if (!peer_trusted && left.size() >= 6 && left.substr(0, 6) == "⚠S: ") {
                // Draw warning symbol in yellow
                ncplane_set_channels(chat_plane_, warning_yellow_ch);
                ncplane_putstr_yx(chat_plane_, y, 1, "⚠");
                // Draw S: with red background if pending and enabled, else sender color
                if (enable_ack_show_ && vl.pending) {
                    ncplane_set_channels(chat_plane_, pending_red_ch);
                    ncplane_putstr_yx(chat_plane_, y, 4, "S:");
                    ncplane_set_channels(chat_plane_, sender_ch);
                    ncplane_putstr_yx(chat_plane_, y, 6, left.substr(5).c_str());
                } else {
                    ncplane_set_channels(chat_plane_, sender_ch);
                    ncplane_putstr_yx(chat_plane_, y, 4, left.substr(3).c_str());
                }
            } else if (!peer_trusted && left.size() >= 6 && left.substr(0, 6) == "⚠R: ") {
                // Draw warning symbol in yellow
                ncplane_set_channels(chat_plane_, warning_yellow_ch);
                ncplane_putstr_yx(chat_plane_, y, 1, "⚠");
                // Draw rest in receiver color (⚠ = 3 bytes, so "R: " starts at byte 3)
                ncplane_set_channels(chat_plane_, receiver_ch);
                ncplane_putstr_yx(chat_plane_, y, 4, left.substr(3).c_str());
            } else if (enable_ack_show_ && vl.pending && left.size() >= 3 && left.substr(0, 3) == "S: ") {
                // Draw S: with red background for pending messages (only when enabled)
                ncplane_set_channels(chat_plane_, pending_red_ch);
                ncplane_putstr_yx(chat_plane_, y, 1, "S:");
                ncplane_set_channels(chat_plane_, sender_ch);
                ncplane_putstr_yx(chat_plane_, y, 3, left.substr(3).c_str());
            } else {
                ncplane_putstr_yx(chat_plane_, y, 1, left.c_str());
            }
        }

        if (!vl.right.empty() && static_cast<int>(vl.right.size()) <= content_w) {
            ncplane_set_channels(chat_plane_, dt_ch);
            const int x = 1 + content_w - static_cast<int>(vl.right.size());
            ncplane_putstr_yx(chat_plane_, y, x, vl.right.c_str());
        }
    }

    // Always draw input box (messages queue when peer is offline)
    ncplane_cursor_move_yx(chat_plane_, input_y, 1);
    ncplane_rounded_box_sized(chat_plane_, 0, input_border_ch, input_h, input_w, 0);
    ncplane_set_channels(chat_plane_, base_text_ch);
    ncplane_putstr_yx(chat_plane_, input_y, 3, " Input ");

    const std::vector<std::string> wrapped_input = wrap_text(input_text, input_inner_w);
    const int input_visible = input_h - 2;
    int input_start = static_cast<int>(wrapped_input.size()) - input_visible;
    if (input_start < 0) {
        input_start = 0;
    }

    for (int i = 0; i < input_visible; ++i) {
        const int idx = input_start + i;
        if (idx >= static_cast<int>(wrapped_input.size())) {
            break;
        }
        ncplane_putstr_yx(chat_plane_, input_y + 1 + i, 2, wrapped_input[idx].c_str());
    }

    // Draw blinking cursor at end of input
    if (cursor_visible_) {
        int cursor_line = 0;
        int cursor_x = 2;
        
        if (!input_buffer_.empty() && !wrapped_input.empty()) {
            // Cursor at end of text
            cursor_line = static_cast<int>(wrapped_input.size()) - 1 - input_start;
            if (cursor_line >= 0 && cursor_line < input_visible) {
                const std::string& last_line = wrapped_input[cursor_line + input_start];
                cursor_x = 2 + static_cast<int>(last_line.size());
            }
        }
        
        // Ensure cursor is within bounds
        if (cursor_line >= 0 && cursor_line < input_visible && cursor_x < input_w) {
            // Draw cursor as inverse video block
            ncplane_set_channels(chat_plane_, make_channels(0x00, 0x00, 0x00, 0xff, 0xff, 0xff));
            ncplane_putstr_yx(chat_plane_, input_y + 1 + cursor_line, cursor_x, " ");
        }
    }

    // Draw command menu if showing
    if (showing_command_menu_) {
        draw_command_menu();
    }
}


void TerminalUI::draw_command_menu() {
    if (!chat_plane_) return;

    const uint64_t border_ch = make_channels(0x22, 0xc5, 0x5e, 0x0f, 0x17, 0x2a);  // Green border
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x0f, 0x17, 0x2a);
    const uint64_t cmd_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
    const uint64_t dim_ch = make_channels(0x94, 0xa3, 0xb8, 0x0f, 0x17, 0x2a);
    const uint32_t bg_color = 0x0f172a;  // Chat background color

    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(chat_plane_, &rows, &cols);
    if (rows < 12 || cols < 30) return;

    // Compute dynamic column widths
    size_t max_cmd_len = 0;
    size_t max_desc_len = 0;
    for (const auto& cmd : commands_) {
        max_cmd_len = std::max(max_cmd_len, cmd.first.size());
        max_desc_len = std::max(max_desc_len, cmd.second.size());
    }
    const int cmd_col_w = static_cast<int>(max_cmd_len);
    const int spacing = 3;  // Space between command and description
    const int content_w = 2 + cmd_col_w + spacing + static_cast<int>(max_desc_len);  // padding + cmd + gap + desc
    const int menu_w = std::min(content_w + 4, static_cast<int>(cols) - 4);  // +4 for borders/padding
    const int menu_h = static_cast<int>(commands_.size()) + 3;  // Title + commands + padding
    const int menu_x = (static_cast<int>(cols) - menu_w) / 2;
    const int menu_y = static_cast<int>(rows) - menu_h - 4;  // Above input box

    // Clear background behind menu to prevent text bleeding
    uint64_t bg_ch = 0;
    ncchannels_set_bg_rgb(&bg_ch, bg_color);
    ncchannels_set_fg_rgb(&bg_ch, bg_color);
    for (int y = menu_y; y < menu_y + menu_h && y < static_cast<int>(rows); ++y) {
        for (int x = menu_x; x < menu_x + menu_w && x < static_cast<int>(cols); ++x) {
            ncplane_set_channels(chat_plane_, bg_ch);
            ncplane_putstr_yx(chat_plane_, y, x, " ");
        }
    }

    // Draw menu box
    ncplane_cursor_move_yx(chat_plane_, menu_y, menu_x);
    ncplane_rounded_box_sized(chat_plane_, 0, border_ch, menu_h, menu_w, 0);

    // Title
    ncplane_set_channels(chat_plane_, title_ch);
    ncplane_on_styles(chat_plane_, NCSTYLE_BOLD);
    ncplane_putstr_yx(chat_plane_, menu_y, menu_x + 2, " Commands ");
    ncplane_off_styles(chat_plane_, NCSTYLE_BOLD);

    // Commands with dynamic spacing
    for (size_t i = 0; i < commands_.size(); ++i) {
        int y = menu_y + 1 + static_cast<int>(i);
        ncplane_set_channels(chat_plane_, cmd_ch);
        ncplane_putstr_yx(chat_plane_, y, menu_x + 2, commands_[i].first.c_str());
        // Description
        ncplane_set_channels(chat_plane_, dim_ch);
        int desc_x = menu_x + 2 + cmd_col_w + spacing;
        if (desc_x < static_cast<int>(cols) - 2) {
            ncplane_putstr_yx(chat_plane_, y, desc_x, commands_[i].second.c_str());
        }
    }
}


void TerminalUI::close_command_menu() {
    showing_command_menu_ = false;
    selected_command_ = 0;
}


void TerminalUI::execute_command(int cmd_idx) {
    if (cmd_idx < 0 || cmd_idx >= static_cast<int>(commands_.size())) return;

    // Puts the command in the input buffer as autocomplete; user can add more text and press Enter to send.
    input_buffer_ = commands_[cmd_idx].first + " ";

    close_command_menu();
}


void TerminalUI::handle_command_input(const std::string& cmd_line) {
    // Parse command and optional arguments
    size_t space_pos = cmd_line.find(' ');
    std::string cmd = (space_pos == std::string::npos) ? cmd_line : cmd_line.substr(0, space_pos);
    std::string args = (space_pos == std::string::npos) ? "" : cmd_line.substr(space_pos + 1);

    // Check if peer is offline - only /DOWNLOAD, /STATUS, /CLEAR are allowed
    bool peer_online = is_selected_peer_online();
    std::string peer_name = "self";
    if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
        peer_name = people_rows_[selected_peer_index_].username;
    }
    if (!peer_online && peer_name != "self" && cmd != "/DOWNLOAD" && cmd != "/STATUS" && cmd != "/CLEAR" && cmd != "/INFO") {
        show_alert_popup("Unable to send: " + peer_name + " is offline");
        return;
    }

    if (cmd == "/HI") {
        std::string msg = args.empty() ? "Hello!" : args;
        // Send the message
        if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
            const PeerInfo peer = people_rows_[selected_peer_index_];
            if (peer.username == "self") {
                add_chat_message("self", true, msg, local_datetime_now(), TerminalUI::unix_epoch_ms_now());
            } else if (on_send_chat_) {
                std::string msg_id = on_send_chat_(peer, msg);
                if (!msg_id.empty()) {
                    add_chat_message(peer.username, true, msg, local_datetime_now(), TerminalUI::unix_epoch_ms_now(), msg_id, "pending");
                    {
                        std::lock_guard<std::mutex> lock(pending_acks_mutex_);
                        pending_acks_[msg_id] = peer.username;
                    }
                }
            }
        }
    } else if (cmd == "/BYE") {
        std::string msg = args.empty() ? "Goodbye!" : args;
        if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
            const PeerInfo peer = people_rows_[selected_peer_index_];
            if (peer.username == "self") {
                add_chat_message("self", true, msg, local_datetime_now(), TerminalUI::unix_epoch_ms_now());
            } else if (on_send_chat_) {
                std::string msg_id = on_send_chat_(peer, msg);
                if (!msg_id.empty()) {
                    add_chat_message(peer.username, true, msg, local_datetime_now(), TerminalUI::unix_epoch_ms_now(), msg_id, "pending");
                    {
                        std::lock_guard<std::mutex> lock(pending_acks_mutex_);
                        pending_acks_[msg_id] = peer.username;
                    }
                }
            }
        }
    } else if (cmd == "/STATUS") {
        add_debug("Command: STATUS - showing peer status");
        std::string active_name = "self";
        if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
            active_name = people_rows_[selected_peer_index_].username;
        }
        bool online = is_selected_peer_online();
        bool trusted = is_selected_peer_trusted();
        add_debug("Peer: " + active_name + " | Online: " + (online ? "yes" : "no") + " | Trusted: " + (trusted ? "yes" : "no"));
    } else if (cmd == "/SELFKEY") {
        if (on_show_identity_) {
            on_show_identity_();
        } else {
            add_debug("SELFKEY: identity display not available");
        }
    } else if (cmd == "/INFO") {
        std::string active_name = "self";
        if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
            active_name = people_rows_[selected_peer_index_].username;
        }
        add_debug("INFO: requesting info for peer=" + active_name);
        if (active_name == "self") {
            add_debug("INFO: cannot request info from self");
        } else if (on_request_info_) {
            add_debug("INFO: calling on_request_info_ callback");
            on_request_info_(active_name);
        } else {
            add_debug("INFO: info request not available (no callback)");
        }
    } else if (cmd == "/CLEAR") {
        std::string active_name = "self";
        if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
            active_name = people_rows_[selected_peer_index_].username;
        }
        // Show confirmation modal instead of directly clearing
        show_clear_modal(active_name);
    } else if (cmd == "/UPLOAD") {
        if (args.empty()) {
            add_debug("Usage: /UPLOAD <filepath>");
            return;
        }
        if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
            add_debug("No peer selected for file upload");
            return;
        }
        const PeerInfo peer = people_rows_[selected_peer_index_];
        if (peer.username == "self") {
            add_debug("Cannot upload file to self");
            return;
        }
        if (!is_selected_peer_online()) {
            add_debug("Peer is offline, cannot upload file");
            return;
        }
        if (!is_selected_peer_trusted()) {
            add_debug("Peer is not trusted, cannot upload file");
            return;
        }

        // Expand "~" and resolve ".."/"." so paths like "~/dir" or "../dir/file" work.
        const std::string resolved_path = resolve_local_path(args);

        // Get file size for the popup
        struct stat st;
        if (stat(resolved_path.c_str(), &st) != 0) {
            add_debug("Cannot access file: " + args);
            return;
        }

        // Show upload popup
        show_upload_popup(resolved_path, peer.username, static_cast<uint64_t>(st.st_size));

        // Trigger file upload via callback
        if (on_upload_file_) {
            if (on_upload_file_(resolved_path, peer.username, peer.ip, peer.tcp_port)) {
                add_debug("Started uploading " + resolved_path + " to " + peer.username);
            } else {
                add_debug("Failed to start upload");
                close_upload_popup();
            }
        } else {
            add_debug("Upload not implemented");
            close_upload_popup();
        }
    } else if (cmd == "/DOWNLOAD") {
        // Show download popup with file list
        show_download_popup();
    } else {
        add_debug("Unknown command: " + cmd);
    }
}
