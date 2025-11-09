#ifndef ALS_DIMMER_CAN_ALS_SENSOR_HPP
#define ALS_DIMMER_CAN_ALS_SENSOR_HPP

#include "als-dimmer/interfaces.hpp"
#include <string>
#include <atomic>
#include <chrono>

namespace als_dimmer {

/**
 * @brief CAN bus ALS (Ambient Light Sensor) implementation
 *
 * Receives lux data from CAN bus messages (ID 0x0A2 by default).
 * Protocol: 8-byte message with 3-byte little-endian lux value,
 * status byte, sequence counter, config index, and 16-bit checksum.
 *
 * Compatible with CANable USB-to-CAN dongles and Linux SocketCAN.
 */
class CANALSSensor : public SensorInterface {
public:
    CANALSSensor(const std::string& can_interface,
                 uint32_t can_id,
                 uint32_t timeout_ms);
    ~CANALSSensor() override;

    bool init() override;
    float readLux() override;
    bool isHealthy() const override;
    std::string getType() const override { return "can_als"; }

private:
    // Configuration
    std::string can_interface_;  // e.g., "can0"
    uint32_t can_id_;            // CAN message ID (e.g., 0x0A2)
    uint32_t timeout_ms_;        // Timeout for stale data detection

    // Runtime state
    int socket_fd_;              // SocketCAN file descriptor
    std::atomic<float> last_lux_;  // Last valid lux reading
    std::chrono::steady_clock::time_point last_update_time_;
    bool initialized_;

    // CAN message structure (matches ESP32 protocol)
    struct __attribute__((packed)) CANMessage {
        uint8_t lux_low;         // Byte 0: Lux bits 0-7
        uint8_t lux_mid;         // Byte 1: Lux bits 8-15
        uint8_t lux_high;        // Byte 2: Lux bits 16-23
        uint8_t status;          // Byte 3: 0x00=OK, 0x01=Error
        uint8_t sequence;        // Byte 4: Sequence counter
        uint8_t config_idx;      // Byte 5: Config index
        uint16_t checksum;       // Bytes 6-7: Checksum (little-endian)
    };

    // Helper methods
    bool validateChecksum(const CANMessage& msg) const;
    uint32_t extractLux(const CANMessage& msg) const;
    bool receiveCANMessage(CANMessage& msg);
    bool isDataStale() const;
};

} // namespace als_dimmer

#endif // ALS_DIMMER_CAN_ALS_SENSOR_HPP
