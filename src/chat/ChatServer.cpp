#include "chat/ChatServer.h"

#include "chat.pb.h"
#include "envelope.pb.h"
#include "crypto/SessionManager.h"
#include "db/DatabaseManager.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <random>

namespace {
bool send_all(int fd, const void* data, size_t len);
bool recv_all(int fd, void* data, size_t len);
int64_t unix_epoch_ms();
std::string iso_utc_now();

bool send_envelope(int fd, const Envelope& env, SessionManager* sm = nullptr,
                    const std::string& peer_name = "") {
    std::string payload;
    if (!env.SerializeToString(&payload)) return false;

    if (sm && sm->isReady(peer_name)) {
        auto encrypted = sm->encrypt(peer_name,
            std::vector<uint8_t>(payload.begin(), payload.end()));
        if (!encrypted) return false;
        Envelope wrapper;
        wrapper.set_type(Envelope::CHAT);
        wrapper.set_ciphertext(encrypted->first.data(), encrypted->first.size());
        wrapper.set_nonce(encrypted->second.data(), encrypted->second.size());
        if (!wrapper.SerializeToString(&payload)) return false;
    }

    uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
    return send_all(fd, &len_be, sizeof(len_be)) && send_all(fd, payload.data(), payload.size());
}

bool recv_envelope(int fd, Envelope& env) {
    uint32_t len_be = 0;
    if (!recv_all(fd, &len_be, sizeof(len_be))) return false;
    uint32_t len = ntohl(len_be);
    if (len == 0 || len > 32 * 1024 * 1024) return false;
    std::string payload(len, '\0');
    if (!recv_all(fd, payload.data(), len)) return false;
    return env.ParseFromString(payload);
}

bool send_all(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = send(fd, p + sent, len - sent, MSG_NOSIGNAL);
#else
        const ssize_t n = send(fd, p + sent, len - sent, 0);
#endif
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* data, size_t len) {
    char* p = static_cast<char*>(data);
    size_t got = 0;
    while (got < len) {
        const ssize_t n = recv(fd, p + got, len - got, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

int64_t unix_epoch_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string iso_utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&tt, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}
}  // namespace

ChatServer::ChatServer() = default;

ChatServer::~ChatServer() { stop(); }

void ChatServer::set_database(DatabaseManager* db) {
    db_ = db;
}

void ChatServer::set_file_progress_callback(
    std::function<void(const std::string&, uint64_t, uint64_t, bool)> callback) {
    file_progress_callback_ = std::move(callback);
}

void ChatServer::set_file_received_callback(
    std::function<void(const std::string&, bool, const std::string&, const std::string&, uint64_t)> callback) {
    file_received_callback_ = std::move(callback);
}

std::string ChatServer::compute_sha256(const std::vector<uint8_t>& data) {
    // Simple hash for now - in production use proper SHA256
    // For now return a placeholder based on size and first/last bytes
    std::ostringstream oss;
    oss << std::hex << data.size();
    if (!data.empty()) {
        oss << std::setw(2) << std::setfill('0') << static_cast<int>(data.front());
        oss << std::setw(2) << std::setfill('0') << static_cast<int>(data.back());
    }
    return oss.str();
}

bool ChatServer::send_file_offer(const std::string& from_user, const std::string& to_user,
                                 const std::string& ip, uint16_t port, const std::string& filepath) {
    // Read file
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "chat: failed to open file " << filepath << "\n";
        return false;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0 || size > 100 * 1024 * 1024) {  // Max 100MB
        std::cerr << "chat: file too large or empty: " << filepath << "\n";
        return false;
    }

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "chat: failed to read file " << filepath << "\n";
        return false;
    }

    // Extract filename from path
    std::string filename = filepath;
    size_t last_slash = filepath.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        filename = filepath.substr(last_slash + 1);
    }

    // Generate transfer ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::ostringstream transfer_id;
    transfer_id << "ft-" << unix_epoch_ms() << "-";
    for (int i = 0; i < 8; ++i) {
        transfer_id << std::hex << dis(gen);
    }

    // Create transfer record
    auto transfer = std::make_shared<OutgoingFileTransfer>();
    transfer->transfer_id = transfer_id.str();
    transfer->filename = filename;
    transfer->file_size = static_cast<uint64_t>(size);
    transfer->sha256_hash = compute_sha256(buffer);
    transfer->from_user = from_user;
    transfer->peer_name = to_user;
    transfer->ip = ip;
    transfer->port = port;
    transfer->file_data = std::move(buffer);
    transfer->status = FileTransferStatus::PENDING;
    transfer->bytes_sent = 0;
    transfer->cancelled = false;

    {
        std::lock_guard<std::mutex> lock(file_transfers_mutex_);
        outgoing_transfers_[transfer->transfer_id] = transfer;
    }

    // Save to database
    if (db_) {
        db_->saveFileTransfer(transfer->transfer_id, filename, transfer->file_size,
                              transfer->sha256_hash, to_user, true, "pending",
                              iso_utc_now(), unix_epoch_ms());
        // Keep local copy so sent files can also be downloaded later.
        db_->saveFileData(transfer->transfer_id, transfer->file_data);
    }

    // Ensure connection
    if (!connect_to(ip, port, to_user)) {
        return false;
    }

    // Send file offer
    FileOffer offer;
    offer.set_transfer_id(transfer->transfer_id);
    offer.set_filename(filename);
    offer.set_file_size(transfer->file_size);
    offer.set_sha256_hash(transfer->sha256_hash);

    // Wrap in envelope (we need to use the protobuf message type)
    // For now, we'll use a special prefix in content to indicate file offer
    // The actual implementation will parse different message types

    std::cout << "chat: sending file offer " << transfer->transfer_id 
              << " for " << filename << " (" << transfer->file_size << " bytes) to " << to_user << "\n";

    // Start sending chunks in a separate thread
    std::thread(&ChatServer::send_file_chunks, this, transfer->transfer_id).detach();

    return true;
}

void ChatServer::send_file_chunks(const std::string& transfer_id) {
    std::shared_ptr<OutgoingFileTransfer> transfer;
    {
        std::lock_guard<std::mutex> lock(file_transfers_mutex_);
        auto it = outgoing_transfers_.find(transfer_id);
        if (it == outgoing_transfers_.end()) {
            return;
        }
        transfer = it->second;
    }

    int fd = get_outbound_fd(transfer->ip, transfer->port);
    if (fd < 0) {
        std::cerr << "chat: no connection for file transfer " << transfer_id << "\n";
        transfer->status = FileTransferStatus::FAILED;
        return;
    }

    // Send file offer envelope
    {
        FileOffer offer;
        offer.set_transfer_id(transfer->transfer_id);
        offer.set_filename(transfer->filename);
        offer.set_file_size(transfer->file_size);
        offer.set_sha256_hash(transfer->sha256_hash);
        offer.set_from_user(transfer->from_user);
        offer.set_to_user(transfer->peer_name);

        Envelope env;
        env.set_type(Envelope::FILE_OFFER);
        *env.mutable_file_offer() = std::move(offer);
        if (!send_envelope(fd, env, session_manager_, transfer->peer_name)) {
            std::cerr << "chat: failed to send file offer\n";
            transfer->status = FileTransferStatus::FAILED;
            return;
        }
    }

    // Wait a bit for response (simplified - should wait for FileResponse)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Now send chunks
    constexpr size_t CHUNK_SIZE = 32768;  // 32KB chunks
    transfer->status = FileTransferStatus::IN_PROGRESS;

    if (db_) {
        db_->updateFileTransferStatus(transfer_id, "in_progress");
    }

    for (size_t offset = 0; offset < transfer->file_data.size() && !transfer->cancelled; offset += CHUNK_SIZE) {
        size_t chunk_size = std::min(CHUNK_SIZE, transfer->file_data.size() - offset);
        
        FileChunk chunk;
        chunk.set_transfer_id(transfer_id);
        chunk.set_chunk_index(static_cast<uint32_t>(offset / CHUNK_SIZE));
        chunk.set_data(transfer->file_data.data() + offset, chunk_size);
        chunk.set_is_last(offset + chunk_size >= transfer->file_data.size());

        Envelope env;
        env.set_type(Envelope::FILE_CHUNK);
        *env.mutable_file_chunk() = std::move(chunk);
        if (!send_envelope(fd, env, session_manager_, transfer->peer_name)) {
            std::cerr << "chat: failed to send file chunk\n";
            transfer->status = FileTransferStatus::FAILED;
            if (db_) {
                db_->updateFileTransferStatus(transfer_id, "failed");
            }
            return;
        }

        transfer->bytes_sent += chunk_size;

        if (file_progress_callback_) {
            file_progress_callback_(transfer_id, transfer->bytes_sent, transfer->file_size, false);
        }

        // Small delay to not overwhelm the network
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (transfer->cancelled) {
        transfer->status = FileTransferStatus::CANCELLED;
        if (db_) {
            db_->updateFileTransferStatus(transfer_id, "cancelled");
        }
        return;
    }

    // Send complete envelope
    {
        FileComplete complete;
        complete.set_transfer_id(transfer_id);
        complete.set_sha256_hash(transfer->sha256_hash);

        Envelope env;
        env.set_type(Envelope::FILE_COMPLETE);
        *env.mutable_file_complete() = std::move(complete);
        send_envelope(fd, env, session_manager_, transfer->peer_name);
    }

    transfer->status = FileTransferStatus::COMPLETE;
    if (db_) {
        db_->updateFileTransferStatus(transfer_id, "complete");
    }

    if (file_progress_callback_) {
        file_progress_callback_(transfer_id, transfer->file_size, transfer->file_size, true);
    }

    if (file_received_callback_) {
        file_received_callback_(transfer->peer_name, true, transfer_id, transfer->filename,
                                transfer->file_size);
    }

    std::cout << "chat: file transfer " << transfer_id << " complete\n";
}

void ChatServer::cancel_file_transfer(const std::string& transfer_id) {
    std::lock_guard<std::mutex> lock(file_transfers_mutex_);
    auto it = outgoing_transfers_.find(transfer_id);
    if (it != outgoing_transfers_.end()) {
        it->second->cancelled = true;
    }
}

bool ChatServer::download_file(const std::string& transfer_id, const std::string& download_path) {
    if (!db_) {
        return false;
    }

    auto data = db_->loadFileData(transfer_id);
    if (!data) {
        std::cerr << "chat: no file data found for transfer " << transfer_id << "\n";
        return false;
    }

    std::ofstream file(download_path, std::ios::binary);
    if (!file) {
        std::cerr << "chat: failed to create download file " << download_path << "\n";
        return false;
    }

    file.write(reinterpret_cast<const char*>(data->data()), data->size());
    return file.good();
}

std::string ChatServer::endpoint_key(const std::string& ip, uint16_t port) {
    return ip + ":" + std::to_string(port);
}

std::string ChatServer::resolve_peer_name(const std::string& peer_ip) {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    auto it = name_by_ip_.find(peer_ip);
    return (it != name_by_ip_.end()) ? it->second : peer_ip;
}

std::string ChatServer::session_key_for_ip(const std::string& peer_ip) {
    std::lock_guard<std::mutex> lock(session_keys_mutex_);
    auto it = ip_to_session_key_.find(peer_ip);
    return (it != ip_to_session_key_.end()) ? it->second : resolve_peer_name(peer_ip);
}

int ChatServer::get_outbound_fd(const std::string& ip, uint16_t port) {
    const std::string key = endpoint_key(ip, port);
    std::lock_guard<std::mutex> lock(outbound_mutex_);
    auto it = outbound_fd_by_endpoint_.find(key);
    return (it != outbound_fd_by_endpoint_.end()) ? it->second : -1;
}

void ChatServer::handle_chat_message(const std::string& from_user, const ChatMessage& msg) {
    std::cout << "chat: recv from=" << msg.from_user()
              << " to=" << msg.to_user()
              << " msg_id=" << msg.msg_id()
              << " ts=" << msg.iso_datetime()
              << " content=\"" << msg.content() << "\"\n";

    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&, int64_t)> cb;
    {
        std::lock_guard<std::mutex> lock(receive_handler_mutex_);
        cb = on_receive_;
    }
    if (cb) {
        cb(msg.from_user(), msg.to_user(), msg.content(), msg.iso_datetime(), msg.timestamp_ms());
    }
}

bool ChatServer::start() {
    if (running_) {
        return true;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "chat: failed to create socket\n";
        return false;
    }

    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 200000;
    setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(0);    // Let OS allocate an available port.
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "chat: bind failed\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) < 0) {
        std::cerr << "chat: getsockname failed\n";
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    port_ = ntohs(bound.sin_port);

    if (listen(listen_fd_, 16) < 0) {
        std::cerr << "chat: listen failed\n";
        close(listen_fd_);
        listen_fd_ = -1;
        port_ = 0;
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&ChatServer::accept_loop, this);
    std::cout << "chat: listening on tcp port " << port_ << "\n";
    return true;
}

void ChatServer::stop() {
    running_ = false;
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        for (auto& kv : outbound_fd_by_endpoint_) {
            if (kv.second >= 0) {
                close(kv.second);
            }
        }
        outbound_fd_by_endpoint_.clear();
    }
    port_ = 0;
}

bool ChatServer::connect_to(const std::string& ip, uint16_t port, const std::string& peer_name) {
    const std::string key = endpoint_key(ip, port);
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        auto it = outbound_fd_by_endpoint_.find(key);
        if (it != outbound_fd_by_endpoint_.end() && it->second >= 0) {
            std::cout << "chat: already connected to " << peer_name << " (" << key << ")\n";
            return true;
        }
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "chat: connect socket create failed for " << peer_name << "\n";
        return false;
    }

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) {
        std::cerr << "chat: invalid peer IP '" << ip << "' for " << peer_name << "\n";
        close(fd);
        return false;
    }

    std::cout << "chat: connecting to " << peer_name << " at " << ip << ":" << port << "\n";
    if (connect(fd, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
        std::cerr << "chat: connect failed to " << peer_name << " (" << ip << ":" << port
                  << ")\n";
        close(fd);
        return false;
    }

    std::cout << "chat: connected to " << peer_name << " (" << ip << ":" << port << ")\n";
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        outbound_fd_by_endpoint_[key] = fd;
    }

    // Initiator: send handshake and spawn a thread to read the response.
    if (session_manager_) {
        session_manager_->initSession(peer_name, true);
        auto payload = session_manager_->buildHandshakePayload(peer_name);
        if (!payload.empty()) {
            Envelope env;
            env.set_type(Envelope::HANDSHAKE);
            env.mutable_handshake()->ParseFromArray(payload.data(), static_cast<int>(payload.size()));
            if (logger_) logger_("Sending handshake to " + peer_name);
            send_envelope(fd, env);
        }

        // Set a short read timeout so the thread exits cleanly if no response arrives.
        timeval tv{};
        tv.tv_sec = 5;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::thread([this, fd, peer_name]() {
            Envelope resp;
            if (recv_envelope(fd, resp) && resp.type() == Envelope::HANDSHAKE) {
                if (logger_) logger_("Received handshake response from " + peer_name);
                const auto& hs = resp.handshake();
                std::vector<uint8_t> their_ed25519(hs.ed25519_pk().begin(), hs.ed25519_pk().end());
                std::vector<uint8_t> their_x25519(hs.x25519_pk().begin(), hs.x25519_pk().end());
                std::vector<uint8_t> sig(hs.signature().begin(), hs.signature().end());
                if (!session_manager_->hasSession(peer_name)) {
                    session_manager_->initSession(peer_name, true);
                }
                session_manager_->processHandshake(peer_name, their_ed25519, their_x25519, sig);
            }
        }).detach();
    }

    // Flush any pending messages now that we're connected
    flush_pending_messages(peer_name, ip, port);

    return true;
}

bool ChatServer::send_chat(const std::string& from_user, const std::string& to_user,
                           const std::string& ip, uint16_t port, const std::string& content) {
    if (content.empty()) {
        return false;
    }

    const uint64_t seq = ++msg_counter_;
    std::string msg_id = from_user + "-" + std::to_string(unix_epoch_ms()) + "-" + std::to_string(seq);
    int64_t ts_ms = unix_epoch_ms();
    std::string ts_iso = iso_utc_now();

    // Save to DB as pending before attempting send
    if (db_) {
        db_->saveMessage(to_user, true, content, ts_iso, ts_ms, "pending", msg_id);
    }

    if (!connect_to(ip, port, to_user)) {
        // Queue for later delivery
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_messages_[to_user].push_back(
                PendingMessage{from_user, to_user, content, msg_id, ts_iso, ts_ms});
        }
        std::cerr << "chat: peer " << to_user << " offline, message queued\n";
        return true;  // Message is persisted, will be sent when peer comes online
    }

    ChatMessage msg;
    msg.set_msg_id(msg_id);
    msg.set_from_user(from_user);
    msg.set_to_user(to_user);
    msg.set_content(content);
    msg.set_timestamp_ms(ts_ms);
    msg.set_iso_datetime(ts_iso);

    Envelope env;
    env.set_type(Envelope::CHAT);
    *env.mutable_chat() = std::move(msg);

    int fd = get_outbound_fd(ip, port);
    if (fd < 0) {
        // Queue for later delivery
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_messages_[to_user].push_back(
                PendingMessage{from_user, to_user, content, msg_id, ts_iso, ts_ms});
        }
        return true;
    }

    if (!send_envelope(fd, env, session_manager_, to_user)) {
        std::cerr << "chat: send failed to " << to_user << ", queuing for retry\n";
        const std::string key = endpoint_key(ip, port);
        {
            std::lock_guard<std::mutex> lock(outbound_mutex_);
            auto it = outbound_fd_by_endpoint_.find(key);
            if (it != outbound_fd_by_endpoint_.end()) {
                close(it->second);
                outbound_fd_by_endpoint_.erase(it);
            }
        }
        // Queue for later delivery
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_messages_[to_user].push_back(
                PendingMessage{from_user, to_user, content, msg_id, ts_iso, ts_ms});
        }
        return true;
    }

    // Mark as sent in DB
    if (db_) {
        db_->updateMessageStatus(msg_id, "sent");
    }

    std::cout << "chat: sent msg_id=" << env.chat().msg_id() << " to=" << to_user
              << " at " << ip << ":" << port << " ts=" << env.chat().iso_datetime() << "\n";
    return true;
}

void ChatServer::flush_pending_messages(const std::string& peer_name,
                                         const std::string& ip, uint16_t port) {
    std::vector<PendingMessage> to_send;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto it = pending_messages_.find(peer_name);
        if (it == pending_messages_.end() || it->second.empty()) {
            return;
        }
        to_send = std::move(it->second);
        pending_messages_.erase(it);
    }

    if (!connect_to(ip, port, peer_name)) {
        // Re-queue all if still can't connect
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto& queue = pending_messages_[peer_name];
        queue.insert(queue.end(), to_send.begin(), to_send.end());
        return;
    }

    int fd = get_outbound_fd(ip, port);
    if (fd < 0) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        auto& queue = pending_messages_[peer_name];
        queue.insert(queue.end(), to_send.begin(), to_send.end());
        return;
    }

    for (auto& pm : to_send) {
        ChatMessage msg;
        msg.set_msg_id(pm.msg_id);
        msg.set_from_user(pm.from_user);
        msg.set_to_user(pm.to_user);
        msg.set_content(pm.content);
        msg.set_timestamp_ms(pm.timestamp_ms);
        msg.set_iso_datetime(pm.timestamp);

        Envelope env;
        env.set_type(Envelope::CHAT);
        *env.mutable_chat() = std::move(msg);

        if (send_envelope(fd, env, session_manager_, peer_name)) {
            if (db_) {
                db_->updateMessageStatus(pm.msg_id, "sent");
            }
            std::cout << "chat: flushed queued msg_id=" << pm.msg_id << " to=" << peer_name << "\n";
        } else {
            // Re-queue this one and any remaining
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto& queue = pending_messages_[peer_name];
            queue.push_back(pm);
            // Also re-queue remaining
            for (auto it = to_send.begin() + (&pm - to_send.data() + 1); it != to_send.end(); ++it) {
                queue.push_back(*it);
            }
            break;
        }
    }
}

void ChatServer::register_peer(const std::string& peer_name, const std::string& ip) {
    if (peer_name.empty() || ip.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(peers_mutex_);
    name_by_ip_[ip] = peer_name;
}

void ChatServer::disconnect_peer(const std::string& peer_name) {
    if (peer_name.empty()) {
        return;
    }

    // Find IP for peer name
    std::string peer_ip;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& kv : name_by_ip_) {
            if (kv.second == peer_name) {
                peer_ip = kv.first;
                break;
            }
        }
    }

    if (peer_ip.empty()) {
        return;
    }

    // Close connections to this IP
    {
        std::lock_guard<std::mutex> lock(outbound_mutex_);
        for (auto it = outbound_fd_by_endpoint_.begin(); it != outbound_fd_by_endpoint_.end(); ) {
            if (it->first.find(peer_ip) == 0) {
                std::cout << "chat: closing connection to " << peer_name << " (" << it->first << ")\n";
                if (it->second >= 0) {
                    close(it->second);
                }
                it = outbound_fd_by_endpoint_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Clear session key mapping so the next handshake re-establishes it.
    {
        std::lock_guard<std::mutex> lock(session_keys_mutex_);
        ip_to_session_key_.erase(peer_ip);
    }
}

void ChatServer::set_receive_handler(
    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&, int64_t)>
        handler) {
    std::lock_guard<std::mutex> lock(receive_handler_mutex_);
    on_receive_ = std::move(handler);
}

void ChatServer::accept_loop() {
    while (running_) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            if (running_) {
                std::cerr << "chat: accept failed\n";
            }
            continue;
        }

        char ipbuf[INET_ADDRSTRLEN];
        const char* peer_ip = inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
        const std::string ip = (peer_ip ? peer_ip : "unknown");
        std::string peer_name = "unknown";
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            auto it = name_by_ip_.find(ip);
            if (it != name_by_ip_.end()) {
                peer_name = it->second;
            }
        }
        const uint16_t peer_port = ntohs(peer.sin_port);
        std::cout << "chat: accepted connection from " << ip << ":" << peer_port
                            << " name=" << peer_name << "\n";
        std::thread(&ChatServer::handle_inbound_connection, this, fd, ip, peer_port).detach();
    }
}

void ChatServer::handle_inbound_connection(int fd, const std::string& peer_ip, uint16_t peer_port) {
    // Ensure accepted chat sockets block for reads; do not inherit short listener timeout.
    timeval no_timeout{};
    no_timeout.tv_sec = 0;
    no_timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &no_timeout, sizeof(no_timeout));

    while (running_) {
        Envelope env;
        if (!recv_envelope(fd, env)) {
            break;
        }

        const std::string from_user = resolve_peer_name(peer_ip);
        const std::string session_key = session_key_for_ip(peer_ip);

        // Decrypt if ciphertext is present
        if (!env.ciphertext().empty()) {
            if (logger_) {
                std::string hex;
                const std::string& ct = env.ciphertext();
                for (size_t i = 0; i < ct.size() && i < 64; ++i) {
                    char buf[3];
                    std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned char>(ct[i]));
                    hex += buf;
                }
                if (ct.size() > 64) hex += "...";
                logger_("Received encrypted envelope from " + from_user +
                        " ciphertext=" + std::to_string(env.ciphertext().size()) +
                        " nonce_len=" + std::to_string(env.nonce().size()) +
                        " hex=" + hex);
            }
            if (!session_manager_ || !session_manager_->isReady(session_key)) {
                std::cerr << "chat: encrypted envelope but no session for " << from_user
                          << " (session key: " << session_key << ")\n";
                continue;
            }
            std::vector<uint8_t> nonce(env.nonce().begin(), env.nonce().end());
            std::vector<uint8_t> cipher(env.ciphertext().begin(), env.ciphertext().end());
            auto plaintext = session_manager_->decrypt(session_key, cipher, nonce);
            if (!plaintext) {
                std::cerr << "chat: decryption failed for " << from_user << "\n";
                continue;
            }
            if (!env.ParseFromArray(plaintext->data(), static_cast<int>(plaintext->size()))) {
                std::cerr << "chat: failed to parse decrypted envelope from " << from_user << "\n";
                continue;
            }
        }

        switch (env.type()) {
            case Envelope::CHAT:
                handle_chat_message(from_user, env.chat());
                break;
            case Envelope::FILE_OFFER:
                handle_file_offer(from_user, peer_ip, peer_port, env.file_offer());
                break;
            case Envelope::FILE_CHUNK:
                handle_file_chunk(from_user, env.file_chunk());
                break;
            case Envelope::FILE_COMPLETE:
                handle_file_complete(from_user, env.file_complete());
                break;
            case Envelope::HANDSHAKE: {
                if (!session_manager_) break;
                if (logger_) logger_("Received handshake from " + from_user);
                const auto& hs = env.handshake();
                std::vector<uint8_t> their_ed25519(hs.ed25519_pk().begin(), hs.ed25519_pk().end());
                std::vector<uint8_t> their_x25519(hs.x25519_pk().begin(), hs.x25519_pk().end());
                std::vector<uint8_t> sig(hs.signature().begin(), hs.signature().end());

                // Always generate fresh ephemeral keys for an inbound handshake.
                // This ensures the session key is derived from the keys actually
                // used on this connection, avoiding stale/mismatched sessions.
                session_manager_->initSession(session_key, false);
                bool ok = session_manager_->processHandshake(session_key, their_ed25519, their_x25519, sig);
                if (!ok) {
                    std::cerr << "chat: handshake verification FAILED for " << from_user << "\n";
                    return;
                }
                // Lock in the session key for this IP so later messages
                // use the same key even if discovery renames the peer.
                {
                    std::lock_guard<std::mutex> lock(session_keys_mutex_);
                    ip_to_session_key_[peer_ip] = session_key;
                }
                // Responder: send our handshake back
                if (!session_manager_->weInitiated(session_key)) {
                    auto payload = session_manager_->buildHandshakePayload(session_key);
                    if (!payload.empty()) {
                        if (logger_) logger_("Sending handshake response to " + from_user);
                        Envelope resp;
                        resp.set_type(Envelope::HANDSHAKE);
                        resp.mutable_handshake()->ParseFromArray(payload.data(), static_cast<int>(payload.size()));
                        send_envelope(fd, resp);
                    }
                }
                break;
            }
            default:
                std::cerr << "chat: unknown envelope type " << env.type() << "\n";
                break;
        }
    }

    close(fd);
}

void ChatServer::handle_file_offer(const std::string& from_user, const std::string& ip, 
                                   uint16_t port, const FileOffer& offer) {
    const std::string resolved_from_user =
        offer.from_user().empty() ? from_user : offer.from_user();

    if (is_peer_trusted_ && !is_peer_trusted_(resolved_from_user)) {
        std::cerr << "chat: rejecting file offer from untrusted peer " << resolved_from_user << "\n";
        return;
    }

    std::cout << "chat: received file offer " << offer.transfer_id() 
              << " for " << offer.filename() 
              << " (" << offer.file_size() << " bytes) from " << resolved_from_user << "\n";

    auto transfer = std::make_shared<IncomingFileTransfer>();
    transfer->transfer_id = offer.transfer_id();
    transfer->filename = offer.filename();
    transfer->file_size = offer.file_size();
    transfer->sha256_hash = offer.sha256_hash();
    transfer->from_user = resolved_from_user;
    transfer->chunks.reserve(offer.file_size());
    transfer->expected_chunks = static_cast<uint32_t>((offer.file_size() + 32767) / 32768);
    transfer->received_chunks = 0;
    transfer->status = FileTransferStatus::IN_PROGRESS;
    transfer->timestamp_ms = unix_epoch_ms();
    transfer->iso_datetime = iso_utc_now();

    {
        std::lock_guard<std::mutex> lock(file_transfers_mutex_);
        incoming_transfers_[offer.transfer_id()] = transfer;
    }

    // Save to database
    if (db_) {
        db_->saveFileTransfer(offer.transfer_id(), offer.filename(), offer.file_size(),
                              offer.sha256_hash(), resolved_from_user, false, "in_progress",
                              transfer->iso_datetime, transfer->timestamp_ms);
    }

    // Notify UI only after complete file is received (in handle_file_chunk).
}

void ChatServer::handle_file_chunk(const std::string& from_user, const FileChunk& chunk) {
    std::shared_ptr<IncomingFileTransfer> transfer;
    {
        std::lock_guard<std::mutex> lock(file_transfers_mutex_);
        auto it = incoming_transfers_.find(chunk.transfer_id());
        if (it == incoming_transfers_.end()) {
            std::cerr << "chat: received chunk for unknown transfer " << chunk.transfer_id() << "\n";
            return;
        }
        transfer = it->second;
    }

    // Append chunk data
    const std::string& data = chunk.data();
    transfer->chunks.insert(transfer->chunks.end(), data.begin(), data.end());
    transfer->received_chunks++;

    // Check if complete
    if (chunk.is_last() || transfer->chunks.size() >= transfer->file_size) {
        // Verify size
        if (transfer->chunks.size() == transfer->file_size) {
            transfer->status = FileTransferStatus::COMPLETE;
            
            // Save to database
            if (db_) {
                db_->saveFileData(chunk.transfer_id(), transfer->chunks);
                db_->updateFileTransferStatus(chunk.transfer_id(), "complete");
            }

            std::cout << "chat: file " << transfer->filename << " received completely ("
                      << transfer->chunks.size() << " bytes)\n";

            // Notify UI
            if (file_received_callback_) {
                file_received_callback_(transfer->from_user, false, chunk.transfer_id(),
                                        transfer->filename, transfer->file_size);
            }
        } else {
            std::cerr << "chat: file size mismatch for " << chunk.transfer_id() 
                      << " (got " << transfer->chunks.size() << ", expected " 
                      << transfer->file_size << ")\n";
            transfer->status = FileTransferStatus::FAILED;
            if (db_) {
                db_->updateFileTransferStatus(chunk.transfer_id(), "failed");
            }
        }
    }
}

void ChatServer::handle_file_complete(const std::string& from_user, const FileComplete& complete) {
    std::lock_guard<std::mutex> lock(file_transfers_mutex_);
    auto it = incoming_transfers_.find(complete.transfer_id());
    if (it != incoming_transfers_.end()) {
        std::cout << "chat: file transfer " << complete.transfer_id() 
                  << " marked complete by sender\n";
    }
}

void ChatServer::send_file_response(const std::string& to_user, const std::string& ip, 
                                    uint16_t port, const std::string& transfer_id, bool accepted) {
    // For now, auto-accept all file transfers
    // In the future, this should send a FileResponse message
}
