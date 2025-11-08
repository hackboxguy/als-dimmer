#include "als-dimmer/interfaces.hpp"
#include <iostream>
#include <memory>

#ifdef HAVE_DDCUTIL
#include <ddcutil_c_api.h>

namespace als_dimmer {

/**
 * DDC/CI output using libddcutil
 * Controls monitor brightness via DDC/CI protocol
 */
class DDCUtilOutput : public OutputInterface {
public:
    DDCUtilOutput(int display_number)
        : display_number_(display_number), dh_(nullptr), current_brightness_(0) {}

    ~DDCUtilOutput() {
        if (dh_) {
            ddca_close_display(dh_);
        }
    }

    bool init() override {
        std::cout << "[DDCUtil] Initializing DDC/CI for display " << display_number_ << "\n";

        // Get display list
        DDCA_Display_Info_List* dlist = nullptr;
        DDCA_Status rc = ddca_get_display_info_list2(false, &dlist);

        if (rc != 0) {
            std::cerr << "[DDCUtil] Failed to get display list: " << ddca_rc_name(rc) << "\n";
            return false;
        }

        if (dlist->ct == 0) {
            std::cerr << "[DDCUtil] No displays found\n";
            ddca_free_display_info_list(dlist);
            return false;
        }

        std::cout << "[DDCUtil] Found " << dlist->ct << " display(s)\n";

        if (display_number_ >= dlist->ct) {
            std::cerr << "[DDCUtil] Display number " << display_number_
                      << " out of range (0-" << (dlist->ct - 1) << ")\n";
            ddca_free_display_info_list(dlist);
            return false;
        }

        // Open display
        rc = ddca_open_display2(dlist->info[display_number_].dref, false, &dh_);
        ddca_free_display_info_list(dlist);

        if (rc != 0) {
            std::cerr << "[DDCUtil] Failed to open display: " << ddca_rc_name(rc) << "\n";
            return false;
        }

        std::cout << "[DDCUtil] Display opened successfully\n";

        // Try to read current brightness
        int brightness = getCurrentBrightness();
        if (brightness >= 0) {
            current_brightness_ = brightness;
            std::cout << "[DDCUtil] Current brightness: " << current_brightness_ << "%\n";
        } else {
            std::cout << "[DDCUtil] Warning: Could not read current brightness\n";
            current_brightness_ = 50;  // Default
        }

        return true;
    }

    bool setBrightness(int brightness) override {
        if (!dh_) {
            std::cerr << "[DDCUtil] Display not initialized\n";
            return false;
        }

        // Clamp to valid range
        if (brightness < 0) brightness = 0;
        if (brightness > 100) brightness = 100;

        // VCP feature code 0x10 is brightness
        DDCA_Status rc = ddca_set_non_table_vcp_value(dh_, 0x10, 0, brightness);

        if (rc != 0) {
            std::cerr << "[DDCUtil] Failed to set brightness: " << ddca_rc_name(rc) << "\n";
            return false;
        }

        current_brightness_ = brightness;
        return true;
    }

    int getCurrentBrightness() override {
        if (!dh_) {
            return -1;
        }

        DDCA_Non_Table_Vcp_Value valrec;
        DDCA_Status rc = ddca_get_non_table_vcp_value(dh_, 0x10, &valrec);

        if (rc != 0) {
            std::cerr << "[DDCUtil] Failed to get brightness: " << ddca_rc_name(rc) << "\n";
            return -1;
        }

        current_brightness_ = valrec.sl;
        return current_brightness_;
    }

    std::string getType() const override {
        return "ddcutil";
    }

private:
    int display_number_;
    DDCA_Display_Handle dh_;
    int current_brightness_;
};

// Factory function
std::unique_ptr<OutputInterface> createDDCUtilOutput(int display_number) {
    return std::make_unique<DDCUtilOutput>(display_number);
}

} // namespace als_dimmer

#endif // HAVE_DDCUTIL
