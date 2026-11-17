# SASSI over TCP — notes

How Kaffelogic Studio (version 7.6.4) talks to a WiFi module over TCP, as far as we understand it. These notes were
written to make `kl-sassi` interoperate with Studio. They describe observed behaviour and message formats only;
no Studio code is reproduced here.

Sources are marked:

- **(Studio)** — read from how Studio 7.6.4 behaves/is built; not independently confirmed against a real module.
- **(measured)** — observed on our setup (real Studio ↔ `kl-sassi`, or the roaster ↔ `kl-msc`).
- **(guess)** — our interpretation, not confirmed.

Anything not listed here we do not know. Corrections are welcome.

## Packet format (Studio)

```
KL*<type>|<timestamp ms, hex>|<field 2>|...|<field N>|<crc>\r
```

- Field 1 is always the timestamp. Studio checks that the number of `|`-separated parts equals the number of
  fields defined for the type plus 2.
- CRC: CRC-16/XMODEM (poly `0x1021`, i.e. Python's `binascii.crc_hqx(body, crc_initial)`) over everything up to and
  including the last `|`, written as 4 lower-case hex digits. `crc_initial` is chosen by the device in field 11 of
  its CONNECTION_REQUEST (hex) and already applies to that packet.
- An empty packet (a bare `\r`) is ignored, but counts as "data received" for the idle timeout.
- Packet timestamps should follow Studio's clock (taken from TIME_SYNC). **(measured)** With the Pi's own clock,
  Studio reported a latency of several minutes.

## Handshake (Studio is the TCP client, port 9056)

1. Studio connects and sends `\r`. It expects a CONNECTION_REQUEST within 3.5 s.
2. Device → `KL*2` CONNECTION_REQUEST, fields:
   `platform | capabilities | serial | version (≥1) | model | domain | description | max_packet | max_filename | crc_initial`
   - platform: `1` RP2040, `2` AT90USB1287, `10` host (Studio itself), `11` TCP bridge. `kl-sassi` uses `11`.
   - capabilities: bit 5 rescue, bit 6 install, bit 7 "user info" → bits 8–15 carry a style
     (`0` Kaffelogic, `1` Nucleus; any other style makes Studio disconnect).
3. Studio → `KL*3` TIME_SYNC: `platform (10) | capabilities | date | version`. Device → `KL*4` (timestamp only).
   Studio now shows "Connected".
4. Studio → INFO_REQUEST code 9 (operational status), then starts syncing logs and profiles.

Keeping the connection:

- Studio drops the connection if it receives **nothing** for 15 s.
- With platform 11, a BRIDGE_NOTIFY (130) arriving more than 60 s after the last "refresh" makes Studio close and
  reconnect. A folder sync does the same (at most every 20 s).
- **(measured)** If that refresh happens during a live roast, the live view is lost. `kl-sassi` therefore sends a
  bare `\r` as keep-alive while a live log is open. With that, the connection stayed up for the whole roast and
  cooling (13 minutes), and Studio opened the final log on NOTIFY_STATUS 7.
- Close: `KL*115`, action 4, data 1.

## Message types (fields after the timestamp)

| # | name | fields |
|---|---|---|
| 1 | PACKET_ACK | — |
| 5 / 6 | DIR_LIST request / response | req: path, –, format · resp: path, outcome, format, seq, base64 |
| 7 / 8 | FILE request / response | req: path · resp: path, outcome, date_modified, seq, base64 |
| 9 / 10 | FILE_PUSH / ack | push: path, action (1 = save, `\|128` on the last part), date_modified, seq, base64 · ack: path, outcome |
| 11 / 12 | FILE_DELETE / ack | path, – · ack: path, outcome |
| 13 / 14 | INFO request / response | data, code (the response must carry the same code) |
| 15 / 16 | ACTION / ack | data, code, serial · ack: data, outcome, – |
| 30 | NOTIFY_STATUS (device → Studio) | data, code: `1` folder updated → Studio syncs · `6` busy · `7` not busy (data = path of the log, Studio then opens it) · `2` fault |
| 32 | NOTIFY_FILE (device → Studio) | path, outcome, –, seq, base64 — used for the **live log** |
| 109–131 | BRIDGE_* | same shapes, addressed to the module itself (WiFi credentials, RSSI as `rssi:-50` in 130 code 100, etc.) |

**Multi-part transfers.** Outcome `0` on intermediate parts, `128` on the last one. The receiver answers every
non-final part with `KL*1`. `seq` starts at 1. Payload per packet before base64:
`(max_packet − 57 − len(path)) * 3 // 4` bytes.

**Outcome codes.** `0` ok, `3` not found, `30` denied, `50` cannot save, `101` sequence error, `103` busy.

**Directory listing.** Lines separated by `\r`, columns by TAB: `mode` (space = file, `>` = directory), name,
date, size.

**Dates.** `YYYYMMDDwHHMMSS`, where `w` = ISO weekday % 7, in UTC.

**INFO data.** `name:value;name:value`. Keys Studio reads:

- operational status (9): `sassi_file_lock` (`0` = unlocked; anything else and Studio does not sync),
  `default_profile`, `profile_pack_version`
- system (3): `firmware`, `language_min`
- file system (4): `used`, `size`

"Roaster preferences" in Studio uses INFO 101 answered by the roaster itself, which a stick cannot do.

## Folder sync (Studio)

- Remote folders: `kaffelogic/roast-profiles` (`.kpro`) and `kaffelogic/roast-logs` (`.klog`, archive mode → kept
  locally in `roast-logs-archive`).
- Studio compares three lists — local, device, and a "base" from the previous sync — using **exact** equality of
  modification times.
- **(measured)** FAT stores times in 2-second steps, so after pushing a file with an odd-second time Studio
  downloads it once more; it settles after that.
- At the start Studio also requests the file `last_connection_uuid` (in the root) and rewrites it if it differs.
  `kl-sassi` keeps that file outside the stick image.

## Live log (measured)

The roaster writes the `.klog` **incrementally** during the roast: one line per second, a `WRITE(10)` roughly every
0.5 s (the data sector and the directory entry with the new file size). `kl-sassi` uses that:

1. A `WRITE(10)` in the last 20 s and the newest `logNNNN.klog` growing → NOTIFY_STATUS code 6 (busy).
2. NOTIFY_FILE (32) with the whole file so far — the first part must start with `log_file_name:` — then only new
   complete lines.
3. 20 s without writes → NOTIFY_STATUS code 7 with the log path. The path must be the same string as in the
   NOTIFY_FILE packets, otherwise Studio does not open the final log.

Studio displayed the live graph when it connected in the middle of a roast. A new connection during a roast
receives the whole file again.

## Discovery (Studio)

Studio tries, in order: the previous IP (setting `wifi_module_previous_ip`) → `192.168.4.1` if it is on the same
subnet → the ARP table (host name `KaffelogicWM` in the line, or MAC OUI `78-21-84` / `4c-d5-77`) →
`gethostbyname("KaffelogicWM")` (on Windows: DNS, then LLMNR/NetBIOS). Each candidate is probed with a TCP connect to
port 9056, timeout 1 s.

*Scan for Wifi module* is a plain TCP connect to port 9056 on .1–.254 of the PC's local subnets plus
`192.168.4.1`. **(measured)** In our setup the scan did not find the Pi, while the host-name route did.

Studio's `multicast` code coordinates several Studio instances; it is not used for discovery.

## The roaster side (measured, model KN1007B, older board)

- Full-speed USB host. Commands seen: `REQUEST SENSE`, `PREVENT ALLOW MEDIUM REMOVAL`, `TEST UNIT READY`,
  `READ(10)`, `WRITE(10)`. Never `INQUIRY` or `READ CAPACITY`.
- Rejects a FAT32 volume whose dirty flag (boot sector byte `0x41`) is set: "USB error / check device".
- Every `.klog` gets the modification time 2001-01-01 and contains no date; the roaster has no clock.
- Presented with a CDC-ACM serial device, the roaster resets it and assigns an address but never sends
  `SET_CONFIGURATION`. Other device classes were not tried.
