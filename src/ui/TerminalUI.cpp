#include "ui/TerminalUI.h"

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

namespace {
uint64_t make_channels(unsigned fg_r, unsigned fg_g, unsigned fg_b, unsigned bg_r,
                       unsigned bg_g, unsigned bg_b) {
    uint64_t ch = 0;
    ncchannels_set_fg_rgb8(&ch, fg_r, fg_g, fg_b);
    ncchannels_set_bg_rgb8(&ch, bg_r, bg_g, bg_b);
    return ch;
}

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

int64_t TerminalUI::TerminalUI::unix_epoch_ms_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string normalize_datetime_display(const std::string& input) {
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

std::string TerminalUI::local_datetime_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return oss.str();
}

TerminalUI::TerminalUI(bool debug_mode, std::string self_name,
                       std::function<void(const PeerInfo&)> on_peer_activate,
                       std::function<bool(const PeerInfo&, const std::string&)> on_send_chat,
                       std::function<void(const std::string&)> on_peer_offline)
    : debug_mode_(debug_mode),
      self_name_(std::move(self_name)),
      on_peer_activate_(std::move(on_peer_activate)),
      on_send_chat_(std::move(on_send_chat)),
      on_peer_offline_(std::move(on_peer_offline)) {}

TerminalUI::~TerminalUI() { stop(); }

bool TerminalUI::initDatabase() {
    db_manager_ = std::make_unique<DatabaseManager>(self_name_);
    if (!db_manager_->init()) {
        std::cerr << "ui: failed to initialize database\n";
        return false;
    }

    // Load all existing peers from database
    std::vector<std::string> db_peers = db_manager_->getAllPeers();
    for (const auto& peer_name : db_peers) {
        // Add peer to chat map (will be displayed in people list)
        // They will appear as offline until discovered via UDP
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (peers_.find(peer_name) == peers_.end()) {
            peers_[peer_name] = PeerInfo{peer_name, "", 0};
        }
    }

    // Load all messages from database
    std::vector<ChatMessageRecord> records = db_manager_->loadAllMessages();
    for (const auto& record : records) {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        auto& thread = chats_by_peer_[record.peer_name];
        thread.push_back(ChatItem{record.is_sender, record.content, normalize_datetime_display(record.timestamp)});
        if (thread.size() > 2000) {
            thread.erase(thread.begin(), thread.begin() + static_cast<long>(thread.size() - 2000));
        }
    }

    // Load unread counts from database
    for (const auto& peer_name : db_peers) {
        int count = db_manager_->getUnreadCount(peer_name);
        if (count > 0) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            unread_counts_[peer_name] = count;
        }
    }

    add_debug("loaded " + std::to_string(records.size()) + " messages from database");
    return true;
}

bool TerminalUI::init() {
    notcurses_options opts{};
    opts.flags = NCOPTION_NO_WINCH_SIGHANDLER;

    tty_fp_ = std::fopen("/dev/tty", "w");
    if (!tty_fp_) {
        return false;
    }

    nc_ = notcurses_init(&opts, tty_fp_);
    if (!nc_) {
        std::fclose(tty_fp_);
        tty_fp_ = nullptr;
        return false;
    }

    if (!start_stdio_capture()) {
        notcurses_stop(nc_);
        nc_ = nullptr;
        std::fclose(tty_fp_);
        tty_fp_ = nullptr;
        return false;
    }

    if (debug_mode_) {
        add_debug("debug mode enabled");
    }
    notcurses_mice_enable(nc_, NCMICE_ALL_EVENTS);
    return true;
}

void TerminalUI::run_timeout_checker() {
    using namespace std::chrono;
    constexpr auto kTimeout = std::chrono::seconds(10);

    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<std::string> peers_to_offline;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            const auto now = steady_clock::now();
            for (auto it = last_seen_.begin(); it != last_seen_.end(); ) {
                if (now - it->second > kTimeout) {
                    const std::string& name = it->first;
                    if (online_peers_.count(name) > 0) {
                        peers_to_offline.push_back(name);
                        online_peers_.erase(name);
                    }
                    it = last_seen_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        for (const auto& name : peers_to_offline) {
            add_debug("peer timeout: " + name + " marked offline");
            if (on_peer_offline_) {
                on_peer_offline_(name);
            }
        }
    }
}

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

        // Handle Ctrl+Shift+D to show own public key
        if (key_action &&
            (input == static_cast<uint32_t>('d') || input == static_cast<uint32_t>('D')) &&
            (ncinput_ctrl_p(&in) || in.ctrl) && (ncinput_shift_p(&in) || in.shift)) {
            if (on_show_identity_) {
                on_show_identity_();
            }
            continue;  // Don't add 'D' to input buffer
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
                    if (on_send_chat_(peer, text)) {
                        add_chat_message(peer.username, true, text, local_datetime_now(), TerminalUI::unix_epoch_ms_now());
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
            if (input_buffer_.size() < 8192) {
                input_buffer_.push_back(static_cast<char>(input));
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

void TerminalUI::stop() {
    running_ = false;

    if (timeout_checker_thread_.joinable()) {
        timeout_checker_thread_.join();
    }

    destroy_layout();
    stop_stdio_capture();

    if (nc_) {
        notcurses_stop(nc_);
        nc_ = nullptr;
    }
    if (tty_fp_) {
        std::fclose(tty_fp_);
        tty_fp_ = nullptr;
    }
}

void TerminalUI::upsert_peer(const std::string& name, const std::string& ip, uint16_t tcp_port,
                             bool trusted, bool untrusted_legacy, const std::string& fingerprint) {
    if (name.empty() || ip.empty() || tcp_port == 0 || name == self_name_) {
        return;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_[name] = PeerInfo{name, ip, tcp_port, fingerprint};
    online_peers_.insert(name);
    last_seen_[name] = std::chrono::steady_clock::now();
    
    // Update trust status
    if (untrusted_legacy) {
        mismatch_peers_.insert(name);
        pending_peers_.erase(name);
        trusted_peers_.erase(name);
    } else if (trusted) {
        trusted_peers_.insert(name);
        pending_peers_.erase(name);
        mismatch_peers_.erase(name);
    } else {
        pending_peers_.insert(name);
        trusted_peers_.erase(name);
        mismatch_peers_.erase(name);
    }
}

void TerminalUI::mark_peer_offline(const std::string& name) {
    if (name.empty() || name == self_name_) {
        return;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    online_peers_.erase(name);
    last_seen_.erase(name);
    add_debug("peer BYE received: " + name + " marked offline");
}

void TerminalUI::increment_unread(const std::string& peer_name) {
    if (peer_name.empty() || peer_name == self_name_) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        ++unread_counts_[peer_name];
    }
    if (db_manager_) {
        db_manager_->incrementUnreadCount(peer_name);
    }
}

void TerminalUI::clear_unread(const std::string& peer_name) {
    if (peer_name.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        unread_counts_[peer_name] = 0;
    }
    if (db_manager_) {
        db_manager_->clearUnreadCount(peer_name);
        db_manager_->markMessagesAsRead(peer_name);
    }
}

int TerminalUI::get_unread_count(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = unread_counts_.find(peer_name);
    return (it != unread_counts_.end()) ? it->second : 0;
}

void TerminalUI::show_key_mismatch(const std::string& name, const std::string& new_fingerprint,
                                   const std::string& stored_fingerprint) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    mismatch_peers_.insert(name);
    trusted_peers_.erase(name);
    pending_peers_.erase(name);
    add_debug("SECURITY WARNING: key mismatch for " + name);
    add_debug("  Stored: " + stored_fingerprint);
    add_debug("  New:    " + new_fingerprint);
}

void TerminalUI::add_chat_message(const std::string& peer_name, bool sender,
                                  const std::string& content, const std::string& datetime,
                                  int64_t timestamp_ms) {
    if (peer_name.empty() || content.empty()) {
        return;
    }

    // Check if peer is trusted before saving to database
    bool is_trusted = false;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        is_trusted = (peer_name == "self") || (trusted_peers_.count(peer_name) > 0);
    }

    // Only save to database for trusted peers
    if (db_manager_ && is_trusted) {
        db_manager_->saveMessage(peer_name, sender, content, datetime, timestamp_ms,
                                 sender ? "sent" : "unread");
    }

    // Increment unread count for received messages from non-selected peers
    if (!sender) {
        bool is_selected = false;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
                is_selected = (people_rows_[selected_peer_index_].username == peer_name);
            }
        }
        if (!is_selected) {
            increment_unread(peer_name);
        }
    }

    std::lock_guard<std::mutex> lock(chat_mutex_);
    auto& thread = chats_by_peer_[peer_name];
    thread.push_back(ChatItem{sender, content, normalize_datetime_display(datetime)});
    if (thread.size() > 2000) {
        thread.erase(thread.begin(), thread.begin() + static_cast<long>(thread.size() - 2000));
    }
}

void TerminalUI::add_attachment_message(const std::string& peer_name, bool sender,
                                        const std::string& filename, uint64_t file_size,
                                        const std::string& datetime, int64_t timestamp_ms) {
    if (peer_name.empty() || filename.empty()) {
        return;
    }

    // Format attachment message with paperclip emoji
    std::string content = "📎 Attachment: " + filename + " (" + std::to_string((file_size + 1023) / 1024) + " KB)";

    // Check if peer is trusted before saving to database
    bool is_trusted = false;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        is_trusted = (peer_name == "self") || (trusted_peers_.count(peer_name) > 0);
    }

    // Only save to database for trusted peers
    if (db_manager_ && is_trusted) {
        db_manager_->saveMessage(peer_name, sender, content, datetime, timestamp_ms);
    }

    std::lock_guard<std::mutex> lock(chat_mutex_);
    auto& thread = chats_by_peer_[peer_name];
    thread.push_back(ChatItem{sender, content, normalize_datetime_display(datetime)});
    if (thread.size() > 2000) {
        thread.erase(thread.begin(), thread.begin() + static_cast<long>(thread.size() - 2000));
    }
}

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

    // Separator is at index trusted_count (0-based visual position)
    // If separator is shown and click is at or below it, adjust index
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

    // If pending, show trust modal
    if (is_pending) {
        std::string fp = peer.fingerprint.empty() ? "UNKNOWN" : peer.fingerprint;
        show_trust_modal(peer.username, fp);
        return true;
    }

    // If not trusted (legacy or mismatch), don't allow activation
    if (!is_trusted) {
        return false;
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

    // Show untrusted peer name in red in the title
    std::string title = " Chat: " + active_name + " ";
    uint64_t title_text_ch = base_text_ch;
    if (!peer_trusted && active_name != "self") {
        title = " Chat: " + active_name + "(untrusted) ";
        title_text_ch = warning_ch;
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

        for (size_t i = 0; i < text_lines.size(); ++i) {
            VisualLine line;
            line.sender = msg.sender;
            line.left = text_lines[i];
            if (i == 0 && inline_dt) {
                line.right = msg.datetime;
            }
            visual.push_back(std::move(line));
        }

        if (!inline_dt && !msg.datetime.empty()) {
            visual.push_back(VisualLine{msg.sender, "", msg.datetime});
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
            // Check if this line starts with warning symbol (for untrusted peers)
            // ⚠ is 3 bytes in UTF-8, so "⚠S: " is 6 bytes, "⚠R: " is 6 bytes
            if (!peer_trusted && left.size() >= 6 && left.substr(0, 6) == "⚠S: ") {
                // Draw warning symbol in yellow
                ncplane_set_channels(chat_plane_, warning_yellow_ch);
                ncplane_putstr_yx(chat_plane_, y, 1, "⚠");
                // Draw rest in sender color (⚠ = 3 bytes, so "S: " starts at byte 3)
                ncplane_set_channels(chat_plane_, sender_ch);
                ncplane_putstr_yx(chat_plane_, y, 4, left.substr(3).c_str());
            } else if (!peer_trusted && left.size() >= 6 && left.substr(0, 6) == "⚠R: ") {
                // Draw warning symbol in yellow
                ncplane_set_channels(chat_plane_, warning_yellow_ch);
                ncplane_putstr_yx(chat_plane_, y, 1, "⚠");
                // Draw rest in receiver color (⚠ = 3 bytes, so "R: " starts at byte 3)
                ncplane_set_channels(chat_plane_, receiver_ch);
                ncplane_putstr_yx(chat_plane_, y, 4, left.substr(3).c_str());
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

    const int menu_w = std::min(40, static_cast<int>(cols) - 4);
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

    // Commands - just reference, no selection
    for (size_t i = 0; i < commands_.size(); ++i) {
        int y = menu_y + 1 + static_cast<int>(i);
        ncplane_set_channels(chat_plane_, cmd_ch);
        ncplane_putstr_yx(chat_plane_, y, menu_x + 2, commands_[i].first.c_str());
        // Description
        ncplane_set_channels(chat_plane_, dim_ch);
        int desc_x = menu_x + 10;
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

    // Just put the command in the input buffer as autocomplete
    // User can then type more text and press Enter to send
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
    if (!peer_online && peer_name != "self" && cmd != "/DOWNLOAD" && cmd != "/STATUS" && cmd != "/CLEAR") {
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
                if (on_send_chat_(peer, msg)) {
                    add_chat_message(peer.username, true, msg, local_datetime_now(), TerminalUI::unix_epoch_ms_now());
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
                if (on_send_chat_(peer, msg)) {
                    add_chat_message(peer.username, true, msg, local_datetime_now(), TerminalUI::unix_epoch_ms_now());
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
        
        // Get file size for the popup
        struct stat st;
        if (stat(args.c_str(), &st) != 0) {
            add_debug("Cannot access file: " + args);
            return;
        }
        
        // Show upload popup
        show_upload_popup(args, peer.username, static_cast<uint64_t>(st.st_size));
        
        // Trigger file upload via callback
        if (on_upload_file_) {
            if (on_upload_file_(args, peer.username, peer.ip, peer.tcp_port)) {
                add_debug("Started uploading " + args + " to " + peer.username);
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

void TerminalUI::show_trust_modal(const std::string& peer_name, const std::string& fingerprint) {
    if (showing_trust_modal_) {
        close_trust_modal();
    }
    showing_trust_modal_ = true;
    trust_modal_peer_ = peer_name;
    trust_modal_fingerprint_ = fingerprint;
    trust_modal_selected_button_ = 0;  // Default to Accept
    trust_modal_plane_ = nullptr;  // Will be created in render
}

void TerminalUI::close_trust_modal() {
    showing_trust_modal_ = false;
    trust_modal_peer_.clear();
    trust_modal_fingerprint_.clear();
    if (trust_modal_plane_) {
        ncplane_destroy(trust_modal_plane_);
        trust_modal_plane_ = nullptr;
    }
}

void TerminalUI::draw_trust_modal() {
    if (!showing_trust_modal_ || !nc_) return;

    // Get terminal dimensions
    unsigned term_rows = 0, term_cols = 0;
    notcurses_term_dim_yx(nc_, &term_rows, &term_cols);

    // Modal dimensions
    const int modal_w = 60;
    const int modal_h = 12;
    const int modal_x = (static_cast<int>(term_cols) - modal_w) / 2;
    const int modal_y = (static_cast<int>(term_rows) - modal_h) / 2;

    // Create or recreate the modal plane
    if (!trust_modal_plane_) {
        trust_modal_plane_ = make_plane(modal_y, modal_x, modal_h, modal_w, "trust_modal");
        if (!trust_modal_plane_) return;
    }

    // Colors
    const uint64_t border_ch = make_channels(0xff, 0xaa, 0x44, 0x22, 0x12, 0x29);
    const uint64_t title_ch = make_channels(0xff, 0xff, 0xff, 0x22, 0x12, 0x29);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x22, 0x12, 0x29);
    const uint64_t dim_ch = make_channels(0x80, 0x80, 0x80, 0x22, 0x12, 0x29);
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

    // Warning
    ncplane_set_channels(trust_modal_plane_, make_channels(0xff, 0x44, 0x44, 0x22, 0x12, 0x29));
    ncplane_putstr_yx(trust_modal_plane_, 8, 4, "Verify this fingerprint out-of-band!");

    // Buttons
    const int button_y = modal_h - 3;
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

bool TerminalUI::handle_trust_modal_click(int abs_y, int abs_x) {
    if (!showing_trust_modal_ || !trust_modal_plane_) return false;

    int plane_y = 0, plane_x = 0;
    ncplane_abs_yx(trust_modal_plane_, &plane_y, &plane_x);

    unsigned h = 0, w = 0;
    ncplane_dim_yx(trust_modal_plane_, &h, &w);

    const int button_y = static_cast<int>(h) - 3;
    const int rel_y = abs_y - plane_y;
    const int rel_x = abs_x - plane_x;

    if (rel_y != button_y) return false;

    // Check Accept button (x: 12-21)
    if (rel_x >= 12 && rel_x <= 21) {
        if (on_trust_peer_) {
            on_trust_peer_(trust_modal_peer_);
        }
        add_debug("trusted peer: " + trust_modal_peer_);
        close_trust_modal();
        return true;
    }

    // Check Reject button (x: 35-44)
    if (rel_x >= 35 && rel_x <= 44) {
        add_debug("rejected peer: " + trust_modal_peer_);
        close_trust_modal();
        return true;
    }

    return false;
}

bool TerminalUI::handle_trust_modal_key(char32_t ch) {
    if (!showing_trust_modal_) return false;

    switch (ch) {
        case NCKEY_LEFT:
        case NCKEY_RIGHT:
            trust_modal_selected_button_ = 1 - trust_modal_selected_button_;  // Toggle 0/1
            return true;
        case NCKEY_ENTER:
        case '\n':
        case '\r':
            if (trust_modal_selected_button_ == 0) {
                // Accept
                if (on_trust_peer_) {
                    on_trust_peer_(trust_modal_peer_);
                }
                add_debug("trusted peer: " + trust_modal_peer_);
            } else {
                // Reject
                add_debug("rejected peer: " + trust_modal_peer_);
            }
            close_trust_modal();
            return true;
        case 27:  // Escape
        case 'q':
        case 'Q':
            add_debug("cancelled trust dialog for: " + trust_modal_peer_);
            close_trust_modal();
            return true;
    }
    return false;
}

void TerminalUI::show_identity_popup(const std::string& fingerprint) {
    if (showing_identity_popup_) {
        close_identity_popup();
    }
    showing_identity_popup_ = true;
    identity_popup_fingerprint_ = fingerprint;
    identity_popup_plane_ = nullptr;
}

void TerminalUI::close_identity_popup() {
    showing_identity_popup_ = false;
    identity_popup_fingerprint_.clear();
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

    // Modal dimensions
    const int modal_w = 50;
    const int modal_h = 8;
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
    ncplane_putstr_yx(identity_popup_plane_, 1, 2, " Your Public Key Fingerprint ");
    ncplane_off_styles(identity_popup_plane_, NCSTYLE_BOLD);

    // Fingerprint (centered)
    ncplane_set_channels(identity_popup_plane_, fp_ch);
    int fp_x = (modal_w - static_cast<int>(identity_popup_fingerprint_.size())) / 2;
    if (fp_x < 2) fp_x = 2;
    ncplane_putstr_yx(identity_popup_plane_, 3, fp_x, identity_popup_fingerprint_.c_str());

    // Instruction
    ncplane_set_channels(identity_popup_plane_, text_ch);
    std::string instr = "Share this with others to verify your identity";
    int instr_x = (modal_w - static_cast<int>(instr.size())) / 2;
    if (instr_x < 2) instr_x = 2;
    ncplane_putstr_yx(identity_popup_plane_, 5, instr_x, instr.c_str());

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

    // Work backwards from the end to find the first line that fits in max_rows
    // when accounting for text wrapping.
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

void TerminalUI::add_debug(const std::string& line) {
    if (!debug_mode_) {
        return;
    }
    std::lock_guard<std::mutex> lock(debug_mutex_);
    debug_lines_.push_back(line);
    if (debug_lines_.size() > 2000) {
        debug_lines_.erase(debug_lines_.begin(),
                           debug_lines_.begin() + static_cast<long>(debug_lines_.size() - 2000));
    }
}

bool TerminalUI::start_stdio_capture() {
    int stdout_pipe[2];
    int stderr_pipe[2];

    if (pipe(stdout_pipe) != 0) {
        return false;
    }
    if (pipe(stderr_pipe) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    orig_stdout_fd_ = dup(STDOUT_FILENO);
    orig_stderr_fd_ = dup(STDERR_FILENO);
    if (orig_stdout_fd_ < 0 || orig_stderr_fd_ < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        close_fd(orig_stdout_fd_);
        close_fd(orig_stderr_fd_);
        return false;
    }

    stdout_pipe_read_ = stdout_pipe[0];
    stdout_pipe_write_ = stdout_pipe[1];
    stderr_pipe_read_ = stderr_pipe[0];
    stderr_pipe_write_ = stderr_pipe[1];

    if (dup2(stdout_pipe_write_, STDOUT_FILENO) < 0 || dup2(stderr_pipe_write_, STDERR_FILENO) < 0) {
        stop_stdio_capture();
        return false;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    capture_running_ = true;
    stdout_thread_ = std::thread(&TerminalUI::capture_loop, this, stdout_pipe_read_, "stdout");
    stderr_thread_ = std::thread(&TerminalUI::capture_loop, this, stderr_pipe_read_, "stderr");
    return true;
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
            std::string when = normalize_datetime_display(file.timestamp);
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

void TerminalUI::stop_stdio_capture() {
    if (orig_stdout_fd_ == -1 && orig_stderr_fd_ == -1) {
        return;
    }

    fflush(stdout);
    fflush(stderr);

    if (orig_stdout_fd_ != -1) {
        dup2(orig_stdout_fd_, STDOUT_FILENO);
    }
    if (orig_stderr_fd_ != -1) {
        dup2(orig_stderr_fd_, STDERR_FILENO);
    }

    close_fd(orig_stdout_fd_);
    close_fd(orig_stderr_fd_);
    close_fd(stdout_pipe_write_);
    close_fd(stderr_pipe_write_);

    capture_running_ = false;

    if (stdout_thread_.joinable()) {
        stdout_thread_.join();
    }
    if (stderr_thread_.joinable()) {
        stderr_thread_.join();
    }

    close_fd(stdout_pipe_read_);
    close_fd(stderr_pipe_read_);
}

void TerminalUI::capture_loop(int read_fd, const char* stream_name) {
    std::string pending;
    char buffer[512];

    while (capture_running_) {
        const ssize_t n = read(read_fd, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }

        pending.append(buffer, static_cast<size_t>(n));
        size_t pos = 0;
        while (true) {
            const size_t newline = pending.find('\n', pos);
            if (newline == std::string::npos) {
                pending.erase(0, pos);
                break;
            }

            const std::string line = pending.substr(pos, newline - pos);
            if (debug_mode_) {
                add_debug(std::string("[") + stream_name + "] " + line);
            }
            pos = newline + 1;
        }
    }

    if (!pending.empty() && debug_mode_) {
        add_debug(std::string("[") + stream_name + "] " + pending);
    }
}

void TerminalUI::close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}
