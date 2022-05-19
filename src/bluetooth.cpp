#include "bluetooth.h"

#include <BluetoothSerial.h>

// TODO: Implement a way to clear out the "known" client so that we can re-run
//       discovery after disconnecting. Without this we'll need to reset the
//       module to allow the user to reconfigure the device to connect to.
// TODO: BluetoothSerial has a fair few other defficiences that would be good
//       to address at some point. We try and offer a sane API from our module
//       that isn't tightly bound to BluetoothSerial and synthesizes some of
//       the more egregious omissions.
//         - No callback for discovery completing.
//         - Client connection is blocking only.
//         - No callback for client disconnection.
BluetoothSerial SerialBT;

std::function<void()> on_scan_finished_callback = nullptr;
std::function<void(bool connected)> on_disconnect_callback = nullptr;

TaskHandle_t scan_task = nullptr;

void Bluetooth::init(const std::string &name) {
    SerialBT.begin(name.c_str(), true);
}

void Bluetooth::deinit() {
    if (on_disconnect_callback) {
        on_disconnect_callback(false);
        on_disconnect_callback = nullptr;
    }

    SerialBT.end();
}

bool Bluetooth::scan(std::function<void(const AdvertisedDevice &advertisedDevice)> on_device, std::function<void()> on_finished) {
    bool scanning = SerialBT.discoverAsync([=](BTAdvertisedDevice *advertisedDevice) {
        auto address = advertisedDevice->getAddress().getNative();

        AdvertisedDevice device = {};
        std::copy(*address, (*address) + sizeof(esp_bd_addr_t), device.address.begin());
        device.name = advertisedDevice->haveName() ? std::make_optional(advertisedDevice->getName()) : std::nullopt;
        device.rssi = advertisedDevice->haveRSSI() ? std::make_optional(advertisedDevice->getRSSI()) : std::nullopt;

        on_device(device);
    }, SerialBT.MAX_INQ_TIME);

    if (!scanning) {
        return false;
    }

    on_scan_finished_callback = on_finished;

    // This is a gross hack in the name of providing a nicer API for the future.
    xTaskCreateUniversal([](void *pvParameters) {
        ulTaskNotifyTake(pdTRUE, SerialBT.MAX_INQ_TIME / portTICK_PERIOD_MS);

        SerialBT.discoverAsyncStop();

        on_scan_finished_callback();
        on_scan_finished_callback = nullptr;

        scan_task = nullptr;
        vTaskDelete(nullptr);
    }, "btScanComplete", getArduinoLoopTaskStackSize(), nullptr, 1, &scan_task, ARDUINO_RUNNING_CORE);

    return true;
}

void Bluetooth::cancelScan() {
    if (scan_task) {
        xTaskNotifyGive(scan_task);
    }
}

bool Bluetooth::connect(std::array<uint8_t, 6> address, std::function<void(bool connected)> on_changed, uint32_t retry_count) {
    if (on_disconnect_callback) {
        on_disconnect_callback(false);
        on_disconnect_callback = nullptr;
    }

    bool connected = false;
    for (uint32_t i = 0; i < retry_count; ++i) {
        if (SerialBT.connect(address.data())) {
            connected = true;
            break;
        }
    }

    if (!connected) {
        return false;
    }

    on_disconnect_callback = on_changed;
    on_changed(true);
    return true;
}

void Bluetooth::disconnect() {
    SerialBT.disconnect();

    if (on_disconnect_callback) {
        on_disconnect_callback(false);
        on_disconnect_callback = nullptr;
    }
}

bool Bluetooth::isConnected() {
    if (SerialBT.hasClient()) {
        return true;
    }

    if (on_disconnect_callback) {
        on_disconnect_callback(false);
        on_disconnect_callback = nullptr;
    }

    return false;
}

bool Bluetooth::write(const std::vector<uint8_t> &data) {
    if (!isConnected()) {
        return false;
    }

    return SerialBT.write(data.data(), data.size()) != 0;
}

void Bluetooth::setDataCallback(std::function<void(const std::vector<uint8_t> &data)> callback) {
    SerialBT.onData([=](const uint8_t *buffer, size_t size) {
        callback(std::vector(buffer, buffer + size));
    });
}
