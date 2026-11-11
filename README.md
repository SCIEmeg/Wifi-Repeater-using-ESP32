# ESP32 Wi-Fi Repeater (NAT router)

An ESP-IDF project that lets an ESP32 join an existing 2.4 GHz Wi-Fi network
as a station and expose a separate access point for client devices. Traffic
from AP clients is routed using IPv4 NAPT (NAT).

This is a router/repeater in the practical sense; it is **not** a transparent
Layer-2 bridge. Clients receive addresses in `192.168.4.0/24`.

## Features

- STA uplink with reconnects
- SoftAP with DHCP at `192.168.4.1`
- IPv4 forwarding and NAPT
- Browser setup page at `http://192.168.4.1`
- Persistent Wi-Fi settings in NVS
- Form validation and HTML escaping for saved SSIDs

## Defaults

| Setting | Value |
| --- | --- |
| AP SSID | `ESP32_Repeater` |
| AP password | `repeater123` |
| AP IP | `192.168.4.1` |

## Build and flash

Install ESP-IDF 5.x, open an ESP-IDF terminal in this directory, then run:

```powershell
idf.py set-target esp32
idf.py build
idf.py -p COM3 flash monitor
```

Connect a phone or computer to the default AP, open `http://192.168.4.1`,
enter the upstream Wi-Fi details, and save. The ESP32 restarts with the new
configuration. The uplink must be a 2.4 GHz Wi-Fi network.

## Hardware notes

- An original ESP32 has one 2.4 GHz radio, so AP and STA share a channel.
- NAPT needs enough free RAM; use a normal ESP32-WROOM/WROVER-class board.
- Change the default AP password before normal use.
