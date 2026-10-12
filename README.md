# CIBYP Micro

Portable **C11 device runtime** for Could I Be Your Partner. Desktop or Mini runs the Agent, models, memory and approvals; Micro executes explicitly granted hardware tools. It is not a standalone desktop-equivalent Agent.

![Checks](https://github.com/B5-Software/CIBYP-Micro/actions/workflows/check.yml/badge.svg)

Initial alpha: host simulator plus an **experimental ESP32-S3 / ESP-IDF reference port**. No actual SBC/MCU board has yet been qualified. Do not treat simulator tests or unsigned CI firmware as production hardware certification.

## Host simulation

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
npm ci
node scripts/connect-simulator.cjs ./build/micro-simulator /private/desk.json gpio:17 ./desk.journal
```

First create the matching device ID and `gpio_read` / `gpio_write` resource policy using Mini's `pair-device` or desktop Settings → Remote devices. The simulator exchanges exactly the same authenticated WebSocket profile and C parser as a real device. Its GPIO is simulated; its binary journal file is for testing, not a production storage port. To preserve simulator history, pass a stable journal path as documented by the bridge's usage output. Node/ws is required only for this host bridge/testing, not for the C device core.

## ESP32-S3

Install ESP-IDF 5.5.1, then:

```sh
cd ports/esp-idf
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Configure Wi-Fi, **WSS** gateway URL, device ID/token and an explicitly wired safe GPIO under CIBYP. GPIO defaults to disabled. ESP's certificate bundle verifies the server; SNTP establishes a clock before TLS. Local/private CAs need a deliberately provisioned certificate adaptation; disabling certificate/hostname validation is not supported. Menuconfig credentials are a development provisioning path and become part of the firmware; production provisioning must keep them out of source control and use appropriate protected storage.

The port reassembles both WebSocket fragments and split network reads within a 4 KiB message bound. Network callbacks enqueue into a bounded single-work queue; a worker handles parsing, NVS commits and tools. Overload closes/restarts the connection rather than accumulating actions. Disconnects drive GPIO LOW, local monotonic timers bound HIGH output, and stale connection epochs cannot start new writes. Telemetry reports free/minimum heap, worker stack watermark and reset reason every 30 seconds. A failed NVS journal is never automatically erased. Application OTA, Secure Boot/eFuse provisioning and standalone model calls are intentionally not implemented in this first Device profile.

## Portable core and custom tools

`include/cibyp_micro.h` exposes storage/GPIO HAL callbacks, fixed-buffer JSON parsing, fragment assembly and `cm_register_tool`. Provide atomic durable journal storage and a platform timer/watchdog in your port. A tool supplies its input schema, function pointer, context and `requires_lease`; validate device-specific parameters inside the callback. Result JSON must fit the supplied capacity. No remote code compilation or plugin execution occurs on the MCU. A simple semantic sensor example is in `examples/custom-tool.c`.

The gateway controls permissions; firmware tools cannot grant themselves access. Device calls carry operation identity, a persistent gateway identity, durable increasing sequence, lease/fencing data and remaining deadline. Results survive reboot. Interrupted actions return `RESULT_UNKNOWN`; confirmed records rotate under a persistent high-water mark, so retired requests cannot repeat a physical write. Unknown records are retained; 16 unresolved actions require manual inspection/recovery. Rebinding a different gateway requires deliberate local provisioning. This design does not claim physical exactly-once execution.

Measured on the Windows host compiler: runtime 21,768 bytes, journal 20,588 bytes, fragment assembler 4,128 bytes. The ESP port adds a pending frame, output buffer, worker stack, Wi-Fi, TLS and NVS; its total is **above** the draft 32 KiB protocol budget and remains a hardware measurement/optimization item. CI compiles ESP32-S3 and host platforms, and runs ASan/UBSan on Linux; fuzz smoke inputs exercise the bounded parser but do not replace extended fuzzing/endurance tests.

Protocol and SDK: [CIBYP-Remote](https://github.com/B5-Software/CIBYP-Remote). Gateway: [CIBYP-Mini](https://github.com/B5-Software/CIBYP-Mini). GPL-3.0-or-later.
