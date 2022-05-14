#include <Arduino.h>
#include <BLEDevice.h>

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