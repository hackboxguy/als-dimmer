#include <iostream>
#include <iomanip>
#include <cstdint>
#include <algorithm>

// BCD encoding function (same as in i2c_dimmer_output.cpp)
static void decimalToBCD16(int value, uint8_t& msb, uint8_t& lsb) {
    // Clamp to valid range (0-999 for 3-digit BCD)
    value = std::max(0, std::min(999, value));

    // Extract decimal digits
    int thousands = (value / 1000) % 10;
    int hundreds = (value / 100) % 10;
    int tens = (value / 10) % 10;
    int ones = value % 10;

    // Pack into BCD bytes
    // MSB: [thousands:4bits][hundreds:4bits]
    // LSB: [tens:4bits][ones:4bits]
    msb = static_cast<uint8_t>((thousands << 4) | hundreds);
    lsb = static_cast<uint8_t>((tens << 4) | ones);
}

void testBCD(int value, uint8_t expected_msb, uint8_t expected_lsb) {
    uint8_t msb, lsb;
    decimalToBCD16(value, msb, lsb);

    bool passed = (msb == expected_msb) && (lsb == expected_lsb);

    std::cout << "Value: " << std::setw(3) << value
              << " -> BCD: 0x" << std::hex << std::setfill('0')
              << std::setw(2) << (int)msb << " 0x" << std::setw(2) << (int)lsb
              << " (expected: 0x" << std::setw(2) << (int)expected_msb
              << " 0x" << std::setw(2) << (int)expected_lsb << ")"
              << std::dec << std::setfill(' ')
              << " - " << (passed ? "PASS" : "FAIL") << "\n";
}

int main() {
    std::cout << "=== BCD Encoding Test ===\n\n";

    // Test cases for dimmer800 (0-800 range)
    testBCD(0,   0x00, 0x00);  // 0 -> 0x00 0x00
    testBCD(1,   0x00, 0x01);  // 1 -> 0x00 0x01
    testBCD(99,  0x00, 0x99);  // 99 -> 0x00 0x99
    testBCD(100, 0x01, 0x00);  // 100 -> 0x01 0x00
    testBCD(123, 0x01, 0x23);  // 123 -> 0x01 0x23
    testBCD(456, 0x04, 0x56);  // 456 -> 0x04 0x56 (example from spec)
    testBCD(800, 0x08, 0x00);  // 800 -> 0x08 0x00 (max for dimmer800)
    testBCD(999, 0x09, 0x99);  // 999 -> 0x09 0x99 (max for 3-digit BCD)

    std::cout << "\n=== Brightness Percentage to BCD (dimmer800) ===\n\n";

    // Test common brightness percentages scaled to 0-800 range
    // Formula: native_value = (percent / 100.0) * 800
    int brightness_levels[] = {0, 25, 50, 75, 100};
    for (int percent : brightness_levels) {
        int native_value = static_cast<int>((percent / 100.0) * 800);
        uint8_t msb, lsb;
        decimalToBCD16(native_value, msb, lsb);

        std::cout << "Brightness: " << std::setw(3) << percent << "% "
                  << "-> Native: " << std::setw(3) << native_value
                  << " -> BCD: 0x" << std::hex << std::setfill('0')
                  << std::setw(2) << (int)msb << " 0x" << std::setw(2) << (int)lsb
                  << std::dec << std::setfill(' ') << "\n";
    }

    std::cout << "\n=== Test Complete ===\n";

    return 0;
}
