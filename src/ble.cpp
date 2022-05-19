#include "ble.h"

#include "log.h"
#include "bluetooth.h"

#include <BLEDevice.h>
#include <BLE2902.h>
#include <BLE2904.h>

static bool is_client_connected = false;
static std::vector<BLE2902 *> client_config_descriptors;

// Battery Service
static BLECharacteristic *battery_level = nullptr;

// Bridge Service
static BLECharacteristic *battery_voltage = nullptr;

// TODO: There is a fair amount of complexity around supporting multiple connections.
//       We don't currently need that, but it does need validating that we're being sane.
//
//       Current expectation:
//           BLEServer stops advertising when a client connects. If we wanted multiple
//           we'd need to re-start advertising onConnect. We currently re-start in
//           onDisconnect instead - need to ensure no one can sneak by that and connect
//           anyway.
//
//       Known pain points:
//           BLE2902's state needs to be per-connection - or the enabled state for
//           notifications will be shared by all peers. notify() checks its presence
//           and value before looping the clients, so we'd need to modify BLECharacteristic
//           to implement it properly.
static class MyBleServerCallbacks: public BLEServerCallbacks {
    virtual void onConnect(BLEServer *server) override {
        Log::print("ble client connected\n");

        is_client_connected = true;

        // TODO: It may be worthwhile to add an application-level heartbeat so that
        //       we can forcibly disconnect peers that aren't doing anything.
    }

    void onDisconnect(BLEServer *server) override {
        Log::print("ble client disconnected\n");

        is_client_connected = false;

        // Reset the notifications / indications preference.
        for (auto client_config : client_config_descriptors) {
            client_config->setNotifications(false);
            client_config->setIndications(false);
        }

        // We have to restart advertising each time a client disconnects.
        server->getAdvertising()->start();
    }
} ble_server_callbacks;

void Ble::init(const std::string &name, uint32_t pin_code) {
    BLEDevice::init(name);
    // BLEDevice::setPower(ESP_PWR_LVL_P9);

    // We use this as setSecurityCallbacks appears to opt in to a bunch of behaviour we don't want.
    // TODO: It's possible all the extra bits are events that are never called with our config though.
    BLEDevice::setCustomGapHandler([](esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
        if (event != ESP_GAP_BLE_AUTH_CMPL_EVT) {
            return;
        }

        auto ev_param = param->ble_security.auth_cmpl;
        if (ev_param.success) {
            Log::print("ble connection authorized\n");
            return;
        }

        // 81 bad pin
        // 85 cancel
        Log::printf("ble connection auth failed, reason: %d\n", ev_param.fail_reason);

        // TODO: Can / should we kick off the peer?
    });

    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pin_code, sizeof(pin_code));

    esp_ble_io_cap_t io_cap = ESP_IO_CAP_OUT;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));

    // TODO: Allow bonding?
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(&ble_server_callbacks);

    initBridgeService(server);
    initBatteryService(server);

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(X1_GATT_UUID_BRIDGE_SVC);
    advertising->start();
}

void Ble::deinit() {
    BLEDevice::deinit(true);
}

bool Ble::isClientConnected() {
    return is_client_connected;
}

void Ble::updateBatteryLevel(uint8_t level, uint32_t millivolts) {
    if (battery_voltage) {
        battery_voltage->setValue(millivolts);
    }

    if (battery_level) {
        battery_level->setValue(&level, sizeof(uint8_t));
        battery_level->notify();
    }
}

void Ble::initBatteryService(BLEServer *server) {
    BLEService *service = server->createService(BLEUUID((uint16_t)ESP_GATT_UUID_BATTERY_SERVICE_SVC));

    BLECharacteristic *characteristic = service->createCharacteristic(BLEUUID((uint16_t)ESP_GATT_UUID_BATTERY_LEVEL), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ);

    BLE2902 *client_configuration_descriptor = new BLE2902();
    client_configuration_descriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
    client_config_descriptors.push_back(client_configuration_descriptor);
    characteristic->addDescriptor(client_configuration_descriptor);

    battery_level = characteristic;

    service->start();
}

void Ble::initBridgeService(BLEServer *server) {
    BLEService *service = server->createService(X1_GATT_UUID_BRIDGE_SVC);

    createSerialDataCharacteristic(service);
    battery_voltage = createBatteryVoltageCharacteristic(service);

    service->start();
}

BLECharacteristic *Ble::createSerialDataCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *pCharacteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = pCharacteristic->getData();
            size_t length = pCharacteristic->getLength();

            Log::print("ble serial data written:");
            for (size_t i = 0; i < length; ++i) {
                Log::printf(" %02X", data[i]);
            }
            Log::print("\n");

            if (Bluetooth::isConnected()) {
                // TODO: Should we validate anything about the data before passing it on? Probably a good idea.
                Bluetooth::write(std::vector(data, data + length));
            }
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_SERIAL_DATA, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Serial Data");
    characteristic->addDescriptor(description_descriptor);

    BLE2902 *configuration_descriptor = new BLE2902();
    configuration_descriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENC_MITM);
    client_config_descriptors.push_back(configuration_descriptor);
    characteristic->addDescriptor(configuration_descriptor);

    Bluetooth::setDataCallback([=](const std::vector<uint8_t> &data) {
        Log::printf("got %d byte response:", data.size());
        for (uint8_t byte : data) {
            Log::printf(" 0x%02X", byte);
        }
        Log::print("\n");

        static std::vector<uint8_t> buffer;

        for (uint8_t byte : data) {
            buffer.push_back(byte);

            if (byte != 0x0A) {
                continue;
            }

            Log::printf("got %d byte command:", buffer.size());
            for (uint8_t byte : buffer) {
                Log::printf(" 0x%02X", byte);
            }
            Log::print("\n");

            characteristic->setValue(buffer.data(), buffer.size());
            characteristic->notify();

            buffer.clear();
        }
    });

    return characteristic;
}

BLECharacteristic *Ble::createBatteryVoltageCharacteristic(BLEService *service) {
    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_BATTERY_VOLTAGE, BLECharacteristic::PROPERTY_READ);
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Battery Voltage (mV)");
    characteristic->addDescriptor(description_descriptor);

    BLE2904 *presentation_descriptor = new BLE2904();
    presentation_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    presentation_descriptor->setFormat(BLE2904::FORMAT_UINT32);
    presentation_descriptor->setExponent(-3);
    presentation_descriptor->setNamespace(1);
    presentation_descriptor->setUnit(0x2728); // volts
    presentation_descriptor->setDescription(0);
    characteristic->addDescriptor(presentation_descriptor);

    return characteristic;
}
