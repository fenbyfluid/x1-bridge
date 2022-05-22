#pragma once

#include <string>

#define X1_GATT_UUID_BRIDGE_SVC      "00001000-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_BATTERY_VOLTAGE "00002000-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_SERIAL_DATA     "00002001-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_BT_SCAN         "00002002-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_BT_CONNECT      "00002003-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_CONFIG_NAME     "00002004-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_CONFIG_PIN_CODE "00002005-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_CONFIG_BT_ADDR  "00002006-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_DEBUG_LOG       "00002007-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_RESTART         "00002008-7858-48fb-b797-8613e960da6a"
#define X1_GATT_UUID_OTA_UPDATE      "00002009-7858-48fb-b797-8613e960da6a"

// BLE API:
//   X1_GATT_UUID_SERIAL_DATA
//     - Notify: received full command from connected BT SPP
//     - Write: send data to connected BT SPP
//
//   X1_GATT_UUID_BT_SCAN
//     - Read: current scan state
//     - Notify: on new device found
//     - Write: start / stop scan
//   X1_GATT_UUID_BT_CONNECT
//     - Read / Notify: current connection state
//     - Write: connect / disconnect
//
//   X1_GATT_UUID_CONFIG_NAME
//     - Read / Write: string tied to config, restart required
//   X1_GATT_UUID_CONFIG_PIN_CODE
//     - Write: u32 tied to config, restart required
//   X1_GATT_UUID_CONFIG_BT_ADDR
//     - Read / Write: u8[6] tied to config, restart required if already set
//
//   X1_GATT_UUID_BATTERY_VOLTAGE
//     - Read: current battery voltage
//   X1_GATT_UUID_DEBUG_LOG
//     - Notify: log write
//   X1_GATT_UUID_RESTART
//     - Write: restart module, param to erase config first
//   X1_GATT_UUID_OTA_UPDATE
//     - Write: ota update message
//     - Notify: ota update status

class BLEServer;
class BLEService;
class BLECharacteristic;

class Ble {
public:
    static void init(const std::string &name, uint32_t pinCode);
    static void deinit();
    static bool isClientConnected();
    static void updateBatteryLevel(uint8_t level, uint32_t millivolts);

private:
    static void initBatteryService(BLEServer *server);
    static void initBridgeService(BLEServer *server);
    static BLECharacteristic *createSerialDataCharacteristic(BLEService *service);
    static BLECharacteristic *createBluetoothScanCharacteristic(BLEService *service);
    static BLECharacteristic *createBluetoothConnectCharacteristic(BLEService *service);
    static BLECharacteristic *createBatteryVoltageCharacteristic(BLEService *service);
    static BLECharacteristic *createDebugLogCharacteristic(BLEService *service);
    static BLECharacteristic *createRestartCharacteristic(BLEService *service);
    static BLECharacteristic *createOtaUpdateCharacteristic(BLEService *service);
};
