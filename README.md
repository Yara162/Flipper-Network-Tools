# Flipper Wi-Fi Tools

A Wi-Fi utility for the Flipper Zero and its official Wi-Fi Devboard, made for AV
commissioning: check the wireless that presentation systems, touch panels and streaming
devices depend on, without getting a laptop out.

| Tool | What it tells you |
|---|---|
| Wi-Fi survey | Every network in range with channel, signal strength and security, plus a networks-per-channel chart |
| Signal walk test | Live signal strength of one network as you walk the room, with minimum, maximum and average |
| Join-the-network check | Whether you can join, how long it took, the address DHCP gave you, and whether the gateway, DNS and internet answer |
| Discovery check | Which AirPlay, Chromecast, Dante, NDI and similar services are visible on that network |

It has two parts: firmware for the devboard ([`esp32-firmware/`](esp32-firmware/)) and a
Flipper app that drives it ([`flipper-app/`](flipper-app/)). Prebuilt files are in
[`bin/`](bin/).

## Limits

- **2.4 GHz only.** The devboard's ESP32-S2 chip cannot see or join 5 GHz or 6 GHz networks.
- **No enterprise sign-in.** Networks using 802.1X (a username and password, or
  certificates) are listed but cannot be joined.
- **Up to 32 networks and 40 services** are kept per scan.

## Install

### 1. Flash the devboard

This replaces the devboard's stock debugger firmware. See "Going back" below to restore it.

1. Connect the devboard to a computer with a USB-C cable (not through the Flipper).
2. Put it in bootloader mode: hold **BOOT**, press and release **RESET**, then release **BOOT**.
3. Flash `bin/wifi-tools-devboard.bin` to address `0x0`, either way:
   - **In a browser:** open Espressif's web flasher, [esptool-js](https://espressif.github.io/esptool-js/),
     in Chrome or Edge. Connect, set the flash address to `0x0`, choose the file and program it.
   - **From a terminal:**

     ```bash
     python3 -m pip install esptool
     ```

     ```bash
     python3 -m esptool --chip esp32s2 write_flash 0x0 wifi-tools-devboard.bin
     ```

4. Press **RESET**, unplug the cable and attach the devboard to the Flipper.

### 2. Install the app

With qFlipper, copy `bin/wifi_tools.fap` to `SD Card/apps/GPIO/`. The app is built for
Flipper firmware 1.5.1 (API 88.2); rebuild it with
[ufbt](https://github.com/flipperdevices/flipperzero-ufbt) for other versions.

## Use

Open Apps → GPIO → Wi-Fi Tools. Each menu entry starts with a scan and a list of networks,
strongest first: channel, signal in dBm, a `*` for secured networks, and the name.

| Screen | Controls |
|---|---|
| Network list | Up/Down move, Left scans again, Right shows the channel chart. From "Wi-Fi survey", OK starts a walk test on the selected network and holding OK starts a join check. From the other menu entries, OK chooses the network. |
| Walk test | OK resets the minimum, maximum and average. Back stops. |
| Join check | When it finishes, OK runs discovery. Back leaves the network. |
| Discovery | Up/Down move through the services; the bottom lines show the selected one's address, port and host name. |

Signal quality on the walk test: Excellent is -55 dBm or better, Good to -67, Fair to -75,
Weak to -82, Poor below that.

The join check reports, in order: time to join and time for DHCP to answer; your address
and prefix length; the gateway and its ping time; the DNS server and whether it resolved
a name (`example.com`); and a ping to a public address (`1.1.1.1`) as the internet test.

### Passwords

Passwords are typed on the Flipper's keyboard and **saved in plain text** on the SD card
in `apps_data/wifi_tools/passwords.txt`, so they are offered again next time. The file
holds pairs of lines: a network name, then its password. You can edit it on a computer,
which is also the way to enter a password containing characters the Flipper's keyboard
lacks. Delete the file to forget them. The devboard itself never stores a password.

## Status

| Item | Status |
|---|---|
| Wi-Fi survey | Working on real hardware |
| Signal walk test | Working on real hardware |
| Join-the-network check | Not yet confirmed on hardware |
| Discovery check | Not yet confirmed on hardware |

## Going back

To restore the devboard's original firmware, follow Flipper's
[firmware update guide](https://docs.flipper.net/development/hardware/wifi-developer-board/update).

## Build

Devboard firmware, with [PlatformIO](https://platformio.org/):

```bash
cd esp32-firmware
```

```bash
pio run
```

Flipper app:

```bash
cd flipper-app
```

```bash
ufbt
```

## How the parts talk

The Flipper sends one text command per line over its UART (pins 13 and 14) at 115200 baud;
the devboard answers with lines starting with `$`. The protocol is described at the top of
[`esp32-firmware/src/main.cpp`](esp32-firmware/src/main.cpp).

## Licence

Copyright (C) 2026 Yara162. Released under the terms in [LICENSE](LICENSE).
