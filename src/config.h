#pragma once

#include <esp_err.h>

#include <string>
#include <array>
#include <optional>

class Config {
public:
    static std::string getName();
    static void setName(const std::string &name);

    static uint32_t getPinCode();
    static void setPinCode(uint32_t pin_code);

    static uint32_t getConnectedIdleTimeout();
    static void setConnectedIdleTimeout(uint32_t timeout);

    static uint32_t getDisconnectedIdleTimeout();
    static void setDisconnectedIdleTimeout(uint32_t timeout);

    static std::optional<std::array<uint8_t, 6>> getBtAddress();
    static void setBtAddress(const std::optional<std::array<uint8_t, 6>> &bt_address);

    static void reset();

private:
    static uint32_t ensureInitialized();
    static void throwError(const std::string &label, esp_err_t err);
    static uint32_t getUint32(const char *key, uint32_t fallback);
    static void setUint32(const char *key, uint32_t value);
};
