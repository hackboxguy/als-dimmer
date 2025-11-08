#include "als-dimmer/control_interface.hpp"
#include "als-dimmer/json_protocol.hpp"
#include "als-dimmer/logger.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>

namespace als_dimmer {

ControlInterface::ControlInterface(const ControlConfig& config)
    : config_(config)
    , tcp_server_fd_(-1)
    , unix_server_fd_(-1)
    , running_(false) {
}

ControlInterface::~ControlInterface() {
    stop();
}

bool ControlInterface::start() {
    running_ = true;

    // Start TCP socket listener if enabled
    if (config_.tcp_socket.enabled) {
        // Create TCP socket
        tcp_server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (tcp_server_fd_ < 0) {
            LOG_ERROR("ControlInterface", "Failed to create TCP socket: " << strerror(errno));
            return false;
        }

        // Set socket options
        int opt = 1;
        if (setsockopt(tcp_server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOG_ERROR("ControlInterface", "Failed to set SO_REUSEADDR: " << strerror(errno));
            close(tcp_server_fd_);
            return false;
        }

        // Bind TCP socket
        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.tcp_socket.listen_port);

        if (inet_pton(AF_INET, config_.tcp_socket.listen_address.c_str(), &address.sin_addr) <= 0) {
            LOG_ERROR("ControlInterface", "Invalid TCP address: " << config_.tcp_socket.listen_address);
            close(tcp_server_fd_);
            return false;
        }

        if (bind(tcp_server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
            LOG_ERROR("ControlInterface", "Failed to bind TCP socket to "
                      << config_.tcp_socket.listen_address << ":"
                      << config_.tcp_socket.listen_port << ": " << strerror(errno));
            close(tcp_server_fd_);
            return false;
        }

        // Listen on TCP socket
        if (listen(tcp_server_fd_, 5) < 0) {
            LOG_ERROR("ControlInterface", "Failed to listen on TCP socket: " << strerror(errno));
            close(tcp_server_fd_);
            return false;
        }

        tcp_accept_thread_ = std::thread(&ControlInterface::acceptTcpClients, this);
        LOG_INFO("ControlInterface", "TCP socket listening on "
                 << config_.tcp_socket.listen_address << ":"
                 << config_.tcp_socket.listen_port);
    }

    // Start Unix socket listener if enabled
    if (config_.unix_socket.enabled) {
        if (!createUnixSocket()) {
            if (config_.tcp_socket.enabled && tcp_server_fd_ >= 0) {
                close(tcp_server_fd_);
            }
            return false;
        }

        unix_accept_thread_ = std::thread(&ControlInterface::acceptUnixClients, this);
        LOG_INFO("ControlInterface", "Unix socket listening on " << config_.unix_socket.path);
    }

    return true;
}

void ControlInterface::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // Close TCP server socket
    if (tcp_server_fd_ >= 0) {
        close(tcp_server_fd_);
        tcp_server_fd_ = -1;
    }

    // Close Unix server socket
    if (unix_server_fd_ >= 0) {
        close(unix_server_fd_);
        unix_server_fd_ = -1;
    }

    // Remove Unix socket file
    if (config_.unix_socket.enabled && !config_.unix_socket.path.empty()) {
        unlink(config_.unix_socket.path.c_str());
    }

    // Wait for accept threads
    if (tcp_accept_thread_.joinable()) {
        tcp_accept_thread_.join();
    }
    if (unix_accept_thread_.joinable()) {
        unix_accept_thread_.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : client_fds_) {
            if (fd >= 0) {
                close(fd);
            }
        }
        client_fds_.clear();
    }

    // Wait for client threads
    for (auto& thread : client_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    client_threads_.clear();

    LOG_DEBUG("ControlInterface", "Stopped");
}

bool ControlInterface::createUnixSocket() {
    // Remove stale socket if it exists
    removeStaleUnixSocket();

    // Create Unix socket
    unix_server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (unix_server_fd_ < 0) {
        LOG_ERROR("ControlInterface", "Failed to create Unix socket: " << strerror(errno));
        return false;
    }

    // Bind Unix socket
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config_.unix_socket.path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(unix_server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("ControlInterface", "Failed to bind Unix socket to "
                  << config_.unix_socket.path << ": " << strerror(errno));
        close(unix_server_fd_);
        unix_server_fd_ = -1;
        return false;
    }

    // Set permissions
    if (!setUnixSocketPermissions()) {
        close(unix_server_fd_);
        unix_server_fd_ = -1;
        unlink(config_.unix_socket.path.c_str());
        return false;
    }

    // Listen on Unix socket
    if (listen(unix_server_fd_, 5) < 0) {
        LOG_ERROR("ControlInterface", "Failed to listen on Unix socket: " << strerror(errno));
        close(unix_server_fd_);
        unix_server_fd_ = -1;
        unlink(config_.unix_socket.path.c_str());
        return false;
    }

    return true;
}

bool ControlInterface::setUnixSocketPermissions() {
    // Set file permissions (chmod)
    mode_t mode = 0;
    const std::string& perm = config_.unix_socket.permissions;

    // Parse octal string to mode_t
    for (size_t i = 0; i < perm.length(); i++) {
        mode = mode * 8 + (perm[i] - '0');
    }

    if (chmod(config_.unix_socket.path.c_str(), mode) < 0) {
        LOG_ERROR("ControlInterface", "Failed to set permissions on Unix socket: " << strerror(errno));
        return false;
    }

    // Set ownership (chown)
    uid_t uid = -1;
    gid_t gid = -1;

    // Get UID from owner name
    if (!config_.unix_socket.owner.empty()) {
        struct passwd* pwd = getpwnam(config_.unix_socket.owner.c_str());
        if (pwd == nullptr) {
            LOG_ERROR("ControlInterface", "Failed to get UID for user: " << config_.unix_socket.owner);
            return false;
        }
        uid = pwd->pw_uid;
    }

    // Get GID from group name
    if (!config_.unix_socket.group.empty()) {
        struct group* grp = getgrnam(config_.unix_socket.group.c_str());
        if (grp == nullptr) {
            LOG_ERROR("ControlInterface", "Failed to get GID for group: " << config_.unix_socket.group);
            return false;
        }
        gid = grp->gr_gid;
    }

    if (chown(config_.unix_socket.path.c_str(), uid, gid) < 0) {
        LOG_ERROR("ControlInterface", "Failed to set ownership on Unix socket: " << strerror(errno));
        return false;
    }

    LOG_DEBUG("ControlInterface", "Unix socket permissions set: "
              << config_.unix_socket.permissions << " "
              << config_.unix_socket.owner << ":"
              << config_.unix_socket.group);

    return true;
}

void ControlInterface::removeStaleUnixSocket() {
    // Check if socket file exists
    struct stat st;
    if (stat(config_.unix_socket.path.c_str(), &st) == 0) {
        // File exists, check if it's a socket
        if (S_ISSOCK(st.st_mode)) {
            // Try to connect to see if it's active
            int test_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (test_fd >= 0) {
                struct sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                strncpy(addr.sun_path, config_.unix_socket.path.c_str(), sizeof(addr.sun_path) - 1);

                if (connect(test_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                    // Connection failed, socket is stale
                    LOG_WARN("ControlInterface", "Removing stale Unix socket: " << config_.unix_socket.path);
                    unlink(config_.unix_socket.path.c_str());
                } else {
                    // Connection succeeded, another instance is running
                    close(test_fd);
                    LOG_ERROR("ControlInterface", "Unix socket already in use: " << config_.unix_socket.path);
                }
                close(test_fd);
            }
        } else {
            // Not a socket, remove it
            LOG_WARN("ControlInterface", "Removing non-socket file: " << config_.unix_socket.path);
            unlink(config_.unix_socket.path.c_str());
        }
    }
}

void ControlInterface::acceptTcpClients() {
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(tcp_server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (running_) {
                LOG_ERROR("ControlInterface", "TCP accept failed: " << strerror(errno));
            }
            continue;
        }

        // Get client IP
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        LOG_DEBUG("ControlInterface", "TCP client connected from " << client_ip);

        // Store client FD
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
        }

        // Create thread to handle client
        client_threads_.push_back(std::thread(&ControlInterface::handleClient, this, client_fd, SocketType::TCP));
    }
}

void ControlInterface::acceptUnixClients() {
    while (running_) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(unix_server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (running_) {
                LOG_ERROR("ControlInterface", "Unix accept failed: " << strerror(errno));
            }
            continue;
        }

        LOG_DEBUG("ControlInterface", "Unix socket client connected");

        // Store client FD
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
        }

        // Create thread to handle client
        client_threads_.push_back(std::thread(&ControlInterface::handleClient, this, client_fd, SocketType::UNIX));
    }
}

void ControlInterface::handleClient(int client_fd, SocketType socket_type) {
    const char* socket_type_str = (socket_type == SocketType::TCP) ? "TCP" : "Unix";
    char buffer[4096];

    while (running_) {
        // Read data
        ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (n <= 0) {
            break;  // Connection closed or error
        }

        buffer[n] = '\0';

        // Process JSON commands line by line (each line should be a complete JSON object)
        std::string data(buffer);
        std::istringstream iss(data);
        std::string line;

        while (std::getline(iss, line)) {
            // Remove trailing newline/carriage return
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }

            if (line.empty()) {
                continue;
            }

            LOG_DEBUG("ControlInterface", socket_type_str << " command: " << line);

            // Queue command with client FD
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                CommandEntry entry;
                entry.command = line;
                entry.client_fd = client_fd;
                entry.socket_type = socket_type;
                command_queue_.push_back(entry);
            }
        }
    }

    LOG_DEBUG("ControlInterface", socket_type_str << " client disconnected");
    close(client_fd);

    // Remove from client_fds_
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_fds_.erase(std::remove(client_fds_.begin(), client_fds_.end(), client_fd), client_fds_.end());
    }
}

bool ControlInterface::hasCommand() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return !command_queue_.empty();
}

std::string ControlInterface::getNextCommand() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (command_queue_.empty()) {
        return "";
    }

    std::string cmd = command_queue_.front().command;
    command_queue_.erase(command_queue_.begin());
    return cmd;
}

void ControlInterface::sendResponse(const std::string& response) {
    std::string msg = response + "\n";

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (int fd : client_fds_) {
        if (fd >= 0) {
            ssize_t sent = send(fd, msg.c_str(), msg.length(), MSG_NOSIGNAL);
            if (sent < 0) {
                LOG_ERROR("ControlInterface", "Failed to send to client: " << strerror(errno));
            }
        }
    }
}

void ControlInterface::broadcast(const std::string& message) {
    sendResponse(message);
}

void ControlInterface::updateStatus(const SystemStatus& status) {
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = status;
}

std::string ControlInterface::processJsonCommand(const std::string& json_command) {
    try {
        // Parse the JSON command
        protocol::ParsedCommand cmd = protocol::parseCommand(json_command);

        // Check protocol version compatibility
        if (!cmd.version.empty() && cmd.version != PROTOCOL_VERSION) {
            LOG_WARN("ControlInterface", "Protocol version mismatch: client="
                     << cmd.version << " server=" << PROTOCOL_VERSION);
        }

        // Process command based on type
        switch (cmd.type) {
            case protocol::CommandType::GET_STATUS: {
                std::lock_guard<std::mutex> lock(status_mutex_);
                std::string mode_str = (status_.mode == OperatingMode::AUTO) ? "auto" : "manual";
                return protocol::generateStatusResponse(
                    status_.mode == OperatingMode::AUTO,
                    status_.current_brightness,
                    status_.lux,
                    status_.zone
                );
            }

            case protocol::CommandType::SET_MODE:
            case protocol::CommandType::SET_BRIGHTNESS:
            case protocol::CommandType::ADJUST_BRIGHTNESS:
            case protocol::CommandType::GET_CONFIG:
                // These will be handled in main.cpp
                return protocol::generateResponse(
                    protocol::ResponseStatus::SUCCESS,
                    "Command queued for processing",
                    nlohmann::json::object()
                );

            case protocol::CommandType::UNKNOWN:
            default:
                return protocol::generateErrorResponse(
                    "Unknown command type",
                    "UNKNOWN_COMMAND"
                );
        }

    } catch (const nlohmann::json::parse_error& e) {
        LOG_ERROR("ControlInterface", "JSON parse error: " << e.what());
        return protocol::generateErrorResponse(
            std::string("Invalid JSON: ") + e.what(),
            "PARSE_ERROR"
        );
    } catch (const std::exception& e) {
        LOG_ERROR("ControlInterface", "Command processing error: " << e.what());
        return protocol::generateErrorResponse(
            std::string("Internal error: ") + e.what(),
            "INTERNAL_ERROR"
        );
    }
}

} // namespace als_dimmer
