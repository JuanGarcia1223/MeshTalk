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

using tui_detail::format_display_datetime;

int64_t TerminalUI::TerminalUI::unix_epoch_ms_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
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


TerminalUI::TerminalUI(bool debug_mode, bool enable_ack_show, std::string self_name,
                       std::function<void(const PeerInfo&)> on_peer_activate,
                       std::function<std::string(const PeerInfo&, const std::string&)> on_send_chat,
                       std::function<void(const std::string&)> on_peer_offline)
    : debug_mode_(debug_mode),
      enable_ack_show_(enable_ack_show),
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
        std::lock_guard<std::mutex> lock(peers_mutex_);
        if (peers_.find(peer_name) == peers_.end()) {
            peers_[peer_name] = PeerInfo{peer_name, "", 0};
        }
    }

    {
        auto known = db_manager_->getAllKnownPeers();
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& kp : known) {
            if (kp.trust_status == "trusted") {
                trusted_peers_.insert(kp.username);
                pending_peers_.erase(kp.username);
                mismatch_peers_.erase(kp.username);
            }
            if (peers_.find(kp.username) == peers_.end()) {
                peers_[kp.username] = PeerInfo{kp.username, "", 0};
            }
        }
    }

    // Load all messages from database
    std::vector<ChatMessageRecord> records = db_manager_->loadAllMessages();
    for (const auto& record : records) {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        auto& thread = chats_by_peer_[record.peer_name];
        thread.push_back(ChatItem{record.is_sender, record.content, format_display_datetime(record.timestamp, record.timestamp_ms), record.msg_id, record.status});
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
    notcurses_mice_enable(nc_, NCMICE_BUTTON_EVENT);
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
                                  int64_t timestamp_ms, const std::string& msg_id,
                                  const std::string& status) {
    if (peer_name.empty() || content.empty()) {
        return;
    }

    // Save all messages to database (including pending peers)
    if (db_manager_) {
        db_manager_->saveMessage(peer_name, sender, content, datetime, timestamp_ms,
                                 status.empty() ? (sender ? "sent" : "delivered") : status, msg_id);
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
    thread.push_back(ChatItem{sender, content, format_display_datetime(datetime, timestamp_ms), msg_id, status});
    if (thread.size() > 2000) {
        thread.erase(thread.begin(), thread.begin() + static_cast<long>(thread.size() - 2000));
    }
}


void TerminalUI::add_attachment_message(const std::string& peer_name, bool sender,
                                        const std::string& filename, uint64_t file_size,
                                        const std::string& datetime, int64_t timestamp_ms,
                                        const std::string& msg_id, const std::string& status) {
    if (peer_name.empty() || filename.empty()) {
        return;
    }

    // Format attachment message with paperclip emoji
    std::string content = "📎 Attachment: " + filename + " (" + std::to_string((file_size + 1023) / 1024) + " KB)";

    // Save all messages to database (including pending peers)
    if (db_manager_) {
        db_manager_->saveMessage(peer_name, sender, content, datetime, timestamp_ms,
                                 status.empty() ? (sender ? "sent" : "delivered") : status, msg_id);
    }

    std::lock_guard<std::mutex> lock(chat_mutex_);
    auto& thread = chats_by_peer_[peer_name];
    thread.push_back(ChatItem{sender, content, format_display_datetime(datetime, timestamp_ms), msg_id, status});
    if (thread.size() > 2000) {
        thread.erase(thread.begin(), thread.begin() + static_cast<long>(thread.size() - 2000));
    }
}


void TerminalUI::handle_delivery_ack(const std::string& peer_name, const std::string& msg_id, bool success) {
    {
        std::lock_guard<std::mutex> lock(pending_acks_mutex_);
        pending_acks_.erase(msg_id);
    }
    if (db_manager_) {
        db_manager_->updateMessageStatus(msg_id, success ? "delivered" : "failed");
    }
    // Update the message status in memory
    {
        std::lock_guard<std::mutex> lock(chat_mutex_);
        auto it = chats_by_peer_.find(peer_name);
        if (it != chats_by_peer_.end()) {
            for (auto& item : it->second) {
                if (item.msg_id == msg_id) {
                    item.status = success ? "delivered" : "failed";
                    break;
                }
            }
        }
    }
}


std::optional<TerminalUI::PeerInfo> TerminalUI::get_peer_info(const std::string& peer_name) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = peers_.find(peer_name);
    if (it != peers_.end()) {
        return it->second;
    }
    return std::nullopt;
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
