QUANTA WINDOWS FIRMWARE INSTALLER (ESP32-WROOM-32E-N8)

This is a double-click Windows launcher, not a self-contained EXE.
Requires Python 3 and internet on first launch to install esptool.

1. Connect ESP-PROG-2 PROG header to the Quanta board and connect USB to PC.
   Original ESP32 UART0: programmer TX -> GPIO3 RX; programmer RX -> GPIO1 TX;
   GND -> GND; IO0 -> GPIO0; EN -> EN. Use only 3.3 V logic.
2. Ensure board has a safe 3.3 V supply. Do not connect two power outputs.
3. Extract all ZIP files to a folder; double-click Install_Quanta.bat.
4. Select the COM port.
5. For new boards, choose FACTORY INSTALL. This erases ALL data.
6. For future firmware, choose Browse and select the new firmware.bin,
   then choose UPDATE FIRMWARE. It keeps NVS and SPIFFS, but resets OTA
   boot selection to app0. Do not use a BIN built for a different chip or
   partition layout. Updates require compatible app0 image <= 0x1E0000.

Firmware layout from provided partitions.bin:
  bootloader 0x1000; partitions 0x8000; app0 0x10000 (0x1E0000);
  app1 0x1F0000; NVS 0x9000; OTA metadata 0xE000; SPIFFS 0x3D0000.

NOTE: This package has not been tested with your physical board.
The bundled BIN files are those provided by the user and must be verified
as intended for Quanta. Back up any important settings before flashing.
