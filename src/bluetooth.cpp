#include "bluetooth.h"

#include <BluetoothSerial.h>

#include <memory>

// TODO: Implement a way to clear out the "known" client so that we can re-run
//       discovery after disconnecting. Without this we'll need to reset the
//       module to allow the user to reconfigure the device to connect to.
// TODO: BluetoothSerial has a fair few other deficiencies that would be good
//       to address at some point. We try and offer a sane API from our module
//       that isn't tightly bound to BluetoothSerial and synthesizes some of
//       the more egregious omissions.
//         - No callback for discovery completing.
//         - Client connection is blocking only.
//         - No callback for client disconnection.
BluetoothSerial SerialBT;

bool can_scan = true;

std::function<void(bool canceled)> on_scan_finished_callback = nullptr;
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

bool Bluetooth::canScan() {
    return can_scan;
}

bool Bluetooth::scan(std::function<void(const AdvertisedDevice &advertisedDevice)> on_device, std::function<void(bool canceled)> on_finished) {
    if (!can_scan) {
        return false;
    }

    // TODO: Handle there already being a scan in progress (end it immediately).
    //       Need to use scan_task and pay attention to the state of on_scan_finished_callback.
    //       Probably need to move on_scan_finished_callback to task params rather than global.

    // TODO: BluetoothSerial can call our callback before it has discovered the device name,
    //       and it'll only call us once per device. We'd like a call every change instead.
    //       As we can't easily get to the callbacks without completely replacing BluetoothSerial,
    //       we should refactor our logic below to poll SerialBT.getScanResults() while scanning.
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
        bool canceled = ulTaskNotifyTake(pdTRUE, SerialBT.MAX_INQ_TIME / portTICK_PERIOD_MS) != 0;

        SerialBT.discoverAsyncStop();

        on_scan_finished_callback(canceled);
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

void Bluetooth::connect(std::array<uint8_t, 6> address, std::function<void(bool connected)> on_changed, uint32_t retry_count) {
    // BluetoothSerial doesn't support discovery after having ever attempted to connect.
    can_scan = false;

    if (on_disconnect_callback) {
        on_disconnect_callback(false);
        on_disconnect_callback = nullptr;
    }

    struct ConnectTaskParams {
        std::array<uint8_t, 6> address;
        std::function<void(bool connected)> on_changed;
        uint32_t retry_count;
    };

    ConnectTaskParams *params = new ConnectTaskParams {
        address,
        on_changed,
        retry_count,
    };

    // This is a gross hack in the name of providing a nicer API for the future.
    xTaskCreateUniversal([](void *pvParameters) {
        std::unique_ptr<ConnectTaskParams> params(static_cast<ConnectTaskParams *>(pvParameters));

        bool connected = false;
        for (uint32_t i = 0; i < params->retry_count; ++i) {
            if (SerialBT.connect(params->address.data())) {
                connected = true;
                break;
            }
        }

        params->on_changed(connected);
        if (!connected) {
            return;
        }

        on_disconnect_callback = params->on_changed;
        vTaskDelete(nullptr);
    }, "btConnect", getArduinoLoopTaskStackSize(), params, 1, nullptr, ARDUINO_RUNNING_CORE);
}

void Bluetooth::disconnect() {
    // This is a gross hack in the name of providing a nicer API for the future.
    xTaskCreateUniversal([](void *pvParameters) {
        SerialBT.disconnect();

        if (on_disconnect_callback) {
            on_disconnect_callback(false);
            on_disconnect_callback = nullptr;
        }

        vTaskDelete(nullptr);
    }, "btDisconnect", getArduinoLoopTaskStackSize(), nullptr, 1, nullptr, ARDUINO_RUNNING_CORE);
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
