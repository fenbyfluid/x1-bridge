#include <Arduino.h>
#include <BLEDevice.h>
#include <BluetoothSerial.h>

BluetoothSerial SerialBT;

class MyBleServerCallbacks: public BLEServerCallbacks {
  void onDisconnect(BLEServer *pServer) override {
    // We have to restart advertising each time a client disconnects.
    pServer->getAdvertising()->start();
  }
} g_BleServerCallbacks;

class MyBatteryBleCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *pCharacteristic, esp_ble_gatts_cb_param_t *param) override {
    uint32_t batteryMv = analogReadMilliVolts(A13) * 2;
    pCharacteristic->setValue(batteryMv);

    // 4234 (mostly 4232) appears to be our actual max
    // 3218 seems to be the minimum seen when re-connecting usb after death
    // seeing as low as 3016 via ble after disconnecting again
    // got stuck at 3.9v charge after discharge test - needed a cold reboot
    float batteryPct = ((batteryMv - 3000) / (4200.0 - 3000.0)) * 100;
    Serial.printf("battery: %dmV %.02f%%\n", batteryMv, batteryPct);
  }
} g_BatteryBleCharacteristicCallbacks;

BLECharacteristic *g_pBatteryVoltageCharacteristic;

void setup()
{
  Serial.begin(115200);
  Serial.println("Hello, World!");

  // initialize LED digital pin as an output.
  pinMode(LED_BUILTIN, OUTPUT);

  BLEDevice::init("X1 Bridge");

  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(&g_BleServerCallbacks);

  const char *serviceUuid = "7c75bd31-7858-48fb-b797-8613e960da6a";

  BLEService *pService = pServer->createService(serviceUuid);

  g_pBatteryVoltageCharacteristic = pService->createCharacteristic("763dcccd-2473-43ec-92a0-ccf695f0e4cc", BLECharacteristic::PROPERTY_READ);
  g_pBatteryVoltageCharacteristic->setCallbacks(&g_BatteryBleCharacteristicCallbacks);

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(serviceUuid);
  pAdvertising->start();

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
  delay(10 * 1000);

  Serial.println("disconnecting...");
  SerialBT.disconnect();
}

void loop()
{
  // turn the LED on (HIGH is the voltage level)
  digitalWrite(LED_BUILTIN, HIGH);

  // wait for a second
  delay(1000);

  // turn the LED off by making the voltage LOW
  digitalWrite(LED_BUILTIN, LOW);
  
   // wait for a second
  delay(1000);
}