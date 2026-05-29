#include "ui/TerminalUI.h"

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

std::string local_datetime_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return oss.str();
}

int64_t unix_epoch_ms_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string normalize_datetime_display(const std::string& input) {
    if (input.empty()) {
        return local_datetime_now();
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
}  // namespace

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

        if (key_action &&
            (input == static_cast<uint32_t>('c') || input == static_cast<uint32_t>('C')) &&
            (ncinput_ctrl_p(&in) || in.ctrl)) {
            running_ = false;
            break;
        }

        if (key_action && input == NCKEY_UP) {
            if (!people_rows_.empty()) {
                if (selected_peer_index_ <= 0) {
                    selected_peer_index_ = static_cast<int>(people_rows_.size()) - 1;
                } else {
                    --selected_peer_index_;
                }
            }
        } else if (key_action && input == NCKEY_DOWN) {
            if (!people_rows_.empty()) {
                selected_peer_index_ =
                    (selected_peer_index_ + 1) % static_cast<int>(people_rows_.size());
            }
        } else if (key_action && (input == NCKEY_ENTER || input == '\n' || input == '\r')) {
            if (!input_buffer_.empty() && selected_peer_index_ >= 0 &&
                selected_peer_index_ < static_cast<int>(people_rows_.size())) {
                // Don't allow sending if peer is offline
                if (!is_selected_peer_online()) {
                    add_debug("cannot send: peer is offline");
                } else {
                    const std::string text = input_buffer_;
                    const PeerInfo peer = people_rows_[selected_peer_index_];

                    if (peer.username == "self") {
                        add_chat_message("self", true, text, local_datetime_now(), unix_epoch_ms_now());
                    } else if (on_send_chat_) {
                        if (on_send_chat_(peer, text)) {
                            add_chat_message(peer.username, true, text, local_datetime_now(), unix_epoch_ms_now());
                        }
                    }
                }
                input_buffer_.clear();
            }
        } else if (key_action &&
                   (input == NCKEY_BACKSPACE || input == 127 || input == '\b')) {
            if (!input_buffer_.empty()) {
                input_buffer_.pop_back();
            }
        } else if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                   (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
            handle_people_click(in.y, in.x);
        } else if (key_action && input >= 32 && input <= 126) {
            if (input_buffer_.size() < 8192) {
                input_buffer_.push_back(static_cast<char>(input));
            }
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

void TerminalUI::upsert_peer(const std::string& name, const std::string& ip, uint16_t tcp_port) {
    if (name.empty() || ip.empty() || tcp_port == 0 || name == self_name_) {
        return;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_[name] = PeerInfo{name, ip, tcp_port};
    online_peers_.insert(name);
    last_seen_[name] = std::chrono::steady_clock::now();
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

void TerminalUI::add_chat_message(const std::string& peer_name, bool sender,
                                  const std::string& content, const std::string& datetime,
                                  int64_t timestamp_ms) {
    if (peer_name.empty() || content.empty()) {
        return;
    }

    // Save to database with timestamp_ms for proper ordering
    if (db_manager_) {
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

    std::vector<PeerInfo> peers;
    std::set<std::string> online_set;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers.push_back(PeerInfo{"self", "", 0});
        for (const auto& kv : peers_) {
            peers.push_back(kv.second);
        }
        online_set = online_peers_;
    }
    people_rows_ = std::move(peers);

    if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        selected_peer_index_ = 0;
    }

    for (size_t i = 0; i < people_rows_.size(); ++i) {
        const int y = 2 + static_cast<int>(i);
        if (y >= static_cast<int>(rows) - 1) {
            break;
        }

        const bool selected = static_cast<int>(i) == selected_peer_index_;
        const bool is_self = people_rows_[i].username == "self";
        const bool is_online = !is_self && online_set.count(people_rows_[i].username) > 0;

        ncplane_set_channels(people_plane_, selected ? selected_ch : text_ch);
        if (selected) {
            ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
        }

        // Build line: "  ● username" or "  ○ username"
        std::string line;
        if (is_self) {
            line = "  " + people_rows_[i].username;
        } else {
            // Use ● (filled circle) for online, ○ (empty circle) for offline
            line = is_online ? "  ● " : "  ○ ";
            line += people_rows_[i].username;
        }

        if (line.size() > cols - 2) {
            line.resize(cols - 2);
        }

        // Draw the dot in green if online
        if (!is_self && is_online && cols > 4) {
            // Draw the prefix with green dot
            ncplane_set_channels(people_plane_, online_ch);
            ncplane_putstr_yx(people_plane_, y, 1, " ●");
            // Draw the rest with normal text color
            ncplane_set_channels(people_plane_, selected ? selected_ch : text_ch);
            if (selected) {
                ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
            }
            std::string name_part = " " + people_rows_[i].username;
            if (name_part.size() > cols - 4) {
                name_part.resize(cols - 4);
            }
            ncplane_putstr_yx(people_plane_, y, 3, name_part.c_str());
        } else {
            ncplane_putstr_yx(people_plane_, y, 1, line.c_str());
        }

        if (selected) {
            ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
        }
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

    const int idx = rel_y - 2;
    if (idx < 0 || idx >= static_cast<int>(people_rows_.size())) {
        return false;
    }

    selected_peer_index_ = idx;
    return activate_selected_peer();
}

bool TerminalUI::activate_selected_peer() {
    if (selected_peer_index_ < 0 || selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        return false;
    }

    const PeerInfo peer = people_rows_[selected_peer_index_];
    if (peer.username == "self" || peer.ip.empty() || peer.tcp_port == 0) {
        return false;
    }

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

void TerminalUI::draw_chat() {
    const uint64_t border_ch = make_channels(0x93, 0xc5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t base_text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
    const uint64_t sender_ch = make_channels(0x86, 0xef, 0xac, 0x0f, 0x17, 0x2a);
    const uint64_t receiver_ch = make_channels(0x93, 0xc5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t dt_ch = make_channels(0x94, 0xa3, 0xb8, 0x0f, 0x17, 0x2a);
    const uint64_t input_border_ch = make_channels(0xc4, 0xb5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t offline_ch = make_channels(0x94, 0xa3, 0xb8, 0x0f, 0x17, 0x2a);

    std::string active_name = "self";
    if (selected_peer_index_ >= 0 && selected_peer_index_ < static_cast<int>(people_rows_.size())) {
        active_name = people_rows_[selected_peer_index_].username;
    }

    const bool peer_online = is_selected_peer_online();

    draw_panel(chat_plane_, " Chat: " + active_name + " ", border_ch, base_text_ch, 0x0f172a,
               0x111c33, 0x0f172a, 0x111c33);

    if (!chat_plane_) {
        return;
    }

    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(chat_plane_, &rows, &cols);
    if (rows < 8 || cols < 16) {
        return;
    }

    // If peer is offline, show offline message instead of input box
    if (!peer_online) {
        const std::string offline_msg = " Offline - cannot send messages ";
        const int msg_len = static_cast<int>(offline_msg.size());
        const int center_x = (static_cast<int>(cols) - msg_len) / 2;
        const int bottom_y = static_cast<int>(rows) - 2;
        ncplane_set_channels(chat_plane_, offline_ch);
        ncplane_putstr_yx(chat_plane_, bottom_y, std::max(1, center_x), offline_msg.c_str());
        ncplane_set_channels(chat_plane_, base_text_ch);
        // Don't draw input box, return early after drawing messages
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
        const std::string prefix = msg.sender ? "S: " : "R: ";
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
        start = static_cast<int>(visual.size()) - visible_lines;
    }

    int y = chat_top;
    for (int i = start; i < static_cast<int>(visual.size()) && y <= chat_bottom; ++i, ++y) {
        const VisualLine& vl = visual[i];
        ncplane_set_channels(chat_plane_, vl.sender ? sender_ch : receiver_ch);

        int max_left = content_w;
        if (!vl.right.empty()) {
            max_left = std::max(0, content_w - static_cast<int>(vl.right.size()) - 1);
        }

        std::string left = vl.left;
        if (static_cast<int>(left.size()) > max_left) {
            left.resize(static_cast<size_t>(max_left));
        }

        if (!left.empty()) {
            ncplane_putstr_yx(chat_plane_, y, 1, left.c_str());
        }

        if (!vl.right.empty() && static_cast<int>(vl.right.size()) <= content_w) {
            ncplane_set_channels(chat_plane_, dt_ch);
            const int x = 1 + content_w - static_cast<int>(vl.right.size());
            ncplane_putstr_yx(chat_plane_, y, x, vl.right.c_str());
        }
    }

    // Only draw input box if peer is online
    if (peer_online) {
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

    const int visible = static_cast<int>(rows) - 2;
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(debug_mutex_);
        lines = debug_lines_;
    }

    const int total = static_cast<int>(lines.size());
    const int start = std::max(0, total - visible);

    ncplane_set_channels(debug_plane_, text_ch);
    int row = 1;
    for (int i = start; i < total && row < static_cast<int>(rows) - 1; ++i, ++row) {
        std::string line = lines[i];
        if (line.size() > cols - 2) {
            line.resize(cols - 2);
        }
        ncplane_putstr_yx(debug_plane_, row, 1, line.c_str());
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
