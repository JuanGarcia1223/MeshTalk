#include "ui/TerminalUI.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace {
uint64_t make_channels(unsigned fg_r, unsigned fg_g, unsigned fg_b, unsigned bg_r,
                                             unsigned bg_g, unsigned bg_b) {
    uint64_t ch = 0;
    ncchannels_set_fg_rgb8(&ch, fg_r, fg_g, fg_b);
    ncchannels_set_bg_rgb8(&ch, bg_r, bg_g, bg_b);
    return ch;
}
}    // namespace

TerminalUI::TerminalUI(bool debug_mode, std::string self_name,
                                             std::function<void(const PeerInfo&)> on_peer_activate)
        : debug_mode_(debug_mode),
            self_name_(std::move(self_name)),
            on_peer_activate_(std::move(on_peer_activate)) {}

TerminalUI::~TerminalUI() { stop(); }

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

void TerminalUI::run() {
    if (!nc_ || running_) {
        return;
    }
    running_ = true;
    add_debug("press Ctrl+C to quit");
    render();

    ncinput in{};
    while (running_) {
        timespec ts{};
        ts.tv_sec = 0;
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
                selected_peer_index_ = (selected_peer_index_ + 1) %
                                                             static_cast<int>(people_rows_.size());
            }
        } else if (key_action && (input == NCKEY_ENTER || input == '\n' || input == '\r')) {
            if (!input_buffer_.empty()) {
                add_debug(std::string("[input] ") + input_buffer_);
                input_buffer_.clear();
            }
        } else if (key_action && (input == NCKEY_BACKSPACE || input == 127 || input == '\b')) {
            if (!input_buffer_.empty()) {
                input_buffer_.pop_back();
            }
        } else if (nckey_mouse_p(input) && input == NCKEY_BUTTON1 &&
                             (in.evtype == NCTYPE_PRESS || in.evtype == NCTYPE_UNKNOWN)) {
            handle_people_click(in.y, in.x);
        } else if (key_action && input >= 32 && input <= 126) {
            if (input_buffer_.size() < 4096) {
                input_buffer_.push_back(static_cast<char>(input));
            }
        }

        if (debug_mode_) {
            if (input == NCKEY_RESIZE) {
                add_debug("resize event");
            }
        }
        render();
    }
}

void TerminalUI::upsert_peer(const std::string& name, const std::string& ip, uint16_t tcp_port) {
    if (name.empty() || ip.empty() || tcp_port == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    peers_[name] = PeerInfo{name, ip, tcp_port};
}

void TerminalUI::stop() {
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
    running_ = false;
}

void TerminalUI::render() {
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
    if (rows < 8 || cols < 40) {
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

    int people_w = std::max(12, (app_w * 20) / 100);
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
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& kv : peers_) {
            peers.push_back(kv.second);
        }
    }
    people_rows_ = peers;
    if (people_rows_.empty()) {
        selected_peer_index_ = -1;
    } else if (selected_peer_index_ < 0 ||
                         selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        selected_peer_index_ = 0;
    }

    ncplane_set_channels(people_plane_, text_ch);
    if (peers.empty()) {
        ncplane_putstr_yx(people_plane_, 2, 1, "(no peers yet)");
    } else {
        for (unsigned i = 0; i < peers.size(); ++i) {
            const int y = 2 + static_cast<int>(i);
            if (y >= static_cast<int>(rows) - 1) {
                break;
            }
            if (static_cast<int>(i) == selected_peer_index_) {
                ncplane_on_styles(people_plane_, NCSTYLE_BOLD);
            }
            std::string line = "• " + peers[i].username;
            line = line.substr(0, cols - 2);
            ncplane_putstr_yx(people_plane_, y, 1, line.c_str());
            if (static_cast<int>(i) == selected_peer_index_) {
                ncplane_off_styles(people_plane_, NCSTYLE_BOLD);
            }
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
    if (rel_y < 2 || rel_y >= static_cast<int>(h) - 1 || rel_x < 1 ||
            rel_x >= static_cast<int>(w) - 1) {
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
    if (selected_peer_index_ < 0 ||
            selected_peer_index_ >= static_cast<int>(people_rows_.size())) {
        return false;
    }
    const PeerInfo peer = people_rows_[selected_peer_index_];
    if (on_peer_activate_) {
        on_peer_activate_(peer);
        return true;
    }
    return false;
}

void TerminalUI::draw_chat() {
    const uint64_t border_ch = make_channels(0x93, 0xc5, 0xfd, 0x0f, 0x17, 0x2a);
    const uint64_t text_ch = make_channels(0xe2, 0xe8, 0xf0, 0x0f, 0x17, 0x2a);
    draw_panel(chat_plane_, " Chat ", border_ch, text_ch, 0x0f172a, 0x111c33, 0x0f172a, 0x111c33);

    if (!chat_plane_) {
        return;
    }
    unsigned rows = 0;
    unsigned cols = 0;
    ncplane_dim_yx(chat_plane_, &rows, &cols);
    if (rows < 7 || cols < 8) {
        return;
    }

    const int max_input_h = static_cast<int>(rows) - 2;
    const int input_w = static_cast<int>(cols) - 2;
    const int inner_w = std::max(1, input_w - 2);
    const uint64_t input_border_ch = make_channels(0xc4, 0xb5, 0xfd, 0x0f, 0x17, 0x2a);

    const std::string all_text = "> " + input_buffer_;
    const int text_lines =
            std::max(1, static_cast<int>((all_text.size() + static_cast<size_t>(inner_w) - 1) /
                                         static_cast<size_t>(inner_w)));
    const int input_h = std::min(max_input_h, text_lines + 2);
    const int input_y = static_cast<int>(rows) - 1 - input_h;

    ncplane_cursor_move_yx(chat_plane_, input_y, 1);
    ncplane_rounded_box_sized(chat_plane_, 0, input_border_ch, input_h, input_w, 0);

    ncplane_set_channels(chat_plane_, text_ch);
    ncplane_putstr_yx(chat_plane_, input_y, 3, " Input ");

    std::vector<std::string> wrapped;
    for (size_t i = 0; i < all_text.size(); i += static_cast<size_t>(inner_w)) {
        wrapped.push_back(all_text.substr(i, static_cast<size_t>(inner_w)));
    }
    if (wrapped.empty()) {
        wrapped.push_back("> ");
    }

    const int visible = input_h - 2;
    int start = static_cast<int>(wrapped.size()) - visible;
    if (start < 0) {
        start = 0;
    }
    for (int i = 0; i < visible; ++i) {
        const int idx = start + i;
        if (idx >= static_cast<int>(wrapped.size())) {
            break;
        }
        ncplane_putstr_yx(chat_plane_, input_y + 1 + i, 2, wrapped[idx].c_str());
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
        line = line.substr(0, cols - 2);
        ncplane_putstr_yx(debug_plane_, row, 1, line.c_str());
    }
}

void TerminalUI::add_debug(const std::string& line) {
    if (!debug_mode_) {
        return;
    }
    std::lock_guard<std::mutex> lock(debug_mutex_);
    debug_lines_.push_back(line);
    if (debug_lines_.size() > 1000) {
        debug_lines_.erase(debug_lines_.begin());
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
            std::string line = pending.substr(pos, newline - pos);
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
