# X1 Bridge

X1 Bridge is ESP32 firmware which functions as a bridge between BLE capable devices and peripherals using legacy BT classic serial port profile (SPP).

It is primarily designed for use with [Scream Everyware](https://github.com/fenbyfluid/scream-everyware) to control the Scream Labs Model X1 from iOS devices.

## Features

* Generic BLE <-> BT SPP bridge
* Persistent configuration and device pairing
* Comprehensive BLE API for configuration and control
* Battery level monitoring and low-voltage shutoff
* Signed OTA firmware updates

## Supported Hardware

The X1 Bridge firmware is designed to run on the [Adafruit HUZZAH32 ESP32 Feather Board](https://www.adafruit.com/product/3405) (not V2!), but it should be easily adaptable to any other ESP32 module with minor code changes (primarily disabling the battery monitor).

There is no GPIO usage other than the LED and battery monitor, the primary functionality is all using built-in ESP32 peripherals.

## Resources

* [X1 Protocol Documentation](https://github.com/buttplugio/stpihkal/pull/160)
* [PlatformIO Getting Started Tutorial](https://docs.platformio.org/en/latest/tutorials/espressif32/arduino_debugging_unit_testing.html)
* [HUZZAH32 Module Documentation](https://learn.adafruit.com/adafruit-huzzah32-esp32-feather)
