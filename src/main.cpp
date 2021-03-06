#include "config.h"
#include "ble.h"
#include "bluetooth.h"
#include "log.h"

#include <Arduino.h>

void startLedBlinkTask() {
    xTaskCreateUniversal([](void *pvParameters) {
        for (;;) {
            digitalWrite(LED_BUILTIN, HIGH);

            delay(100);

            digitalWrite(LED_BUILTIN, LOW);

            if (!Ble::isClientConnected()) {
                delay(2 * 1000);

                continue;
            }

            delay(150);

            digitalWrite(LED_BUILTIN, HIGH);

            delay(100);

            digitalWrite(LED_BUILTIN, LOW);

            delay(750);
        }
    }, "ledBlink", getArduinoLoopTaskStackSize(), NULL, 1, NULL, ARDUINO_RUNNING_CORE);
}

void checkBatteryLevel() {
    uint32_t millivolts = analogReadMilliVolts(A13) * 2;

    // 4234 (mostly 4232) appears to be our actual max
    // 3218 seems to be the minimum seen when re-connecting usb after death
    // seeing as low as 3016 via ble after disconnecting again
    // got stuck at 3.9v charge after discharge test - needed a cold reboot
    float percentage = (((int32_t)millivolts - 3200) / (4200.0 - 3200.0)) * 100;
    Log::printf("battery: %dmV %.02f%%\n", millivolts, percentage);

    uint8_t level = constrain((int32_t)percentage, 0, 100);
    Ble::updateBatteryLevel(level, millivolts);

    // TODO: Tune the cutoff values.
    if (millivolts < 3100) {
        Log::print("battery level low, going to deep sleep");

        // Gracefully clean up.
        Bluetooth::deinit();
        Ble::deinit();
        delay(5 * 1000);

        esp_deep_sleep_start();
    }
}

void startBatteryMonitorTask() {
    xTaskCreateUniversal([](void *pvParameters) {
        for (;;) {
            // TODO: We probably don't need to run this very often at all.
            delay(60 * 1000);

            checkBatteryLevel();
        }
    }, "batteryMonitor", getArduinoLoopTaskStackSize(), NULL, 1, NULL, ARDUINO_RUNNING_CORE);
}

void setup() {
    Log::print("hello, world\n");

    // Increase our priority so our init tasks don't get interrupted.
    vTaskPrioritySet(nullptr, 10);

    // Turn the LED on immediately so we know we're on.
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    delay(50);

    // Run immediately so that we skip startup if the voltage is too low.
    checkBatteryLevel();

    startBatteryMonitorTask();

    startLedBlinkTask();

    std::string name = Config::getName();
    uint32_t pin_code = Config::getPinCode();
    Log::printf("name: \"%s\", pin code: %06d\n", name.c_str(), pin_code);

    // The name is shared internally in the BT stack, so must be the same for both.
    Ble::init(name, pin_code);
    Bluetooth::init(name);

    // Run battery monitor again now BLE is up to populate the battery level characteristic.
    checkBatteryLevel();

    Log::print("ready\n");
}

void loop() {
    // Just sleep in the loop, all the work is done by tasks.
    // We don't want to just return as that'll waste CPU, but we need to wakeup
    // periodically so that the Arduino core can handle serial events (I think?)
    // delay(1000);

    // We don't need the `serialEvent` callback, so just end the loop task.
    vTaskDelete(nullptr);
}
