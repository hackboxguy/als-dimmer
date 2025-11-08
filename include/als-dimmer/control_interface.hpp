#ifndef ALS_DIMMER_CONTROL_INTERFACE_HPP
#define ALS_DIMMER_CONTROL_INTERFACE_HPP

#include "state_manager.hpp"
#include "config.hpp"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

namespace als_dimmer {

struct SystemStatus {
    OperatingMode mode;
    float lux;
    int target_brightness;
    int current_brightness;
    std::string zone;
    bool sensor_healthy;
    int manual_resume_in_sec;  // Time until auto-resume (for MANUAL_TEMPORARY)
    int uptime_sec;
};

enum class SocketType {
    TCP,
    UNIX
};

class ControlInterface {
public:
    ControlInterface(const ControlConfig& config);
    ~ControlInterface();

    // Start listening for connections
    bool start();

    // Stop listening and close all connections
    void stop();

    // Check if a command is available
    bool hasCommand();

    // Get next command (blocking if none available)
    std::string getNextCommand();

    // Send response to last client
    void sendResponse(const std::string& response);

    // Broadcast message to all connected clients
    void broadcast(const std::string& message);

    // Update system status (for GET_STATUS command)
    void updateStatus(const SystemStatus& status);

private:
    void acceptTcpClients();
    void acceptUnixClients();
    void handleClient(int client_fd, SocketType socket_type);
    std::string processJsonCommand(const std::string& json_command);
    bool createUnixSocket();
    bool setUnixSocketPermissions();
    void removeStaleUnixSocket();

    ControlConfig config_;

    // TCP socket
    int tcp_server_fd_;
    std::thread tcp_accept_thread_;

    // Unix socket
    int unix_server_fd_;
    std::thread unix_accept_thread_;

    std::atomic<bool> running_;

    std::vector<std::thread> client_threads_;
    std::vector<int> client_fds_;
    std::mutex clients_mutex_;

    // Command queue
    struct CommandEntry {
        std::string command;
        int client_fd;
        SocketType socket_type;
    };
    std::vector<CommandEntry> command_queue_;
    std::mutex queue_mutex_;

    // System status
    SystemStatus status_;
    std::mutex status_mutex_;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_CONTROL_INTERFACE_HPP
