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
        std::string fingerprint;  // Public key fingerprint for trust decisions
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

    void upsert_peer(const std::string& name, const std::string& ip, uint16_t tcp_port, 
                     bool trusted = false, bool untrusted_legacy = false,
                     const std::string& fingerprint = "");
    void mark_peer_offline(const std::string& name);
    void show_key_mismatch(const std::string& name, const std::string& new_fingerprint, 
                           const std::string& stored_fingerprint);
    void add_chat_message(const std::string& peer_name, bool sender,
                          const std::string& content, const std::string& datetime,
                          int64_t timestamp_ms = 0);

    // Trust handling
    void set_on_trust_callback(std::function<void(const std::string&)> callback) { on_trust_peer_ = callback; }
    
    // Identity display callback
    void set_on_show_identity(std::function<void()> callback) { on_show_identity_ = callback; }

    // Identity popup
    void show_identity_popup(const std::string& fingerprint);
    void close_identity_popup();
    void draw_identity_popup();
    bool handle_identity_popup_click(int abs_y, int abs_x);
    bool handle_identity_popup_key(char32_t ch);

    void add_debug(const std::string& line);

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
    bool is_selected_peer_trusted();
    
    // Trust modal dialog
    void show_trust_modal(const std::string& peer_name, const std::string& fingerprint);
    void close_trust_modal();
    void draw_trust_modal();
    bool handle_trust_modal_click(int abs_y, int abs_x);
    bool handle_trust_modal_key(char32_t ch);
    
    // Old trust prompt (deprecated)
    void draw_trust_prompt();
    bool handle_trust_keypress(char32_t ch);

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
    std::set<std::string> trusted_peers_;
    std::set<std::string> pending_peers_;
    std::set<std::string> mismatch_peers_;
    std::vector<PeerInfo> people_rows_;
    int selected_peer_index_{0};
    
    // Trust prompt state
    bool showing_trust_prompt_{false};
    std::string trust_prompt_peer_;
    std::string trust_prompt_fingerprint_;
    bool trust_prompt_is_mismatch_{false};
    std::string trust_prompt_stored_fingerprint_;
    
    // Trust modal dialog state
    bool showing_trust_modal_{false};
    std::string trust_modal_peer_;
    std::string trust_modal_fingerprint_;
    int trust_modal_selected_button_{0};  // 0 = Accept, 1 = Reject
    ncplane* trust_modal_plane_{nullptr};

    // Identity popup state
    bool showing_identity_popup_{false};
    std::string identity_popup_fingerprint_;
    ncplane* identity_popup_plane_{nullptr};

    std::unique_ptr<DatabaseManager> db_manager_;
    std::function<void(const std::string&)> on_peer_offline_;
    std::function<void(const std::string&)> on_trust_peer_;
    std::function<void()> on_show_identity_;

    std::thread timeout_checker_thread_;
    void run_timeout_checker();

    std::string input_buffer_;
    int chat_scroll_offset_{0};  // Scroll position in chat (0 = at bottom)
    
    // Blinking cursor state
    bool cursor_visible_{true};
    std::chrono::steady_clock::time_point last_cursor_toggle_;

    std::mutex chat_mutex_;
    std::map<std::string, std::vector<ChatItem>> chats_by_peer_;
};
