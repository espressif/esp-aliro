# Aliro Reader

## Overview

This example demonstrates an Aliro NFC reader using [M5Stack Unit NFC](https://docs.m5stack.com/en/unit/Unit_NFC) and compatible [M5Stack Controllers](https://shop.m5stack.com/collections/controllers).

## Hardware Required

An [M5Stack Controllers](https://shop.m5stack.com/collections/controllers) with:

- HY2.0-4P I2C Port.
- Chip/Target supported by [M5Unit-NFC](https://github.com/m5stack/M5Unit-NFC).

We have tested with below devices:

| Board                                                       | SDA   | SCL   |
| ----------------------------------------------------------- | ----- | ----- |
| [M5Stack NanoC6](https://docs.m5stack.com/en/core/M5NanoC6) | GPIO2 | GPIO1 |
| [M5Stack NanoH2](https://docs.m5stack.com/en/core/NanoH2)   | GPIO2 | GPIO1 |

> M5Stack Unit NFC uses fixed I2C address `0x50`; this example uses 400 kHz and polling mode. `CONFIG_ST25R3916_I2C_PORT` selects the ESP-IDF I2C controller.

## Prerequisite

We have developed this example based on ESP-IDF v5.5.3, but any ESP-IDF version `>=5.2,<6` should be fine.

## Build, Flash, Run

Please check below stuff before running the commands

1. Create or use existing `sdkconfig.defaults.BOARD` (e.g. `sdkconfig.defaults.nanoc6`).
2. Choose your chip for `TARGET` (e.g. `esp32c6` for `sdkconfig.defaults.nanoc6`).

```sh
cd /path/to/esp-aliro/examples/aliro_reader
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.BOARD" set-target TARGET build
idf.py -p <port> erase-flash flash monitor
```

## Validation

On boot, the reader should log a successful ST25R3916 identity read at I2C address `0x50`. When an ISO-14443-4 NFC-A user device is presented, the reader logs ATQA, UID, SAK, and ATS length, then starts the Aliro transaction.

> [!NOTE]
>
> [CSA aliro-actuator reference](https://github.com/aliro-access-control/aliro-actuator) is still in private access as of May 29 2026; request access by applying to CSA.

For end-to-end Aliro validation, we recommend using [CSA aliro-actuator reference](https://github.com/aliro-access-control/aliro-actuator).
