#pragma once

#include <notcurses/notcurses.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TerminalUI {
public:
    struct PeerInfo {
        std::string username;
        std::string ip;
        uint16_t tcp_port{0};
    };

    TerminalUI(bool debug_mode, std::string self_name,
                         std::function<void(const PeerInfo&)> on_peer_activate = {});
    ~TerminalUI();

    TerminalUI(const TerminalUI&) = delete;
    TerminalUI& operator=(const TerminalUI&) = delete;

    bool init();
    void run();
    void stop();
    void upsert_peer(const std::string& name, const std::string& ip, uint16_t tcp_port);

private:
    void render();
    bool ensure_layout();
    void rebuild_layout(unsigned rows, unsigned cols);
    void destroy_layout();
    ncplane* make_plane(int y, int x, int h, int w, const char* name);
    void draw_panel(ncplane* plane, const std::string& title, uint64_t border_channels,
                                    uint64_t text_channels, uint32_t bg_ul, uint32_t bg_ur,
                                    uint32_t bg_ll, uint32_t bg_lr);
    void draw_contacts();
    void draw_chat();
    void draw_debug();
    bool handle_people_click(int abs_y, int abs_x);
    bool activate_selected_peer();
    void add_debug(const std::string& line);
    bool start_stdio_capture();
    void stop_stdio_capture();
    void capture_loop(int read_fd, const char* stream_name);
    void close_fd(int& fd);

    bool debug_mode_{false};
    bool running_{false};
    std::atomic<bool> capture_running_{false};
    std::string self_name_;
    std::function<void(const PeerInfo&)> on_peer_activate_;
    FILE* tty_fp_{nullptr};
    notcurses* nc_{nullptr};
    int orig_stdout_fd_{-1};
    int orig_stderr_fd_{-1};
    int stdout_pipe_read_{-1};
    int stdout_pipe_write_{-1};
    int stderr_pipe_read_{-1};
    int stderr_pipe_write_{-1};
    std::thread stdout_thread_;
    std::thread stderr_thread_;
    ncplane* people_plane_{nullptr};
    ncplane* chat_plane_{nullptr};
    ncplane* debug_plane_{nullptr};
    unsigned last_rows_{0};
    unsigned last_cols_{0};
    std::mutex debug_mutex_;
    std::vector<std::string> debug_lines_;
    std::mutex peers_mutex_;
    std::map<std::string, PeerInfo> peers_;
    std::vector<PeerInfo> people_rows_;
    int selected_peer_index_{-1};
    std::string input_buffer_;
};
