# ESP32S3_CAM

WiFi camera firmware for the **DFRobot AI Camera** (ESP32-S3 N16R8 + OV3660), built on ESP-IDF 6.1.
Everything runs from a browser: live view, camera tuning, log console, OTA updates and system monitoring.

<p align="center">
  <img src=".assets/Screenshot%202026-11-12%20230601.png" alt="Camera page: live MJPEG stream and image/exposure settings" width="32%">
  <img src=".assets/Screenshot%202026-11-12%20230544.png" alt="Web console: shell commands and live log" width="32%">
  <img src=".assets/Screenshot%202026-11-12%20230526.png" alt="System page: chip, flash, PSRAM, CPU load per core, memory and tasks" width="32%">
</p>

## Features

- **Live view** – MJPEG stream up to Full HD, snapshots, multiple viewers.
- **Camera settings** – resolution, JPEG quality, image tuning, effects, mirror/flip, auto/manual exposure and gain, night mode. Saved settings apply at every boot.
- **Web console** – the same Unix-style shell as on UART, plus the live device log.
- **System monitor** – chip and memory info, CPU load per core, per-task CPU and stack usage.
- **WiFi setup portal** – first boot opens the `ESP32S3-Config` hotspot to pick a network.
- **OTA updates** – upload from the browser or `tools/ota_push.py`; two slots with automatic rollback.
- **Files** – 4 MB LittleFS storage, browsable from the web page or the `fs` command.
- **Snapshot upload** – optional periodic JPEG upload to an HTTP endpoint.
- **Light sensor and LED** – LTR-308 ambient light readings and a PWM illumination LED.

## Hardware

| | |
|---|---|
| Board | DFRobot AI Camera (`boards/df_robot_ai_camera`) |
| SoC | ESP32-S3, 2 × LX7 @ 240 MHz |
| Memory | 16 MB flash (QIO), 8 MB octal PSRAM @ 80 MHz |
| Camera | OV3660, 8-bit DVP |
| Sensor | LTR-308ALS ambient light (I2C, shared with the camera's SCCB) |
| LED | PWM illumination LED on GPIO 47 |

Pin assignments are in [boards/df_robot_ai_camera/bsp.h](boards/df_robot_ai_camera/bsp.h).

## Build

The build runs in Docker. The container has **no USB access**: it only builds, you flash from the host.

```
docker network create docker-shared      # once
docker-compose build
docker-compose run -e LOCAL_USER_ID=$(id -u) idf bash
```

Inside the container:

```
make build board=df_robot_ai_camera              # debug build
make build board=df_robot_ai_camera debug=n      # release build
make test                                        # host unit tests
make install board=df_robot_ai_camera            # artefacts -> _deploy/<build_dir>/bin
```

Settings go in `software/application/config/proj.conf` or `boards/df_robot_ai_camera/sdkconfig.board`, never in the generated `sdkconfig`.
The firmware version (`X.Y.Z-<hash>`) comes from the nearest `X.Y.Z` git tag.

## Flash

Flashing runs on the host, because the build container has no USB access. You need
`esptool.py` on the host (`pip install esptool`).

After `make install`, the images are in `_deploy/_build_debug/bin/` (or `_build_release`).
From that folder:

```
esptool.py --chip esp32s3 -p <port> erase_region 0xe000 0x2000
esptool.py --chip esp32s3 -p <port> -b 921600 write_flash \
    0x0     bootloader.bin \
    0x8000  partition-table.bin \
    0x10000 esp32s3_cam.bin
```

`<port>` is the board's serial port: `/dev/ttyACM0` or `/dev/ttyUSB0` on Linux,
`COMx` on Windows.

The first command clears the OTA boot record, so the board boots the new firmware
and not an older one from the second OTA slot. WiFi settings and stored files are
kept.

You only need USB for the first flash; after that, use OTA (see below).

## Use

1. Connect to the **`ESP32S3-Config`** hotspot and pick your WiFi network at `http://192.168.4.1`.
2. Find the device IP in the serial log or with `wifi status`.
3. Open `http://<device-ip>/`. The raw stream is at `/camera/stream`.

Type `help` in the web console or serial monitor to list the commands.

**OTA:** upload `esp32s3_cam.bin` on the OTA page, or:

```
python tools/ota_push.py --ip <device-ip> --file _deploy/_build_release/bin/esp32s3_cam.bin
```
