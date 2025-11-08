#include "als-dimmer/control_interface.hpp"
#include "als-dimmer/logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <sstream>

namespace als_dimmer {

ControlInterface::ControlInterface(const std::string& listen_address, int listen_port)
    : listen_address_(listen_address)
    , listen_port_(listen_port)
    , server_fd_(-1)
    , running_(false) {
}

ControlInterface::~ControlInterface() {
    stop();
}

bool ControlInterface::start() {
    // Create socket
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR("ControlInterface", "Failed to create socket: " << strerror(errno));
        return false;
    }

    // Set socket options
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_ERROR("ControlInterface", "Failed to set SO_REUSEADDR: " << strerror(errno));
        close(server_fd_);
        return false;
    }

    // Bind socket
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons(listen_port_);

    if (inet_pton(AF_INET, listen_address_.c_str(), &address.sin_addr) <= 0) {
        LOG_ERROR("ControlInterface", "Invalid address: " << listen_address_);
        close(server_fd_);
        return false;
    }

    if (bind(server_fd_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        LOG_ERROR("ControlInterface", "Failed to bind to " << listen_address_
                  << ":" << listen_port_ << ": " << strerror(errno));
        close(server_fd_);
        return false;
    }

    // Listen
    if (listen(server_fd_, 5) < 0) {
        LOG_ERROR("ControlInterface", "Failed to listen: " << strerror(errno));
        close(server_fd_);
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&ControlInterface::acceptClients, this);

    LOG_DEBUG("ControlInterface", "Listening on " << listen_address_ << ":" << listen_port_);
    return true;
}

void ControlInterface::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // Close server socket to unblock accept()
    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }

    // Wait for accept thread
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Close all client connections and wait for threads
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : client_fds_) {
            if (fd >= 0) {
                close(fd);
            }
        }
        client_fds_.clear();
    }

    for (auto& thread : client_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    client_threads_.clear();

    LOG_DEBUG("ControlInterface", "Stopped");
}

void ControlInterface::acceptClients() {
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            if (running_) {
                LOG_ERROR("ControlInterface", "Accept failed: " << strerror(errno));
            }
            continue;
        }

        // Get client IP
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        LOG_DEBUG("ControlInterface", "Client connected from " << client_ip);

        // Store client FD
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
        }

        // Create thread to handle client
        client_threads_.push_back(std::thread(&ControlInterface::handleClient, this, client_fd));
    }
}

void ControlInterface::handleClient(int client_fd) {
    char buffer[1024];

    while (running_) {
        // Read data
        ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

        if (n <= 0) {
            break;  // Connection closed or error
        }

        buffer[n] = '\0';

        // Process commands line by line
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

            LOG_DEBUG("ControlInterface", "Received command: " << line);

            // Queue command with client FD
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                CommandEntry entry;
                entry.command = line;
                entry.client_fd = client_fd;
                command_queue_.push_back(entry);
            }
        }
    }

    LOG_DEBUG("ControlInterface", "Client disconnected");
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

std::string ControlInterface::processCommand(const std::string& command) {
    (void)command;  // Unused - commands processed in main.cpp
    return "";
}

} // namespace als_dimmer
