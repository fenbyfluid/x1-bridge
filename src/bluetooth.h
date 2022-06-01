#pragma once

#include <string>
#include <functional>
#include <array>
#include <optional>

struct AdvertisedDevice {
    std::array<uint8_t, 6> address;
    std::string name;
    int8_t rssi;
};

class Bluetooth {
public:
    static void init(const std::string &name);
    static void deinit();
    static bool canScan();
    static bool scan(std::function<void(const AdvertisedDevice &advertisedDevice)> on_device, std::function<void(bool canceled)> on_finished);
    static void cancelScan();
    static void connect(std::array<uint8_t, 6> address, std::function<void(bool connected)> on_changed, std::function<void(uint8_t attempt, uint8_t count)> on_attempt = nullptr, uint8_t retry_count = 5);
    static void disconnect();
    static bool isConnected();
    static bool write(const std::vector<uint8_t> &data);
    static void setDataCallback(std::function<void(const std::vector<uint8_t> &data)> callback);
};
