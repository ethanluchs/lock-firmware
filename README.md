# Bike Lock Firmware

Offline BLE lock for peer-to-peer bike sharing. Capstone, Fall 2026.

## Hardware
- ESP32 DEVKIT V1 (ESP-32S, 30-pin, micro USB) - LAFVIN kit
- USB-to-serial chip: CP2102 (Silicon Labs CP210x VCP driver required on Windows)

## Toolchain
- Arduino IDE
- Board: DOIT ESP32 DEVKIT V1
- esp32 core: 3.3.12
- Serial monitor: 115200 baud
- Port: COM6 (varies by machine)

## Notes
- Onboard LED is GPIO 2.
- If upload hangs on "Connecting....", hold BOOT, tap EN, release BOOT once flashing starts.
