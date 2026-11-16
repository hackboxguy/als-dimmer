#ifndef ALS_DIMMER_PRIVACY_MODE_HPP
#define ALS_DIMMER_PRIVACY_MODE_HPP

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include "als-dimmer/config.hpp"
#include "json.hpp"

namespace als_dimmer {

/*
 * Privacy film (FOG) on/off in the display FPGA (pixelpipe-fpga): bit 0 of
 * page-0 register 0x34, R/W, 1 = on. Both FPGA slaves are ports onto one
 * register store:
 *   0x1E  new slave:    write [0x00 page, 0x34, v]; read [0x00, 0x34] Sr R1
 *   0x1D  legacy slave: write [0,0,0,0x34, v];       read [0,0,0,0x34] Sr R1
 * 0x1E is used whenever it answers; 0x1D only when it does not. Every 0x1E
 * access names page 0 (page-1 0x34 is ALS_STATUS, and other clients move the
 * page), and every read is one repeated-start transfer.
 *
 * The register exists on every image, but only some drive the film, so the
 * switch is offered ("available") only when the FPGA answers on 0x1E, its
 * chip ID (page-0 0x10) and image version (0x00 byte 3) match a row of
 * cfg.supported, and it runs the OTA slot (0x1A = 0x5A), not GOLDEN. A
 * 0x1D-only FPGA (vendor or pre-0x1E image) is detected and reported, but
 * never offered.
 *
 * The FPGA powers up OFF and forgets the setting at every restart (power
 * cycle, OTA activate, JTAG). The daemon owns it: it writes the saved choice
 * when it finds a supported image and re-checks every poll_interval_sec,
 * writing it again whenever 0x34 holds something else, whoever changed it.
 * Writes are read back (bit 0) and retried: a Pi4<->FPGA transfer can rarely
 * corrupt a byte.
 *
 * Main-loop thread only (commands are processed there too): no locking.
 */
class PrivacyMode {
public:
    explicit PrivacyMode(const PrivacyModeConfig& cfg);
    ~PrivacyMode();

    PrivacyMode(const PrivacyMode&) = delete;
    PrivacyMode& operator=(const PrivacyMode&) = delete;

    // The choice to hold (from the state file). Written at the next poll.
    void setDesired(bool on) { desired_ = on; }
    bool desired() const { return desired_; }

    // Call every main-loop iteration; does I2C only when a check is due.
    void poll();

    // Set by a client: write now and read back. false = not available or
    // the write did not take (the next poll retries).
    bool apply(bool on);

    bool available() const { return available_; }

    // {"available", "enabled", "fpga_on", "reachable", "slave", "chip_id",
    //  "version", "slot", "reason"}
    nlohmann::json status() const;

private:
    enum class Slave { None, New, Legacy };

    bool readNew(uint8_t reg, uint8_t* buf, uint16_t len);
    bool readLegacy(uint8_t reg, uint8_t* buf, uint16_t len);
    bool writeReg(uint8_t value);
    bool readPrivacy(uint8_t& raw);
    bool ensureOpen();
    void closeFd();
    // Reads version / chip ID / bitstream type on 0x1E and decides available_.
    void identify();
    bool writeVerified(bool on);

    PrivacyModeConfig cfg_;
    int fd_ = -1;
    bool desired_ = false;
    Slave slave_ = Slave::None;
    bool available_ = false;
    bool reachable_ = false;
    int fpga_on_ = -1;             // bit 0 of the last read, -1 = none yet
    std::array<uint8_t, 9> ident_{};   // version[4] chip_id[4] type[1] of the image last identified
    bool identified_ = false;
    std::string reason_ = "not probed yet";
    int probe_count_ = 0;
    std::chrono::steady_clock::time_point next_check_;
};

}  // namespace als_dimmer

#endif  // ALS_DIMMER_PRIVACY_MODE_HPP
