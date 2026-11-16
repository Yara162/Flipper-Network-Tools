# Flipper Network Tools

A Wi-Fi utility for the Flipper Zero and its official Wi-Fi Devboard, made for AV
commissioning: check the wireless that presentation systems, touch panels and streaming
devices depend on, without getting a laptop out.

| Tool | What it tells you |
|---|---|
| Wi-Fi survey | Every network in range with channel, signal strength and security, plus a networks-per-channel chart |
| Signal walk test | Live signal strength of one network as you walk the room, with minimum, maximum and average |
| Join-the-network check | Whether you can join, how long it took, the address DHCP gave you, and whether the gateway, DNS and internet answer |
| Discovery check | Which AirPlay, Chromecast, Dante, NDI and similar services are visible on that network |
| Device finder | Every device on the subnet with its address, hardware address and maker, plus a check of common control ports on any one of them |
| Stability logger | Ping time, jitter, packet loss, roams and link drops over as long as you leave it running, saved as a log |
| Control client | Sends PJLink, TCP, UDP, HTTP and Wake-on-LAN commands from a list you keep on the SD card |
| Clock sync | Sets the Flipper's clock from a network time server |

The Wi-Fi tools have two parts: firmware for the devboard
([`esp32-firmware/`](esp32-firmware/)) and a Flipper app that drives it
([`flipper-app/`](flipper-app/)). A wired version for a W5500 Ethernet module is being
built in [`lan-app/`](lan-app/); see [LAN Tools](#lan-tools-wired) below. Prebuilt files
are in [`bin/`](bin/).

## Limits

- **2.4 GHz only.** The devboard's ESP32-S2 chip cannot see or join 5 GHz or 6 GHz networks.
- **No enterprise sign-in.** Networks using 802.1X (a username and password, or
  certificates) are listed but cannot be joined.
- **Up to 32 networks, 40 services and 96 devices** are kept per scan, and at most 12 services of any one type.
- **The device finder sweeps at most 254 addresses.** On a larger subnet it covers the /24 around the Flipper's own address and says so.
- **Use the device finder and port check only on networks you look after** or have permission to examine.

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

Open Apps → GPIO → Wi-Fi Tools. The first three tools work on any network in range. The
rest need a joined network: pick one from the list and enter its password, and the tool
starts once the join succeeds.

**The network stays joined** when you press Back to the menu, and the menu title shows
`On: <network>`. Every tool that needs a network then uses that one straight away. To
change network, run "Join-the-network check" on another one, or choose "Leave network".
A walk test also leaves the joined network.

| Screen | Controls |
|---|---|
| Network list | Up/Down move, Left scans again, Right shows the channel chart. From "Wi-Fi survey", OK starts a walk test on the selected network and holding OK joins it. From the other menu entries, OK chooses the network. |
| Walk test | OK resets the minimum, maximum and average. Back stops. |
| Join check | Back returns to the menu and stays joined. |
| Discovery | Up/Down move through the services; the bottom lines show the selected one's address, port and host name. |
| Device finder | Up/Down move; the bottom lines show the selected device's address and hardware address. OK checks its control ports. |
| Stability logger | Runs until you press Back, which saves the log. |
| Control client | Up/Down choose a command, OK sends it. On the result screen, OK sends it again. |
| Clock sync | Left/Right change the time zone, OK sets the Flipper's clock. |

Signal quality on the walk test: Excellent is -55 dBm or better, Good to -67, Fair to -75,
Weak to -82, Poor below that.

The join check reports, in order: time to join and time for DHCP to answer; your address
and prefix length; the gateway and its ping time; the DNS server and whether it resolved
a name (`example.com`); and a ping to a public address (`1.1.1.1`) as the internet test.

### Device finder

The sweep asks every address on the subnet to identify itself (an ARP request), which
finds devices even when they ignore ping. It takes about 15 seconds for a /24. Each device
is listed by the last part of its address and its maker, looked up from the hardware
address. Phones and laptops that randomise their hardware address show as "Private
address".

OK on a device tries to connect to a short list of control ports and names the ones that
are open: SSH, Telnet, web, RTSP, AMX, Samsung MDC, Q-SYS control, PJLink, Global Caché,
AirPlay, NEC, Chromecast, printer, LG, Sharp and Sony display control, and Crestron.

The device list is saved as `devices_<network>_<date>_<time>.txt`; port checks you make
afterwards are added to the same file.

### Stability logger

Pings the gateway once a second and charts the last two minutes: one column per ping,
taller for slower replies (60 ms or more fills the chart), dotted for a lost ping and
solid for a second spent offline. Below the chart are the latest, average and maximum
ping times, pings lost, jitter (the average change from one ping to the next), roams
between access points and link drops. If the link drops, the devboard rejoins by itself
and logging continues.

Back stops it and saves `stability_<network>_<date>_<time>.txt` with the totals and a
timed list of events: each roam, each run of lost pings and each link drop with its length.

### Control client

Commands live in `apps_data/wifi_tools/commands.txt` on the SD card, one per line as
`Name = type details`. The app creates the file with commented examples the first time
you open the control client.

| Type | Details | Example |
|---|---|---|
| `pjlink` | address, optional `password=...`, then the command | `Projector on = pjlink 192.168.1.50 POWR 1` |
| `tcp` | address:port, then the text to send | `DSP mute = tcp 192.168.1.70:23 DEVICE set mute 1\r\n` |
| `udp` | address:port, then the text to send | `Trigger = udp 192.168.1.90:5000 GO\r` |
| `http` | `GET` or `POST`, a URL, and for POST a body | `Preset 1 = http GET http://192.168.1.80/api/preset/1` |
| `wol` | a hardware (MAC) address | `Wake rack PC = wol AA:BB:CC:DD:EE:FF` |

In `tcp`, `udp` and POST text, `\r`, `\n`, `\t` and `\xNN` stand for the matching bytes,
so binary protocols such as Samsung MDC can be written as `\xAA\x11\x01...`. The result
screen shows OK or FAILED and the first part of the device's reply. Up to 24 commands are
read. `https://` URLs are accepted without checking the device's certificate, since AV
devices nearly always use self-signed ones.

### Clock sync

Asks `pool.ntp.org`, then `time.google.com`, then the network's own gateway for the time.
Network time carries no time zone, so the app works one out from how far the Flipper's
clock already is from it, to the nearest quarter hour, and shows it for you to check and
adjust with Left/Right before OK sets the clock. If the Flipper's clock was far off, the
zone is shown with a `?` and starts at UTC.

### Reports

All reports go to `apps_data/wifi_tools/` on the SD card. The device finder and stability
logger are described above. When a discovery check finishes, the app saves
`report_<network>_<date>_<time>.txt`, and the screen shows "saved".
The report holds the network name, the date and time from the Flipper's clock, the join
check results, and every service found, grouped by type with its name, address, port and
host name. A search stopped early with Back is not saved.

### Passwords

Passwords are typed on the Flipper's keyboard and **saved in plain text** on the SD card
in `apps_data/wifi_tools/passwords.txt`, so they are offered again next time. The file
holds pairs of lines: a network name, then its password. You can edit it on a computer,
which is also the way to enter a password containing characters the Flipper's keyboard
lacks. Delete the file to forget them. The devboard itself never stores a password.

## Status

Tested on a Flipper Zero with firmware 1.5.1 and the official Wi-Fi Devboard.

| Item | Status |
|---|---|
| Wi-Fi survey and channel chart | Working on real hardware |
| Signal walk test | Working on real hardware |
| Join-the-network check | Working on real hardware |
| Discovery check | Working on real hardware |
| Password file edited on a computer | Working on real hardware |
| Saved discovery reports | Working on real hardware |
| Staying joined between tools | Working on real hardware |
| Device finder | Working on real hardware |
| Stability logger | Working on real hardware |
| Clock sync | Working on real hardware |
| Control-port check on a found device | Not yet confirmed on hardware |
| Control client | Not yet confirmed on hardware |

Discovery was confirmed with AirPlay, Chromecast, HomeKit and SSH services. The test
network had no Dante or NDI devices, so those two service types have not been seen in
practice.

## LAN Tools (wired)

A second app, LAN Tools, brings the same checks to wired networks using a W5500 Ethernet
module wired straight to the Flipper. No devboard is involved: the Flipper runs the
networking itself.

**First version:** link state (speed and duplex) and the join-the-network check: DHCP,
gateway ping, DNS lookup and internet ping, with a saved report. The other tools
(discovery, device finder, stability logger, control client, clock sync) are still to
come, along with a wired-only one: reading the switch's name, port and VLAN.

### Wiring

| W5500 module pin | Flipper pin |
|---|---|
| 3.3V | 9 (3V3) |
| GND | 8 (GND) |
| MOSI | 2 (A7) |
| MISO | 3 (A6) |
| SCLK | 5 (B3) |
| SCS | 4 (A4) |
| RST, INT, 5V | leave unconnected |

Power the module from the 3.3 V pin only; do not also connect its 5 V pin.

### Use

Copy `bin/lan_tools.fap` to `SD Card/apps/GPIO/` and open LAN Tools. The app shows the
wiring until it finds the module, then the link state. Plug in a network cable and press
OK to run the check. The report is saved to
`SD Card/apps_data/lan_tools/lan_<date>_<time>.txt`.

The DHCP time shown includes about half a second the DHCP client spends checking that
nobody else already uses the address it was given.

### Status

Working on real hardware (Flipper Zero on firmware 1.5.1 with a W5500 module): wiring,
module detection, link speed and duplex, DHCP, gateway ping, DNS lookup, internet ping
and the saved report.

### How it is built

DHCP and DNS use WIZnet's [ioLibrary](https://github.com/Wiznet/ioLibrary_Driver) (MIT
licence, a copy of the needed files is in `lan-app/lib/ioLibrary/`). The W5500's built-in
networking cannot send pings, so ping and the address lookup it needs are built from raw
Ethernet frames in `lan-app/net.c`.

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

The maker table (`esp32-firmware/src/oui_table.h`) is generated from Wireshark's
manufacturer database:

```bash
python3 tools/make_oui.py manuf esp32-firmware/src/oui_table.h
```

Flipper apps (the same command in `flipper-app/` or `lan-app/`):

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

## Credits

Maker names come from the [Wireshark manufacturer database](https://www.wireshark.org/download/automated/data/manuf),
which is compiled from the IEEE's public registries.

## Licence

Copyright (C) 2026 Yara162. Released under the terms in [LICENSE](LICENSE).
