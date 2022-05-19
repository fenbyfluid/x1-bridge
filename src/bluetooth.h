#pragma once

#include <string>
#include <functional>
#include <array>
#include <optional>

struct AdvertisedDevice {
    std::array<uint8_t, 6> address;
    std::optional<std::string> name;
    std::optional<int8_t> rssi;
};

class Bluetooth {
public:
    static void init(const std::string &name);
    static void deinit();
    static bool scan(std::function<void(const AdvertisedDevice &advertisedDevice)> on_device, std::function<void ()> on_finished);
    static void cancelScan();
    static bool connect(std::array<uint8_t, 6> address, std::function<void(bool connected)> on_changed, uint32_t retry_count = 5);
    static void disconnect();
    static bool isConnected();
    static bool write(const std::vector<uint8_t> &data);
    static void setDataCallback(std::function<void(const std::vector<uint8_t> &data)> callback);
};
