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

void TerminalUI::draw_trust_prompt() {
    if (!chat_plane_) return;

    const uint64_t warning_ch = make_channels(0xff, 0xaa, 0x44, 0x0f, 0x17, 0x2a);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
    const uint64_t dim_ch = make_channels(0x80, 0x80, 0x80, 0x0f, 0x17, 0x2a);

    unsigned rows = 0, cols = 0;
    ncplane_dim_yx(chat_plane_, &rows, &cols);
    if (rows < 6 || cols < 40) return;

    // Get selected peer name
    std::string peer_name = "unknown";
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
            peer_name = people_rows_[selected_peer_index_].username;
        }
    }

    const int box_y = static_cast<int>(rows) - 6;
    const int box_h = 5;
    const int box_w = static_cast<int>(cols) - 2;

    // Draw trust prompt box
    ncplane_cursor_move_yx(chat_plane_, box_y, 1);
    ncplane_rounded_box_sized(chat_plane_, 0, warning_ch, box_h, box_w, 0);

    ncplane_set_channels(chat_plane_, warning_ch);
    ncplane_putstr_yx(chat_plane_, box_y, 3, " Trust Required ");

    ncplane_set_channels(chat_plane_, text_ch);
    std::string line1 = "Peer: " + peer_name;
    ncplane_putstr_yx(chat_plane_, box_y + 1, 2, line1.c_str());

    ncplane_set_channels(chat_plane_, dim_ch);
    ncplane_putstr_yx(chat_plane_, box_y + 2, 2, "Press T to trust, I to ignore");
    ncplane_putstr_yx(chat_plane_, box_y + 3, 2, "Verify fingerprint out-of-band!");
}


bool TerminalUI::handle_trust_keypress(char32_t ch) {
    // Only handle T and I keys for trust decisions
    if (ch != 't' && ch != 'T' && ch != 'i' && ch != 'I') {
        return false;
    }

    std::string peer_name;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
            return false;
        }
        peer_name = people_rows_[selected_peer_index_].username;
        if (peer_name == "self") return false;
        if (trusted_peers_.count(peer_name) > 0) return false;  // Already trusted
    }

    if (ch == 't' || ch == 'T') {
        // Trust this peer
        if (on_trust_peer_) {
            on_trust_peer_(peer_name);
        }
        add_debug("trusted peer: " + peer_name);
    } else {
        // Ignore this peer for now
        add_debug("ignored peer: " + peer_name);
    }
    return true;
}


void TerminalUI::show_trust_modal(const std::string& peer_name, const std::string& fingerprint,
                                   const std::vector<std::pair<std::string, std::string>>& info_entries) {
    std::lock_guard<std::mutex> lock(trust_modal_mutex_);
    if (showing_trust_modal_) {
        showing_trust_modal_ = false;
        trust_modal_info_dirty_ = true;
    }
    showing_trust_modal_ = true;
    trust_modal_peer_ = peer_name;
    trust_modal_fingerprint_ = fingerprint;
    trust_modal_info_entries_ = info_entries;
    trust_modal_selected_button_ = 0;  // Default to Accept
    trust_modal_plane_ = nullptr;  // Will be created in render
    trust_modal_info_dirty_ = false;
}


void TerminalUI::close_trust_modal() {
    std::lock_guard<std::mutex> lock(trust_modal_mutex_);
    showing_trust_modal_ = false;
    trust_modal_peer_.clear();
    trust_modal_fingerprint_.clear();
    trust_modal_info_entries_.clear();
    trust_modal_info_dirty_ = false;
    if (trust_modal_plane_) {
        ncplane_destroy(trust_modal_plane_);
        trust_modal_plane_ = nullptr;
    }
}


void TerminalUI::draw_trust_modal() {
    std::lock_guard<std::mutex> lock(trust_modal_mutex_);
    if (!showing_trust_modal_ || !nc_) return;

    // Get terminal dimensions
    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    // Calculate modal height based on info entries
    const int modal_w = 60;
    const int entry_count = static_cast<int>(trust_modal_info_entries_.size());
    const int info_h = entry_count > 0 ? (entry_count * 2 + 2) : 0;
    const int base_h = 12;
    const int modal_h = std::min(static_cast<int>(term_rows) - 4, base_h + info_h);
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    // Create or recreate the modal plane
    if (!trust_modal_plane_ || trust_modal_info_dirty_) {
        if (trust_modal_plane_) {
            ncplane_destroy(trust_modal_plane_);
        }
        trust_modal_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "trust_modal");
        trust_modal_info_dirty_ = false;
        if (!trust_modal_plane_) return;
    }

    // Colors
    const uint64_t border_ch = make_channels(0xff, 0xaa, 0x44, 0x22, 0x12, 0x29);
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t dim_ch = make_channels(0x80, 0x80, 0x80, 0x22, 0x12, 0x29);
    const uint64_t key_ch = make_channels(0xff, 0xaa, 0x44, 0x22, 0x12, 0x29);
    const uint64_t val_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t button_selected_ch = make_channels(0x00, 0x00, 0x00, 0x22, 0xc5, 0x5e);
    const uint64_t button_unselected_ch = make_channels(0xe2, 0xe8, 0xf0, 0x44, 0x44, 0x44);
    const uint32_t bg_color = 0x221229;

    // Clear and fill background
    ncplane_erase(trust_modal_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(trust_modal_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    // Draw border
    ncplane_set_channels(trust_modal_plane_, border_ch);
    ncplane_rounded_box_sized(trust_modal_plane_, 0, border_ch, modal_h, modal_w, 0);

    // Title
    ncplane_set_channels(trust_modal_plane_, title_ch);
    ncplane_putstr_yx(trust_modal_plane_, 1, 2, " Trust Identity ");

    // Peer name
    ncplane_set_channels(trust_modal_plane_, text_ch);
    std::string name_line = "Peer: " + trust_modal_peer_;
    ncplane_putstr_yx(trust_modal_plane_, 3, 4, name_line.c_str());

    // Fingerprint label
    ncplane_set_channels(trust_modal_plane_, dim_ch);
    ncplane_putstr_yx(trust_modal_plane_, 5, 4, "Fingerprint:");

    // Fingerprint value
    ncplane_set_channels(trust_modal_plane_, text_ch);
    ncplane_putstr_yx(trust_modal_plane_, 6, 6, trust_modal_fingerprint_.c_str());

    int y = 8;

    // Info entries
    if (!trust_modal_info_entries_.empty()) {
        ncplane_set_channels(trust_modal_plane_, dim_ch);
        ncplane_putstr_yx(trust_modal_plane_, y, 4, "Info:");
        y += 1;
        for (const auto& e : trust_modal_info_entries_) {
            if (y >= modal_h - 4) break;
            ncplane_set_channels(trust_modal_plane_, key_ch);
            ncplane_putstr_yx(trust_modal_plane_, y, 6, (e.first + ":").c_str());
            ncplane_set_channels(trust_modal_plane_, val_ch);
            std::string val = e.second;
            if (static_cast<int>(val.size()) > modal_w - 10) {
                val = val.substr(0, modal_w - 13) + "...";
            }
            ncplane_putstr_yx(trust_modal_plane_, y + 1, 8, val.c_str());
            y += 2;
        }
    }

    // Warning
    if (y < modal_h - 3) {
        ncplane_set_channels(trust_modal_plane_, make_channels(0xff, 0x44, 0x44, 0x22, 0x12, 0x29));
        ncplane_putstr_yx(trust_modal_plane_, y, 4, "Verify this fingerprint out-of-band!");
    }

    // Buttons
    const int button_y = modal_h - 2;
    const int accept_x = 12;
    const int reject_x = 35;

    // Accept button
    if (trust_modal_selected_button_ == 0) {
        ncplane_set_channels(trust_modal_plane_, button_selected_ch);
        ncplane_putstr_yx(trust_modal_plane_, button_y, accept_x, "[ Accept ]");
    } else {
        ncplane_set_channels(trust_modal_plane_, button_unselected_ch);
        ncplane_putstr_yx(trust_modal_plane_, button_y, accept_x, "  Accept  ");
    }

    // Reject button
    if (trust_modal_selected_button_ == 1) {
        ncplane_set_channels(trust_modal_plane_, button_selected_ch);
        ncplane_putstr_yx(trust_modal_plane_, button_y, reject_x, "[ Reject ]");
    } else {
        ncplane_set_channels(trust_modal_plane_, button_unselected_ch);
        ncplane_putstr_yx(trust_modal_plane_, button_y, reject_x, "  Reject  ");
    }
}


bool TerminalUI::is_showing_trust_modal_for(const std::string& peer_name) const {
    std::lock_guard<std::mutex> lock(trust_modal_mutex_);
    return showing_trust_modal_ && trust_modal_peer_ == peer_name;
}


void TerminalUI::update_trust_modal_info(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::lock_guard<std::mutex> lock(trust_modal_mutex_);
    trust_modal_info_entries_ = entries;
    // Signal main thread to recreate the plane with new dimensions
    trust_modal_info_dirty_ = true;
}


bool TerminalUI::handle_trust_modal_click(int abs_y, int abs_x) {
    std::unique_lock<std::mutex> lock(trust_modal_mutex_);
    if (!showing_trust_modal_ || !trust_modal_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(trust_modal_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(trust_modal_plane_, &h, &w);

    const int button_y = static_cast<int>(h) - 2;
    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    if (rel_y != button_y) return false;

    // Check Accept button (x: 12-21)
    if (rel_x >= 12 && rel_x <= 21) {
        std::string peer = trust_modal_peer_;
        bool has_callback = (on_trust_peer_ != nullptr);
        showing_trust_modal_ = false;
        trust_modal_peer_.clear();
        trust_modal_fingerprint_.clear();
        trust_modal_info_entries_.clear();
        trust_modal_info_dirty_ = false;
        if (trust_modal_plane_) {
            ncplane_destroy(trust_modal_plane_);
            trust_modal_plane_ = nullptr;
        }
        lock.unlock();
        if (has_callback) {
            on_trust_peer_(peer);
        }
        add_debug("trusted peer: " + peer);
        return true;
    }

    // Check Reject button (x: 35-44)
    if (rel_x >= 35 && rel_x <= 44) {
        std::string peer = trust_modal_peer_;
        showing_trust_modal_ = false;
        trust_modal_peer_.clear();
        trust_modal_fingerprint_.clear();
        trust_modal_info_entries_.clear();
        trust_modal_info_dirty_ = false;
        if (trust_modal_plane_) {
            ncplane_destroy(trust_modal_plane_);
            trust_modal_plane_ = nullptr;
        }
        lock.unlock();
        add_debug("rejected peer: " + peer);
        return true;
    }

    return false;
}


bool TerminalUI::handle_trust_modal_key(char32_t ch) {
    std::unique_lock<std::mutex> lock(trust_modal_mutex_);
    if (!showing_trust_modal_) return false;

    switch (ch) {
        case NCKEY_LEFT:
        case NCKEY_RIGHT:
            trust_modal_selected_button_ = 1 - trust_modal_selected_button_;  // Toggle 0/1
            return true;
        case NCKEY_ENTER:
        case '\n':
        case '\r': {
            std::string peer = trust_modal_peer_;
            bool accept = (trust_modal_selected_button_ == 0);
            bool has_callback = (on_trust_peer_ != nullptr);
            showing_trust_modal_ = false;
            trust_modal_peer_.clear();
            trust_modal_fingerprint_.clear();
            trust_modal_info_entries_.clear();
            trust_modal_info_dirty_ = false;
            if (trust_modal_plane_) {
                ncplane_destroy(trust_modal_plane_);
                trust_modal_plane_ = nullptr;
            }
            lock.unlock();
            if (accept && has_callback) {
                on_trust_peer_(peer);
            }
            add_debug(accept ? "trusted peer: " + peer : "rejected peer: " + peer);
            return true;
        }
        case 27:  // Escape
        case 'q':
        case 'Q': {
            std::string peer = trust_modal_peer_;
            showing_trust_modal_ = false;
            trust_modal_peer_.clear();
            trust_modal_fingerprint_.clear();
            trust_modal_info_entries_.clear();
            trust_modal_info_dirty_ = false;
            if (trust_modal_plane_) {
                ncplane_destroy(trust_modal_plane_);
                trust_modal_plane_ = nullptr;
            }
            lock.unlock();
            add_debug("cancelled trust dialog for: " + peer);
            return true;
        }
    }
    return false;
}


void TerminalUI::show_identity_popup(const std::string& fingerprint,
                                      const std::string& peer_name,
                                      const std::vector<std::pair<std::string, std::string>>& entries) {
    if (showing_identity_popup_) {
        close_identity_popup();
    }
    showing_identity_popup_ = true;
    identity_popup_fingerprint_ = fingerprint;
    identity_popup_peer_name_ = peer_name;
    identity_popup_entries_ = entries;
    identity_popup_plane_ = nullptr;
}


void TerminalUI::close_identity_popup() {
    showing_identity_popup_ = false;
    identity_popup_fingerprint_.clear();
    identity_popup_peer_name_.clear();
    identity_popup_entries_.clear();
    if (identity_popup_plane_) {
        ncplane_destroy(identity_popup_plane_);
        identity_popup_plane_ = nullptr;
    }
}


void TerminalUI::draw_identity_popup() {
    if (!showing_identity_popup_ || !nc_) return;

    // Get terminal dimensions
    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    const bool is_peer_info = !identity_popup_peer_name_.empty();
    const int modal_w = 60;
    const int entry_count = static_cast<int>(identity_popup_entries_.size());
    const int content_h = is_peer_info ? std::max(3, entry_count + 3) : 5;
    const int modal_h = std::min(static_cast<int>(term_rows) - 4, content_h + 6);
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    // Create or recreate the popup plane
    if (!identity_popup_plane_) {
        identity_popup_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "identity_popup");
        if (!identity_popup_plane_) return;
    }

    // Colors
    const uint64_t border_ch = make_channels(0x22, 0xc5, 0x5e, 0x22, 0x12, 0x29);  // Green border
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t fp_ch = make_channels(0xff, 0xff, 0x00, 0x22, 0x12, 0x29);  // Yellow fingerprint
    const uint64_t button_ch = make_channels(0x00, 0x00, 0x00, 0x22, 0xc5, 0x5e);  // Green button
    const uint64_t key_ch = make_channels(0xff, 0xaa, 0x44, 0x22, 0x12, 0x29);
    const uint64_t val_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint32_t bg_color = 0x221229;

    // Clear and fill background
    ncplane_erase(identity_popup_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(identity_popup_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    // Draw border
    ncplane_set_channels(identity_popup_plane_, border_ch);
    ncplane_rounded_box_sized(identity_popup_plane_, 0, border_ch, modal_h, modal_w, 0);

    // Title
    ncplane_set_channels(identity_popup_plane_, title_ch);
    ncplane_on_styles(identity_popup_plane_, NCSTYLE_BOLD);
    if (is_peer_info) {
        std::string title = " Info: " + identity_popup_peer_name_ + " ";
        ncplane_putstr_yx(identity_popup_plane_, 1, 2, title.c_str());
    } else {
        ncplane_putstr_yx(identity_popup_plane_, 1, 2, " Your Public Key Fingerprint ");
    }
    ncplane_off_styles(identity_popup_plane_, NCSTYLE_BOLD);

    int y = 3;
    if (!identity_popup_fingerprint_.empty()) {
        // Fingerprint (centered)
        ncplane_set_channels(identity_popup_plane_, fp_ch);
        int fp_x = (modal_w - static_cast<int>(identity_popup_fingerprint_.size())) / 2;
        if (fp_x < 2) fp_x = 2;
        ncplane_putstr_yx(identity_popup_plane_, y, fp_x, identity_popup_fingerprint_.c_str());
        y = 5;
    }
    if (is_peer_info) {
        // Info entries below fingerprint
        for (const auto& e : identity_popup_entries_) {
            if (y >= modal_h - 2) break;
            std::string line = e.first + ": " + e.second;
            if (static_cast<int>(line.size()) > modal_w - 4) {
                line = line.substr(0, modal_w - 7) + "...";
            }
            ncplane_set_channels(identity_popup_plane_, val_ch);
            ncplane_putstr_yx(identity_popup_plane_, y, 2, line.c_str());
            y += 1;
        }
        if (identity_popup_entries_.empty()) {
            ncplane_set_channels(identity_popup_plane_, val_ch);
            ncplane_putstr_yx(identity_popup_plane_, y, 2, "No info available.");
        }
    } else {
        // Instruction for self-key popup
        ncplane_set_channels(identity_popup_plane_, text_ch);
        std::string instr = "Share this with others to verify your identity";
        int instr_x = (modal_w - static_cast<int>(instr.size())) / 2;
        if (instr_x < 2) instr_x = 2;
        ncplane_putstr_yx(identity_popup_plane_, y, instr_x, instr.c_str());
    }

    // OK button (centered at bottom)
    ncplane_set_channels(identity_popup_plane_, button_ch);
    int btn_x = (modal_w - 8) / 2;
    ncplane_putstr_yx(identity_popup_plane_, modal_h - 2, btn_x, "  [ OK ]  ");
}


bool TerminalUI::handle_identity_popup_click(int abs_y, int abs_x) {
    if (!showing_identity_popup_ || !identity_popup_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(identity_popup_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(identity_popup_plane_, &h, &w);

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    // Check if click is within the OK button area (bottom row, centered)
    if (rel_y == static_cast<int>(h) - 2 && rel_x >= static_cast<int>(w)/2 - 4 && rel_x <= static_cast<int>(w)/2 + 4) {
        close_identity_popup();
        return true;
    }

    // Click anywhere else in the popup also closes it
    if (rel_y >= 0 && rel_y < static_cast<int>(h) && rel_x >= 0 && rel_x < static_cast<int>(w)) {
        return true;  // Absorb click but don't close unless on OK
    }

    return false;
}


bool TerminalUI::handle_identity_popup_key(char32_t ch) {
    if (!showing_identity_popup_) return false;

    switch (ch) {
        case NCKEY_ENTER:
        case '\n':
        case '\r':
        case 27:  // Escape
        case 'q':
        case 'Q':
        case 'o':
        case 'O':
            close_identity_popup();
            return true;
    }
    return false;
}


void TerminalUI::show_clear_modal(const std::string& peer_name) {
    if (showing_clear_modal_) {
        close_clear_modal();
    }
    showing_clear_modal_ = true;
    clear_modal_peer_ = peer_name;
    clear_modal_selected_button_ = 0;
    clear_modal_plane_ = nullptr;
}


void TerminalUI::close_clear_modal() {
    showing_clear_modal_ = false;
    clear_modal_peer_.clear();
    if (clear_modal_plane_) {
        ncplane_destroy(clear_modal_plane_);
        clear_modal_plane_ = nullptr;
    }
}


void TerminalUI::draw_clear_modal() {
    if (!showing_clear_modal_ || !nc_) return;

    // Get terminal dimensions
    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    // Modal dimensions
    const int modal_w = 50;
    const int modal_h = 8;
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    // Create or recreate the modal plane
    if (!clear_modal_plane_) {
        clear_modal_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "clear_modal");
        if (!clear_modal_plane_) return;
    }

    // Colors
    const uint64_t border_ch = make_channels(0xff, 0xaa, 0x44, 0x22, 0x12, 0x29);  // Orange border
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t warning_ch = make_channels(0xff, 0x44, 0x44, 0x22, 0x12, 0x29);  // Red warning
    const uint64_t button_selected_ch = make_channels(0x00, 0x00, 0x00, 0x22, 0xc5, 0x5e);
    const uint64_t button_unselected_ch = make_channels(0xe2, 0xe8, 0xf0, 0x44, 0x44, 0x44);
    const uint32_t bg_color = 0x221229;

    // Clear and fill background
    ncplane_erase(clear_modal_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(clear_modal_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    // Draw border
    ncplane_set_channels(clear_modal_plane_, border_ch);
    ncplane_rounded_box_sized(clear_modal_plane_, 0, border_ch, modal_h, modal_w, 0);

    // Title
    ncplane_set_channels(clear_modal_plane_, title_ch);
    ncplane_on_styles(clear_modal_plane_, NCSTYLE_BOLD);
    ncplane_putstr_yx(clear_modal_plane_, 1, 2, " Clear Chat History ");
    ncplane_off_styles(clear_modal_plane_, NCSTYLE_BOLD);

    // Warning message
    ncplane_set_channels(clear_modal_plane_, warning_ch);
    std::string warn_line = "Clear all messages for " + clear_modal_peer_ + "?";
    ncplane_putstr_yx(clear_modal_plane_, 3, 4, warn_line.c_str());

    // Note
    ncplane_set_channels(clear_modal_plane_, text_ch);
    ncplane_putstr_yx(clear_modal_plane_, 4, 4, "This will also clear from database.");

    // Buttons
    const int button_y = modal_h - 2;
    const int accept_x = 12;
    const int reject_x = 30;

    // Accept button
    if (clear_modal_selected_button_ == 0) {
        ncplane_set_channels(clear_modal_plane_, button_selected_ch);
        ncplane_putstr_yx(clear_modal_plane_, button_y, accept_x, "[ Accept ]");
    } else {
        ncplane_set_channels(clear_modal_plane_, button_unselected_ch);
        ncplane_putstr_yx(clear_modal_plane_, button_y, accept_x, "  Accept  ");
    }

    // Reject button
    if (clear_modal_selected_button_ == 1) {
        ncplane_set_channels(clear_modal_plane_, button_selected_ch);
        ncplane_putstr_yx(clear_modal_plane_, button_y, reject_x, "[ Reject ]");
    } else {
        ncplane_set_channels(clear_modal_plane_, button_unselected_ch);
        ncplane_putstr_yx(clear_modal_plane_, button_y, reject_x, "  Reject  ");
    }
}


bool TerminalUI::handle_clear_modal_click(int abs_y, int abs_x) {
    if (!showing_clear_modal_ || !clear_modal_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(clear_modal_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(clear_modal_plane_, &h, &w);

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    const int button_y = static_cast<int>(h) - 2;
    const int accept_x_start = 12;
    const int accept_x_end = 22;
    const int reject_x_start = 30;
    const int reject_x_end = 40;

    if (rel_y == button_y) {
        if (rel_x >= accept_x_start && rel_x <= accept_x_end) {
            // Accept - clear from memory and DB
            {
                std::lock_guard<std::mutex> lock(chat_mutex_);
                chats_by_peer_[clear_modal_peer_].clear();
            }
            if (db_manager_) {
                db_manager_->clearMessagesForPeer(clear_modal_peer_);
            }
            add_debug("Cleared chat history for: " + clear_modal_peer_);
            close_clear_modal();
            return true;
        } else if (rel_x >= reject_x_start && rel_x <= reject_x_end) {
            // Reject - just close
            add_debug("Cancelled clearing chat for: " + clear_modal_peer_);
            close_clear_modal();
            return true;
        }
    }

    // Click outside closes modal (cancel)
    if (rel_y < 0 || rel_y >= static_cast<int>(h) || rel_x < 0 || rel_x >= static_cast<int>(w)) {
        close_clear_modal();
        return true;
    }

    return false;
}


bool TerminalUI::handle_clear_modal_key(char32_t ch) {
    if (!showing_clear_modal_) return false;

    switch (ch) {
        case NCKEY_LEFT:
        case NCKEY_RIGHT:
            clear_modal_selected_button_ = 1 - clear_modal_selected_button_;  // Toggle 0/1
            return true;
        case NCKEY_ENTER:
        case '\n':
        case '\r':
            if (clear_modal_selected_button_ == 0) {
                // Accept - clear from memory and DB
                {
                    std::lock_guard<std::mutex> lock(chat_mutex_);
                    chats_by_peer_[clear_modal_peer_].clear();
                }
                if (db_manager_) {
                    db_manager_->clearMessagesForPeer(clear_modal_peer_);
                }
                add_debug("Cleared chat history for: " + clear_modal_peer_);
            } else {
                // Reject
                add_debug("Cancelled clearing chat for: " + clear_modal_peer_);
            }
            close_clear_modal();
            return true;
        case 27:  // Escape
        case 'q':
        case 'Q':
            add_debug("Cancelled clearing chat for: " + clear_modal_peer_);
            close_clear_modal();
            return true;
    }
    return false;
}


void TerminalUI::show_alert_popup(const std::string& message) {
    if (showing_alert_popup_) {
        close_alert_popup();
    }
    showing_alert_popup_ = true;
    alert_message_ = message;
    alert_popup_plane_ = nullptr;
}


void TerminalUI::close_alert_popup() {
    showing_alert_popup_ = false;
    alert_message_.clear();
    if (alert_popup_plane_) {
        ncplane_destroy(alert_popup_plane_);
        alert_popup_plane_ = nullptr;
    }
}


void TerminalUI::draw_alert_popup() {
    if (!showing_alert_popup_ || !nc_) return;

    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    const int modal_w = 56;
    const int modal_h = 7;
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    if (!alert_popup_plane_) {
        alert_popup_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "alert_popup");
        if (!alert_popup_plane_) return;
    }

    const uint64_t border_ch = make_channels(0xff, 0x88, 0x22, 0x22, 0x12, 0x29);
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t button_ch = make_channels(0x00, 0x00, 0x00, 0x22, 0xc5, 0x5e);
    const uint32_t bg_color = 0x221229;

    ncplane_erase(alert_popup_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(alert_popup_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    ncplane_set_channels(alert_popup_plane_, border_ch);
    ncplane_rounded_box_sized(alert_popup_plane_, 0, border_ch, modal_h, modal_w, 0);

    ncplane_set_channels(alert_popup_plane_, title_ch);
    ncplane_on_styles(alert_popup_plane_, NCSTYLE_BOLD);
    ncplane_putstr_yx(alert_popup_plane_, 1, 2, " Alert ");
    ncplane_off_styles(alert_popup_plane_, NCSTYLE_BOLD);

    ncplane_set_channels(alert_popup_plane_, text_ch);
    std::string msg = alert_message_;
    if (static_cast<int>(msg.size()) > modal_w - 6) {
        msg = msg.substr(0, modal_w - 9) + "...";
    }
    int msg_x = (modal_w - static_cast<int>(msg.size())) / 2;
    if (msg_x < 2) msg_x = 2;
    ncplane_putstr_yx(alert_popup_plane_, 3, msg_x, msg.c_str());

    ncplane_set_channels(alert_popup_plane_, button_ch);
    ncplane_putstr_yx(alert_popup_plane_, modal_h - 2, (modal_w - 10) / 2, "  [ OK ]  ");
}


bool TerminalUI::handle_alert_popup_click(int abs_y, int abs_x) {
    if (!showing_alert_popup_ || !alert_popup_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(alert_popup_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(alert_popup_plane_, &h, &w);

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    if (rel_y == static_cast<int>(h) - 2 && rel_x >= static_cast<int>(w) / 2 - 5 && rel_x <= static_cast<int>(w) / 2 + 5) {
        close_alert_popup();
        return true;
    }

    if (rel_y >= 0 && rel_y < static_cast<int>(h) && rel_x >= 0 && rel_x < static_cast<int>(w)) {
        return true;
    }

    return false;
}


bool TerminalUI::handle_alert_popup_key(char32_t ch) {
    if (!showing_alert_popup_) return false;

    switch (ch) {
        case NCKEY_ENTER:
        case '\n':
        case '\r':
        case 27:  // Escape
        case 'o':
        case 'O':
            close_alert_popup();
            return true;
    }
    return false;
}
