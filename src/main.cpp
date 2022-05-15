#include <Arduino.h>
#include <BLEDevice.h>
#include <BLE2902.h>
#include <BLE2904.h>
#include <BluetoothSerial.h>

BluetoothSerial SerialBT;

BLECharacteristic *g_pBatteryVoltageCharacteristic;
BLECharacteristic *g_pBatteryLevelCharacteristic;

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
//           notifications will be shared by all peers. We should be able to add our
//           own callbacks and copy the right state in/out there.
class MyBleServerCallbacks: public BLEServerCallbacks {
  virtual void onConnect(BLEServer *pServer) override {
    Serial.println("ble device connected");
  }

  void onDisconnect(BLEServer *pServer) override {
    Serial.println("ble device disconnected");

    // Reset the notifications / indications preference.
    auto batteryVoltageClientConfig = (BLE2902 *)g_pBatteryVoltageCharacteristic->getDescriptorByUUID(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_CLIENT_CONFIG));
    batteryVoltageClientConfig->setNotifications(false);
    batteryVoltageClientConfig->setIndications(false);

    auto batteryLevelClientConfig = (BLE2902 *)g_pBatteryLevelCharacteristic->getDescriptorByUUID(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_CLIENT_CONFIG));
    batteryLevelClientConfig->setNotifications(false);
    batteryLevelClientConfig->setIndications(false);

    // We have to restart advertising each time a client disconnects.
    pServer->getAdvertising()->start();
  }
} g_BleServerCallbacks;

// TODO: We're not using this to provide the value any more, just left as an example for now.
class MyBatteryBleCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *pCharacteristic, esp_ble_gatts_cb_param_t *param) override {
    Serial.printf("reading ble characteristic: %s\n", pCharacteristic->toString().c_str());

    // pCharacteristic->setValue(...);
  }
} g_BatteryBleCharacteristicCallbacks;

void setup()
{
  Serial.begin(115200);

  // When deploying from macOS, we're late to connect.
  delay(5 * 1000);

  // Initialize LED digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  xTaskCreateUniversal([](void *pvParameters) {
    for (;;) {
      // Turn the LED on (HIGH is the voltage level)
      digitalWrite(LED_BUILTIN, HIGH);

      delay(100);

      // Turn the LED off by making the voltage LOW
      digitalWrite(LED_BUILTIN, LOW);

      delay(2 * 1000);
    }
  }, "ledBlink", getArduinoLoopTaskStackSize(), NULL, 1, NULL, ARDUINO_RUNNING_CORE);

  xTaskCreateUniversal([](void *pvParameters) {
    for (;;) {
      uint32_t batteryMv = analogReadMilliVolts(A13) * 2;

      if (g_pBatteryVoltageCharacteristic) {
        g_pBatteryVoltageCharacteristic->setValue(batteryMv);
        g_pBatteryVoltageCharacteristic->notify();
      }

      // 4234 (mostly 4232) appears to be our actual max
      // 3218 seems to be the minimum seen when re-connecting usb after death
      // seeing as low as 3016 via ble after disconnecting again
      // got stuck at 3.9v charge after discharge test - needed a cold reboot
      float batteryPct = (((int32_t)batteryMv - 3200) / (4200.0 - 3200.0)) * 100;
      Serial.printf("battery (task): %dmV %.02f%%\n", batteryMv, batteryPct);

      if (g_pBatteryLevelCharacteristic) {
        uint8_t batteryLevel = constrain((int32_t)batteryPct, 0, 100);
        g_pBatteryLevelCharacteristic->setValue(&batteryLevel, 1);

        // TODO: Should we only notify when the value has actually changed?
        g_pBatteryLevelCharacteristic->notify();
      }

      // TODO: Tune the cutoff values.
      if (batteryMv < 3200) {
        // Gracefully clean up.
        SerialBT.end();
        BLEDevice::deinit(true);

        delay(5 * 1000);

        // TODO: We shouldn't need to do this - outputs have to be explicitly preserved during deep sleep.
        digitalWrite(LED_BUILTIN, LOW);

        if (batteryMv >= 3000) {
          // Initial safety cut off - wake up again after 10 minutes.
          // Otherwise we'll need a physical reset.
          esp_sleep_enable_timer_wakeup(10 * 60 * 1000000);
        }

        esp_deep_sleep_start();
      }

      // TODO: We probably don't need to run this very often at all.
      delay(60 * 1000);
    }
  }, "batteryMonitor", getArduinoLoopTaskStackSize(), NULL, 1, NULL, ARDUINO_RUNNING_CORE);

  // Sleep for another second to ensure the battery monitor has had a chance to run before we power up fully.
  delay(1000);

  BLEDevice::init("X1 Bridge");
  // BLEDevice::setPower(ESP_PWR_LVL_P9);

  // Calling this appears to require auth immediately, instead of when reading a protected characteristic.
  // BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT_MITM);

  // We use this as setSecurityCallbacks appears to opt in to a bunch of behaviour we don't want.
  // TODO: It's possible all the extra bits are events that are never called with our config though.
  BLEDevice::setCustomGapHandler([](esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    if (event != ESP_GAP_BLE_AUTH_CMPL_EVT) {
      return;
    }

    auto ev_param = param->ble_security.auth_cmpl;
    if (ev_param.success) {
      Serial.println("ble connection authorized");
      return;
    }

    // 81 bad pin
    // 85 cancel
    Serial.printf("ble connection auth failed, reason: %d\n", ev_param.fail_reason);

    // TODO: Can / should we kick off the peer?
  });

  BLESecurity *pSecurity = new BLESecurity();
  // TODO: Get from config.
  pSecurity->setStaticPIN(123456);
  // TODO: Allow bonding?
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM);

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(&g_BleServerCallbacks);

  const char *serviceUuid = "7c75bd31-7858-48fb-b797-8613e960da6a";
  BLEService *pService = pServer->createService(serviceUuid);

  g_pBatteryVoltageCharacteristic = pService->createCharacteristic("763dcccd-2473-43ec-92a0-ccf695f0e4cc", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_pBatteryVoltageCharacteristic->setCallbacks(&g_BatteryBleCharacteristicCallbacks);
  g_pBatteryVoltageCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);

  BLEDescriptor *batteryVoltageDescriptionDescriptor = new BLEDescriptor(BLEUUID((uint16_t)ESP_GATT_UUID_CHAR_DESCRIPTION));
  batteryVoltageDescriptionDescriptor->setAccessPermissions(ESP_GATT_PERM_READ);
  batteryVoltageDescriptionDescriptor->setValue("Battery Voltage (mV)");
  g_pBatteryVoltageCharacteristic->addDescriptor(batteryVoltageDescriptionDescriptor);

  // We're not really interested in notify support for this one, but it's a useful example for a protected characteristic for now.
  BLE2902 *batteryVoltageClientCharacteristicConfigurationDescriptor = new BLE2902();
  batteryVoltageClientCharacteristicConfigurationDescriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENC_MITM);
  g_pBatteryVoltageCharacteristic->addDescriptor(batteryVoltageClientCharacteristicConfigurationDescriptor);

  BLE2904 *batteryVoltagePresentationDescriptor = new BLE2904();
  batteryVoltagePresentationDescriptor->setAccessPermissions(ESP_GATT_PERM_READ);
  batteryVoltagePresentationDescriptor->setFormat(BLE2904::FORMAT_UINT32);
  batteryVoltagePresentationDescriptor->setExponent(-3);
  batteryVoltagePresentationDescriptor->setNamespace(1);
  batteryVoltagePresentationDescriptor->setUnit(0x2728); // volts
  batteryVoltagePresentationDescriptor->setDescription(0);
  g_pBatteryVoltageCharacteristic->addDescriptor(batteryVoltagePresentationDescriptor);

  pService->start();

  BLEService *pBatteryService = pServer->createService(BLEUUID((uint16_t)ESP_GATT_UUID_BATTERY_SERVICE_SVC));

  g_pBatteryLevelCharacteristic = pBatteryService->createCharacteristic(BLEUUID((uint16_t)ESP_GATT_UUID_BATTERY_LEVEL), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  g_pBatteryLevelCharacteristic->setCallbacks(&g_BatteryBleCharacteristicCallbacks);

  BLE2902 *batteryLevelClientCharacteristicConfigurationDescriptor = new BLE2902();
  batteryVoltagePresentationDescriptor->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  g_pBatteryLevelCharacteristic->addDescriptor(batteryLevelClientCharacteristicConfigurationDescriptor);

  pBatteryService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  // We probably only want to advertise our custom service for discovery.
  pAdvertising->addServiceUUID(serviceUuid);
  // This service gets doubled-up in the advertising data if we include it. It seems harmless though.
  // pAdvertising->addServiceUUID(BLEUUID((uint16_t)ESP_GATT_UUID_BATTERY_SERVICE_SVC));
  pAdvertising->start();

  // return;

  SerialBT.begin("X1 Bridge", true);

  // Do a discovery scan - we don't want to always do this, just during first time setup.
  SerialBT.discoverAsync([](BTAdvertisedDevice *pAdvertisedDevice) {
    Serial.printf("new bt device: %s\n", pAdvertisedDevice->toString().c_str());
  });

  // Discovery can only be done while disconnected, so give us a mo to see the results.
  // TODO: It doesn't seem possible to do a discovery scan after having ever connected to a device.
  delay(10 * 1000);

  // We don't actually need this - it just clears out our callback.
  SerialBT.discoverAsyncStop();

  SerialBT.onData([](const uint8_t *buffer, size_t size) {
    static uint8_t commandBuffer[32] = {};
    static size_t commandBufferCount = 0;

    for (size_t i = 0; i < size; ++i) {
      commandBuffer[commandBufferCount++] = buffer[i];

      if (buffer[i] == 0x0A) {
        Serial.printf("got %d byte response:", commandBufferCount);
        for (size_t i = 0; i < commandBufferCount; ++i) {
          Serial.printf(" 0x%02X", commandBuffer[i]);
        }
        Serial.println();

        commandBufferCount = 0;
      }
    }
  });

  Serial.println("connecting...");

  uint8_t bluetoothMac[] = { 0x00, 0x06, 0x66, 0xb9, 0xe7, 0x4f };

  bool connected = false;
  for (int i = 0; i < 10; ++i) {
    connected = SerialBT.connect(bluetoothMac);
    if (connected) {
      break;
    }

    Serial.println("failed to connect");
  }

  if (!connected) {
    Serial.println("connecting timed out");
    return;
  }

  Serial.println("connected");

  uint8_t getFirmwareVersionCommand[] = { 0x47, 0x73, 0x0A };
  SerialBT.write(getFirmwareVersionCommand, sizeof(getFirmwareVersionCommand));

  uint8_t getInfoCommand[] = { 0x47, 0x30, 0x0A };
  SerialBT.write(getInfoCommand, sizeof(getInfoCommand));

  // Wait for the responses to have arrived.
  delay(5 * 1000);

  Serial.println("disconnecting...");
  SerialBT.disconnect();
}

void loop()
{
  // Just sleep in the loop, all the work is done by tasks.
  // We don't want to just return as that'll waste CPU, but we need to wakeup
  // periodically so that the Arduino core can handle serial events (I think?)
  // delay(1000);

  // We don't need the `serialEvent` callback, so just end the loop task.
  vTaskDelete(NULL);
}
