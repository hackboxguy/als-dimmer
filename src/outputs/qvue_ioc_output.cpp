#include "als-dimmer/interfaces.hpp"
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <fstream>
#include <memory>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

namespace als_dimmer {

/**
 * Backlight dimming of a 3x QVue display board running rh850-baremetal
 * display_manager (QVUE_9090, v00.23+ / every release v01.00+), through its
 * IOC slave at 0x66 behind the DS90UH983/988 pass-through. The stock firmware
 * (and qvue_legacy) speak the vendor protocol at 0x32 instead: output type
 * "qvue". Which one a unit runs is decided by als-dimmer-select-config.sh.
 *
 * Registers (rh850-baremetal docs/i2c_register_map.md, "FPGA brightness
 * block"), all big-endian, one block per FPGA output:
 *   0x0240 + 4i  OUT_REQ[i] hi   RW, staged
 *   0x0241 + 4i  OUT_REQ[i] lo   RW, writing it commits the request (clamped to MAX)
 *   0x0214       BRIGHT_MAX      RO, 0x07FF
 * Output i drives display i+1. 0x0210 (BRIGHT_REQ, what ioc_tcon writes) is
 * OUT_REQ[0] only, which is why this is a separate output type.
 *
 * Scale: the FPGA's 12-bit level 1:1, MAX 0x07FF. It is half the stock host
 * scale (the firmware's stock-equivalent mapping), so value_range [20, 2047]
 * with response_exponent 2.2 gives the same percent -> nits curve as the
 * "qvue" config's [40, 4095]: the backlight lights from about 12-16 counts
 * (rig, i1 Display Pro, 2026-11-04: 12 -> 1.8, 16 -> 14, 20 -> 35, 30 -> 83,
 * 40 -> 115, 0x100 -> 868 nits full white). off_at_zero sends 0 at 0 %.
 *
 * The 983/988 pass-through drops the next write to 0x66 after a read whose
 * data contains 0x67 (not as its first byte). Every write here is therefore
 * preceded by a plain 1-byte read (one byte cannot carry a 0x67 after its
 * first byte), the same guard disptool uses, and each request is read back
 * (hi <= 0x07, so the 2-byte read-back cannot be corrupted) and rewritten up
 * to kWriteTries times.
 *
 * Writes only on change; every kRecheckEvery calls output 0's request is read
 * and the cache dropped if it differs, so a firmware restart (boot level
 * 0x1C2, a warm OTA) is put right within seconds.
 *
 * LD / PC persistence (owner, 2026-11-05: the head unit owns it). The QVue
 * FPGA (UPDATE 0xD9+) has run-time local dimming / pixel compensation
 * switches at page 0 0x2C / 0x2D (bit 0: 0 = on, 1 = off), reached through
 * the MCU's PPSPI bridge (page 0x05 / window 0x0600). It powers up with both
 * on and loses the choice at every reconfiguration (power cycle, FPGA update
 * end, GOLDEN fallback). The choice lives in fpga-ldpc-state.json (written
 * by the Stream Deck keys and the Qt apps; /data/cluster where it exists,
 * else /tmp). Every kLdpcPollEvery calls FPGA_LINK + its DONE counter
 * (0x0118..0x0119) are read; at start-up, when the link comes up, when the
 * counter moves, and every kLdpcFullEvery calls the FPGA's 0x2C..0x2D are
 * read and whatever differs from the file is written and read back a frame
 * later. Never: on an image before 0xD9 (no switch), while a flash script
 * holds /tmp/target-flash.lock, or while another writer holds
 * /tmp/fpga-ldpc.lock (the deck keys' lock: taken non-blocking, never
 * created here, since a root-owned file in /tmp would lock the pi-run keys
 * out under protected_regular). A bridge answer of 0xF2 (an FPGA flash
 * session is open) is not an error: the next poll tries again. The bridge
 * command counter must move by exactly one for our command, so a command of
 * another process is never taken for ours.
 */
class QvueIocOutput : public OutputInterface {
public:
    QvueIocOutput(const std::string& device, uint8_t address, const std::vector<int>& displays,
                  int range_lo, int range_hi, double response_exponent, bool off_at_zero)
        : device_(device), address_(address), displays_(displays),
          range_lo_(range_lo), range_hi_(range_hi), exponent_(response_exponent),
          off_at_zero_(off_at_zero), i2c_fd_(-1), cached_percent_(-1), cached_raw_(-1),
          write_count_(0), fail_count_(0), calls_(0) {}

    /** Called from setBrightness() on every update cycle (500 ms). */
    void maintainLdpc() {
        ++ldpc_calls_;
        if (!ldpc_pending_ && (ldpc_calls_ % kLdpcPollEvery) != 0) {
            return;
        }
        uint8_t lk[2];
        guard();
        // FPGA_LINK is 0x00 / 0x01 / 0x02 / 0xFF, never 0x67, so the DONE
        // counter after it cannot be clipped by the pass-through.
        if (!readBlock(kRegFpgaLink, lk, 2)) {
            return;
        }
        const int link = lk[0];
        const int done = lk[1];
        if (link != 0x01) {
            if (ldpc_last_link_ == 0x01) {
                std::cout << "[QVUE_IOC] LD/PC: FPGA link 0x" << std::hex << link << std::dec
                          << " (reconfiguring?); will re-apply when it is back" << std::endl;
            }
            ldpc_last_link_ = link;
            return;
        }
        std::string reason;
        if (ldpc_last_link_ < 0) {
            reason = "start-up";
        } else if (ldpc_last_link_ != 0x01) {
            reason = "FPGA link back";
        } else if (done != ldpc_last_done_) {
            reason = "FPGA reconfigured";
        } else if (ldpc_pending_) {
            reason = ldpc_pending_reason_;
        } else if ((ldpc_calls_ % kLdpcFullEvery) == 0) {
            reason = "periodic check";
        } else {
            return;
        }
        ldpc_last_link_ = link;
        ldpc_last_done_ = done;
        ldpc_pending_ = !checkLdpc(reason);
        ldpc_pending_reason_ = reason;
    }


    ~QvueIocOutput() override {
        if (i2c_fd_ >= 0) {
            close(i2c_fd_);
        }
    }

    bool init() override {
        std::cout << "[QVUE_IOC] Initializing on " << device_
                  << " at address 0x" << std::hex << (int)address_ << std::dec
                  << " (display_manager OUT_REQ 0x0240+4i), displays";
        for (int d : displays_) {
            std::cout << " " << d;
        }
        std::cout << ", raw range [" << range_lo_ << ", " << range_hi_
                  << "], response exponent " << exponent_
                  << (off_at_zero_ ? ", 0 % = off" : "") << std::endl;

        i2c_fd_ = open(device_.c_str(), O_RDWR);
        if (i2c_fd_ < 0) {
            std::cerr << "[QVUE_IOC] Failed to open I2C device: " << strerror(errno) << "\n";
            return false;
        }

        // Not fatal: the link may come up after the daemon (the hh983 driver
        // retries its probe). The first setBrightness() writes regardless.
        guard();
        const int fw_max = readU16(kRegBrightMax);
        if (fw_max < 0) {
            std::cerr << "[QVUE_IOC] BRIGHT_MAX (0x0214) not readable yet; writing anyway\n";
        } else {
            std::cout << "[QVUE_IOC] firmware MAX " << fw_max << std::endl;
            if (fw_max != 0xFFFF && range_hi_ > fw_max) {
                std::cerr << "[QVUE_IOC] config value_range max " << range_hi_
                          << " is above the firmware's MAX " << fw_max << "; it will clamp\n";
            }
        }
        return true;
    }

    bool setBrightness(int brightness) override {
        if (i2c_fd_ < 0) {
            std::cerr << "[QVUE_IOC] Output not initialized\n";
            return false;
        }
        maintainLdpc();
        int pct = brightness;
        if (pct < 0)   { pct = 0; }
        if (pct > 100) { pct = 100; }

        if (pct == cached_percent_ && (++calls_ % kRecheckEvery) == 0) {
            guard();
            const int req0 = readU16(outReg(displays_.front() - 1));
            if (req0 >= 0 && req0 != cached_raw_) {
                std::cout << "[QVUE_IOC] output " << displays_.front() - 1 << " reads " << req0
                          << ", expected " << cached_raw_ << " (firmware restarted?); rewriting\n";
                cached_percent_ = -1;
            }
        }
        if (pct == cached_percent_) {
            return true;
        }

        const int raw = percentToRaw(pct);
        for (int disp : displays_) {
            if (!writeOutput(disp - 1, raw)) {
                if (fail_count_ < 5 || (fail_count_ % 120) == 0) {
                    std::cerr << "[QVUE_IOC] Failed to set brightness " << pct << "% (raw " << raw
                              << ") on display " << disp << ": " << strerror(errno) << "\n";
                }
                fail_count_++;
                return false;   // cache untouched: the next cycle retries
            }
        }
        if (fail_count_ > 0) {
            std::cout << "[QVUE_IOC] writes succeed again after " << fail_count_ << " failures" << std::endl;
            fail_count_ = 0;
        }
        cached_percent_ = pct;
        cached_raw_ = raw;
        write_count_++;
        if (write_count_ <= 10) {
            std::cout << "[QVUE_IOC] " << pct << "% -> raw " << raw << std::endl;
        }
        return true;
    }

    int getCurrentBrightness() override {
        return cached_percent_;
    }

    std::string getType() const override {
        return "qvue_ioc";
    }

private:
    static const uint16_t kRegBrightMax = 0x0214;
    static const uint16_t kRegOutReq0   = 0x0240;
    static const int kWriteTries        = 3;
    static const unsigned kRecheckEvery = 20;   // x update interval (500 ms) = 10 s

    static uint16_t outReg(int out) { return static_cast<uint16_t>(kRegOutReq0 + 4 * out); }

    // ---- LD / PC persistence ---------------------------------------------
    static const uint16_t kRegIfCaps    = 0x0101;   // bit 5: PPSPI bridge
    static const uint16_t kRegFpgaLink  = 0x0118;   // + 0x0119 DONE counter
    static const uint16_t kRegFpgaImage = 0x0127;   // IDENTIFY byte 7
    static const uint16_t kRegFbOp      = 0x0500;   // OP PAGE ADDR LEN CMD
    static const uint16_t kRegFbStatus  = 0x0505;   // STATUS ST ID BRS COUNT
    static const uint16_t kRegFbData    = 0x0600;
    static const uint8_t  kFbRead = 0x0B, kFbWrite = 0x02;
    static const uint8_t  kFbBusy = 0x01, kFbDone = 0x02, kFbSession = 0xF2;
    static const uint8_t  kFpgaRegLd = 0x2C;        // page 0; 0x2D = PC
    static const int      kFirstImageWithSwitch = 0xD9;
    static const unsigned kLdpcPollEvery = 4;       // x 500 ms = 2 s
    static const unsigned kLdpcFullEvery = 20;      // x 500 ms = 10 s

    static std::string ldpcStateFile() {
        struct stat st;
        if (stat("/data/cluster", &st) == 0 && S_ISDIR(st.st_mode)) {
            return "/data/cluster/fpga-ldpc-state.json";
        }
        return "/tmp/fpga-ldpc-state.json";
    }

    static double nowSec() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    /** Whether a flash script holds the shared target lock (never created here). */
    static bool flashRunning() {
        const int fd = open("/tmp/target-flash.lock", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        const bool busy = flock(fd, LOCK_EX | LOCK_NB) != 0;
        close(fd);   // releases it if we took it
        return busy;
    }

    /**
     * One pass: false = try again at the next poll (busy, or an I2C / bridge
     * failure), true = done (applied, already right, or nothing to do).
     */
    bool checkLdpc(const std::string& reason) {
        if (flashRunning()) {
            return false;
        }
        if (!ldpc_caps_checked_) {
            uint8_t caps = 0xFF;
            guard();
            if (!readBlock(kRegIfCaps, &caps, 1)) {
                return false;
            }
            ldpc_has_bridge_ = caps != 0xFF && (caps & 0x20);
            ldpc_caps_checked_ = true;
            if (!ldpc_has_bridge_) {
                std::cout << "[QVUE_IOC] LD/PC: no PPSPI bridge in this firmware (IF_CAPS 0x"
                          << std::hex << (int)caps << std::dec << "); not managing LD/PC" << std::endl;
            }
        }
        if (!ldpc_has_bridge_) {
            return true;
        }
        uint8_t image = 0;
        guard();
        if (!readBlock(kRegFpgaImage, &image, 1)) {
            return false;
        }
        if (image < kFirstImageWithSwitch) {
            if (image != ldpc_noswitch_image_) {
                std::cout << "[QVUE_IOC] LD/PC: FPGA image 0x" << std::hex << (int)image << std::dec
                          << " has no run-time LD/PC switch (0xD9+); not managing LD/PC" << std::endl;
                ldpc_noswitch_image_ = image;
            }
            return true;
        }
        ldpc_noswitch_image_ = -1;

        bool want_ld = true, want_pc = true;
        const std::string path = ldpcStateFile();
        if (!loadLdpcChoice(path, want_ld, want_pc)) {
            return true;   // no choice recorded (or unreadable, logged): nothing to enforce
        }

        // The deck keys' lock, non-blocking; absent = no key has run this boot.
        int lock_fd = open("/tmp/fpga-ldpc.lock", O_RDONLY | O_CLOEXEC);
        if (lock_fd >= 0 && flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            close(lock_fd);
            return false;
        }
        const bool ok = applyLdpc(want_ld, want_pc, reason, path);
        if (lock_fd >= 0) {
            close(lock_fd);
        }
        return ok;
    }

    bool loadLdpcChoice(const std::string& path, bool& ld, bool& pc) {
        std::ifstream in(path);
        if (!in) {
            return false;
        }
        try {
            nlohmann::json j;
            in >> j;
            ld = j.value("local_dimming", true);
            pc = j.value("pixel_compensation", true);
            ldpc_file_bad_logged_ = false;
            return true;
        } catch (const std::exception& e) {
            if (!ldpc_file_bad_logged_) {
                std::cerr << "[QVUE_IOC] LD/PC: cannot read " << path << ": " << e.what() << "\n";
                ldpc_file_bad_logged_ = true;
            }
            return false;
        }
    }

    bool applyLdpc(bool want_ld, bool want_pc, const std::string& reason, const std::string& path) {
        uint8_t cur[2];
        uint8_t status = 0;
        if (!bridgeOp(kFbRead, 0, kFpgaRegLd, 2, cur, 0, 2, status)) {
            return ldpcFail("bridge read of 0x2C..0x2D failed");
        }
        if (status == kFbSession) {
            return false;   // an FPGA flash session is open: later
        }
        if (status != kFbDone) {
            return ldpcFail("bridge read status 0x" + hex2(status));
        }
        const bool ld_on = (cur[0] & 1) == 0;
        const bool pc_on = (cur[1] & 1) == 0;
        if (ld_on == want_ld && pc_on == want_pc) {
            ldpc_fail_count_ = 0;
            return true;
        }
        const uint8_t regs[2] = { kFpgaRegLd, static_cast<uint8_t>(kFpgaRegLd + 1) };
        const bool want[2] = { want_ld, want_pc };
        const bool have[2] = { ld_on, pc_on };
        for (int i = 0; i < 2; i++) {
            if (want[i] == have[i]) {
                continue;
            }
            uint8_t v = want[i] ? 0x00 : 0x01;
            if (!bridgeOp(kFbWrite, 0, regs[i], 1, &v, 1, 0, status) || status != kFbDone) {
                if (status == kFbSession) {
                    return false;
                }
                return ldpcFail("bridge write of 0x" + hex2(regs[i]) + " failed (status 0x" +
                                hex2(status) + ")");
            }
        }
        // The switch takes effect at the next frame; a read in the same frame
        // still shows the old state. Two frames at 60 Hz.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        uint8_t rb[2];
        if (!bridgeOp(kFbRead, 0, kFpgaRegLd, 2, rb, 0, 2, status) || status != kFbDone) {
            return ldpcFail("read-back after the write failed");
        }
        if (((rb[0] & 1) == 0) != want_ld || ((rb[1] & 1) == 0) != want_pc) {
            return ldpcFail("read-back differs (0x2C=0x" + hex2(rb[0]) + " 0x2D=0x" + hex2(rb[1]) + ")");
        }
        ldpc_fail_count_ = 0;
        std::cout << "[QVUE_IOC] LD/PC: re-applied LD " << (want_ld ? "on" : "off") << ", PC "
                  << (want_pc ? "on" : "off") << " from " << path << " (" << reason
                  << "; the FPGA had LD " << (ld_on ? "on" : "off") << ", PC "
                  << (pc_on ? "on" : "off") << ")" << std::endl;
        return true;
    }

    bool ldpcFail(const std::string& what) {
        if (ldpc_fail_count_ < 5 || (ldpc_fail_count_ % 100) == 0) {
            std::cerr << "[QVUE_IOC] LD/PC: " << what << "; retrying\n";
        }
        ldpc_fail_count_++;
        return false;
    }

    static std::string hex2(unsigned v) {
        char b[4];
        snprintf(b, sizeof b, "%02X", v & 0xFF);
        return b;
    }

    /**
     * One command through the MCU's PPSPI bridge, as disptool's bridgeOp:
     * payload into the window (read back), wait while busy, write the 5-byte
     * command block, wait for FB_COUNT to move by exactly one and the status
     * to leave busy; a command FB_COUNT shows never arrived is sent again.
     * True = the MCU ran it; `status` is then FB_STATUS (0x02 = done).
     */
    bool bridgeOp(uint8_t op, uint8_t page, uint8_t addr, uint8_t len,
                  uint8_t* data, unsigned slen, unsigned rlen, uint8_t& status) {
        status = 0xFF;
        if (slen) {
            bool ok = false;
            uint8_t chk[16];
            for (int t = 0; t < 3 && !ok && slen <= sizeof(chk); t++) {
                ok = writeBlock(kRegFbData, data, slen) && readBlock(kRegFbData, chk, slen) &&
                     std::equal(data, data + slen, chk);
            }
            if (!ok) {
                return false;
            }
        }
        uint8_t blk[5];
        if (!readBlock(kRegFbStatus, blk, 5)) {
            return false;
        }
        double t0 = nowSec();
        while (blk[0] == kFbBusy) {
            if (nowSec() - t0 > 0.5) {
                return false;   // someone else's command: later
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!readBlock(kRegFbStatus, blk, 5)) {
                return false;
            }
        }
        const uint8_t count0 = blk[4];
        const uint8_t cmd[5] = { op, page, addr, len, 1 };
        for (int attempt = 0; attempt < 3; attempt++) {
            if (!writeBlock(kRegFbOp, cmd, sizeof(cmd))) {
                continue;
            }
            t0 = nowSec();
            for (;;) {
                if (readBlock(kRegFbStatus, blk, 5)) {
                    if (blk[4] != count0 && blk[0] != kFbBusy) {
                        if (blk[4] != static_cast<uint8_t>(count0 + 1)) {
                            return false;   // another command ran too: not ours to trust
                        }
                        status = blk[0];
                        return !rlen || readBlock(kRegFbData, data, rlen);
                    }
                    if (blk[0] != kFbBusy && blk[4] == count0 && nowSec() - t0 > 0.03) {
                        break;              // FB_CMD never arrived: send it again
                    }
                }
                if (nowSec() - t0 > 0.5) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return false;
    }

    /** Guarded register write: [reg hi, reg lo, data...]. */
    bool writeBlock(uint16_t reg, const uint8_t* data, size_t n) {
        uint8_t frame[2 + 16];
        if (n > 16) {
            return false;
        }
        frame[0] = static_cast<uint8_t>(reg >> 8);
        frame[1] = static_cast<uint8_t>(reg & 0xFF);
        std::copy(data, data + n, frame + 2);
        guard();
        return writeRaw(frame, 2 + n);
    }

    /**
     * Guarded atomic read with disptool's splice rule: bytes after a 0x67 in
     * the data may have lost bit 7 in the pass-through, so the tail after it
     * is read again (guarded), until no 0x67 is left before the end.
     */
    bool readBlock(uint16_t reg, uint8_t* buf, size_t n) {
        if (!readOnce(reg, buf, n)) {
            return false;
        }
        for (size_t k = 0; k + 1 < n; k++) {
            if (buf[k] != 0x67) {
                continue;
            }
            const size_t off = k + 1;
            guard();
            if (!readOnce(static_cast<uint16_t>(reg + off), buf + off, n - off)) {
                return false;
            }
        }
        return true;
    }

    bool readOnce(uint16_t reg, uint8_t* buf, size_t n) {
        uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
        struct i2c_msg msgs[2];
        msgs[0].addr = address_;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(ptr);
        msgs[0].buf = ptr;
        msgs[1].addr = address_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = static_cast<uint16_t>(n);
        msgs[1].buf = buf;
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = msgs;
        xfer.nmsgs = 2;
        return ioctl(i2c_fd_, I2C_RDWR, &xfer) >= 0;
    }

    int percentToRaw(int pct) const {
        if (pct == 0 && off_at_zero_) {
            return 0;
        }
        const double frac = std::pow(pct / 100.0, exponent_);
        return range_lo_ + static_cast<int>(std::lround(frac * (range_hi_ - range_lo_)));
    }

    bool writeOutput(int out, int raw) {
        const uint16_t reg = outReg(out);
        const uint8_t frame[4] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF),
                                   (uint8_t)((raw >> 8) & 0xFF), (uint8_t)(raw & 0xFF) };
        for (int t = 0; t < kWriteTries; t++) {
            guard();
            if (!writeRaw(frame, sizeof(frame))) {
                continue;
            }
            guard();
            if (readU16(reg) == raw) {
                return true;
            }
        }
        return false;
    }

    /** The 983/988 write guard: a plain 1-byte read (result ignored). */
    void guard() {
        uint8_t b = 0;
        struct i2c_msg msg;
        msg.addr = address_;
        msg.flags = I2C_M_RD;
        msg.len = 1;
        msg.buf = &b;
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = &msg;
        xfer.nmsgs = 1;
        (void)ioctl(i2c_fd_, I2C_RDWR, &xfer);
    }

    /** One I2C_RDWR write on an unbound device node (the hh983 driver holds 0x18). */
    bool writeRaw(const uint8_t* data, size_t len) {
        struct i2c_msg msg;
        msg.addr = address_;
        msg.flags = 0;
        msg.len = static_cast<uint16_t>(len);
        msg.buf = const_cast<uint8_t*>(data);
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = &msg;
        xfer.nmsgs = 1;
        return ioctl(i2c_fd_, I2C_RDWR, &xfer) >= 0;
    }

    int readU16(uint16_t reg) {
        uint8_t ptr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
        uint8_t b[2] = { 0, 0 };
        struct i2c_msg msgs[2];
        msgs[0].addr = address_;
        msgs[0].flags = 0;
        msgs[0].len = sizeof(ptr);
        msgs[0].buf = ptr;
        msgs[1].addr = address_;
        msgs[1].flags = I2C_M_RD;
        msgs[1].len = 2;
        msgs[1].buf = b;
        struct i2c_rdwr_ioctl_data xfer;
        xfer.msgs = msgs;
        xfer.nmsgs = 2;
        if (ioctl(i2c_fd_, I2C_RDWR, &xfer) < 0) {
            return -1;
        }
        return (b[0] << 8) | b[1];
    }

    std::string device_;
    uint8_t address_;
    std::vector<int> displays_;
    int range_lo_;
    int range_hi_;
    double exponent_;
    bool off_at_zero_;
    int i2c_fd_;
    int cached_percent_;   // -1 = unknown, so the next update always writes
    int cached_raw_;
    unsigned write_count_;
    unsigned fail_count_;
    unsigned calls_;

    // LD / PC persistence
    unsigned ldpc_calls_ = 0;
    bool ldpc_pending_ = true;          // start-up: check at the first cycle
    std::string ldpc_pending_reason_ = "start-up";
    int ldpc_last_link_ = -1;           // -1 = not read yet
    int ldpc_last_done_ = -1;
    bool ldpc_caps_checked_ = false;
    bool ldpc_has_bridge_ = false;
    int ldpc_noswitch_image_ = -1;
    bool ldpc_file_bad_logged_ = false;
    unsigned ldpc_fail_count_ = 0;
};

std::unique_ptr<OutputInterface> createQvueIocOutput(const std::string& device,
                                                     const std::string& address_str,
                                                     const std::vector<int>& displays,
                                                     int range_lo, int range_hi,
                                                     double response_exponent,
                                                     bool off_at_zero) {
    uint8_t address = static_cast<uint8_t>(std::stoi(address_str, nullptr, 16));
    return std::unique_ptr<OutputInterface>(
        new QvueIocOutput(device, address, displays, range_lo, range_hi, response_exponent,
                          off_at_zero));
}

} // namespace als_dimmer
