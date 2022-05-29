#include "ble.h"

#include "log.h"
#include "bluetooth.h"
#include "defaults.h"
#include "config.h"

#include <BLEDevice.h>
#include <BLE2902.h>
#include <BLE2904.h>

#include <esp_sleep.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/error.h>

static void gracefulCleanup() {
    Bluetooth::deinit();
    Ble::deinit();
    vTaskDelay((2 * 1000) / portTICK_PERIOD_MS);
}

static void gracefulSleep() {
    xTaskCreatePinnedToCore([](void *) {
        vTaskDelay((1 * 1000) / portTICK_PERIOD_MS);

        gracefulCleanup();

        Log::print("cleanup complete, sleeping\n");
        esp_deep_sleep_start();
    }, "sleep", CONFIG_ESP_MAIN_TASK_STACK_SIZE, nullptr, 1, nullptr, CONFIG_ARDUINO_RUNNING_CORE);
}

static void gracefulRestart() {
    xTaskCreatePinnedToCore([](void *) {
        vTaskDelay((1 * 1000) / portTICK_PERIOD_MS);

        gracefulCleanup();

        Log::print("cleanup complete, restarting\n");
        esp_restart();
    }, "restart", CONFIG_ESP_MAIN_TASK_STACK_SIZE, nullptr, 1, nullptr, CONFIG_ARDUINO_RUNNING_CORE);
}

static std::optional<std::array<uint8_t, 6>> connected_client = std::nullopt;
static time_t last_activity_time = 0;

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
    virtual void onConnect(BLEServer *server, esp_ble_gatts_cb_param_t *param) override {
        Log::print("ble client connected\n");

        assert(!connected_client.has_value());

        std::array<uint8_t, 6> address;
        std::copy(param->connect.remote_bda, param->connect.remote_bda + 6, address.begin());
        connected_client = std::make_optional(address);

        last_activity_time = time(nullptr);
        Log::printf("client connect time: %d\n", last_activity_time);
    }

    void onDisconnect(BLEServer *server) override {
        connected_client = std::nullopt;

        // Reset the notifications / indications preference.
        for (auto client_config : client_config_descriptors) {
            client_config->setNotifications(false);
            client_config->setIndications(false);
        }

        Log::print("ble client disconnected\n");

        last_activity_time = time(nullptr);
        Log::printf("client disconnect time: %d\n", last_activity_time);

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
        //       Apparently we can, lets try this.
        esp_ble_gap_disconnect(ev_param.bd_addr);
    });

    BLEDevice::setCustomGattsHandler([](esp_gatts_cb_event_t event, esp_gatt_if_t gatt_if, esp_ble_gatts_cb_param_t *param) {
        // We can crash if we try to notify from inside here, we also don't want the log updating the activity time.
        auto suspender = Log::suspendOutputCallback();

        // Log::printf("gatt event: %d, %d\n", event, gatt_if);

        // The BLEDevice library doesn't handle this, and it keeps catching us out.
        if (event == ESP_GATTS_ADD_CHAR_EVT && param->add_char.status != ESP_GATT_OK) {
            Log::printf("!!! ESP_GATTS_ADD_CHAR_EVT failed (%02x), check handle count !!!\n", param->add_char.status);
        } else if (event == ESP_GATTS_ADD_CHAR_DESCR_EVT && param->add_char_descr.status != ESP_GATT_OK) {
            Log::printf("!!! ESP_GATTS_ADD_CHAR_DESCR_EVT failed (%02x), check handle count !!!\n", param->add_char_descr.status);
        }

        // ESP_GATTS_CONF_EVT is fired when our notifications are confirmed.
        if (event == ESP_GATTS_READ_EVT || event == ESP_GATTS_WRITE_EVT || event == ESP_GATTS_EXEC_WRITE_EVT || event == ESP_GATTS_CONF_EVT) {
            last_activity_time = time(nullptr);

            Log::printf("updated client last activity time: %d (reason: %d)\n", last_activity_time, event);
        }
    });

    xTaskCreatePinnedToCore([](void *parameters) {
        for (;;) {
            time_t now = time(nullptr);
            time_t idle_time = now - last_activity_time;

            {
                // We don't want the log updating the activity time.
                auto suspender = Log::suspendOutputCallback();
                Log::printf("client idle time: %d\n", idle_time);
            }

            if (connected_client) {
                // TODO: Tune this. Currently set so our battery monitor keeps
                //       the connection alive if notifications are enabled.
                if (idle_time >= Config::getConnectedIdleTimeout()) {
                    Log::print("disconnecting client due to idle timeout\n");
                    esp_ble_gap_disconnect(connected_client->data());
                    last_activity_time = now;
                }
            } else {
                if (idle_time >= Config::getDisconnectedIdleTimeout()) {
                    Log::print("going to sleep due to idle timeout\n");
                    gracefulCleanup();
                    esp_deep_sleep_start();
                }
            }

            vTaskDelay((30 * 1000) / portTICK_PERIOD_MS);
        }
    }, "bleTimeout", CONFIG_ARDUINO_LOOP_STACK_SIZE, nullptr, 1, nullptr, CONFIG_ARDUINO_RUNNING_CORE);

    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &pin_code, sizeof(pin_code));

    esp_ble_io_cap_t io_cap = ESP_IO_CAP_OUT;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));

    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));

    uint8_t only_accept_specified_auth = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &only_accept_specified_auth, sizeof(only_accept_specified_auth));

    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));

    int bonded_count = esp_ble_get_bond_device_num();
    Log::printf("have %d bonded ble devices\n", bonded_count);

    std::vector<esp_ble_bond_dev_t> bonded_devices(bonded_count);
    esp_ble_get_bond_device_list(&bonded_count, bonded_devices.data());
    for (const auto &device : bonded_devices) {
        Log::printf("  %02X:%02X:%02X:%02X:%02X:%02X\n",
            device.bd_addr[0], device.bd_addr[1], device.bd_addr[2],
            device.bd_addr[3], device.bd_addr[4], device.bd_addr[5]);
    }

    BLEServer *server = BLEDevice::createServer();
    server->setCallbacks(&ble_server_callbacks);

    initBridgeService(server);
    initBatteryService(server);

    BLEAdvertising *advertising = BLEDevice::getAdvertising();
    advertising->addServiceUUID(X1_GATT_UUID_BRIDGE_SVC);
    advertising->start();
}

void Ble::deinit() {
    BLEDevice::deinit();
}

bool Ble::isClientConnected() {
    return connected_client.has_value();
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
    // Enough handles need to be allocated for the characteristics and their descriptions.
    // If there aren't enough, things will start disappearing when querying the service.
    // Need approximately (2 * number of characteristics) + number of descriptors.
    BLEService *service = server->createService(BLEUUID(X1_GATT_UUID_BRIDGE_SVC), 60);

    createSerialDataCharacteristic(service);
    createBluetoothScanCharacteristic(service);
    createBluetoothConnectCharacteristic(service);
    createConfigNameCharacteristic(service);
    createConfigPinCodeCharacteristic(service);
    createConfigBluetoothAddressCharacteristic(service);
    createConfigConnectedIdleTimeoutCharacteristic(service);
    createConfigDisconnectedIdleTimeoutCharacteristic(service);
    battery_voltage = createBatteryVoltageCharacteristic(service);
    createDebugLogCharacteristic(service);
    createRestartCharacteristic(service);
    createSleepCharacteristic(service);
    createOtaUpdateCharacteristic(service);

    service->start();
}

BLECharacteristic *Ble::createSerialDataCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

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
            Log::printf(" %02X", byte);
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
                Log::printf(" %02X", byte);
            }
            Log::print("\n");

            characteristic->setValue(buffer.data(), buffer.size());
            characteristic->notify();

            buffer.clear();
        }
    });

    return characteristic;
}

BLECharacteristic *Ble::createBluetoothScanCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onRead(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t value = 0;
            if (is_scanning) {
                value = 1;
            } else if (!Bluetooth::canScan()) {
                value = 0xFF;
            }

            characteristic->setValue(&value, 1);
        }

        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            if (length < 1) {
                return;
            }

            bool cancel_scan = (data[0] == 0);
            Log::printf("ble client %s bt scan\n", cancel_scan ? "canceled" : "requested");

            if (cancel_scan) {
                Bluetooth::cancelScan();
                return;
            }

            is_scanning = Bluetooth::scan([=](const AdvertisedDevice &advertisedDevice) {
                // TODO: Filter to X1 devices using COD (0x1F00) and name prefix (SLMK1).
                //       Name: SLMK1xxxx, Address: 00:06:66:xx:xx:xx, cod: 7936, rssi: -60
                const auto &name = advertisedDevice.name;
                const auto &address = advertisedDevice.address;
                Log::printf("new bt device: %s (%02X:%02X:%02X:%02X:%02X:%02X) %d\n",
                    name.c_str(),
                    address[0], address[1], address[2], address[3], address[4], address[5],
                    advertisedDevice.rssi);

                std::vector<uint8_t> value(address.size() + 1 + name.size());
                auto after_address = std::copy(address.begin(), address.end(), value.begin());
                *after_address = advertisedDevice.rssi;
                std::copy(name.begin(), name.end(), after_address + 1);
                characteristic->setValue(value.data(), value.size());
                characteristic->notify();
            }, [=](bool canceled) {
                Log::printf("bluetooth discovery %s\n", canceled ? "canceled" : "completed");

                is_scanning = false;

                std::array<uint8_t, 7> null_update = {};
                characteristic->setValue(null_update.data(), null_update.size());
                characteristic->notify();
            });
        }

    private:
        bool is_scanning = false;
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_BT_SCAN, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Bluetooth Scan");
    characteristic->addDescriptor(description_descriptor);

    BLE2902 *configuration_descriptor = new BLE2902();
    configuration_descriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENC_MITM);
    client_config_descriptors.push_back(configuration_descriptor);
    characteristic->addDescriptor(configuration_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createBluetoothConnectCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onRead(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t value = 0;
            if (Bluetooth::isConnected()) {
                value = 1;
            } else if (!Config::getBtAddress()) {
                value = 0xFF;
            }

            characteristic->setValue(&value, 1);
        }

        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            if (length < 1) {
                return;
            }

            if (data[0] == 0) {
                Log::print("disconnecting from device\n");
                Bluetooth::disconnect();
                return;
            }

            const auto address_opt = Config::getBtAddress();
            if (!address_opt) {
                Log::print("can not connect, address not set\n");
                return;
            }

            const auto &address = *address_opt;
            Log::printf("connecting to %02X:%02X:%02X:%02X:%02X:%02X\n",
                address[0], address[1], address[2], address[3], address[4], address[5]);

            Bluetooth::connect(address, [=](bool connected) {
                Log::printf("connection state changed, now %s\n", connected ? "connected" : "disconnected");

                uint8_t value = connected ? 1 : 0;
                characteristic->setValue(&value, 1);
                characteristic->notify();
            });
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_BT_CONNECT, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Bluetooth Connect");
    characteristic->addDescriptor(description_descriptor);

    BLE2902 *configuration_descriptor = new BLE2902();
    configuration_descriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENC_MITM);
    client_config_descriptors.push_back(configuration_descriptor);
    characteristic->addDescriptor(configuration_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createConfigNameCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onRead(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            auto name = Config::getName();
            characteristic->setValue(name);
        }

        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            auto name = characteristic->getValue();
            Config::setName(name);

            Log::printf("changed name to \"%s\"\n", name.c_str());
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_CONFIG_NAME, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Name");
    characteristic->addDescriptor(description_descriptor);

    BLE2904 *presentation_descriptor = new BLE2904();
    presentation_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    presentation_descriptor->setFormat(BLE2904::FORMAT_UTF8);
    presentation_descriptor->setExponent(0);
    presentation_descriptor->setNamespace(1);
    presentation_descriptor->setUnit(0x2700); // unitless
    presentation_descriptor->setDescription(0);
    characteristic->addDescriptor(presentation_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createConfigPinCodeCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            if (length != 4) {
                Log::printf("attempt to set pin code had wrong value length (%d != %d)\n", length, 4);
                return;
            }

            uint32_t pin_code = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
            if (pin_code > 999999) {
                Log::printf("attempt to set pin code out of bounds: %d\n", pin_code);
                return;
            }

            Config::setPinCode(pin_code);

            Log::printf("changed pin code to %06d\n", pin_code);
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_CONFIG_PIN_CODE, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Pin Code");
    characteristic->addDescriptor(description_descriptor);

    BLE2904 *presentation_descriptor = new BLE2904();
    presentation_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    presentation_descriptor->setFormat(BLE2904::FORMAT_UINT32);
    presentation_descriptor->setExponent(0);
    presentation_descriptor->setNamespace(1);
    presentation_descriptor->setUnit(0x2700); // unitless
    presentation_descriptor->setDescription(0);
    characteristic->addDescriptor(presentation_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createConfigBluetoothAddressCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onRead(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            auto address = Config::getBtAddress();
            if (!address) {
                characteristic->setValue(nullptr, 0);
                return;
            }

            auto name = Config::getBtAddressName().value_or("");

            std::vector<uint8_t> value(address->size() + name.size());
            auto after_address = std::copy(address->begin(), address->end(), value.begin());
            std::copy(name.begin(), name.end(), after_address);
            characteristic->setValue(value.data(), value.size());
        }

        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            if (length == 0) {
                Config::setBtAddress(std::nullopt);
                Config::setBtAddressName(std::nullopt);

                Log::print("cleared bt addr\n");
                return;
            }

            if (length < 6) {
                Log::printf("attempt to set bt addr had wrong value length (%d < %d)\n", length, 6);
                return;
            }

            std::array<uint8_t, 6> address;
            std::copy(data, data + address.size(), address.begin());
            Config::setBtAddress(address);

            std::string name(length - address.size(), '\0');
            std::copy(data + address.size(), data + length, name.begin());
            Config::setBtAddressName(name);

            Log::printf("changed bt addr to %02X:%02X:%02X:%02X:%02X:%02X (%s)\n",
                address[0], address[1], address[2], address[3], address[4], address[5],
                name.c_str());
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_CONFIG_BT_ADDR, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Bluetooth Address");
    characteristic->addDescriptor(description_descriptor);

#if 0
    // TODO: We cant include a presentation descriptor for this one as the value is
    //       composite, and that needs multiple which the wrapper doesn't support.
    BLE2904 *presentation_descriptor = new BLE2904();
    presentation_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    presentation_descriptor->setFormat(BLE2904::FORMAT_UINT48);
    presentation_descriptor->setExponent(0);
    presentation_descriptor->setNamespace(1);
    presentation_descriptor->setUnit(0x2700); // unitless
    presentation_descriptor->setDescription(0);
    characteristic->addDescriptor(presentation_descriptor);
#endif

    return characteristic;
}

BLECharacteristic *Ble::createConfigConnectedIdleTimeoutCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onRead(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint32_t timeout = Config::getConnectedIdleTimeout();

            characteristic->setValue(timeout);
        }

        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            if (length != 4) {
                Log::printf("attempt to set connected idle timeout had wrong value length (%d != %d)\n", length, 4);
                return;
            }

            uint32_t timeout = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];

            Config::setConnectedIdleTimeout(timeout);

            Log::printf("changed connected idle timeout to %d\n", timeout);
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_CONFIG_CON_IDLE, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Connected Idle Timeout");
    characteristic->addDescriptor(description_descriptor);

    BLE2904 *presentation_descriptor = new BLE2904();
    presentation_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    presentation_descriptor->setFormat(BLE2904::FORMAT_UINT32);
    presentation_descriptor->setExponent(0);
    presentation_descriptor->setNamespace(1);
    presentation_descriptor->setUnit(0x2703); // seconds
    presentation_descriptor->setDescription(0);
    characteristic->addDescriptor(presentation_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createConfigDisconnectedIdleTimeoutCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onRead(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint32_t timeout = Config::getDisconnectedIdleTimeout();

            characteristic->setValue(timeout);
        }

        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            if (length != 4) {
                Log::printf("attempt to set disconnected idle timeout had wrong value length (%d != %d)\n", length, 4);
                return;
            }

            uint32_t timeout = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];

            Config::setDisconnectedIdleTimeout(timeout);

            Log::printf("changed disconnected idle timeout to %d\n", timeout);
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_CONFIG_CON_IDLE, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Disconnected Idle Timeout");
    characteristic->addDescriptor(description_descriptor);

    BLE2904 *presentation_descriptor = new BLE2904();
    presentation_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    presentation_descriptor->setFormat(BLE2904::FORMAT_UINT32);
    presentation_descriptor->setExponent(0);
    presentation_descriptor->setNamespace(1);
    presentation_descriptor->setUnit(0x2703); // seconds
    presentation_descriptor->setDescription(0);
    characteristic->addDescriptor(presentation_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createBatteryVoltageCharacteristic(BLEService *service) {
    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_BATTERY_VOLTAGE, BLECharacteristic::PROPERTY_READ);
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Battery Voltage");
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

BLECharacteristic *Ble::createDebugLogCharacteristic(BLEService *service) {
    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_DEBUG_LOG, BLECharacteristic::PROPERTY_NOTIFY);
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Debug Log");
    characteristic->addDescriptor(description_descriptor);

    BLE2902 *configuration_descriptor = new BLE2902();
    configuration_descriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENC_MITM);
    client_config_descriptors.push_back(configuration_descriptor);
    characteristic->addDescriptor(configuration_descriptor);

    Log::setOutputCallback([=](const char *message) {
        characteristic->setValue(message);
        characteristic->notify();
    });

    return characteristic;
}

BLECharacteristic *Ble::createRestartCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            bool erase_config = (length >= 1) && data[0] != 0;
            Log::printf("reboot request from ble client (%s config reset)\n", erase_config ? "with" : "without");

            if (erase_config) {
                Config::reset();

                Log::print("config reset\n");
            }

            gracefulRestart();
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_RESTART, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Restart");
    characteristic->addDescriptor(description_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createSleepCharacteristic(BLEService *service) {
    class Callbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            Log::printf("sleep request from ble client\n");

            gracefulSleep();
        }
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_SLEEP, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("Sleep");
    characteristic->addDescriptor(description_descriptor);

    return characteristic;
}

BLECharacteristic *Ble::createOtaUpdateCharacteristic(BLEService *service) {
#if defined OTA_PUBLIC_KEY_X && defined OTA_PUBLIC_KEY_Y
    class Callbacks: public BLECharacteristicCallbacks {
        void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
            this->characteristic = characteristic;

            uint8_t *data = characteristic->getData();
            size_t length = characteristic->getLength();

            // uint8_t message type
            //   01: start
            //      uint8_t format (always 1)
            //      uint32_t total image size
            //   02: chunk
            //      uint8_t data[]
            //   03: finish
            //      uint8_t signature[]

#if 0
            Log::printf("%d bytes ble ota data received:", length);
            for (size_t i = 0; i < length; ++i) {
                Log::printf(" %02X", data[i]);
            }
            Log::print("\n");
#endif

            if (length < 1) {
                return;
            }

            switch (data[0]) {
                case 1:
                    onOtaStart(&data[1], length - 1);
                    break;
                case 2:
                    onOtaChunk(&data[1], length - 1);
                    break;
                case 3:
                    onOtaFinish(&data[1], length - 1);
                    break;
                default:
                    Log::printf("invalid ble ota type: %02X\n", data[0]);
                    break;
            }
        }

        void onOtaStart(const uint8_t *data, size_t length) {
            if (length != 5) {
                Log::printf("invalid ble ota start message length: %d\n", length);
                return;
            }

            if (data[0] != 0x01) {
                Log::printf("invalid ble ota format: %d\n", data[1]);
                return;
            }

            ota_partition = esp_ota_get_next_update_partition(nullptr);
            if (!ota_partition) {
                Log::print("ble ota partition not found\n");
                return;
            }

            image_size = (data[4] << 24) | (data[3] << 16) | (data[2] << 8) | data[1];

            esp_err_t err = esp_ota_begin(ota_partition, image_size, &ota_handle);
            if (err != ESP_OK) {
                Log::printf("ble ota failed to start: %s (%d)\n", esp_err_to_name(err), err);
                notify(false);
                image_size = 0;
                return;
            }

            Log::printf("ble ota update started (%s), expecting %d bytes\n", ota_partition->label, image_size);
            bytes_written = 0;

            mbedtls_sha256_init(&sha_ctx);
            mbedtls_sha256_starts_ret(&sha_ctx, 0);
        }

        void onOtaChunk(const uint8_t *data, size_t length) {
            if (image_size == 0) {
                Log::print("ble ota chunk message received without start\n");
                return;
            }

            if (length == 0) {
                return;
            }

            if ((bytes_written + length) > image_size) {
                Log::printf("ble ota chunk out of bounds: (%d + %d) > %d\n", bytes_written, length, image_size);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                mbedtls_sha256_free(&sha_ctx);
                return;
            }

            esp_err_t err = esp_ota_write(ota_handle, data, length);
            if (err != ESP_OK) {
                Log::printf("ble ota failed to write: %s (%d)\n", esp_err_to_name(err), err);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                mbedtls_sha256_free(&sha_ctx);
                return;
            }

            mbedtls_sha256_update_ret(&sha_ctx, data, length);

            bytes_written += length;

            Log::printf("ble ota update chunk processed (%d / %d bytes)\n", bytes_written, image_size);
        }

        void onOtaFinish(const uint8_t *data, size_t length) {
            if (image_size == 0) {
                Log::print("ble ota finish message received without start\n");
                return;
            }

            if (bytes_written != image_size) {
                Log::printf("ble ota finish message image size mismatch (%d != %d)\n", bytes_written, image_size);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                mbedtls_sha256_free(&sha_ctx);
                return;
            }

            uint8_t hash[32];
            mbedtls_sha256_finish_ret(&sha_ctx, hash);
            mbedtls_sha256_free(&sha_ctx);

            Log::print("ble ota image hash: ");
            for (uint8_t byte : hash) {
                Log::printf("%02x", byte);
            }
            Log::print("\n");

            char sig_error[256];

            mbedtls_ecp_keypair sig_key;
            mbedtls_ecp_keypair_init(&sig_key);

            int sig_err = mbedtls_ecp_group_load(&sig_key.grp, MBEDTLS_ECP_DP_SECP256R1);

            if (sig_err != 0) {
                mbedtls_strerror(sig_err, sig_error, sizeof(sig_error));
                Log::printf("ble ota signature verification failed - mbedtls_ecp_group_load: %s (%d)\n", sig_error, sig_err);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                return;
            }

            sig_err = mbedtls_ecp_point_read_string(&sig_key.Q, 16, QUOTE(OTA_PUBLIC_KEY_X), QUOTE(OTA_PUBLIC_KEY_Y));

            if (sig_err != 0) {
                mbedtls_strerror(sig_err, sig_error, sizeof(sig_error));
                Log::printf("ble ota signature verification failed - mbedtls_ecp_point_read_string: %s (%d)\n", sig_error, sig_err);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                return;
            }

            mbedtls_ecdsa_context sig_ctx;
            mbedtls_ecdsa_init(&sig_ctx);

            sig_err = mbedtls_ecdsa_from_keypair(&sig_ctx, &sig_key);

            if (sig_err != 0) {
                mbedtls_strerror(sig_err, sig_error, sizeof(sig_error));
                Log::printf("ble ota signature verification failed - mbedtls_ecdsa_from_keypair: %s (%d)\n", sig_error, sig_err);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                return;
            }

            sig_err = mbedtls_ecdsa_read_signature(&sig_ctx, hash, sizeof(hash), data, length);

            if (sig_err != 0) {
                mbedtls_strerror(sig_err, sig_error, sizeof(sig_error));
                Log::printf("ble ota signature verification failed - mbedtls_ecdsa_read_signature: %s (%d)\n", sig_error, sig_err);
                notify(false);
                image_size = 0;
                esp_ota_abort(ota_handle);
                return;
            }

            mbedtls_ecdsa_free(&sig_ctx);
            mbedtls_ecp_keypair_free(&sig_key);

            Log::printf("ble ota signature verification passed\n");

            esp_err_t err = esp_ota_end(ota_handle);
            if (err != ESP_OK) {
                Log::printf("ble ota failed to validate: %s (%d)\n", esp_err_to_name(err), err);
                notify(false);
                image_size = 0;
                return;
            }

            err = esp_ota_set_boot_partition(ota_partition);
            if (err != ESP_OK) {
                Log::printf("ble ota failed to switch partition: %s (%d)\n", esp_err_to_name(err), err);
                notify(false);
                image_size = 0;
                return;
            }

            Log::print("ble ota complete\n");
            notify(true);

            // We're in the BLE handler task here. We can't suspend it.
            // TODO: We should probably move more of the update process out.
            gracefulRestart();
        }

        void notify(bool success) {
            uint8_t value = success ? 1 : 0;
            characteristic->setValue(&value, 1);
            characteristic->notify();
        }

        BLECharacteristic *characteristic = nullptr;
        size_t image_size = 0;
        size_t bytes_written = 0;
        const esp_partition_t *ota_partition = nullptr;
        esp_ota_handle_t ota_handle;
        mbedtls_sha256_context sha_ctx;
    };

    BLECharacteristic *characteristic = service->createCharacteristic(X1_GATT_UUID_OTA_UPDATE, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
    characteristic->setCallbacks(new Callbacks());
    characteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);

    BLEDescriptor *description_descriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
    description_descriptor->setAccessPermissions(ESP_GATT_PERM_READ);
    description_descriptor->setValue("OTA Update");
    characteristic->addDescriptor(description_descriptor);

    BLE2902 *configuration_descriptor = new BLE2902();
    configuration_descriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENC_MITM);
    client_config_descriptors.push_back(configuration_descriptor);
    characteristic->addDescriptor(configuration_descriptor);

    return characteristic;
#else
    #warning "OTA_PUBLIC_KEY_X/Y not defined, OTA update will not be available"
    Log::print("signing key not defined, ota updates disabled\n");

    return nullptr;
#endif
}
