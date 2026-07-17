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

void TerminalUI::run() {
    if (!nc_ || running_) {
        return;
    }

    running_ = true;
    last_cursor_toggle_ = std::chrono::steady_clock::now();
    timeout_checker_thread_ = std::thread(&TerminalUI::run_timeout_checker, this);
    add_debug("press Ctrl+C to quit");
    render();

    ncinput in{};
    while (running_) {
        timespec ts{};
        ts.tv_nsec = 200000000;
        const uint32_t input = notcurses_get(nc_, &ts, &in);

        if (input == 0) {
            render();
            continue;
        }

        const bool key_action =
            (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_REPEAT ||
             in.evtype == NCTYPE_UNKNOWN);

        // Handle identity popup first (it blocks other inputs)
        if (showing_identity_popup_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_identity_popup_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_identity_popup_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            // Any other input while popup is open just re-renders
            render();
            continue;
        }

        // Handle clear modal (it blocks other inputs)
        if (showing_clear_modal_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_clear_modal_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_clear_modal_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            // Any other input while modal is open just re-renders
            render();
            continue;
        }

        // Handle trust modal (it blocks other inputs)
        if (showing_trust_modal_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_trust_modal_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_trust_modal_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            // Any other input while modal is open just re-renders
            render();
            continue;
        }

        // Handle upload popup (it blocks other inputs)
        if (showing_upload_popup_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_upload_popup_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_upload_popup_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            // Any other input while popup is open just re-renders
            render();
            continue;
        }

        // Handle download popup (it blocks other inputs)
        if (showing_download_popup_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_download_popup_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_download_popup_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            // Any other input while popup is open just re-renders
            render();
            continue;
        }

        // Handle download result popup
        if (showing_download_result_popup_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_download_result_popup_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_download_result_popup_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            render();
            continue;
        }

        // Handle alert popup
        if (showing_alert_popup_) {
            if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
                if (handle_alert_popup_click(in.y, in.x)) {
                    render();
                    continue;
                }
            } else if (key_action && handle_alert_popup_key(static_cast<char32_t>(input))) {
                render();
                continue;
            }
            render();
            continue;
        }

        if (key_action &&
            (input == static_cast<uint32_t>('c') || input == static_cast<uint32_t>('C')) &&
            (ncinput_ctrl_p(&in) || in.ctrl)) {
            running_ = false;
            break;
        }

        // Arrow keys scroll chat history
        if (key_action && input == NCKEY_UP) {
            ++chat_scroll_offset_;
        } else if (key_action && input == NCKEY_DOWN) {
            if (chat_scroll_offset_ > 0) {
                --chat_scroll_offset_;
            }
        } else if (key_action && input == NCKEY_PGDOWN) {
            chat_scroll_offset_ = 0;  // Jump to bottom
        } else if (key_action && (input == NCKEY_ENTER || input == '\n' || input == '\r')) {
            // Close command menu on Enter (whether sending message or executing command)
            if (showing_command_menu_) {
                close_command_menu();
            }
            if (!input_buffer_.empty() && selected_peer_index_ >= 0 &&
                selected_peer_index_ < static_cast<int>(people_rows_.size())) {
                // Check if input is a command (starts with /)
                if (input_buffer_[0] == '/') {
                    handle_command_input(input_buffer_);
                    input_buffer_.clear();
                    chat_scroll_offset_ = 0;
                    continue;
                }
                const std::string text = input_buffer_;
                const PeerInfo peer = people_rows_[selected_peer_index_];

                if (peer.username == "self") {
                    add_chat_message("self", true, text, local_datetime_now(), TerminalUI::unix_epoch_ms_now());
                } else if (!is_selected_peer_online()) {
                    show_alert_popup("Unable to send: " + peer.username + " is offline");
                } else if (on_send_chat_) {
                    std::string msg_id = on_send_chat_(peer, text);
                    if (!msg_id.empty()) {
                        add_chat_message(peer.username, true, text, local_datetime_now(), TerminalUI::unix_epoch_ms_now(), msg_id, "pending");
                        {
                            std::lock_guard<std::mutex> lock(pending_acks_mutex_);
                            pending_acks_[msg_id] = peer.username;
                        }
                    }
                }
                input_buffer_.clear();
                chat_scroll_offset_ = 0;  // Auto-scroll to bottom after sending
            }
        } else if (key_action &&
                   (input == NCKEY_BACKSPACE || input == 127 || input == '\b')) {
            if (!input_buffer_.empty()) {
                input_buffer_.pop_back();
                // Close command menu if buffer is now empty or doesn't start with '/'
                if (showing_command_menu_ && (input_buffer_.empty() || input_buffer_[0] != '/')) {
                    close_command_menu();
                }
            }
        } else if (key_action && input == 27) {  // Escape key
            if (showing_command_menu_) {
                close_command_menu();
            }
        } else if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                   (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
            handle_people_click(in.y, in.x);
        } else if (nckey_mouse_p(input) && input == NCKEY_SCROLL_UP) {
            ++chat_scroll_offset_;
        } else if (nckey_mouse_p(input) && input == NCKEY_SCROLL_DOWN) {
            if (chat_scroll_offset_ > 0) {
                --chat_scroll_offset_;
            }
        } else if (key_action && input >= 32 && input <= 126) {
            // in.id is the unshifted key under the Kitty keyboard protocol (Shift+/ reports '/'); eff_text has the shift-corrected char.
            const uint32_t typed = in.eff_text[0] != 0 ? in.eff_text[0] : input;
            if (typed >= 32 && typed <= 126 && input_buffer_.size() < 8192) {
                input_buffer_.push_back(static_cast<char>(typed));
                // Check if we just typed '/' as first character
                if (input_buffer_ == "/") {
                    showing_command_menu_ = true;
                    selected_command_ = 0;
                }
            }
        }

        // Toggle cursor blink every 500ms
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_cursor_toggle_).count() > 500) {
            cursor_visible_ = !cursor_visible_;
            last_cursor_toggle_ = now;
        }

        render();
    }
}
