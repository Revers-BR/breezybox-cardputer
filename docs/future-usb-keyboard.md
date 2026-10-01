# USB keyboard (not implemented)

Status: idea, not started. Recorded 2026-10-01.

Goal: type on a USB keyboard plugged into the Cardputer's USB-C port, the
same way a Bluetooth keyboard works today.

## What the hardware can do

- The ESP32-S3 has a USB OTG controller that can run as a USB host.
- ESP-IDF provides a USB host stack and an HID host class driver
  (`espressif/usb_host_hid`) that delivers keyboard reports.

## What stands in the way

1. **The port is the console.** The S3 has one USB PHY, on GPIO19/20. The
   firmware uses it as USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`):
   serial console, `idf.py monitor`, flashing without buttons. Host mode takes
   the PHY away from USB-Serial-JTAG while the keyboard is in use.
   - Flashing still works: hold G0 at power-on; the boot ROM always starts
     with USB-Serial-JTAG.
   - Serial logs during development would need a UART.
2. **VBUS power, unverified.** A host must supply 5 V to the device. The
   Cardputer's USB-C is wired as a charging input; whether it can output 5 V
   on battery is not known for either the original or the ADV.
   - Check: on battery, plug in a USB-C to USB-A OTG adapter and measure
     VBUS to GND on the USB-A socket.
   - If 0 V: a powered hub or a Y-cable that injects 5 V is required.
   - The port presents as a device (Rd pull-downs on CC). An OTG adapter
     handles this; a plain C-to-C cable to a keyboard may not.
3. **Memory.** USB host stack plus HID driver, estimated 10-20 KB, not
   measured. Fits alongside WiFi after the strip-rendering and 512-byte FAT
   changes. WiFi, Bluetooth and USB together would be tight.

## Proposed shape

- `usb kbd` (matching the `wifi <sub>` / `bt <sub>` command style) switches the
  port to host mode and starts the HID host driver.
- Key events go through `my_console_bt_receive`, the path the Bluetooth
  keyboard already uses, so the shell, `vi` and Lua get them unchanged.
- Reboot returns the port to USB-Serial-JTAG.

## First step

Measure VBUS on battery (item 2). It decides whether this works with a plain
OTG adapter or only through a powered hub.
