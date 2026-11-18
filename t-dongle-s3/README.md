# KLDongle: the same idea on a LILYGO T-Dongle-S3

An Arduino port of this project for the [LILYGO T-Dongle-S3](https://github.com/Xinyuan-LilyGO/T-Dongle-S3)
(ESP32-S3 with a USB-A plug, a microSD slot and an APA102 LED). It plugs straight into the roaster's USB-A port,
so no case or cable is needed, and it is ready a few seconds after the roaster is switched on.

| | Pi Zero version | T-Dongle-S3 version |
|---|---|---|
| Stick storage | image file on the Pi's SD card | the dongle's microSD card itself (raw sectors) |
| Studio over WiFi (sync, live graph, send profile) | `kl-sassi` | port of `kl-sassi` (same protocol, same rules) |
| Web page (browse / download / upload / delete) | - | yes, port 80 |
| Roast start times | `roast-dates.json` | kept in flash, shown on the web page |
| Writes from Studio | off by default | on by default (`ALLOW_WRITE=0` turns them off) |
| FTP | - | not included |

> **New and lightly tested.** Built with esp32 core 3.3.13. So far: Studio sync over WiFi and the live log of an
> empty roast on a Nano 7 with the older board. Keep a copy of your stick's files.

## How it works

- **USB stick**: TinyUSB mass storage that reads and writes the microSD card's sectors directly.
  The roaster sees the card exactly as you formatted it.
- **Reading for Studio and the web page**: a fresh FatFs mount for every operation, so nothing stale is cached
  while the roaster writes underneath. All SD access, from the roaster and from the dongle, goes through one lock, so
  commands to the card never interleave.
- **Writing** (profile from Studio, web upload or delete, the `!ip` profile): the dongle soft-disconnects from the
  roaster, as if the stick were unplugged. It then writes through FatFs, unmounts, clears the FAT dirty flag
  (see the main README) and reconnects. The roaster only writes while roasting (one log file, a line appended
  every second), so writes are refused while the roaster wrote anything in the last 20 s. The stick is away for
  about a second: the window closes 1 s after the last write, and web uploads up to 64 KB are collected in RAM
  first. If the roaster tries to write while the stick is away, it is handed back immediately and the event is
  logged. Deleted files are moved to `/kl-trash/` on the card.
- **Roast detection**: a roast is running while the newest `logNNNN.klog` grows, or a newer log appears; 5 s without growth ends it
  (Studio then switches from the live view to the final log). The
  logs folder is only checked while the USB host has written recently. Plain write activity is not counted as a
  roast, because a computer as USB host writes all the time. The live graph, the roast start time, the status page
  and the LED all use this. Writes from the dongle are still refused after *any* host write in the last 20 s.
- **Studio**: TCP port 9056, the same SASSI handling as `kl-sassi`, including the live log (see
  [docs/PROTOCOL.md](../docs/PROTOCOL.md)). The dongle answers LLMNR and NetBIOS queries for `KaffelogicWM` and
  registers that name as its DHCP host name. Files Studio pushes outside the profile folder (such as
  `last_connection_uuid`) are kept in the dongle's flash, never on the card. Firmware files are refused, so a `kaffelogic/firmware` folder you keep on the card for the
  roaster's update mode is never touched.
- **`!ip` profile**: once WiFi is up, the dongle writes `kaffelogic/roast-profiles/!ip.kpro`, whose name is the
  dongle's IP address, so you can read the address on the roaster. It is only rewritten when the address changes.
  It is hidden from Studio. `IP_PROFILE=0` turns it off.

## Building (Arduino IDE)

1. Install the **esp32** boards package (Espressif, version 3.x).
2. Open `KLDongle/KLDongle.ino`. No extra libraries are needed.
3. Tools menu: Board **ESP32S3 Dev Module**, USB Mode **USB-OTG (TinyUSB)**, USB CDC On Boot **Disabled**,
   Flash Size **16MB**, PSRAM **Disabled**.
4. To flash, hold the button on the dongle while plugging it into the computer (download mode), then upload.
   Unplug and replug afterwards. In normal use the dongle's USB port is the stick, not a serial port.

## The SD card

- FAT32. Either copy the contents of your Kaffelogic stick onto a freshly formatted card, or clone the stick
  image onto it.
- The dongle never formats the card itself (a card that fails to mount once must not be wiped). With an
  unformatted card the LED blinks red and no `wifi_config.txt` can be created, but the dongle still appears as a USB
  disk: plug it into a computer, format the disk as FAT32 (MS-DOS FAT on a Mac), copy your stick's files onto it
  and replug. A formatted card without `wifi_config.txt` gets a template on the next start.
- Put `wifi_config.txt` in the root (it is created with defaults on first start if missing):

```
SSID=YourNetworkName
PSK=YourPassword
# optional
HOSTNAME=KaffelogicWM
SERIAL=KLDONGLE01
ALLOW_WRITE=1
IP_PROFILE=1
KEEPALIVE=bridge
```

The file is compatible with the older `USBESPKL` sketch; its `FTP_*` lines are ignored.

## Using it

- Plug it into the roaster. The profiles appear as with a normal stick.
- Browse to `http://<ip>/` (or `http://kaffelogicwm.local/`) for the file manager, status and `/log`.
- In Studio, enable *Options → Enable Wifi module discovery*. If Studio does not find the dongle, add
  `<ip> KaffelogicWM` to the PC's `hosts` file and give the dongle a fixed address in your router.

LED:

| Colour | Meaning |
|---|---|
| orange | starting |
| blinking green | USB stick working, no WiFi |
| blue | WiFi connected |
| cyan | Studio connected |
| breathing orange ↔ red | roasting (the newest log is growing) |
| yellow | writing to the card; the stick is briefly away from the roaster |
| blinking red | no SD card, or the card is not formatted (FAT32) |

## Limitations

The same as the Pi version (one Studio connection, no roaster preferences or commands). Also:

- Without PSRAM, pushes from Studio are limited to 256 KB (profiles are a few KB).
- The web page has no password. Use it on your own network only.
