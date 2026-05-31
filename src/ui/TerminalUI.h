#pragma once

#include <notcurses/notcurses.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "db/DatabaseManager.h"

class TerminalUI {
public:
    struct PeerInfo {
        std::string username;
        std::string ip;
        uint16_t tcp_port{0};
    };

    struct ChatItem {
        bool sender{false};
        std::string content;
        std::string datetime;
    };

    TerminalUI(bool debug_mode, std::string self_name,
               std::function<void(const PeerInfo&)> on_peer_activate = {},
               std::function<bool(const PeerInfo&, const std::string&)> on_send_chat = {},
               std::function<void(const std::string&)> on_peer_offline = {});
    ~TerminalUI();

    bool initDatabase();
    DatabaseManager* getDatabase() { return db_manager_.get(); }

    TerminalUI(const TerminalUI&) = delete;
    TerminalUI& operator=(const TerminalUI&) = delete;

    bool init();
    void run();
    void stop();

    void upsert_peer(const std::string& name, const std::string& ip, uint16_t tcp_port);
    void mark_peer_offline(const std::string& name);
    void add_chat_message(const std::string& peer_name, bool sender,
                          const std::string& content, const std::string& datetime,
                          int64_t timestamp_ms = 0);

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
    bool is_selected_peer_online();
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
    std::function<bool(const PeerInfo&, const std::string&)> on_send_chat_;

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
    std::map<std::string, std::chrono::steady_clock::time_point> last_seen_;
    std::set<std::string> online_peers_;
    std::vector<PeerInfo> people_rows_;
    int selected_peer_index_{0};

    std::unique_ptr<DatabaseManager> db_manager_;
    std::function<void(const std::string&)> on_peer_offline_;

    std::thread timeout_checker_thread_;
    void run_timeout_checker();

    std::string input_buffer_;

    std::mutex chat_mutex_;
    std::map<std::string, std::vector<ChatItem>> chats_by_peer_;
};
