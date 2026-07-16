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
using tui_detail::format_display_datetime;

void TerminalUI::show_upload_popup(const std::string& filename, const std::string& target_peer, uint64_t file_size) {
    if (showing_upload_popup_) {
        close_upload_popup();
    }
    showing_upload_popup_ = true;
    upload_filename_ = filename;
    upload_target_peer_ = target_peer;
    upload_file_size_ = file_size;
    upload_bytes_sent_ = 0;
    upload_complete_ = false;
    upload_cancelled_ = false;
    upload_popup_plane_ = nullptr;
}


void TerminalUI::close_upload_popup() {
    showing_upload_popup_ = false;
    upload_filename_.clear();
    upload_target_peer_.clear();
    upload_file_size_ = 0;
    upload_bytes_sent_ = 0;
    upload_complete_ = false;
    upload_cancelled_ = false;
    if (upload_popup_plane_) {
        ncplane_destroy(upload_popup_plane_);
        upload_popup_plane_ = nullptr;
    }
}


void TerminalUI::draw_upload_popup() {
    if (!showing_upload_popup_ || !nc_) return;

    // Get terminal dimensions
    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    // Modal dimensions
    const int modal_w = 60;
    const int modal_h = 10;
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    // Create or recreate the popup plane
    if (!upload_popup_plane_) {
        upload_popup_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "upload_popup");
        if (!upload_popup_plane_) return;
    }

    // Colors
    const uint64_t border_ch = make_channels(0x22, 0xc5, 0x5e, 0x22, 0x12, 0x29);  // Green border
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t dim_ch = make_channels(0x94, 0xa3, 0xb8, 0x22, 0x12, 0x29);
    const uint64_t progress_ch = make_channels(0x22, 0xc5, 0x5e, 0x22, 0x12, 0x29);  // Green progress
    const uint64_t button_ch = make_channels(0x00, 0x00, 0x00, 0x22, 0xc5, 0x5e);
    const uint32_t bg_color = 0x221229;

    // Clear and fill background
    ncplane_erase(upload_popup_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(upload_popup_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    // Draw border
    ncplane_set_channels(upload_popup_plane_, border_ch);
    ncplane_rounded_box_sized(upload_popup_plane_, 0, border_ch, modal_h, modal_w, 0);

    // Title
    ncplane_set_channels(upload_popup_plane_, title_ch);
    ncplane_on_styles(upload_popup_plane_, NCSTYLE_BOLD);
    ncplane_putstr_yx(upload_popup_plane_, 1, 2, " File Upload ");
    ncplane_off_styles(upload_popup_plane_, NCSTYLE_BOLD);

    // File info
    ncplane_set_channels(upload_popup_plane_, text_ch);
    std::string file_line = "File: " + upload_filename_;
    if (static_cast<int>(file_line.size()) > modal_w - 4) {
        file_line = file_line.substr(0, modal_w - 7) + "...";
    }
    ncplane_putstr_yx(upload_popup_plane_, 3, 4, file_line.c_str());

    std::string peer_line = "To: " + upload_target_peer_;
    ncplane_putstr_yx(upload_popup_plane_, 4, 4, peer_line.c_str());

    // Calculate progress
    int progress_percent = 0;
    if (upload_file_size_ > 0) {
        progress_percent = static_cast<int>((upload_bytes_sent_ * 100) / upload_file_size_);
    }
    if (upload_complete_) progress_percent = 100;

    // Progress bar
    const int bar_width = modal_w - 10;
    int filled_width = (progress_percent * bar_width) / 100;
    
    ncplane_set_channels(upload_popup_plane_, dim_ch);
    ncplane_putstr_yx(upload_popup_plane_, 6, 4, "[");
    ncplane_putstr_yx(upload_popup_plane_, 6, 4 + bar_width + 1, "]");
    
    // Draw filled part
    ncplane_set_channels(upload_popup_plane_, progress_ch);
    std::string filled(filled_width, '=');
    std::string empty(bar_width - filled_width, ' ');
    ncplane_putstr_yx(upload_popup_plane_, 6, 5, filled.c_str());
    ncplane_set_channels(upload_popup_plane_, dim_ch);
    ncplane_putstr_yx(upload_popup_plane_, 6, 5 + filled_width, empty.c_str());

    // Percentage
    ncplane_set_channels(upload_popup_plane_, text_ch);
    std::string pct_str = std::to_string(progress_percent) + "%";
    int pct_x = modal_w - 4 - static_cast<int>(pct_str.size());
    ncplane_putstr_yx(upload_popup_plane_, 6, pct_x, pct_str.c_str());

    // Button (CANCEL or OK)
    ncplane_set_channels(upload_popup_plane_, button_ch);
    if (upload_complete_) {
        ncplane_putstr_yx(upload_popup_plane_, modal_h - 2, (modal_w - 8) / 2, "  [ OK ]  ");
    } else if (upload_cancelled_) {
        ncplane_putstr_yx(upload_popup_plane_, modal_h - 2, (modal_w - 12) / 2, "[ Cancelled ]");
    } else {
        ncplane_putstr_yx(upload_popup_plane_, modal_h - 2, (modal_w - 10) / 2, " [ CANCEL ] ");
    }
}


void TerminalUI::update_upload_progress(uint64_t bytes_sent, bool complete) {
    upload_bytes_sent_ = bytes_sent;
    upload_complete_ = complete;
}


bool TerminalUI::handle_upload_popup_click(int abs_y, int abs_x) {
    if (!showing_upload_popup_ || !upload_popup_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(upload_popup_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(upload_popup_plane_, &h, &w);

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    // Check if click is on the button
    const int button_y = static_cast<int>(h) - 2;
    const int button_x_start = (static_cast<int>(w) - 10) / 2;
    const int button_x_end = button_x_start + 10;

    if (rel_y == button_y && rel_x >= button_x_start && rel_x <= button_x_end) {
        if (upload_complete_ || upload_cancelled_) {
            close_upload_popup();
        } else {
            upload_cancelled_ = true;
            add_debug("Upload cancelled by user");
        }
        return true;
    }

    // Click outside does nothing (must click button)
    return false;
}


bool TerminalUI::handle_upload_popup_key(char32_t ch) {
    if (!showing_upload_popup_) return false;

    switch (ch) {
        case NCKEY_ENTER:
        case '\n':
        case '\r':
        case 27:  // Escape
        case 'q':
        case 'Q':
            if (upload_complete_ || upload_cancelled_) {
                close_upload_popup();
            } else {
                upload_cancelled_ = true;
                add_debug("Upload cancelled by user");
            }
            return true;
    }
    return false;
}


void TerminalUI::show_download_popup() {
    if (showing_download_popup_) {
        close_download_popup();
    }
    showing_download_popup_ = true;
    download_selected_index_ = 0;
    download_popup_plane_ = nullptr;
}


void TerminalUI::close_download_popup() {
    showing_download_popup_ = false;
    download_selected_index_ = 0;
    if (download_popup_plane_) {
        ncplane_destroy(download_popup_plane_);
        download_popup_plane_ = nullptr;
    }
}


void TerminalUI::draw_download_popup() {
    if (!showing_download_popup_ || !nc_) return;

    unsigned rows = 0, cols = 0;
    notcurses_stddim_yx(nc_, &rows, &cols);

    const int modal_w = std::min(70, static_cast<int>(cols) - 4);
    const int modal_h = std::min(20, static_cast<int>(rows) - 4);
    const int modal_y = (static_cast<int>(rows) - modal_h) / 2;
    const int modal_x = (static_cast<int>(cols) - modal_w) / 2;

    if (!download_popup_plane_) {
        download_popup_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "download_popup");
        if (!download_popup_plane_) return;
    }

    // Colors
    const uint64_t border_ch = make_channels(0x44, 0xff, 0xaa, 0x0f, 0x17, 0x2a);
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x0f, 0x17, 0x2a);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
    const uint64_t dim_ch = make_channels(0x80, 0x80, 0x80, 0x0f, 0x17, 0x2a);
    const uint64_t selected_ch = make_channels(0x00, 0x00, 0x00, 0x44, 0xc5, 0x5e);
    const uint32_t bg_color = 0x0f172a;

    ncplane_erase(download_popup_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(download_popup_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    ncplane_set_channels(download_popup_plane_, border_ch);
    ncplane_rounded_box_sized(download_popup_plane_, 0, border_ch, modal_h, modal_w, 0);

    ncplane_set_channels(download_popup_plane_, title_ch);
    ncplane_on_styles(download_popup_plane_, NCSTYLE_BOLD);
    ncplane_putstr_yx(download_popup_plane_, 1, 2, " File Transfers - Select to Download ");
    ncplane_off_styles(download_popup_plane_, NCSTYLE_BOLD);

    // Load file transfers from database
    std::vector<FileTransferRecord> files;
    if (db_manager_) {
        for (const auto& rec : db_manager_->loadAllFileTransfers()) {
            if (rec.status == "complete") {
                files.push_back(rec);
            }
        }
    }

    if (files.empty()) {
        ncplane_set_channels(download_popup_plane_, dim_ch);
        ncplane_putstr_yx(download_popup_plane_, modal_h / 2, (modal_w - 25) / 2, "No file transfers available");
    } else {
        const int list_start_y = 3;
        const int max_visible = modal_h - 5;
        
        // Adjust selection if needed
        if (download_selected_index_ >= static_cast<int>(files.size())) {
            download_selected_index_ = static_cast<int>(files.size()) - 1;
        }
        if (download_selected_index_ < 0) {
            download_selected_index_ = 0;
        }

        // Calculate scroll offset
        int scroll_offset = 0;
        if (download_selected_index_ >= max_visible) {
            scroll_offset = download_selected_index_ - max_visible + 1;
        }

        for (int i = 0; i < max_visible && (i + scroll_offset) < static_cast<int>(files.size()); ++i) {
            const auto& file = files[i + scroll_offset];
            int y = list_start_y + i;
            
            bool is_selected = (i + scroll_offset) == download_selected_index_;
            
            std::string direction = file.is_sender ? "→ " : "← ";
            std::string when = format_display_datetime(file.timestamp, file.timestamp_ms);
            std::string line = direction + file.filename + " (" + std::to_string((file.file_size + 1023) / 1024) + " KB) " + when;
            
            // Truncate if too long
            if (static_cast<int>(line.size()) > modal_w - 8) {
                line = line.substr(0, modal_w - 11) + "...";
            }
            
            if (is_selected) {
                ncplane_set_channels(download_popup_plane_, selected_ch);
            } else {
                ncplane_set_channels(download_popup_plane_, text_ch);
            }
            
            // Pad with spaces to fill width
            while (static_cast<int>(line.size()) < modal_w - 6) {
                line += " ";
            }
            
            ncplane_putstr_yx(download_popup_plane_, y, 3, line.c_str());
        }
    }

    // Instructions
    ncplane_set_channels(download_popup_plane_, dim_ch);
    ncplane_putstr_yx(download_popup_plane_, modal_h - 2, 4, "↑/↓: Select  Enter: Download  Esc: Close");
}


bool TerminalUI::handle_download_popup_click(int abs_y, int abs_x) {
    if (!showing_download_popup_ || !download_popup_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(download_popup_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(download_popup_plane_, &h, &w);

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    // Click outside closes popup
    if (rel_y < 0 || rel_y >= static_cast<int>(h) || rel_x < 0 || rel_x >= static_cast<int>(w)) {
        close_download_popup();
        return true;
    }

    return false;
}


bool TerminalUI::handle_download_popup_key(char32_t ch) {
    if (!showing_download_popup_) return false;

    std::vector<FileTransferRecord> files;
    if (db_manager_) {
        for (const auto& rec : db_manager_->loadAllFileTransfers()) {
            if (rec.status == "complete") {
                files.push_back(rec);
            }
        }
    }

    switch (ch) {
        case NCKEY_UP:
            if (download_selected_index_ > 0) {
                download_selected_index_--;
            }
            return true;
        case NCKEY_DOWN:
            if (download_selected_index_ < static_cast<int>(files.size()) - 1) {
                download_selected_index_++;
            }
            return true;
        case NCKEY_ENTER:
        case '\n':
        case '\r':
            if (download_selected_index_ >= 0 && download_selected_index_ < static_cast<int>(files.size())) {
                const auto& file = files[download_selected_index_];
                if (on_download_file_) {
                    // Create downloads directory if needed
                    const char* home = getenv("HOME");
                    std::string download_dir = home ? std::string(home) + "/downloads" : "/tmp/downloads";
                    struct stat st;
                    if (stat(download_dir.c_str(), &st) != 0) {
                        mkdir(download_dir.c_str(), 0755);
                    }

                    std::string download_path = download_dir + "/" + file.filename;
                    close_download_popup();
                    if (on_download_file_(file.transfer_id, download_path)) {
                        show_download_result_popup("Saved to: " + download_path);
                    } else {
                        show_download_result_popup("Failed to download " + file.filename);
                    }
                }
            }
            return true;
        case 27:  // Escape
        case 'q':
        case 'Q':
            close_download_popup();
            return true;
    }
    return false;
}


void TerminalUI::show_download_result_popup(const std::string& message) {
    if (showing_download_result_popup_) {
        close_download_result_popup();
    }
    showing_download_result_popup_ = true;
    download_result_message_ = message;
    download_result_popup_plane_ = nullptr;
}


void TerminalUI::close_download_result_popup() {
    showing_download_result_popup_ = false;
    download_result_message_.clear();
    if (download_result_popup_plane_) {
        ncplane_destroy(download_result_popup_plane_);
        download_result_popup_plane_ = nullptr;
    }
}


void TerminalUI::draw_download_result_popup() {
    if (!showing_download_result_popup_ || !nc_) return;

    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    const int modal_w = 56;
    const int modal_h = 7;
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    if (!download_result_popup_plane_) {
        download_result_popup_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "download_result_popup");
        if (!download_result_popup_plane_) return;
    }

    const uint64_t border_ch = make_channels(0x22, 0xc5, 0x5e, 0x22, 0x12, 0x29);
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t button_ch = make_channels(0x00, 0x00, 0x00, 0x22, 0xc5, 0x5e);
    const uint32_t bg_color = 0x221229;

    ncplane_erase(download_result_popup_plane_);
    uint64_t ul = 0, ur = 0, ll = 0, lr = 0;
    ncchannels_set_bg_rgb(&ul, bg_color);
    ncchannels_set_bg_rgb(&ur, bg_color);
    ncchannels_set_bg_rgb(&ll, bg_color);
    ncchannels_set_bg_rgb(&lr, bg_color);
    ncchannels_set_fg_rgb(&ul, bg_color);
    ncchannels_set_fg_rgb(&ur, bg_color);
    ncchannels_set_fg_rgb(&ll, bg_color);
    ncchannels_set_fg_rgb(&lr, bg_color);
    ncplane_gradient(download_result_popup_plane_, 0, 0, modal_h, modal_w, " ", 0, ul, ur, ll, lr);

    ncplane_set_channels(download_result_popup_plane_, border_ch);
    ncplane_rounded_box_sized(download_result_popup_plane_, 0, border_ch, modal_h, modal_w, 0);

    ncplane_set_channels(download_result_popup_plane_, title_ch);
    ncplane_on_styles(download_result_popup_plane_, NCSTYLE_BOLD);
    ncplane_putstr_yx(download_result_popup_plane_, 1, 2, " Download Result ");
    ncplane_off_styles(download_result_popup_plane_, NCSTYLE_BOLD);

    ncplane_set_channels(download_result_popup_plane_, text_ch);
    std::string msg = download_result_message_;
    if (static_cast<int>(msg.size()) > modal_w - 6) {
        msg = msg.substr(0, modal_w - 9) + "...";
    }
    int msg_x = (modal_w - static_cast<int>(msg.size())) / 2;
    if (msg_x < 2) msg_x = 2;
    ncplane_putstr_yx(download_result_popup_plane_, 3, msg_x, msg.c_str());

    ncplane_set_channels(download_result_popup_plane_, button_ch);
    ncplane_putstr_yx(download_result_popup_plane_, modal_h - 2, (modal_w - 10) / 2, "  [ OK ]  ");
}


bool TerminalUI::handle_download_result_popup_click(int abs_y, int abs_x) {
    if (!showing_download_result_popup_ || !download_result_popup_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(download_result_popup_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(download_result_popup_plane_, &h, &w);

    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    if (rel_y == static_cast<int>(h) - 2 && rel_x >= static_cast<int>(w) / 2 - 5 && rel_x <= static_cast<int>(w) / 2 + 5) {
        close_download_result_popup();
        return true;
    }

    if (rel_y >= 0 && rel_y < static_cast<int>(h) && rel_x >= 0 && rel_x < static_cast<int>(w)) {
        return true;
    }

    return false;
}


bool TerminalUI::handle_download_result_popup_key(char32_t ch) {
    if (!showing_download_result_popup_) return false;

    switch (ch) {
        case NCKEY_ENTER:
        case '\n':
        case '\r':
        case 27:  // Escape
        case 'q':
        case 'Q':
        case 'o':
        case 'O':
            close_download_result_popup();
            return true;
    }
    return false;
}
