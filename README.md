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

The X1 Bridge firmware is designed to run on any ESP32-based development board. There is board-specific support for a voltage-based battery monitor and a LED connection status indicator (simple or RGB), but the primary functionality is all using built-in ESP32 peripherals.

Tested with:
* [Adafruit HUZZAH32 ESP32 Feather](https://www.adafruit.com/product/3405) (not V2!)
* [M5Stack Atom-Echo](https://shop.m5stack.com/products/atom-echo-smart-speaker-dev-kit)

See `platformio.ini` for build flags to specify board-specific functionality.

## Resources

* [X1 Protocol Documentation](https://github.com/buttplugio/stpihkal/pull/160)
* [PlatformIO Getting Started Tutorial](https://docs.platformio.org/en/latest/tutorials/espressif32/arduino_debugging_unit_testing.html)
* [HUZZAH32 Module Documentation](https://learn.adafruit.com/adafruit-huzzah32-esp32-feather)
* [Atom-Echo Module Documentation](https://docs.m5stack.com/en/atom/atomecho)
