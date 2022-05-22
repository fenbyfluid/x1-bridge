#include "ble.h"

#include "log.h"
#include "bluetooth.h"
#include "defaults.h"
#include "config.h"

#include <BLEDevice.h>
#include <BLE2902.h>
#include <BLE2904.h>

#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/error.h>

static void gracefulRestart() {
    xTaskCreatePinnedToCore([](void *) {
        vTaskDelay((1 * 1000) / portTICK_PERIOD_MS);

        // Gracefully clean up.
        Bluetooth::deinit();
        Ble::deinit();
        vTaskDelay((5 * 1000) / portTICK_PERIOD_MS);

        Log::print("cleanup complete, restarting\n");
        esp_restart();
    }, "restart", CONFIG_ESP_MAIN_TASK_STACK_SIZE, nullptr, 1, nullptr, CONFIG_ARDUINO_RUNNING_CORE);
}

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
    BLEDevice::deinit();
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
    // Enough handles need to be allocated for the characteristics and their descriptions.
    // If there aren't enough, things will start disappearing when querying the service.
    // TODO: Is failure indicated to the ESP_GATTS_ADD_CHAR_EVT event?
    BLEService *service = server->createService(BLEUUID(X1_GATT_UUID_BRIDGE_SVC), 30);

    createSerialDataCharacteristic(service);
    createBluetoothScanCharacteristic(service);
    createBluetoothConnectCharacteristic(service);
    battery_voltage = createBatteryVoltageCharacteristic(service);
    createDebugLogCharacteristic(service);
    createRestartCharacteristic(service);
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

            if (data[0] == 0) {
                Bluetooth::cancelScan();
                return;
            }

            Bluetooth::scan([=](const AdvertisedDevice &advertisedDevice) {
                // TODO: Filter to X1 devices using COD (0x1F00) and name prefix (SLMK1).
                //       Name: SLMK1xxxx, Address: 00:06:66:xx:xx:xx, cod: 7936, rssi: -60
                std::string name = advertisedDevice.name ? *advertisedDevice.name : "";
                const auto &address = advertisedDevice.address;
                Log::printf("new bt device: %s (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                    !name.empty() ? name.c_str() : "-unset-",
                    address[0], address[1], address[2], address[3], address[4], address[5]);

                std::vector<uint8_t> value(address.size() + name.size());
                auto after_address = std::copy(address.begin(), address.end(), value.begin());
                std::copy(name.begin(), name.end(), after_address);
                characteristic->setValue(value.data(), value.size());
                characteristic->notify();
            }, [=](bool canceled) {
                Log::printf("bluetooth discovery %s\n", canceled ? "canceled" : "completed");

                is_scanning = false;

                std::array<uint8_t, 6> null_address = {};
                characteristic->setValue(null_address.data(), null_address.size());
                characteristic->notify();
            });

            is_scanning = true;
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

            image_size = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4];

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
