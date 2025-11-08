#ifndef ALS_DIMMER_CONTROL_INTERFACE_HPP
#define ALS_DIMMER_CONTROL_INTERFACE_HPP

#include "state_manager.hpp"
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

class ControlInterface {
public:
    ControlInterface(const std::string& listen_address, int listen_port);
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
    void acceptClients();
    void handleClient(int client_fd);
    std::string processCommand(const std::string& command);

    std::string listen_address_;
    int listen_port_;
    int server_fd_;
    std::atomic<bool> running_;

    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    std::vector<int> client_fds_;
    std::mutex clients_mutex_;

    // Command queue
    struct CommandEntry {
        std::string command;
        int client_fd;
    };
    std::vector<CommandEntry> command_queue_;
    std::mutex queue_mutex_;

    // System status
    SystemStatus status_;
    std::mutex status_mutex_;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_CONTROL_INTERFACE_HPP
