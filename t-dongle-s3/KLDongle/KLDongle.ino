/*
 * KLDongle - Kaffelogic Nano 7 (older board, USB-A stick port) on a LILYGO T-Dongle-S3
 *
 * The dongle plugs straight into the roaster's USB-A port and does three things at once:
 *   1. To the roaster it is a USB stick: raw sectors of the microSD card (USB MSC, TinyUSB).
 *   2. To Kaffelogic Studio it looks like the WiFi module (SASSI over TCP 9056): log/profile sync, live roast
 *      graph, sending profiles to the "roaster". Port of pi/kl-sassi.py; protocol notes in docs/PROTOCOL.md.
 *   3. A web page (port 80) to browse, download, upload and delete files on the card.
 *
 * Writing while the roaster has the stick: the roaster caches nothing we can invalidate, so (like the Pi version)
 * every write first unplugs the stick from the roaster (USB soft-disconnect), writes through FatFs, unmounts,
 * clears the FAT "dirty" flag and plugs the stick back in. Writes are refused (BUSY) while the roaster has written
 * to the stick in the last 90 s, i.e. while roasting. Reading is done with a fresh FatFs mount per operation.
 *
 * Arduino IDE settings (Tools menu), board "ESP32S3 Dev Module", esp32 core 3.x:
 *   USB Mode: "USB-OTG (TinyUSB)"     USB CDC On Boot: "Disabled"     Flash Size: 16MB     PSRAM: Disabled
 *   Partition Scheme: "16M Flash (3MB APP/9.9MB FATFS)" (size limit only; the table comes from partitions.csv)
 *   No extra libraries are needed.
 *
 * Configuration: wifi_config.txt in the root of the SD card (created with defaults if missing):
 *   SSID=...  PSK=...  HOSTNAME=KaffelogicWM  SERIAL=KLDONGLE01  ALLOW_WRITE=1  IP_PROFILE=1  KEEPALIVE=bridge
 *
 * Unofficial, not affiliated with Kaffelogic. Use at your own risk; keep a copy of your stick's files.
 */

#if ARDUINO_USB_MODE
#error "Set Tools > USB Mode to 'USB-OTG (TinyUSB)' - the USB stick needs TinyUSB"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#warning "USB CDC On Boot is enabled: the roaster will see a combined serial + storage device. 'Disabled' is recommended."
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <NetBIOS.h>
#include <Preferences.h>
#include <functional>
#include <vector>
#include <algorithm>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include "USB.h"
#include "USBMSC.h"
#include "tusb.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
extern "C" {          // same include order as the core's SD library (sd_diskio.cpp)
#include "ff.h"
#include "diskio.h"
#include "diskio_impl.h"
}
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "mbedtls/base64.h"
#include "webui.h"

#define KLDONGLE_VERSION "1.2.0"

// ------------------------------------------------------------------ T-Dongle-S3 pins
#define PIN_SD_CLK 12
#define PIN_SD_CMD 16
#define PIN_SD_D0  14
#define PIN_SD_D1  17
#define PIN_SD_D2  21
#define PIN_SD_D3  18
#define PIN_LED_DI 40   // APA102
#define PIN_LED_CI 39

// ------------------------------------------------------------------ constants
static const char* PROFILE_DIR = "kaffelogic/roast-profiles";
static const char* LOG_DIR     = "kaffelogic/roast-logs";
static const char* TRASH_DIR   = "kl-trash";            // deleted files are moved here (on the card, outside kaffelogic/)
static const char* CONFIG_FILE = "wifi_config.txt";
static const char* IP_PROFILE  = "kaffelogic/roast-profiles/!ip.kpro";

static const int      MAX_PACKET     = 8192;
// The roaster only writes while roasting (and cooling): one log file, a line appended every second. So a write in
// the last 20 s means a roast is in progress and the stick must stay with the roaster.
static const uint32_t BUSY_WINDOW_MS = 20000;   // roaster wrote within this time = roasting -> no writes, sassi_file_lock
static const uint32_t LIVE_WINDOW_MS = 20000;   // USB host wrote within this time = check whether a roast is running
static const uint32_t ROAST_DATE_GIVEUP_MS = 60000;   // stop trying to add roast_date this long after the last line
static const uint32_t ROAST_IDLE_MS  = 5000;    // newest log did not grow for this long = roast finished (it grows every second)
static const uint32_t WRITE_IDLE_MS  = 1000;    // write window closes after this much idle time (batches Studio pushes)
static const size_t   UPLOAD_RAM_MAX = 64 * 1024; // web uploads up to this size are buffered first -> shortest window
static const size_t   PUSH_MAX       = 256 * 1024;
static const size_t   OVERLAY_MAX_FILE = 2048;
static const size_t   OVERLAY_MAX_FILES = 8;

// packet types
enum {
  T_ACK = 1, T_CONN_REQ = 2, T_TIME_SYNC = 3, T_TIME_SYNC_ACK = 4,
  T_DIR_REQ = 5, T_DIR_RESP = 6, T_FILE_REQ = 7, T_FILE_RESP = 8, T_FILE_PUSH = 9, T_FILE_PUSH_ACK = 10,
  T_FILE_DEL = 11, T_FILE_DEL_ACK = 12, T_INFO_REQ = 13, T_INFO_RESP = 14, T_ACTION = 15, T_ACTION_ACK = 16,
  T_NOTIFY_STATUS = 30, T_NOTIFY_STATUS_ACK = 31, T_NOTIFY_FILE = 32, T_NOTIFY_FILE_ACK = 33,
  T_B_FILE_PUSH = 109, T_B_FILE_PUSH_ACK = 110, T_B_INFO_REQ = 113, T_B_INFO_RESP = 114, T_B_ACTION = 115, T_B_ACTION_ACK = 116,
  T_B_NOTIFY = 130, T_B_NOTIFY_ACK = 131
};
// outcomes
enum { OC_OK = 0, OC_NOT_VALID = 2, OC_NOT_FOUND = 3, OC_SYS_ERR = 4, OC_DENIED = 30, OC_CANNOT_SAVE = 50, OC_CANNOT_DELETE = 53,
       OC_SEQ_ERR = 101, OC_BUSY = 103, OC_LAST = 128 };
// info codes
enum { INFO_FOLDER_UPDATED = 1, INFO_SYSTEM = 3, INFO_FILESYS = 4, INFO_BUSY = 6, INFO_NOT_BUSY = 7,
       INFO_OPSTATUS = 9, INFO_BRIDGE_STATUS = 100 };
enum { ACT_CONN_CONTROL = 4, ACT_FS_CLEANUP = 110 };

// ------------------------------------------------------------------ types
struct Config {
  String ssid, psk;
  String hostname = "KaffelogicWM";
  String serial;                        // empty = the roaster's serial from its logs
  String firmware = "9.9.9";             // reported to Studio; "log" = the roaster's real version from its logs
  bool allowWrite = true;
  bool ipProfile = true;
  bool keepaliveCr = false;
  bool roastDate = true;
  int keepLogs = -1;                     // -1 = auto: 100 on internal flash, off on an SD card; 0 = off
};

struct Entry {
  String name;
  bool dir;
  uint32_t mtime;   // UTC epoch
  uint32_t size;
};

struct OvFile {      // "overlay": files Studio pushes outside the stick folders (kept in NVS, never on the card)
  String path;
  std::vector<uint8_t> data;
  uint32_t mtime;
};

typedef std::vector<String> Fields;
typedef std::function<int(uint32_t off, uint8_t* buf, size_t n)> Reader;
typedef std::function<Fields(int oc, int seq, const String& b64)> Mk;
typedef std::function<void(const FILINFO&)> DirCb;

// ------------------------------------------------------------------ globals
static Config cfg;
static sdmmc_card_t gCard;
static bool gSdOk = false;            // storage available (SD card or internal flash)
static bool gNoFs = false;            // storage present but no FAT file system (unformatted)
static SemaphoreHandle_t gSdLock;     // one storage transaction at a time (USB host vs. our FatFs)
enum { STORE_NONE, STORE_SD, STORE_FLASH };
static int gStore = STORE_NONE;       // SD card if one is inserted, otherwise the internal flash
static uint32_t gBlkSectors = 0;      // size in 512-byte sectors
static wl_handle_t gWl = WL_INVALID_HANDLE;
static size_t gWlSector = 4096;
static uint8_t gSecBuf[4096];         // read-modify-write buffer for the internal flash
static SemaphoreHandle_t gFsLock;     // recursive: FatFs mount/use
static SemaphoreHandle_t gLogLock;
static BYTE gPdrv = 0xFF;
static char gDrv[3] = "0:";
static FATFS gFs;
static int gFsDepth = 0;
static bool gFsOk = false;
static bool gWriteWin = false;        // stick detached from the roaster, FatFs mounted for writing
static uint32_t gLastFsWrite = 0;
static FIL gSassiFil, gWebFil, gWrFil, gUpFil, gTmpFil;
static bool gUploadOpen = false;          // upload streaming into a tmp file (large uploads only)
static bool gUpBuffering = false;         // upload still collecting in RAM
static std::vector<uint8_t> gUpBuf;
static String gUploadRel, gUploadTmp;
static int gUploadErr = OC_OK;
static uint32_t gWinAttempts = 0;         // gHostWriteAttempts when the current write window opened

static USBMSC MSC;
static bool gUsbStarted = false;
static volatile bool gUsbAttached = false;
static volatile uint32_t gLastHostWrite = 0;
static volatile bool gHostEverWrote = false;
static volatile uint32_t gHostWrites = 0, gHostReads = 0, gUsbErrors = 0, gHostWriteAttempts = 0;

static WebServer web(80);
static WiFiServer sassiSrv(9056);
static WiFiUDP llmnrUdp;
static Preferences gDates;            // roast start times (the roaster has no clock)
static Preferences gOvPrefs;
static std::vector<OvFile> gOverlay;
static bool gNetUp = false;
static IPAddress gLastIp;
static bool gIpProfileDone = false;
static uint16_t gCrc0 = 0;

static volatile bool gStudioConnected = false;
static char gStudioPeer[48] = "";
static char gLiveName[32] = "";
// Roast detector: a roast is running while the newest logNNNN.klog grows (or a newer one appears).
static volatile bool gRoasting = false;
static char gRoastName[32] = "";
static volatile uint32_t gRoastLastGrow = 0;
static String gPendName;                     // roast whose start is known only against uptime (no clock yet)
static uint32_t gPendStartMs = 0;

static char gLog[8192];
static size_t gLogHead = 0;
static bool gLogWrapped = false;

// SASSI buffers (one Studio session at a time, used only by the sassi task)
static char gRx[16384];
static size_t gRxLen = 0;
static uint8_t gChunk[6144];
static char gB64[8400];
static uint8_t gWebBuf[4096];

// ------------------------------------------------------------------ logging (RAM ring, shown at /log)
bool timeValid() { return time(nullptr) > 1700000000; }

void klog(const char* fmt, ...) {
  char line[300];
  int n;
  if (timeValid()) {
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    n = strftime(line, sizeof line, "%m-%d %H:%M:%S ", &tm);
  } else {
    n = snprintf(line, sizeof line, "+%lus ", (unsigned long)(millis() / 1000));
  }
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof line - n - 2, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  line[len] = 0;
  Serial.print(line);
  if (xSemaphoreTake(gLogLock, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < len; i++) {
      gLog[gLogHead++] = line[i];
      if (gLogHead >= sizeof gLog) { gLogHead = 0; gLogWrapped = true; }
    }
    xSemaphoreGive(gLogLock);
  }
}

String logText() {
  String s;
  xSemaphoreTake(gLogLock, portMAX_DELAY);
  s.reserve(sizeof gLog);
  if (gLogWrapped)
    for (size_t i = gLogHead; i < sizeof gLog; i++) s += gLog[i];
  for (size_t i = 0; i < gLogHead; i++) s += gLog[i];
  xSemaphoreGive(gLogLock);
  return s;
}

// ------------------------------------------------------------------ APA102 status LED
void setLED(uint8_t r, uint8_t g, uint8_t b, uint8_t brightness) {
  auto out = [](uint8_t v) {
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_LED_DI, (v >> i) & 1);
      digitalWrite(PIN_LED_CI, LOW);
      digitalWrite(PIN_LED_CI, HIGH);
    }
  };
  for (int i = 0; i < 4; i++) out(0x00);                  // start frame
  out(0xE0 | (brightness & 0x1F));
  out(b); out(g); out(r);
  for (int i = 0; i < 4; i++) out(0xFF);                  // end frame
}

void initLED() {
  pinMode(PIN_LED_DI, OUTPUT);
  pinMode(PIN_LED_CI, OUTPUT);
  setLED(0, 0, 0, 0);
}

// ------------------------------------------------------------------ dates
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yoe + era * 400) + (m <= 2);
}

uint32_t mkEpoch(int y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s) {
  return (uint32_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

uint32_t fatToEpoch(WORD fdate, WORD ftime) {
  unsigned mo = (fdate >> 5) & 15, d = fdate & 31;
  unsigned s = (ftime & 31) * 2;
  if (mo < 1 || mo > 12) mo = 1;
  if (d < 1) d = 1;
  if (s > 59) s = 59;
  return mkEpoch(1980 + (fdate >> 9), mo, d, (ftime >> 11) % 24, ((ftime >> 5) & 63) % 60, s);
}

void epochToFat(uint32_t t, WORD& fdate, WORD& ftime) {
  int y; unsigned mo, d;
  civilFromDays(t / 86400, y, mo, d);
  uint32_t r = t % 86400;
  if (y < 1980) { y = 1980; mo = 1; d = 1; r = 0; }
  fdate = (WORD)(((y - 1980) << 9) | (mo << 5) | d);
  ftime = (WORD)(((r / 3600) << 11) | (((r / 60) % 60) << 5) | ((r % 60) / 2));
}

// YYYYMMDDwHHMMSS, w = ISO weekday % 7 (Sunday = 0), UTC
String sassiDt(uint32_t t) {
  int64_t days = t / 86400;
  int y; unsigned mo, d;
  civilFromDays(days, y, mo, d);
  uint32_t r = t % 86400;
  unsigned w = (unsigned)((days + 4) % 7);                // 1970-01-01 was a Thursday
  char b[20];
  snprintf(b, sizeof b, "%04d%02u%02u%u%02lu%02lu%02lu", y, mo, d, w,
           (unsigned long)(r / 3600), (unsigned long)((r / 60) % 60), (unsigned long)(r % 60));
  return String(b);
}

bool parseDt(const String& s, uint32_t& t) {
  if (s.length() < 15) return false;
  for (int i = 0; i < 15; i++) if (!isdigit((unsigned char)s[i])) return false;
  int y = s.substring(0, 4).toInt();
  int mo = s.substring(4, 6).toInt(), d = s.substring(6, 8).toInt();
  int h = s.substring(9, 11).toInt(), mi = s.substring(11, 13).toInt(), se = s.substring(13, 15).toInt();
  if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59) return false;
  t = mkEpoch(y, mo, d, h, mi, se);
  return true;
}

// ------------------------------------------------------------------ CRC-16/XMODEM (poly 0x1021)
uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc) {
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

// ------------------------------------------------------------------ SD card (raw sectors, shared by USB and FatFs)
bool sdInit() {
  struct { int width; int khz; } tries[] = { {4, SDMMC_FREQ_DEFAULT}, {1, SDMMC_FREQ_DEFAULT}, {4, SDMMC_FREQ_PROBING} };
  for (auto& t : tries) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = t.khz;
    host.flags &= ~(SDMMC_HOST_FLAG_DDR | SDMMC_HOST_FLAG_8BIT);
    if (t.width == 1) host.flags &= ~SDMMC_HOST_FLAG_4BIT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = t.width;
    slot.clk = (gpio_num_t)PIN_SD_CLK;
    slot.cmd = (gpio_num_t)PIN_SD_CMD;
    slot.d0 = (gpio_num_t)PIN_SD_D0;
    slot.d1 = (gpio_num_t)PIN_SD_D1;
    slot.d2 = (gpio_num_t)PIN_SD_D2;
    slot.d3 = (gpio_num_t)PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t e = sdmmc_host_init();
    if (e == ESP_OK) e = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (e == ESP_OK) e = sdmmc_card_init(&host, &gCard);
    if (e == ESP_OK) {
      klog("SD: %d-bit, %d kHz, %lu sectors of %d bytes", t.width, t.khz,
           (unsigned long)gCard.csd.capacity, gCard.csd.sector_size);
      return true;
    }
    klog("SD init (%d-bit, %d kHz) failed: %s", t.width, t.khz, esp_err_to_name(e));
    sdmmc_host_deinit();
    delay(100);
  }
  return false;
}

// Internal flash (no card inserted): the "ffat" partition through ESP-IDF wear levelling. Flash is erased in 4 KB
// sectors but the roaster expects 512-byte sectors, so a write is read-modify-erase-write of each 4 KB sector it
// touches - skipped when the data is unchanged (the roaster rewrites the same FAT sectors a lot).
bool flashInit() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  if (!p) { klog("no internal storage partition (see partitions.csv)"); return false; }
  esp_err_t e = wl_mount(p, &gWl);
  if (e != ESP_OK) { klog("internal storage: wear levelling failed: %s", esp_err_to_name(e)); return false; }
  gWlSector = wl_sector_size(gWl);
  if (gWlSector > sizeof gSecBuf || gWlSector % 512) { klog("internal storage: unexpected sector size %u", (unsigned)gWlSector); return false; }
  gBlkSectors = wl_size(gWl) / 512;
  klog("no SD card - using internal flash: %lu KB", (unsigned long)(gBlkSectors / 2));
  return true;
}

static esp_err_t flashWriteLocked(const uint8_t* src, uint32_t lba, uint32_t count) {
  size_t off = (size_t)lba * 512, len = (size_t)count * 512;
  while (len) {
    size_t sec = off / gWlSector * gWlSector, in = off - sec;
    size_t n = len < gWlSector - in ? len : gWlSector - in;
    esp_err_t e = wl_read(gWl, sec, gSecBuf, gWlSector);
    if (e != ESP_OK) return e;
    if (memcmp(gSecBuf + in, src, n) != 0) {                  // unchanged data: no erase, no wear
      memcpy(gSecBuf + in, src, n);
      if ((e = wl_erase_range(gWl, sec, gWlSector)) != ESP_OK) return e;
      if ((e = wl_write(gWl, sec, gSecBuf, gWlSector)) != ESP_OK) return e;
    }
    off += n; src += n; len -= n;
  }
  return ESP_OK;
}

esp_err_t blkRead(void* buf, uint32_t lba, uint32_t count) {
  if ((uint64_t)lba + count > gBlkSectors) return ESP_ERR_INVALID_SIZE;
  xSemaphoreTake(gSdLock, portMAX_DELAY);
  esp_err_t e = gStore == STORE_SD ? sdmmc_read_sectors(&gCard, buf, lba, count)
                                   : wl_read(gWl, (size_t)lba * 512, buf, (size_t)count * 512);
  xSemaphoreGive(gSdLock);
  return e;
}

esp_err_t blkWrite(const void* buf, uint32_t lba, uint32_t count) {
  if ((uint64_t)lba + count > gBlkSectors) return ESP_ERR_INVALID_SIZE;
  xSemaphoreTake(gSdLock, portMAX_DELAY);
  esp_err_t e = gStore == STORE_SD ? sdmmc_write_sectors(&gCard, buf, lba, count)
                                   : flashWriteLocked((const uint8_t*)buf, lba, count);
  xSemaphoreGive(gSdLock);
  return e;
}

// FatFs disk driver on top of blkRead/blkWrite (so FatFs and the USB host never interleave on the storage)
static DSTATUS dioInit(unsigned char pdrv) { return 0; }
static DSTATUS dioStatus(unsigned char pdrv) { return 0; }
static DRESULT dioRead(unsigned char pdrv, unsigned char* buff, uint32_t sector, unsigned count) {
  return blkRead(buff, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}
static DRESULT dioWrite(unsigned char pdrv, const unsigned char* buff, uint32_t sector, unsigned count) {
  return blkWrite(buff, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}
static DRESULT dioIoctl(unsigned char pdrv, unsigned char cmd, void* buff) {
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*)buff = gBlkSectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
    case CTRL_TRIM: return RES_OK;
  }
  return RES_ERROR;
}
static const ff_diskio_impl_t gDiskio = { dioInit, dioStatus, dioRead, dioWrite, dioIoctl };

static uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool isVbr(const uint8_t* b) {
  return (b[0] == 0xEB || b[0] == 0xE9) && b[510] == 0x55 && b[511] == 0xAA && le16(b + 11) == 512;
}

// The roaster shows "USB error / check device" if the FAT "dirty" flag is set (see README). Clear it.
void clearDirtyFlag() {
  uint8_t* b = (uint8_t*)heap_caps_malloc(512, MALLOC_CAP_DMA);
  if (!b) return;
  uint32_t lba = 0;
  if (blkRead(b, 0, 1) == ESP_OK) {
    if (!isVbr(b) && b[510] == 0x55 && b[511] == 0xAA) {            // MBR: first FAT partition
      for (int i = 0; i < 4; i++) {
        uint8_t type = b[0x1BE + 16 * i + 4];
        if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0B || type == 0x0C || type == 0x0E) {
          lba = le32(b + 0x1BE + 16 * i + 8);
          break;
        }
      }
      if (lba == 0 || blkRead(b, lba, 1) != ESP_OK) { free(b); return; }
    }
    if (isVbr(b)) {
      int off = le16(b + 22) == 0 ? 0x41 : 0x25;                     // FAT32 : FAT12/16
      if (b[off]) {
        klog("FAT dirty flag was 0x%02x - cleared", b[off]);
        b[off] = 0;
        blkWrite(b, lba, 1);
      }
    }
  }
  free(b);
}

// ------------------------------------------------------------------ USB mass storage (the roaster's stick)
static int32_t mscRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!gUsbAttached) return -1;
  gHostReads++;
  if (blkRead(buffer, lba, bufsize / 512) == ESP_OK) return bufsize;
  gUsbErrors++;
  return -1;
}

static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  gLastHostWrite = millis();                    // recorded even if refused: a write attempt ends any write window
  gHostEverWrote = true;
  gHostWriteAttempts++;
  if (!gUsbAttached) { gUsbErrors++; return -1; }
  gHostWrites++;
  if (blkWrite(buffer, lba, bufsize / 512) == ESP_OK) return bufsize;
  gUsbErrors++;
  return -1;
}

static bool mscStartStop(uint8_t power_condition, bool start, bool load_eject) { return true; }

bool hostBusy(uint32_t windowMs) { return gHostEverWrote && (millis() - gLastHostWrite) < windowMs; }

void usbDetach() {
  if (!gUsbStarted) return;
  gUsbAttached = false;
  MSC.mediaPresent(false);
  tud_disconnect();
}

void usbAttach() {
  if (!gUsbStarted) return;
  MSC.mediaPresent(true);
  gUsbAttached = true;
  tud_connect();
}

// ------------------------------------------------------------------ FatFs access
// Every use takes the lock and (outside a write window) mounts fresh, so we never act on stale cached sectors
// while the roaster writes underneath us.
struct FsUse {
  bool ok;
  FsUse() {
    xSemaphoreTakeRecursive(gFsLock, portMAX_DELAY);
    if (gFsDepth++ == 0 && !gWriteWin) gFsOk = f_mount(&gFs, gDrv, 1) == FR_OK;
    ok = gFsOk;
  }
  ~FsUse() {
    if (--gFsDepth == 0 && !gWriteWin) f_mount(nullptr, gDrv, 0);
    xSemaphoreGiveRecursive(gFsLock);
  }
};

String fp(const String& rel) { return String(gDrv) + "/" + rel; }

// "a//b/./c" -> "a/b/c"; false if it contains ".."
bool normPath(const String& in, String& out) {
  String s = in;
  s.replace("\\", "/");
  out = "";
  int i = 0;
  while (i <= (int)s.length()) {
    int j = s.indexOf('/', i);
    if (j < 0) j = s.length();
    String part = s.substring(i, j);
    if (part == "..") return false;
    if (part.length() && part != ".") {
      if (out.length()) out += "/";
      out += part;
    }
    i = j + 1;
  }
  return true;
}

String lower(String s) { s.toLowerCase(); return s; }

bool inImage(const String& path) {
  String l = lower(path);
  for (const char* d : { PROFILE_DIR, LOG_DIR })
    if (l == d || l.startsWith(String(d) + "/")) return true;
  return false;
}

String parentOf(const String& rel) { int k = rel.lastIndexOf('/'); return k < 0 ? String("") : rel.substring(0, k); }
String baseOf(const String& rel) { int k = rel.lastIndexOf('/'); return k < 0 ? rel : rel.substring(k + 1); }

bool fatForEach(const String& rel, const DirCb& cb) {
  FsUse u;
  if (!u.ok) return false;
  FF_DIR dir;
  FILINFO fno;
  if (f_opendir(&dir, fp(rel).c_str()) != FR_OK) return false;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0]) cb(fno);
  f_closedir(&dir);
  return true;
}

bool fatStat(const String& rel, Entry& e) {
  FsUse u;
  if (!u.ok) return false;
  if (rel.length() == 0) { e.name = ""; e.dir = true; e.mtime = 0; e.size = 0; return true; }
  FILINFO fno;
  if (f_stat(fp(rel).c_str(), &fno) != FR_OK) return false;
  e.name = fno.fname;
  e.dir = fno.fattrib & AM_DIR;
  e.mtime = fatToEpoch(fno.fdate, fno.ftime);
  e.size = (uint32_t)fno.fsize;
  return true;
}

// first n bytes (or fewer) of a file
bool fatReadHead(const String& rel, String& out, size_t n) {
  FsUse u;
  if (!u.ok) return false;
  if (f_open(&gWrFil, fp(rel).c_str(), FA_READ) != FR_OK) return false;
  size_t sz = f_size(&gWrFil);
  if (sz < n) n = sz;
  out = "";
  out.reserve(n + 1);
  UINT br;
  char buf[256];
  while (n > 0 && f_read(&gWrFil, buf, n < sizeof buf ? n : sizeof buf, &br) == FR_OK && br > 0) {
    for (UINT i = 0; i < br; i++) out += buf[i];
    n -= br;
  }
  f_close(&gWrFil);
  return true;
}

// Cached for a minute: without a valid FSInfo sector FatFs scans the whole FAT, which is slow on a big card.
bool fatUsage(uint64_t& used, uint64_t& size) {
  static uint64_t cUsed = 0, cSize = 0;
  static uint32_t cAt = 0;
  static bool have = false;
  if (!have || millis() - cAt > 60000) {
    FsUse u;
    if (!u.ok) return false;
    DWORD fre;
    FATFS* fs;
    if (f_getfree(gDrv, &fre, &fs) != FR_OK) return false;
#if FF_MAX_SS != FF_MIN_SS
    uint64_t cs = (uint64_t)fs->csize * fs->ssize;
#else
    uint64_t cs = (uint64_t)fs->csize * FF_MAX_SS;
#endif
    cSize = (uint64_t)(fs->n_fatent - 2) * cs;
    cUsed = cSize - (uint64_t)fre * cs;
    cAt = millis();
    have = true;
  }
  used = cUsed;
  size = cSize;
  return true;
}

// newest logNNNN.klog (highest number) in LOG_DIR
bool newestLog(String& name, uint32_t& size) {
  long best = -1;
  fatForEach(LOG_DIR, [&](const FILINFO& f) {
    if (f.fattrib & AM_DIR) return;
    String n = lower(String(f.fname));
    if (!n.startsWith("log") || !n.endsWith(".klog") || n.length() <= 8) return;
    String num = n.substring(3, n.length() - 5);
    for (size_t i = 0; i < num.length(); i++) if (!isdigit((unsigned char)num[i])) return;
    long v = num.toInt();
    if (v > best) { best = v; name = f.fname; size = (uint32_t)f.fsize; }
  });
  return best >= 0;
}

// ------------------------------------------------------------------ write window
// Begin: unplug the stick from the roaster and mount for writing. Must be called with FsUse held.
int beginWriteQuiet(uint32_t quietMs) {
  if (gWriteWin) return OC_OK;
  if (hostBusy(quietMs)) return OC_BUSY;
  uint32_t attempts = gHostWriteAttempts;
  usbDetach();
  delay(100);                                   // let an in-flight USB transfer finish
  if (gHostWriteAttempts != attempts) {         // the roaster started writing just now: give the stick straight back
    usbAttach();
    klog("roaster wrote while detaching - write cancelled, stick returned");
    return OC_BUSY;
  }
  gWinAttempts = attempts;
  gWriteWin = true;
  gFsOk = f_mount(&gFs, gDrv, 1) == FR_OK;      // remount: nothing cached from before the detach
  if (!gFsOk) {
    klog("mount for writing failed - giving the stick back");
    gWriteWin = false;
    usbAttach();
    return OC_SYS_ERR;
  }
  gLastFsWrite = millis();
  klog("stick detached from the roaster for writing");
  return OC_OK;
}

int beginWriteInternal() { return beginWriteQuiet(BUSY_WINDOW_MS); }

int beginWrite() {
  if (!cfg.allowWrite && !gWriteWin) {
    klog("write requested but ALLOW_WRITE=0 -> denied");
    return OC_DENIED;
  }
  return beginWriteInternal();
}

// End: unmount, clear the dirty flag, plug the stick back in. Called periodically; force = now.
void finishWrite(bool force) {
  if (xSemaphoreTakeRecursive(gFsLock, force ? portMAX_DELAY : 0) != pdTRUE) return;
  if (gWriteWin && gFsDepth == 0) {
    uint32_t idle = millis() - gLastFsWrite;
    bool hostWants = gUsbStarted && gHostWriteAttempts != gWinAttempts;   // roaster tried to write: return it now
    bool go = force || hostWants || (idle >= WRITE_IDLE_MS && (!gUploadOpen || idle >= 15000));
    if (go) {
      if (hostWants) klog("WARNING: the roaster tried to write while the stick was away - returning it now");
      if (gUploadOpen) {
        f_close(&gUpFil);
        f_unlink(fp(gUploadTmp).c_str());
        gUploadOpen = false;
        klog("upload abandoned: %s", gUploadRel.c_str());
      }
      f_mount(nullptr, gDrv, 0);
      gFsOk = false;
      clearDirtyFlag();
      gWriteWin = false;
      usbAttach();
      if (gUsbStarted) klog("stick handed back to the roaster");
    }
  }
  xSemaphoreGiveRecursive(gFsLock);
}

void fatMkdirs(const String& relDir) {
  String cur;
  int i = 0;
  while (i < (int)relDir.length()) {
    int j = relDir.indexOf('/', i);
    if (j < 0) j = relDir.length();
    if (cur.length()) cur += "/";
    cur += relDir.substring(i, j);
    f_mkdir(fp(cur).c_str());                 // FR_EXIST is fine
    i = j + 1;
  }
}

// Write a whole file (tmp + rename), optionally set its modification time. Caller decides on permission.
int fatWriteFile(const String& rel, const uint8_t* data, size_t len, uint32_t mtime, bool internal) {
  FsUse u;
  int r = internal ? beginWriteInternal() : beginWrite();
  if (r != OC_OK) return r;
  gLastFsWrite = millis();
  String tmp = rel + ".tmp~";
  fatMkdirs(parentOf(rel));
  if (f_open(&gWrFil, fp(tmp).c_str(), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return OC_CANNOT_SAVE;
  UINT bw = 0;
  FRESULT fr = len ? f_write(&gWrFil, data, len, &bw) : FR_OK;
  FRESULT fc = f_close(&gWrFil);
  if (fr != FR_OK || fc != FR_OK || bw != len) { f_unlink(fp(tmp).c_str()); return OC_CANNOT_SAVE; }
  f_unlink(fp(rel).c_str());
  if (f_rename(fp(tmp).c_str(), fp(rel).c_str()) != FR_OK) return OC_CANNOT_SAVE;
  if (mtime) {
    FILINFO fno;
    epochToFat(mtime, fno.fdate, fno.ftime);
    f_utime(fp(rel).c_str(), &fno);
  }
  gLastFsWrite = millis();
  return OC_OK;
}

String stamp() {
  char b[24];
  if (timeValid()) {
    time_t t = time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
  } else {
    snprintf(b, sizeof b, "up%lu", (unsigned long)millis());
  }
  return String(b);
}

// Delete = move to kl-trash/ (a delete inside kl-trash/ is final). Directories must be empty.
int fatDelete(const String& rel) {
  FsUse u;
  Entry e;
  if (!fatStat(rel, e)) return OC_NOT_FOUND;
  int r = beginWrite();
  if (r != OC_OK) return r;
  gLastFsWrite = millis();
  FRESULT fr;
  if (e.dir || lower(rel).startsWith(String(TRASH_DIR) + "/") || gStore == STORE_FLASH) {
    fr = f_unlink(fp(rel).c_str());
  } else {
    f_mkdir(fp(TRASH_DIR).c_str());
    fr = f_rename(fp(rel).c_str(), fp(String(TRASH_DIR) + "/" + stamp() + "-" + baseOf(rel)).c_str());
  }
  gLastFsWrite = millis();
  if (fr == FR_OK && lower(rel).endsWith(".klog")) {
    String n = baseOf(rel);
    if (n.length() <= 15 && gDates.isKey(n.c_str())) gDates.remove(n.c_str());   // its start-time note goes too
  }
  return fr == FR_OK ? OC_OK : OC_CANNOT_DELETE;
}

// ------------------------------------------------------------------ overlay (NVS)
void ovSave() {
  gOvPrefs.clear();
  gOvPrefs.putUChar("n", gOverlay.size());
  for (size_t i = 0; i < gOverlay.size(); i++) {
    String k = String(i);
    gOvPrefs.putString(("p" + k).c_str(), gOverlay[i].path);
    gOvPrefs.putUInt(("t" + k).c_str(), gOverlay[i].mtime);
    if (gOverlay[i].data.size()) gOvPrefs.putBytes(("d" + k).c_str(), gOverlay[i].data.data(), gOverlay[i].data.size());
  }
}

void ovLoad() {
  gOvPrefs.begin("kl-overlay", false);
  int n = gOvPrefs.getUChar("n", 0);
  for (int i = 0; i < n && i < (int)OVERLAY_MAX_FILES; i++) {
    String k = String(i);
    OvFile f;
    f.path = gOvPrefs.getString(("p" + k).c_str(), "");
    f.mtime = gOvPrefs.getUInt(("t" + k).c_str(), 0);
    size_t len = gOvPrefs.getBytesLength(("d" + k).c_str());
    f.data.resize(len);
    if (len) gOvPrefs.getBytes(("d" + k).c_str(), f.data.data(), len);
    if (f.path.length()) gOverlay.push_back(f);
  }
}

OvFile* ovFind(const String& path) {
  for (auto& f : gOverlay) if (f.path.equalsIgnoreCase(path)) return &f;
  return nullptr;
}

bool ovList(const String& path, std::vector<Entry>& out) {
  String pre = path.length() ? lower(path) + "/" : String("");
  bool any = path.length() == 0;
  for (auto& f : gOverlay) {
    String l = lower(f.path);
    if (!l.startsWith(pre)) continue;
    any = true;
    String rest = f.path.substring(pre.length());
    int k = rest.indexOf('/');
    if (k < 0) {
      out.push_back({ rest, false, f.mtime, (uint32_t)f.data.size() });
    } else {
      String d = rest.substring(0, k);
      bool dup = false;
      for (auto& e : out) if (e.dir && e.name.equalsIgnoreCase(d)) dup = true;
      if (!dup) out.push_back({ d, true, f.mtime, 0 });
    }
  }
  return any;
}

// ------------------------------------------------------------------ store as Studio sees it
int stWrite(const String& path, const std::vector<uint8_t>& data, uint32_t mtime) {
  if (inImage(path)) {
    String l = lower(path);
    if (!(l.startsWith(String(PROFILE_DIR) + "/") && l.endsWith(".kpro"))) return OC_DENIED;
    return fatWriteFile(path, data.data(), data.size(), mtime, false);
  }
  if (data.size() > OVERLAY_MAX_FILE) return OC_CANNOT_SAVE;   // e.g. firmware: never stored, never reaches the stick
  OvFile* f = ovFind(path);
  if (!f) {
    if (gOverlay.size() >= OVERLAY_MAX_FILES) return OC_CANNOT_SAVE;
    gOverlay.push_back(OvFile());
    f = &gOverlay.back();
    f->path = path;
  }
  f->data = data;
  f->mtime = mtime;
  ovSave();
  return OC_OK;
}

int stDelete(const String& path) {
  if (inImage(path)) {
    String l = lower(path);
    if (!((l.startsWith(String(PROFILE_DIR) + "/") && l.endsWith(".kpro")) ||
          (l.startsWith(String(LOG_DIR) + "/") && l.endsWith(".klog"))))
      return OC_DENIED;
    return fatDelete(path);
  }
  for (size_t i = 0; i < gOverlay.size(); i++)
    if (gOverlay[i].path.equalsIgnoreCase(path)) {
      gOverlay.erase(gOverlay.begin() + i);
      ovSave();
      return OC_OK;
    }
  return OC_NOT_FOUND;
}

String firmwareVersion() {
  String name, head;
  uint32_t size;
  FsUse u;
  if (!u.ok || !newestLog(name, size)) return "";
  if (!fatReadHead(String(LOG_DIR) + "/" + name, head, 4000)) return "";
  int i = 0;
  while (i < (int)head.length()) {
    int j = head.indexOf('\n', i);
    if (j < 0) j = head.length();
    String line = head.substring(i, j);
    line.trim();
    if (line.startsWith("firmware_version:")) {
      String v = line.substring(17);
      if (v.length() == 0 || !isdigit((unsigned char)v[0])) return "";
      for (size_t k = 0; k < v.length(); k++) if (!isdigit((unsigned char)v[k]) && v[k] != '.') return "";
      return v;
    }
    i = j + 1;
  }
  return "";
}

long logNumber(const String& name);

// ------------------------------------------------------------------ SASSI session (Studio)
class Session {
 public:
  WiFiClient c;
  String peer;
  bool closed = false;
  uint32_t t0;
  uint64_t tsBase = 0;
  bool synced = false;
  uint32_t lastTx = 0, lastConnReq = 0;
  std::vector<Fields> pending;
  // push in progress
  bool pushActive = false;
  String pushPath;
  int pushSeq = 0;
  bool pushOverflow = false;
  std::vector<uint8_t> pushBuf;
  // folder change detection
  uint32_t sigAt = 0;
  uint64_t sig = 0;
  bool haveSig = false;
  // live log
  bool live = false;
  String livePath;
  uint32_t liveSent = 0;
  uint32_t liveAt = 0;

  Session(WiFiClient& cl, const String& p) : c(cl), peer(p) { t0 = millis(); gRxLen = 0; }

  // --- wire
  bool send(int typ, const Fields& f) {
    char head[40];
    snprintf(head, sizeof head, "KL*%d|%llx|", typ, (unsigned long long)(tsBase + (uint64_t)(millis() - t0)));
    String body = head;
    for (auto& x : f) { body += x; body += '|'; }
    char tail[8];
    snprintf(tail, sizeof tail, "%04x\r", crc16((const uint8_t*)body.c_str(), body.length(), gCrc0));
    body += tail;
    size_t w = c.write((const uint8_t*)body.c_str(), body.length());
    if (w != body.length()) { closed = true; return false; }
    lastTx = millis();
    if (typ != T_B_NOTIFY && typ != T_NOTIFY_FILE && typ != T_FILE_RESP && typ != T_DIR_RESP)
      klog(">> %.150s", body.c_str());
    return true;
  }

  void sendCr() {
    if (c.write((const uint8_t*)"\r", 1) != 1) closed = true;
    lastTx = millis();
  }

  // 1 = packet, 0 = empty/invalid packet, -1 = timeout, -2 = closed
  int recvPacket(uint32_t timeoutMs, Fields& parts) {
    uint32_t start = millis();
    for (;;) {
      char* cr = (char*)memchr(gRx, '\r', gRxLen);
      if (cr) {
        size_t n = cr - gRx;
        *cr = 0;
        const char* s = gRx;
        while (*s == '\n') s++;
        String raw(s);
        memmove(gRx, cr + 1, gRxLen - n - 1);
        gRxLen -= n + 1;
        while (raw.endsWith("\n")) raw.remove(raw.length() - 1);
        if (raw.length() == 0) return 0;
        if (!raw.startsWith("KL*") || raw.indexOf('|') < 0 || raw.length() < 8) {
          klog("<< ;; %.80s", raw.c_str());
          return 0;
        }
        char want[6];
        snprintf(want, sizeof want, "%04x", crc16((const uint8_t*)raw.c_str(), raw.length() - 4, gCrc0));
        if (!lower(raw.substring(raw.length() - 4)).equals(want)) {
          klog("<< bad CRC %.120s", raw.c_str());
          return 0;
        }
        parts.clear();
        int i = 0;
        for (;;) {
          int j = raw.indexOf('|', i);
          if (j < 0) { parts.push_back(raw.substring(i)); break; }
          parts.push_back(raw.substring(i, j));
          i = j + 1;
        }
        if (parts[0] != "KL*1" && parts[0] != "KL*131") klog("<< %.150s", raw.c_str());
        return 1;
      }
      int a = c.available();
      if (a > 0) {
        if (gRxLen >= sizeof gRx - 1) { klog("rx overflow - dropping buffer"); gRxLen = 0; }
        size_t room = sizeof gRx - 1 - gRxLen;
        int r = c.read((uint8_t*)gRx + gRxLen, (size_t)a < room ? (size_t)a : room);
        if (r > 0) gRxLen += r;
        continue;
      }
      if (!c.connected()) { closed = true; return -2; }
      if (millis() - start >= timeoutMs) return -1;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  bool waitAck() {
    uint32_t start = millis();
    while (millis() - start < 10000 && !closed) {
      Fields p;
      int r = recvPacket(10000 - (millis() - start), p);
      if (r < 0) break;
      if (r == 0) continue;
      if (p[0] == "KL*1") return true;
      pending.push_back(p);
    }
    klog("timeout waiting for PACKET_ACK");
    return false;
  }

  // Multi-packet transfer. Studio ACKs every non-final packet.
  bool sendMulti(int typ, const String& name, uint32_t total, const Reader& rd, const Mk& mk) {
    int fit = ((MAX_PACKET - (3 + 3 + 1 + 16 + 1 + (int)name.length() + 1 + 3 + 1 + 15 + 1 + 5 + 1 + 4 + 1)) * 3) / 4;
    size_t canFit = fit < 48 ? 48 : (size_t)fit;
    if (canFit > sizeof gChunk) canFit = sizeof gChunk;
    uint32_t off = 0;
    int seq = 1;
    for (;;) {
      size_t n = total - off < canFit ? total - off : canFit;
      if (n && rd(off, gChunk, n) != (int)n) { klog("read error at %lu of %s", (unsigned long)off, name.c_str()); return false; }
      size_t olen = 0;
      mbedtls_base64_encode((unsigned char*)gB64, sizeof gB64 - 1, &olen, gChunk, n);
      gB64[olen] = 0;
      bool last = off + n >= total;
      if (!send(typ, mk(last ? OC_LAST : OC_OK, seq, String(gB64)))) return false;
      off += n;
      seq++;
      if (last) return true;
      if (!waitAck()) return false;
    }
  }

  bool sendFile(int typ, const String& path, uint32_t from, uint32_t len, const Mk& mk) {
    FsUse u;
    if (!u.ok || f_open(&gSassiFil, fp(path).c_str(), FA_READ) != FR_OK) return false;
    bool ok = sendMulti(typ, path, len, [&](uint32_t off, uint8_t* buf, size_t n) -> int {
      if (f_lseek(&gSassiFil, from + off) != FR_OK) return -1;
      UINT br = 0;
      if (f_read(&gSassiFil, buf, n, &br) != FR_OK) return -1;
      return (int)br;
    }, mk);
    f_close(&gSassiFil);
    return ok;
  }

  // --- handlers
  void connRequest() {
    lastConnReq = millis();
    char crc[8];
    snprintf(crc, sizeof crc, "%x", gCrc0);
    // platform 11 = TCP bridge; capabilities bit7 = "user info" with style 0 (Kaffelogic)
    send(T_CONN_REQ, { "11", "128", cfg.serial, "1", "Nano 7", "kaffelogic.com",
                     "Kaffelogic Nano 7 (stick via T-Dongle-S3)", String(MAX_PACKET), "255", crc });
  }

  String info(int code) {
    if (code == INFO_OPSTATUS) return String("sassi_file_lock:") + (hostBusy(BUSY_WINDOW_MS) ? "1" : "0");
    if (code == INFO_SYSTEM) {
      // Default 9.9.9: newer than any release, so Studio stops offering roaster firmware updates (which cannot
      // be installed through the stick). FIRMWARE=log reports the version from the newest klog.
      String fw = cfg.firmware == "log" ? firmwareVersion() : cfg.firmware;
      String r = "model:Nano 7;serial:" + cfg.serial;
      if (fw.length()) r += ";firmware:" + fw;
      return r;
    }
    if (code == INFO_FILESYS) {
      uint64_t used = 0, size = 0;
      fatUsage(used, size);
      char b[64];
      snprintf(b, sizeof b, "used:%llu;size:%llu", (unsigned long long)used, (unsigned long long)size);
      return String(b);
    }
    return "";
  }

  String bridgeInfo() {
    return "rssi:" + String(WiFi.RSSI()) + ";fw_version:9.9.9;model_number:T-Dongle-S3 (kl-dongle);serial_number:" + cfg.serial;
  }

  void handle(const Fields& p) {
    int typ = p[0].substring(3).toInt();
    auto g = [&](int i) -> String { return i < (int)p.size() - 1 ? p[i] : String(""); };   // field by protocol number
    String path;
    if (typ == T_TIME_SYNC) {
      synced = true;
      tsBase = strtoull(p[1].c_str(), nullptr, 16) - (uint64_t)(millis() - t0);
      uint32_t studioNow;
      if (!timeValid() && parseDt(g(4), studioNow)) {          // no NTP (e.g. no internet): use Studio's clock (UTC)
        struct timeval tv = { (time_t)studioNow, 0 };
        settimeofday(&tv, nullptr);
        klog("clock set from Studio: %s UTC", g(4).c_str());
      }
      send(T_TIME_SYNC_ACK, {});
      klog("Studio connected from %s (Studio time %s)", peer.c_str(), g(4).c_str());
    } else if (typ == T_DIR_REQ) {
      if (!normPath(g(2), path)) { send(T_DIR_RESP, { g(2), String(OC_NOT_VALID), g(4), "0", "" }); return; }
      String body;
      bool found;
      auto add = [&](const String& n, bool dir, uint32_t mt, uint32_t sz) {
        if (n.startsWith(".") || n.endsWith("~") || n.equalsIgnoreCase("!ip.kpro")) return;
        if (body.length()) body += '\r';
        body += dir ? ">\t" : " \t";
        body += n; body += '\t'; body += sassiDt(mt); body += '\t'; body += String(sz);
      };
      if (inImage(path)) {
        found = fatForEach(path, [&](const FILINFO& f) {
          add(String(f.fname), f.fattrib & AM_DIR, fatToEpoch(f.fdate, f.ftime), (uint32_t)f.fsize);
        });
      } else {
        std::vector<Entry> l;
        found = ovList(path, l);
        for (auto& e : l) add(e.name, e.dir, e.mtime, e.size);
      }
      if (!found) { send(T_DIR_RESP, { path, String(OC_NOT_FOUND), g(4), "0", "" }); return; }
      String fmt = g(4);
      sendMulti(T_DIR_RESP, path, body.length(), [&](uint32_t off, uint8_t* buf, size_t n) -> int {
        memcpy(buf, body.c_str() + off, n); return (int)n;
      }, [&](int oc, int seq, const String& b) -> Fields { return { path, String(oc), fmt, String(seq), b }; });
    } else if (typ == T_FILE_REQ) {
      Entry e;
      if (!normPath(g(2), path)) { send(T_FILE_RESP, { g(2), String(OC_NOT_VALID), "", "0", "" }); return; }
      if (inImage(path)) {
        FsUse u;
        if (!fatStat(path, e) || e.dir) { send(T_FILE_RESP, { path, String(OC_NOT_FOUND), "", "0", "" }); return; }
        String dt = sassiDt(e.mtime);
        bool sent = sendFile(T_FILE_RESP, path, 0, e.size,
                 [&](int oc, int seq, const String& b) -> Fields { return { path, String(oc), dt, String(seq), b }; });
        long n = lower(parentOf(path)) == LOG_DIR ? logNumber(baseOf(path)) : -1;
        if (sent && n > gDates.getInt("studio_max", -1)) gDates.putInt("studio_max", n);   // for the log clean-up
      } else {
        OvFile* f = ovFind(path);
        if (!f) { send(T_FILE_RESP, { path, String(OC_NOT_FOUND), "", "0", "" }); return; }
        std::vector<uint8_t> data = f->data;
        String dt = sassiDt(f->mtime);
        sendMulti(T_FILE_RESP, path, data.size(), [&](uint32_t off, uint8_t* buf, size_t n) -> int {
          memcpy(buf, data.data() + off, n); return (int)n;
        }, [&](int oc, int seq, const String& b) -> Fields { return { path, String(oc), dt, String(seq), b }; });
      }
    } else if (typ == T_FILE_PUSH || typ == T_B_FILE_PUSH) {
      int ack = typ == T_FILE_PUSH ? T_FILE_PUSH_ACK : T_B_FILE_PUSH_ACK;
      if (!normPath(g(2), path)) { send(ack, { g(2), String(OC_NOT_VALID) }); return; }
      int action = g(3).toInt();
      int seq = g(5).length() ? g(5).toInt() : 1;
      if (seq == 1) { pushActive = true; pushPath = path; pushSeq = 1; pushOverflow = false; pushBuf.clear(); }
      if (!pushActive || path != pushPath || seq != pushSeq) {
        pushActive = false;
        pushBuf.clear();
        send(ack, { path, String(OC_SEQ_ERR) });
        return;
      }
      String b64 = g(6);
      size_t old = pushBuf.size(), cap = b64.length() * 3 / 4 + 4, olen = 0;
      if (old + cap > PUSH_MAX) {
        pushOverflow = true;
      } else {
        pushBuf.resize(old + cap);
        if (mbedtls_base64_decode(pushBuf.data() + old, cap, &olen, (const unsigned char*)b64.c_str(), b64.length()) != 0) olen = 0;
        pushBuf.resize(old + olen);
      }
      pushSeq++;
      if (!(action & OC_LAST)) { send(T_ACK, {}); return; }
      pushActive = false;
      int oc;
      if (typ == T_B_FILE_PUSH) {
        oc = OC_DENIED;
      } else if (pushOverflow) {
        oc = OC_CANNOT_SAVE;
      } else {
        uint32_t mt = 0;
        if (!parseDt(g(4), mt)) mt = timeValid() ? (uint32_t)time(nullptr) : 0;
        oc = stWrite(path, pushBuf, mt);
      }
      klog("push %s %u bytes -> outcome %d", path.c_str(), (unsigned)pushBuf.size(), oc);
      pushBuf.clear();
      pushBuf.shrink_to_fit();
      send(ack, { path, String(oc) });
    } else if (typ == T_FILE_DEL) {
      if (!normPath(g(2), path)) { send(T_FILE_DEL_ACK, { g(2), String(OC_NOT_VALID) }); return; }
      int oc = stDelete(path);
      klog("delete %s -> outcome %d", path.c_str(), oc);
      send(T_FILE_DEL_ACK, { path, String(oc) });
    } else if (typ == T_INFO_REQ) {
      int code = g(3).toInt();
      send(T_INFO_RESP, { info(code), String(code) });
    } else if (typ == T_B_INFO_REQ) {
      int code = g(3).toInt();
      send(T_B_INFO_RESP, { (code == 100 || code == 101) ? bridgeInfo() : String(""), String(code) });
    } else if (typ == T_ACTION) {
      int code = g(3).toInt();
      if (code == ACT_FS_CLEANUP) send(T_ACTION_ACK, { "0", String(OC_OK), "" });
      else if (code == ACT_CONN_CONTROL) send(T_ACTION_ACK, { g(2), String(OC_OK), "" });
      else {
        klog("ACTION %d data=%s -> denied (not implemented)", code, g(2).c_str());
        send(T_ACTION_ACK, { g(2), String(OC_DENIED), "" });
      }
    } else if (typ == T_B_ACTION) {
      int code = g(3).toInt();
      send(T_B_ACTION_ACK, { g(2), String(code == ACT_CONN_CONTROL ? OC_OK : OC_DENIED), "" });
      if (code == ACT_CONN_CONTROL) { klog("Studio is closing the connection"); closed = true; }
    } else if (typ == T_ACK || typ == T_NOTIFY_STATUS_ACK || typ == T_NOTIFY_FILE_ACK || typ == T_B_NOTIFY_ACK) {
      // nothing
    } else {
      klog("unknown packet type %d", typ);
    }
  }

  // Live log: the roaster writes the klog incrementally (1 line/s). BUSY, then NOTIFY_FILE with the whole file so
  // far (starts with log_file_name:), then only new complete lines. 20 s without writes: NOT_BUSY with the path.
  void liveTick() {
    if (!synced || millis() - liveAt < 1000 || gWriteWin) return;
    liveAt = millis();
    if (!gRoasting) {
      if (live) {
        klog("live: end %s (%lu bytes sent)", livePath.c_str(), (unsigned long)liveSent);
        send(T_NOTIFY_STATUS, { livePath, String(INFO_NOT_BUSY) });
        live = false;
        gLiveName[0] = 0;
      }
      return;
    }
    FsUse u;
    if (!u.ok) return;
    String name = gRoastName;
    String path = String(LOG_DIR) + "/" + name;
    bool same = live && livePath == path;
    if (f_open(&gSassiFil, fp(path).c_str(), FA_READ) != FR_OK) return;
    uint32_t fsz = f_size(&gSassiFil);
    uint32_t from = same ? liveSent : 0;
    if (!same) {
      char head[15] = {0};
      UINT br = 0;
      f_read(&gSassiFil, head, 14, &br);
      if (br != 14 || memcmp(head, "log_file_name:", 14) != 0) { f_close(&gSassiFil); return; }
    }
    // find the last complete line and make sure there are no NULs (not yet written data) before it
    int64_t lastNl = -1, firstNul = -1;
    f_lseek(&gSassiFil, from);
    for (uint32_t pos = from; pos < fsz;) {
      UINT br = 0;
      if (f_read(&gSassiFil, gChunk, sizeof gChunk, &br) != FR_OK || br == 0) break;
      for (UINT i = 0; i < br; i++) {
        if (gChunk[i] == '\n') lastNl = pos + i;
        else if (gChunk[i] == 0 && firstNul < 0) firstNul = pos + i;
      }
      pos += br;
    }
    f_close(&gSassiFil);
    if (!same) {
      klog("live: start %s", path.c_str());
      send(T_NOTIFY_STATUS, { "", String(INFO_BUSY) });
      live = true;
      livePath = path;
      liveSent = 0;
      strncpy(gLiveName, name.c_str(), sizeof gLiveName - 1);
    }
    if (lastNl < 0) return;
    uint32_t end = (uint32_t)lastNl + 1;
    if (end <= liveSent || (firstNul >= 0 && firstNul < (int64_t)end)) return;
    if (sendFile(T_NOTIFY_FILE, path, liveSent, end - liveSent,
                 [&](int oc, int seq, const String& b) -> Fields { return { path, String(oc), "", String(seq), b }; }))
      liveSent = end;
  }

  uint64_t dirSignature() {
    uint64_t s = 0;
    for (const char* d : { PROFILE_DIR, LOG_DIR }) {
      fatForEach(d, [&](const FILINFO& f) {
        if (f.fattrib & AM_DIR) return;
        uint64_t h = 1469598103934665603ULL;                  // FNV-1a over name, then mix size/time
        for (const char* q = f.fname; *q; q++) { h ^= (uint8_t)tolower(*q); h *= 1099511628211ULL; }
        h ^= ((uint64_t)f.fsize << 32) ^ ((uint64_t)f.fdate << 16) ^ f.ftime;
        h *= 1099511628211ULL;
        s += h;                                               // order independent
      });
    }
    return s;
  }

  void periodic() {
    uint32_t now = millis();
    if (synced && now - lastTx > 5000) {
      // During a live log no BRIDGE_NOTIFY: Studio would "renew" the connection and lose the live view.
      if (!cfg.keepaliveCr && !live) send(T_B_NOTIFY, { "rssi:" + String(WiFi.RSSI()), String(INFO_BRIDGE_STATUS) });
      else sendCr();
    }
    if (synced && now - sigAt > 15000 && !gWriteWin && !hostBusy(BUSY_WINDOW_MS)) {
      sigAt = now;
      uint64_t s = dirSignature();
      if (haveSig && s != sig) {
        klog("stick folders changed -> NOTIFY FOLDER_UPDATED");
        send(T_NOTIFY_STATUS, { "", String(INFO_FOLDER_UPDATED) });
      }
      sig = s;
      haveSig = true;
    }
  }

  void run() {
    connRequest();
    while (!closed) {
      Fields p;
      int r;
      if (!pending.empty()) { p = pending.front(); pending.erase(pending.begin()); r = 1; }
      else r = recvPacket(1000, p);
      if (r == -2) break;
      if (r == 0) {
        if (!synced && millis() - lastConnReq > 1000) connRequest();
      } else if (r == 1) {
        handle(p);
      }
      if (closed) break;
      periodic();
      if (closed) break;
      liveTick();
    }
  }
};

void sassiTask(void*) {
  for (;;) {
    if (!gNetUp) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    WiFiClient c = sassiSrv.accept();
    if (!c) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    c.setNoDelay(true);
    String peer = c.remoteIP().toString();
    klog("connection from %s", peer.c_str());
    strncpy(gStudioPeer, peer.c_str(), sizeof gStudioPeer - 1);
    gStudioConnected = true;
    Session* s = new Session(c, peer);
    s->run();
    delete s;
    c.stop();
    gStudioConnected = false;
    gLiveName[0] = 0;
    klog("connection from %s closed", peer.c_str());
    finishWrite(true);
  }
}

// ------------------------------------------------------------------ LLMNR: "<hostname>" -> our IP
void llmnrPoll() {
  int n = llmnrUdp.parsePacket();
  if (n <= 0) return;
  uint8_t q[512];
  int len = llmnrUdp.read(q, sizeof q);
  if (len < 17 || (q[2] & 0x80)) return;
  int l = q[12];
  if (18 + l > len || q[13 + l] != 0) return;
  uint16_t qtype = (q[14 + l] << 8) | q[15 + l];
  if (qtype != 1 && qtype != 255) return;
  if ((int)cfg.hostname.length() != l || strncasecmp((const char*)q + 13, cfg.hostname.c_str(), l) != 0) return;
  IPAddress ip = WiFi.localIP();
  uint8_t a[600];
  int k = 0;
  a[k++] = q[0]; a[k++] = q[1];
  const uint8_t hdr[] = { 0x80, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 };
  memcpy(a + k, hdr, sizeof hdr); k += sizeof hdr;
  memcpy(a + k, q + 12, l + 6); k += l + 6;                       // the question
  const uint8_t ans[] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 30, 0x00, 0x04 };
  memcpy(a + k, ans, sizeof ans); k += sizeof ans;
  for (int i = 0; i < 4; i++) a[k++] = ip[i];
  llmnrUdp.beginPacket(llmnrUdp.remoteIP(), llmnrUdp.remotePort());
  llmnrUdp.write(a, k);
  llmnrUdp.endPacket();
  klog("LLMNR: %s asked for %s", llmnrUdp.remoteIP().toString().c_str(), cfg.hostname.c_str());
}

// ------------------------------------------------------------------ roast detection + start times
// "roast_date:DD/MM/YYYY HH:MM:SS UTC" from the first 1 KB of a log (where roasters with a clock, Studio and the
// dongle put it). 0 if there is none. Caller holds FsUse.
uint32_t logRoastDate(const String& rel) {
  if (f_open(&gWebFil, fp(rel).c_str(), FA_READ) != FR_OK) return 0;
  char head[1025];
  UINT br = 0;
  f_read(&gWebFil, head, 1024, &br);
  f_close(&gWebFil);
  head[br] = 0;
  const char* p = strstr(head, "\nroast_date:");
  unsigned d, mo, y, h, mi, se;
  if (!p || sscanf(p + 12, "%u/%u/%u %u:%u:%u", &d, &mo, &y, &h, &mi, &se) != 6) return 0;
  if (y < 2000 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 59) return 0;
  return mkEpoch(y, mo, d, h, mi, se);
}

long logNumber(const String& name);

// The dongle's own note of a roast's start, until it is in the log. Keeps at most ~100 (NVS is small).
void storeRoastStart(const String& name, uint32_t start) {
  gDates.putUInt(name.c_str(), start);
  long n = logNumber(name);
  if (n > 100) {
    char old[24];
    snprintf(old, sizeof old, "log%04ld.klog", n - 100);
    if (gDates.isKey(old)) gDates.remove(old);
  }
}

long logNumber(const String& name) {
  String n = lower(name);
  if (!n.startsWith("log") || !n.endsWith(".klog") || n.length() <= 8) return -1;
  String num = n.substring(3, n.length() - 5);
  for (size_t i = 0; i < num.length(); i++) if (!isdigit((unsigned char)num[i])) return -1;
  return num.toInt();
}

void recordRoastDate(const String& name);
void collectLogs();
int insertRoastDate(const String& name, uint32_t& newSize);

// About once a second. The roaster appends a line per second to the newest log while roasting and cooling, so
// "the newest log grew, or a newer log appeared" = roasting. Only scans while the USB host is writing (or a roast
// is running), so an idle card is left alone. Plain write activity alone is not a roast (a computer as host
// writes all the time).
void resolvePendingRoastDate();

void roastTick() {
  resolvePendingRoastDate();
  static String seenName;
  static uint32_t seenSize = 0;
  static long seenNum = -1;
  static bool seenValid = false;
  if (gWriteWin) return;
  if (hostBusy(LIVE_WINDOW_MS) || gRoasting || !seenValid) {
    FsUse u;
    String name;
    uint32_t size = 0;
    if (u.ok && newestLog(name, size)) {
      long num = logNumber(name);
      bool grew = seenValid && ((name == seenName && size > seenSize) || (name != seenName && num > seenNum));
      seenName = name; seenSize = size; seenNum = num; seenValid = true;
      if (grew) {
        gRoastLastGrow = millis();
        if (!gRoasting || strcmp(gRoastName, name.c_str()) != 0) {
          strncpy(gRoastName, name.c_str(), sizeof gRoastName - 1);
          gRoasting = true;
          klog("roast detected: %s is growing", name.c_str());
          // A new roast: any note under this name belongs to an older, deleted log (the roaster reuses numbers).
          if (gDates.isKey(name.c_str())) gDates.remove(name.c_str());
          if (gPendName == name) gPendName = "";
        }
        recordRoastDate(name);
      }
    } else if (u.ok) {
      seenValid = true;                                       // no logs yet: the first one will count as new
    }
  }
  if (gRoasting && millis() - gRoastLastGrow >= ROAST_IDLE_MS) {
    // Before reporting the end (Studio then downloads the final log), add roast_date to the log - once the roaster
    // has also stopped writing. Give up after a while rather than keep the roast "running".
    static String datedName;
    if (datedName != gRoastName && millis() - gRoastLastGrow < ROAST_DATE_GIVEUP_MS) {
      if (hostBusy(ROAST_IDLE_MS)) return;                    // roaster still writing (closing the file): wait
      resolvePendingRoastDate();
      uint32_t newSize = 0;
      int r = insertRoastDate(gRoastName, newSize);
      if (r == OC_BUSY) return;                               // try again next tick
      datedName = gRoastName;
      if (r == OC_OK && newSize && seenName == gRoastName) seenSize = newSize;   // our own change is not "growth"
      collectLogs();
    }
    gRoasting = false;
    klog("roast finished: %s", gRoastName);
  }
}

// Add "roast_date:DD/MM/YYYY HH:MM:SS UTC" to the header of a finished log, before "time_jump:" - where roasters
// with a clock write it - so Studio shows the roast date. The file is rewritten to a tmp file and swapped in by
// renames; the roaster's own file date is kept. OC_OK = added, OC_BUSY = try later, anything else = skipped.
int insertRoastDate(const String& name, uint32_t& newSize) {
  if (!cfg.roastDate || !cfg.allowWrite || name.length() > 15) return OC_DENIED;
  uint32_t start = gDates.getUInt(name.c_str(), 0);
  if (!start) return OC_NOT_FOUND;                            // start time unknown (no clock during the roast)
  String rel = String(LOG_DIR) + "/" + name, tmp = rel + ".tmp~", old = rel + ".old~";
  FsUse u;
  if (!u.ok) return OC_SYS_ERR;
  // Look at the header first (read-only): already dated? where to insert?
  if (f_open(&gWrFil, fp(rel).c_str(), FA_READ) != FR_OK) return OC_NOT_FOUND;
  UINT br = 0;
  f_read(&gWrFil, gWebBuf, sizeof gWebBuf - 1, &br);
  f_close(&gWrFil);
  gWebBuf[br] = 0;
  const char* head = (const char*)gWebBuf;
  if (strncmp(head, "log_file_name:", 14) != 0 || strstr(head, "\nroast_date:")) return OC_DENIED;
  const char* at = strstr(head, "\ntime_jump:");
  if (!at) at = strstr(head, "\noffsets");
  if (!at) return OC_DENIED;
  uint32_t insertAt = (uint32_t)(at - head) + 1;              // start of that line

  int r = beginWriteQuiet(ROAST_IDLE_MS);                     // the roaster has been quiet for 5 s
  if (r != OC_OK) return r;
  gLastFsWrite = millis();
  FILINFO orig;
  if (f_stat(fp(rel).c_str(), &orig) != FR_OK) return OC_NOT_FOUND;
  int64_t days = start / 86400;
  int y; unsigned mo, d;
  civilFromDays(days, y, mo, d);
  uint32_t sec = start % 86400;
  char line[48];
  int ll = snprintf(line, sizeof line, "roast_date:%02u/%02u/%04d %02lu:%02lu:%02lu UTC\n", d, mo, y,
                    (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60));
  bool ok = f_open(&gWrFil, fp(rel).c_str(), FA_READ) == FR_OK;
  if (ok && f_open(&gTmpFil, fp(tmp).c_str(), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { f_close(&gWrFil); ok = false; }
  if (ok) {
    UINT bw;
    uint32_t pos = 0;
    bool inserted = false;
    while (ok) {
      UINT want = sizeof gWebBuf;
      if (!inserted && insertAt - pos < want) want = insertAt - pos;
      if (want == 0 && !inserted) {
        ok = f_write(&gTmpFil, line, ll, &bw) == FR_OK && bw == (UINT)ll;
        inserted = true;
        continue;
      }
      if (f_read(&gWrFil, gWebBuf, want, &br) != FR_OK) { ok = false; break; }
      if (br == 0) break;
      ok = f_write(&gTmpFil, gWebBuf, br, &bw) == FR_OK && bw == br;
      pos += br;
    }
    ok = ok && inserted;
    f_close(&gWrFil);
    newSize = f_size(&gTmpFil);
    if (f_close(&gTmpFil) != FR_OK) ok = false;
  }
  // swap: log -> .old~, tmp -> log, drop .old~ (a power cut in between leaves both copies on the card)
  if (ok) ok = f_rename(fp(rel).c_str(), fp(old).c_str()) == FR_OK;
  if (ok) {
    if (f_rename(fp(tmp).c_str(), fp(rel).c_str()) == FR_OK) {
      f_utime(fp(rel).c_str(), &orig);                         // keep the roaster's file date
      f_unlink(fp(old).c_str());
    } else {
      f_rename(fp(old).c_str(), fp(rel).c_str());              // put the original back
      ok = false;
    }
  }
  if (!ok) f_unlink(fp(tmp).c_str());
  gLastFsWrite = millis();
  klog("roast_date %.*s -> %s %s", ll - 12, line + 11, name.c_str(), ok ? "added" : "FAILED (log unchanged)");
  if (ok) gDates.remove(name.c_str());                         // the date now lives in the log
  return ok ? OC_OK : OC_CANNOT_SAVE;
}

// The roaster has no clock (every klog is dated 2001-01-01): remember when this roast started =
// now - seconds in its last line. Clock: NTP, or Studio's TIME_SYNC. Without either yet, the start is kept against
// the dongle's uptime and converted once a clock arrives (same power session).

void resolvePendingRoastDate() {
  if (!gPendName.length() || !timeValid()) return;
  uint32_t start = (uint32_t)time(nullptr) - (millis() - gPendStartMs) / 1000;
  storeRoastStart(gPendName, start);
  klog("roast %s: started %s UTC (clock arrived later)", gPendName.c_str(), sassiDt(start).c_str());
  gPendName = "";
}

void recordRoastDate(const String& name) {
  resolvePendingRoastDate();
  if (name.length() > 15 || gDates.isKey(name.c_str()) || gPendName == name) return;
  FsUse u;
  if (!u.ok) return;
  String rel = String(LOG_DIR) + "/" + name;
  if (f_open(&gWrFil, fp(rel).c_str(), FA_READ) != FR_OK) return;
  char head[15] = {0};
  UINT br = 0;
  f_read(&gWrFil, head, 14, &br);
  uint32_t fsz = f_size(&gWrFil);
  uint32_t from = fsz > 2048 ? fsz - 2048 : 0;
  String tail;
  if (br == 14 && memcmp(head, "log_file_name:", 14) == 0 && f_lseek(&gWrFil, from) == FR_OK) {
    char buf[257];
    while (f_read(&gWrFil, buf, 256, &br) == FR_OK && br > 0) { buf[br] = 0; tail += buf; }
  }
  f_close(&gWrFil);
  int end = tail.lastIndexOf('\n');
  if (end < 0) return;
  tail = tail.substring(0, end);
  for (int n = 0; n < 5 && tail.length(); n++) {               // last few complete lines: "<seconds>.<frac>\t..."
    int k = tail.lastIndexOf('\n');
    String line = tail.substring(k + 1);
    tail = k < 0 ? String("") : tail.substring(0, k);
    int tab = line.indexOf('\t'), dot = line.indexOf('.');
    if (tab <= 0 || dot <= 0 || dot > tab) continue;
    bool num = true;
    for (int i = 0; i < tab; i++) if (!isdigit((unsigned char)line[i]) && i != dot) num = false;
    if (!num) continue;
    float secs = line.substring(0, tab).toFloat();
    if (!timeValid()) {
      gPendName = name;
      gPendStartMs = millis() - (uint32_t)(secs * 1000);
      klog("roast %s: no clock yet - start time kept until NTP or Studio provides one", name.c_str());
      return;
    }
    uint32_t start = (uint32_t)(time(nullptr) - (time_t)secs);
    storeRoastStart(name, start);
    klog("roast %s: started %s UTC", name.c_str(), sassiDt(start).c_str());
    return;
  }
}

// ------------------------------------------------------------------ log clean-up (KEEP_LOGS)
// Keeps the newest N logs: older ones are removed, oldest first - but only logs Studio has already downloaded
// (up to the newest log it fetched), so nothing is lost that was never copied. On an SD card removed logs go to
// kl-trash; on internal flash they are deleted (a trash would use the space). Runs at power-up and after a roast.
void collectLogs() {
  int keep = cfg.keepLogs >= 0 ? cfg.keepLogs : (gStore == STORE_FLASH ? 100 : 0);
  if (keep <= 0 || !cfg.allowWrite) return;
  long studioMax = gDates.getInt("studio_max", -1);
  FsUse u;
  if (!u.ok) return;
  std::vector<std::pair<long, String>> logs;
  fatForEach(LOG_DIR, [&](const FILINFO& f) {
    long n = (f.fattrib & AM_DIR) ? -1 : logNumber(String(f.fname));
    if (n >= 0) logs.push_back({ n, String(f.fname) });
  });
  if ((int)logs.size() <= keep) return;
  std::sort(logs.begin(), logs.end(), [](const std::pair<long, String>& a, const std::pair<long, String>& b) { return a.first < b.first; });
  size_t excess = logs.size() - keep;
  int removed = 0, kept = 0;
  for (size_t i = 0; i < excess; i++) {
    if (logs[i].first > studioMax) { kept++; continue; }      // Studio has not copied it yet
    if (beginWriteQuiet(ROAST_IDLE_MS) != OC_OK) break;        // the roaster is busy: next time
    if (fatDelete(String(LOG_DIR) + "/" + logs[i].second) == OC_OK) removed++;
  }
  if (removed || kept)
    klog("log clean-up: %d logs, keeping %d - removed %d oldest%s", (int)logs.size(), keep, removed,
         kept ? ", kept some Studio has not copied yet" : "");
}

// ------------------------------------------------------------------ config + IP profile
// The roaster's serial number: last part of "model:KN1007B/E/B33072" in the newest log.
String serialFromLogs() {
  String name, head;
  uint32_t size;
  FsUse u;
  if (!u.ok || !newestLog(name, size) || !fatReadHead(String(LOG_DIR) + "/" + name, head, 4000)) return "";
  int i = head.indexOf("\nmodel:");
  if (i < 0) return "";
  int j = head.indexOf('\n', i + 1);
  String m = head.substring(i + 7, j < 0 ? head.length() : j);
  m.trim();
  String sn = m.substring(m.lastIndexOf('/') + 1);
  if (sn.length() < 4 || sn.length() > 20) return "";
  for (size_t k = 0; k < sn.length(); k++) if (!isalnum((unsigned char)sn[k])) return "";
  return sn;
}

void loadConfig() {
  String text;
  if (!fatReadHead(CONFIG_FILE, text, 4096)) {
    klog("%s not found - creating a default one", CONFIG_FILE);
    const char* def =
      "# WiFi Configuration\r\n"
      "SSID=YourNetworkName\r\n"
      "PSK=YourPassword\r\n"
      "\r\n"
      "# Optional\r\n"
      "# HOSTNAME=KaffelogicWM   (the name Kaffelogic Studio looks for)\r\n"
      "# SERIAL=                 (serial shown in Studio; default: the roaster's, from its logs)\r\n"
      "# FIRMWARE=9.9.9          (firmware reported to Studio; log = the roaster's real version)\r\n"
      "# ALLOW_WRITE=1           (0 = Studio and the web page cannot change the card)\r\n"
      "# IP_PROFILE=1            (1 = show the IP address as a profile named !ip on the roaster)\r\n"
      "# KEEPALIVE=bridge        (cr = send a bare CR as keep-alive)\r\n"
      "# ROAST_DATE=1            (1 = add roast_date to each new log, as roasters with a clock do)\r\n"
      "# KEEP_LOGS=auto          (keep the newest N logs; auto = 100 on internal storage, all on an SD card)\r\n";
    fatWriteFile(CONFIG_FILE, (const uint8_t*)def, strlen(def), 0, true);
    return;
  }
  int i = 0;
  while (i < (int)text.length()) {
    int j = text.indexOf('\n', i);
    if (j < 0) j = text.length();
    String line = text.substring(i, j);
    i = j + 1;
    line.trim();
    if (line.startsWith("#")) continue;
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq), v = line.substring(eq + 1);
    k.trim(); v.trim();
    k.toUpperCase();
    if (k == "SSID") cfg.ssid = v;
    else if (k == "PSK") cfg.psk = v;
    else if (k == "HOSTNAME" && v.length()) cfg.hostname = v;
    else if (k == "SERIAL" && v.length()) cfg.serial = v;
    else if (k == "FIRMWARE" && v.length()) cfg.firmware = v;
    else if (k == "ALLOW_WRITE") cfg.allowWrite = v != "0";
    else if (k == "IP_PROFILE") cfg.ipProfile = v != "0";
    else if (k == "KEEPALIVE") cfg.keepaliveCr = lower(v) == "cr";
    else if (k == "ROAST_DATE") cfg.roastDate = v != "0";
    else if (k == "KEEP_LOGS") cfg.keepLogs = lower(v) == "auto" ? -1 : v.toInt();
  }
  if (cfg.ssid == "YourNetworkName") cfg.ssid = "";
  if (!cfg.serial.length()) cfg.serial = serialFromLogs();
  if (!cfg.serial.length()) cfg.serial = "KLDONGLE01";
  klog("config: SSID '%s', hostname %s, writes %s", cfg.ssid.c_str(), cfg.hostname.c_str(), cfg.allowWrite ? "allowed" : "off");
}

// A profile named "!ip" whose name is the dongle's IP address, so you can read it on the roaster's display.
// Only written when it changed; never while roasting.
void ensureIpProfile() {
  if (!cfg.ipProfile || WiFi.status() != WL_CONNECTED) return;
  if (gIpProfileDone && gLastIp == WiFi.localIP()) return;
  String want = "profile_short_name:" + WiFi.localIP().toString() + "\r\n";
  String have;
  {
    FsUse u;
    if (!u.ok) return;
    fatReadHead(IP_PROFILE, have, 128);
  }
  int r = OC_OK;
  if (have != want) {
    r = fatWriteFile(IP_PROFILE, (const uint8_t*)want.c_str(), want.length(), 0, true);
    klog("IP profile %s -> %d", WiFi.localIP().toString().c_str(), r);
  }
  if (r == OC_OK) { gIpProfileDone = true; gLastIp = WiFi.localIP(); }
}

// ------------------------------------------------------------------ web interface
String jsonEsc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char ch = s[i];
    if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
    else if ((uint8_t)ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
    else o += ch;
  }
  return o;
}

String contentType(const String& n) {
  String l = lower(n);
  if (l.endsWith(".html")) return "text/html";
  if (l.endsWith(".json")) return "application/json";
  if (l.endsWith(".txt") || l.endsWith(".klog") || l.endsWith(".kpro") || l.endsWith(".csv")) return "text/plain";
  if (l.endsWith(".png")) return "image/png";
  if (l.endsWith(".jpg")) return "image/jpeg";
  return "application/octet-stream";
}

const char* outcomeText(int oc) {
  switch (oc) {
    case OC_OK: return "OK";
    case OC_BUSY: return "The roaster is busy (roasting or just finished) - try again after the roast";
    case OC_DENIED: return "Writing is switched off (ALLOW_WRITE=0)";
    case OC_NOT_FOUND: return "Not found";
    case OC_CANNOT_DELETE: return "Cannot delete (folders must be empty)";
    case OC_CANNOT_SAVE: return "Cannot save";
    default: return "Error";
  }
}

bool webRel(const char* arg, String& rel) {
  if (!web.hasArg(arg) || !normPath(web.arg(arg), rel)) { web.send(400, "text/plain", "Bad request"); return false; }
  return true;
}

void handleList() {
  String rel;
  if (!webRel("path", rel)) return;
  String out = "[";
  bool first = true;
  bool isLogs = lower(rel) == LOG_DIR;
  bool ok = fatForEach(rel, [&](const FILINFO& f) {
    String n = f.fname;
    // Windows and macOS housekeeping (.Spotlight-V100, .fseventsd, .Trashes, .DS_Store, ._ AppleDouble files)
    if (n.startsWith(".") || n == "System Volume Information" || n == "$RECYCLE.BIN" || n.endsWith(".tmp~")) return;
    if (!first) out += ",";
    first = false;
    uint32_t roasted = 0;
    if (isLogs && lower(n).endsWith(".klog") && !(f.fattrib & AM_DIR)) {
      roasted = logRoastDate(String(LOG_DIR) + "/" + n);       // roast_date in the log itself
      if (!roasted && n.length() <= 15) roasted = gDates.getUInt(n.c_str(), 0);   // not yet written into the log
    }
    String child = n;
    if (rel.length()) child = rel + "/" + n;
    out += "{\"name\":\"" + jsonEsc(n) + "\",\"path\":\"/" + jsonEsc(child) +
           "\",\"size\":" + String((uint32_t)f.fsize) + ",\"dir\":" + ((f.fattrib & AM_DIR) ? "true" : "false") +
           ",\"mtime\":" + String(fatToEpoch(f.fdate, f.ftime)) + ",\"roasted\":" + String(roasted) + "}";
  });
  out += "]";
  if (!ok) { web.send(404, "application/json", "[]"); return; }
  web.send(200, "application/json", out);
}

void handleStatus() {
  uint64_t used = 0, size = 0;
  if (!gWriteWin) fatUsage(used, size);
  char b[900];
  snprintf(b, sizeof b,
           "{\"version\":\"" KLDONGLE_VERSION "\",\"storage\":\"%s\",\"ip\":\"%s\",\"hostname\":\"%s\",\"rssi\":%d,\"studio\":%s,\"studioPeer\":\"%s\",\"live\":\"%s\","
           "\"roasterActive\":%s,\"roasterBusy\":%s,\"lastHostWriteAgo\":%ld,\"usbAttached\":%s,\"writeWindow\":%s,"
           "\"allowWrite\":%s,\"hostReads\":%lu,\"hostWrites\":%lu,\"usbErrors\":%lu,\"used\":%llu,\"size\":%llu,"
           "\"uptime\":%lu,\"heap\":%lu,\"time\":%ld}",
           gStore == STORE_SD ? "SD card" : "internal flash",
           WiFi.localIP().toString().c_str(), jsonEsc(cfg.hostname).c_str(), WiFi.RSSI(),
           gStudioConnected ? "true" : "false", gStudioPeer, gRoasting ? gRoastName : "",
           hostBusy(LIVE_WINDOW_MS) ? "true" : "false", hostBusy(BUSY_WINDOW_MS) ? "true" : "false",
           gHostEverWrote ? (long)((millis() - gLastHostWrite) / 1000) : -1L,
           gUsbAttached ? "true" : "false", gWriteWin ? "true" : "false", cfg.allowWrite ? "true" : "false",
           (unsigned long)gHostReads, (unsigned long)gHostWrites, (unsigned long)gUsbErrors,
           (unsigned long long)used, (unsigned long long)size, (unsigned long)(millis() / 1000),
           (unsigned long)ESP.getFreeHeap(), timeValid() ? (long)time(nullptr) : 0L);
  web.send(200, "application/json", b);
}

void handleDownload() {
  String rel;
  if (!webRel("file", rel)) return;
  FsUse u;
  if (!u.ok || f_open(&gWebFil, fp(rel).c_str(), FA_READ) != FR_OK) { web.send(404, "text/plain", "File not found"); return; }
  uint32_t size = f_size(&gWebFil);
  web.setContentLength(size);
  web.sendHeader("Content-Disposition", "attachment; filename=\"" + baseOf(rel) + "\"");
  web.send(200, contentType(rel), "");
  UINT br;
  while (size > 0 && f_read(&gWebFil, gWebBuf, sizeof gWebBuf, &br) == FR_OK && br > 0) {
    web.sendContent((const char*)gWebBuf, br);
    size -= br;
  }
  f_close(&gWebFil);
}

void handleDelete() {
  String rel;
  if (!webRel("file", rel)) return;
  if (rel.length() == 0) { web.send(400, "text/plain", "Bad request"); return; }
  int r = fatDelete(rel);
  klog("web: delete /%s -> %d", rel.c_str(), r);
  web.send(r == OC_OK ? 200 : (r == OC_BUSY ? 409 : 500), "text/plain", outcomeText(r));
}

void handleUploadData() {
  HTTPUpload& up = web.upload();
  if (up.status == UPLOAD_FILE_START) {
    String dir, rel;
    gUploadErr = OC_OK;
    if (!normPath(web.arg("path"), dir) || !normPath(dir + "/" + up.filename, rel) || baseOf(rel).length() == 0) {
      gUploadErr = OC_NOT_VALID;
      return;
    }
    // Fail early instead of after the transfer. The stick is not touched yet: small uploads are collected in RAM
    // and written in one short window at the end.
    if (!cfg.allowWrite) { gUploadErr = OC_DENIED; return; }
    if (hostBusy(BUSY_WINDOW_MS)) { gUploadErr = OC_BUSY; return; }
    if (gUploadOpen) {
      FsUse u;
      f_close(&gUpFil); f_unlink(fp(gUploadTmp).c_str()); gUploadOpen = false;
    }
    gUploadRel = rel;
    gUploadTmp = rel + ".tmp~";
    gUpBuf.clear();
    gUpBuffering = true;
    klog("web: upload /%s ...", rel.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (gUploadErr != OC_OK) return;
    if (gUpBuffering) {
      if (gUpBuf.size() + up.currentSize <= UPLOAD_RAM_MAX) {
        gUpBuf.insert(gUpBuf.end(), up.buf, up.buf + up.currentSize);
        return;
      }
      // too big for RAM: open the write window now and stream the rest into a tmp file
      FsUse u;
      int r = beginWrite();
      if (r != OC_OK) { gUploadErr = r; gUpBuffering = false; gUpBuf.clear(); return; }
      fatMkdirs(parentOf(gUploadRel));
      if (f_open(&gUpFil, fp(gUploadTmp).c_str(), FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { gUploadErr = OC_CANNOT_SAVE; return; }
      gUploadOpen = true;
      gUpBuffering = false;
      UINT bw = 0;
      if (f_write(&gUpFil, gUpBuf.data(), gUpBuf.size(), &bw) != FR_OK || bw != gUpBuf.size()) gUploadErr = OC_CANNOT_SAVE;
      gUpBuf.clear();
      gUpBuf.shrink_to_fit();
      klog("web: large upload - stick away from the roaster until it finishes");
    }
    if (!gUploadOpen) return;
    FsUse u;
    UINT bw = 0;
    if (f_write(&gUpFil, up.buf, up.currentSize, &bw) != FR_OK || bw != up.currentSize) gUploadErr = OC_CANNOT_SAVE;
    gLastFsWrite = millis();
  } else if (up.status == UPLOAD_FILE_END) {
    if (gUpBuffering) {
      gUpBuffering = false;
      if (gUploadErr == OC_OK) gUploadErr = fatWriteFile(gUploadRel, gUpBuf.data(), gUpBuf.size(), 0, false);
      gUpBuf.clear();
      gUpBuf.shrink_to_fit();
      klog("web: upload /%s (%u bytes) -> %d", gUploadRel.c_str(), (unsigned)up.totalSize, gUploadErr);
      return;
    }
    if (!gUploadOpen) return;
    FsUse u;
    if (f_close(&gUpFil) != FR_OK) gUploadErr = OC_CANNOT_SAVE;
    gUploadOpen = false;
    if (gUploadErr == OC_OK) {
      f_unlink(fp(gUploadRel).c_str());
      if (f_rename(fp(gUploadTmp).c_str(), fp(gUploadRel).c_str()) != FR_OK) gUploadErr = OC_CANNOT_SAVE;
    }
    if (gUploadErr != OC_OK) f_unlink(fp(gUploadTmp).c_str());
    gLastFsWrite = millis();
    klog("web: upload /%s (%u bytes) -> %d", gUploadRel.c_str(), (unsigned)up.totalSize, gUploadErr);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    gUpBuffering = false;
    gUpBuf.clear();
    gUpBuf.shrink_to_fit();
    if (!gUploadOpen) return;
    FsUse u;
    f_close(&gUpFil);
    f_unlink(fp(gUploadTmp).c_str());
    gUploadOpen = false;
    gUploadErr = OC_SYS_ERR;
    klog("web: upload aborted");
  }
}

void handleUploadDone() {
  web.send(gUploadErr == OC_OK ? 200 : (gUploadErr == OC_BUSY ? 409 : 500), "text/plain", outcomeText(gUploadErr));
}

void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html", INDEX_HTML); });
  web.on("/list", HTTP_GET, handleList);
  web.on("/status", HTTP_GET, handleStatus);
  web.on("/download", HTTP_GET, handleDownload);
  web.on("/delete", HTTP_DELETE, handleDelete);
  web.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  web.on("/log", HTTP_GET, []() { web.send(200, "text/plain; charset=utf-8", logText()); });
  web.on("/reboot", HTTP_POST, []() {
    web.send(200, "text/plain", "Rebooting");
    delay(200);
    finishWrite(true);
    ESP.restart();
  });
  web.onNotFound([]() { web.send(404, "text/plain", "Not found"); });
  web.begin();
}

void startNet() {
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  String h = lower(cfg.hostname);
  if (MDNS.begin(h.c_str())) MDNS.addService("http", "tcp", 80);
  NBNS.begin(cfg.hostname.c_str());
  setupWeb();
  sassiSrv.begin();
  sassiSrv.setNoDelay(true);
  gNetUp = true;
  klog("network up: http://%s/  (or http://%s.local/), Studio port 9056", WiFi.localIP().toString().c_str(), h.c_str());
}

// ------------------------------------------------------------------ status LED
void ledTick() {
  static uint32_t last = 0;
  static int lastState = -1;
  int st;
  if (!gSdOk || gNoFs) st = 0;
  else if (gWriteWin) st = 1;
  else if (gRoasting) st = 2;
  else if (gStudioConnected) st = 3;
  else if (WiFi.status() == WL_CONNECTED) st = 4;
  else st = 5;
  bool animated = st == 0 || st == 2 || st == 5;
  if (millis() - last < (st == 2 ? 30u : 250u)) return;
  last = millis();
  if (st == lastState && !animated) return;
  lastState = st;
  bool on = !animated || ((millis() / 500) & 1);
  switch (st) {
    case 0: setLED(on ? 128 : 0, 0, 0, 20); break;            // red blink: no SD card, or card not formatted
    case 1: setLED(128, 128, 0, 8); break;                    // yellow: writing (stick away from the roaster)
    case 2: {                                                 // roasting: breathe orange <-> red (4 s cycle)
      float ph = (millis() % 4000) / 4000.0f;
      float k = 0.5f - 0.5f * cosf(2.0f * PI * ph);           // 0 = red, 1 = orange
      setLED(255, (uint8_t)(90 * k), 0, 8);
      break;
    }
    case 3: setLED(0, 128, 128, 8); break;                    // cyan: Studio connected
    case 4: setLED(0, 0, 128, 8); break;                      // blue: WiFi connected
    case 5: setLED(0, on ? 128 : 0, 0, 8); break;             // green blink: USB stick only, no WiFi
  }
}

// Internal flash is formatted only if it has never held a file system (no boot sector signature). A volume that
// exists but fails to mount is left alone: it may hold logs.
void formatFlashIfBlank() {
  if (blkRead(gWebBuf, 0, 1) != ESP_OK) return;
  if (gWebBuf[510] == 0x55 && gWebBuf[511] == 0xAA) return;
  // FAT16 superfloppy, 2 KB clusters (12.8 MB is too small for FAT32), like a small USB stick
  MKFS_PARM opt = { FM_FAT | FM_SFD, 0, 0, 0, 2048 };
  FRESULT fr = f_mkfs(gDrv, &opt, gWebBuf, sizeof gWebBuf);
  klog("internal storage: first use - formatted FAT16: %s", fr == FR_OK ? "ok" : "FAILED");
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  setCpuFrequencyMhz(160);                                    // 240 is not needed; runs cooler
  Serial.begin(115200);
  gLogLock = xSemaphoreCreateMutex();
  gSdLock = xSemaphoreCreateMutex();
  gFsLock = xSemaphoreCreateRecursiveMutex();
  initLED();
  setLED(128, 64, 0, 8);                                      // orange: starting
  klog("KLDongle %s starting", KLDONGLE_VERSION);

  if (sdInit()) { gStore = STORE_SD; gBlkSectors = gCard.csd.capacity; }
  else if (flashInit()) gStore = STORE_FLASH;
  gSdOk = gStore != STORE_NONE;
  if (!gSdOk) {
    klog("FATAL: no SD card and no internal storage");
    for (;;) { setLED(128, 0, 0, 20); delay(500); setLED(0, 0, 0, 0); delay(500); }
  }
  if (ff_diskio_get_drive(&gPdrv) != ESP_OK) { klog("FATAL: no free FatFs drive"); for (;;) delay(1000); }
  ff_diskio_register(gPdrv, &gDiskio);
  gDrv[0] = '0' + gPdrv;
  if (gStore == STORE_FLASH) formatFlashIfBlank();
  clearDirtyFlag();

  // Before USB starts nobody else uses the card: one write window for config + IP profile.
  xSemaphoreTakeRecursive(gFsLock, portMAX_DELAY);
  gWriteWin = true;
  gFsOk = f_mount(&gFs, gDrv, 1) == FR_OK;
  gLastFsWrite = millis();
  xSemaphoreGiveRecursive(gFsLock);
  gNoFs = !gFsOk;
  if (gNoFs) klog(gStore == STORE_SD ? "no FAT file system on the card - format it FAT32 (the dongle still shows up as a USB disk, so a computer can format it)"
                                     : "internal storage does not mount - NOT reformatting it (it may hold logs); format the USB disk from a computer");
  else loadConfig();
  ovLoad();
  gDates.begin("kl-roast-start", false);                       // new name in 1.1.0: drops 1.0.0's notes (some stale)

  if (cfg.ssid.length()) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(cfg.hostname.c_str());
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfg.ssid.c_str(), cfg.psk.c_str());
    WiFi.setSleep(true);                                      // modem sleep: much less power/heat, ~0.1 s latency
    for (int i = 0; i < 32 && WiFi.status() != WL_CONNECTED; i++) {   // up to 8 s, then the stick starts anyway
      setLED(0, (i & 1) ? 128 : 0, 0, 8);
      delay(250);
    }
    if (WiFi.status() == WL_CONNECTED && gFsOk) ensureIpProfile();
  } else {
    klog("WiFi not configured - edit %s on the stick", CONFIG_FILE);
  }
  if (gFsOk) collectLogs();
  finishWrite(true);

  MSC.vendorID("KLDongle");
  MSC.productID("KL Stick");
  MSC.productRevision("1.0");
  MSC.onRead(mscRead);
  MSC.onWrite(mscWrite);
  MSC.onStartStop(mscStartStop);
  MSC.mediaPresent(true);
  MSC.begin(gBlkSectors, 512);
  gUsbAttached = true;
  USB.begin();
  gUsbStarted = true;
  klog("USB stick started (%lu sectors, %s)", (unsigned long)gBlkSectors, gStore == STORE_SD ? "SD card" : "internal flash");

  xTaskCreatePinnedToCore(sassiTask, "sassi", 12288, nullptr, 1, nullptr, 1);
}

void loop() {
  static uint32_t tMaint = 0, tDates = 0, tIp = 0;
  static IPAddress llmnrIp;
  bool wifi = WiFi.status() == WL_CONNECTED;
  if (wifi && !gNetUp) startNet();
  if (wifi && gNetUp && !(WiFi.localIP() == llmnrIp)) {       // first start or new address: (re)join the LLMNR group
    llmnrUdp.stop();
    llmnrUdp.beginMulticast(IPAddress(224, 0, 0, 252), 5355);
    llmnrIp = WiFi.localIP();
  }
  if (gNetUp) {
    web.handleClient();
    llmnrPoll();
  }
  if (millis() - tMaint > 50) { tMaint = millis(); finishWrite(false); }
  if (millis() - tDates > 1000) { tDates = millis(); roastTick(); }
  if (wifi && (!gIpProfileDone || !(WiFi.localIP() == gLastIp)) && millis() - tIp > 10000) {
    tIp = millis();
    if (!hostBusy(BUSY_WINDOW_MS)) ensureIpProfile();
  }
  ledTick();
  delay(2);
}
