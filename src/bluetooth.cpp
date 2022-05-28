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

    cancelScan();

    // BluetoothSerial can call our callback before it has discovered the device name,
    // and it'll only call us once per device. We'd like a call every change instead.
    // As we can't easily get to the callbacks without completely replacing BluetoothSerial,
    // we periodically clear the result set while scanning so every device update is new.
    bool scanning = SerialBT.discoverAsync([=](BTAdvertisedDevice *advertisedDevice) {
        if (!advertisedDevice->haveName()) {
            return;
        }

        AdvertisedDevice device = {};
        auto address = advertisedDevice->getAddress().getNative();
        std::copy(*address, (*address) + sizeof(esp_bd_addr_t), device.address.begin());
        device.name = advertisedDevice->getName();
        device.rssi = advertisedDevice->haveRSSI() ? advertisedDevice->getRSSI() : 0;

        on_device(device);
    }, SerialBT.MAX_INQ_TIME);

    if (!scanning) {
        return false;
    }

    struct BtScanParams {
        std::function<void(bool canceled)> on_finished;
    };

    // This is a gross hack in the name of providing a nicer API for the future.
    xTaskCreateUniversal([](void *pvParameters) {
        std::unique_ptr<BtScanParams> params(static_cast<BtScanParams *>(pvParameters));

        bool canceled = false;
        for (int t = 0; t < ESP_BT_GAP_MAX_INQ_LEN; ++t) {
            canceled = ulTaskNotifyTake(pdTRUE, SerialBT.INQ_TIME / portTICK_PERIOD_MS) != 0;
            if (canceled) {
                break;
            }

            // We have to clear the scan results each poll as it keys on the address and thus won't update.
            // This will also call it to re-call our callback each update.
            SerialBT.discoverClear();
        }

        if (!canceled) {
            // We don't strictly need to do this, but it clears out the callback.
            SerialBT.discoverAsyncStop();

            // TODO: We might need to actually check it is the same.
            scan_task = nullptr;
        }

        params->on_finished(canceled);

        vTaskDelete(nullptr);
    }, "btScanComplete", getArduinoLoopTaskStackSize(), new BtScanParams {
        on_finished,
    }, 1, &scan_task, ARDUINO_RUNNING_CORE);

    return true;
}

void Bluetooth::cancelScan() {
    if (!scan_task) {
        return;
    }

    SerialBT.discoverAsyncStop();

    xTaskNotifyGive(scan_task);

    scan_task = nullptr;
}

void Bluetooth::connect(std::array<uint8_t, 6> address, std::function<void(bool connected)> on_changed, uint32_t retry_count) {
    // BluetoothSerial doesn't support discovery after having ever attempted to connect.
    can_scan = false;

    cancelScan();

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
