#include "als-dimmer/sensors/can_als_sensor.hpp"
#include "als-dimmer/logger.hpp"
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>

namespace als_dimmer {

CANALSSensor::CANALSSensor(const std::string& can_interface,
                           uint32_t can_id,
                           uint32_t timeout_ms)
    : can_interface_(can_interface)
    , can_id_(can_id)
    , timeout_ms_(timeout_ms)
    , socket_fd_(-1)
    , last_lux_(-1.0f)
    , initialized_(false) {
}

CANALSSensor::~CANALSSensor() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
}

bool CANALSSensor::init() {
    // Create SocketCAN socket
    socket_fd_ = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (socket_fd_ < 0) {
        LOG_ERROR("CANALSSensor", "Failed to create CAN socket: " << strerror(errno));
        return false;
    }

    // Get interface index
    struct ifreq ifr;
    std::strncpy(ifr.ifr_name, can_interface_.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(socket_fd_, SIOCGIFINDEX, &ifr) < 0) {
        LOG_ERROR("CANALSSensor", "Failed to get interface index for "
                  << can_interface_ << ": " << strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Bind socket to CAN interface
    struct sockaddr_can addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(socket_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("CANALSSensor", "Failed to bind CAN socket to "
                  << can_interface_ << ": " << strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Set up CAN filter for our specific message ID
    struct can_filter filter;
    filter.can_id = can_id_;
    filter.can_mask = CAN_SFF_MASK;  // Standard frame filter

    if (setsockopt(socket_fd_, SOL_CAN_RAW, CAN_RAW_FILTER,
                   &filter, sizeof(filter)) < 0) {
        LOG_WARN("CANALSSensor", "Failed to set CAN filter, will receive all messages");
    }

    // Set socket to non-blocking mode
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        LOG_ERROR("CANALSSensor", "Failed to set non-blocking mode: " << strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    last_update_time_ = std::chrono::steady_clock::now();
    initialized_ = true;

    LOG_INFO("CANALSSensor", "Initialized on " << can_interface_
             << " (ID: 0x" << std::hex << can_id_ << std::dec
             << ", timeout: " << timeout_ms_ << "ms)");

    return true;
}

float CANALSSensor::readLux() {
    if (!initialized_) {
        LOG_ERROR("CANALSSensor", "Sensor not initialized");
        return -1.0f;
    }

    // Try to receive new CAN message
    CANMessage msg;
    if (receiveCANMessage(msg)) {
        // Validate status byte
        if (msg.status != 0x00) {
            LOG_WARN("CANALSSensor", "Sensor status error (status=0x"
                     << std::hex << static_cast<int>(msg.status) << std::dec << ")");
            return -1.0f;
        }

        // Validate checksum
        if (!validateChecksum(msg)) {
            LOG_WARN("CANALSSensor", "Invalid checksum, discarding message");
            // Don't update last_lux_, wait for next valid message
            return last_lux_.load();
        }

        // Extract lux value
        uint32_t lux = extractLux(msg);

        // Sanity check (warn if unrealistic but still use the value)
        if (lux > 200000) {
            LOG_WARN("CANALSSensor", "Unusually high lux value: " << lux);
        }

        // Update cached value and timestamp
        last_lux_.store(static_cast<float>(lux));
        last_update_time_ = std::chrono::steady_clock::now();

        LOG_TRACE("CANALSSensor", "Received lux: " << lux
                  << " (seq: " << static_cast<int>(msg.sequence) << ")");
    }

    // Check if data is stale
    if (isDataStale()) {
        LOG_WARN("CANALSSensor", "No CAN data received for " << timeout_ms_ << "ms");
        // Return -1.0 to indicate timeout if we never received valid data
        if (last_lux_.load() < 0.0f) {
            return -1.0f;
        }
        // Otherwise return last valid value
    }

    return last_lux_.load();
}

bool CANALSSensor::receiveCANMessage(CANMessage& msg) {
    struct can_frame frame;
    int nbytes = recv(socket_fd_, &frame, sizeof(frame), MSG_DONTWAIT);

    if (nbytes < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No data available (normal for non-blocking)
            return false;
        }
        LOG_ERROR("CANALSSensor", "CAN receive error: " << strerror(errno));
        return false;
    }

    if (nbytes < static_cast<int>(sizeof(frame))) {
        LOG_WARN("CANALSSensor", "Incomplete CAN frame received");
        return false;
    }

    // Check CAN ID matches (filter should handle this, but double-check)
    if (frame.can_id != can_id_) {
        LOG_DEBUG("CANALSSensor", "Ignoring message with ID 0x"
                  << std::hex << frame.can_id << std::dec);
        return false;
    }

    // Check data length
    if (frame.can_dlc != 8) {
        LOG_WARN("CANALSSensor", "Invalid CAN frame length: " << static_cast<int>(frame.can_dlc));
        return false;
    }

    // Copy data to our message structure
    std::memcpy(&msg, frame.data, sizeof(CANMessage));

    return true;
}

bool CANALSSensor::validateChecksum(const CANMessage& msg) const {
    // Checksum is simple sum of first 6 bytes
    uint16_t calculated = msg.lux_low + msg.lux_mid + msg.lux_high +
                          msg.status + msg.sequence + msg.config_idx;

    // Compare with received checksum (little-endian)
    return (calculated == msg.checksum);
}

uint32_t CANALSSensor::extractLux(const CANMessage& msg) const {
    // 3-byte little-endian lux value
    return static_cast<uint32_t>(msg.lux_low) |
           (static_cast<uint32_t>(msg.lux_mid) << 8) |
           (static_cast<uint32_t>(msg.lux_high) << 16);
}

bool CANALSSensor::isDataStale() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_update_time_).count();
    return elapsed > static_cast<long>(timeout_ms_);
}

bool CANALSSensor::isHealthy() const {
    // Sensor is healthy if initialized and data is not stale
    return initialized_ && !isDataStale();
}

} // namespace als_dimmer
