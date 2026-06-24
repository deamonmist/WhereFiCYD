//main.py
//<=(oo)=> It's the main one y'all. It's axiomatic. It's reflexive. It's...magic.
//+++++++++++++++++++++++++++++//written_by_deamonmist//+++++++++++++++++++++++++++++++++

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BluetoothSerial.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

// ─── Pins ─────────────────────────────────────────────────────────────────────
#define BACKLIGHT_PIN  27
#define TOUCH_CS_PIN   33
XPT2046_Touchscreen touch(TOUCH_CS_PIN, 255);

// ─── Layout ───────────────────────────────────────────────────────────────────
#define SCREEN_W    320
#define SCREEN_H    480
#define NUM_CH       13

// ─── Radio / App state ────────────────────────────────────────────────────────
enum RadioMode  { RADIO_WIFI, RADIO_BLE, RADIO_CLASSIC };
enum AppState   { STATE_SCAN, STATE_LIST, STATE_TRACKER };
RadioMode radioMode = RADIO_WIFI;
AppState  appState  = STATE_SCAN;

TFT_eSPI tft;

// ─── WiFi channel state ───────────────────────────────────────────────────────
volatile uint8_t currentChannel = 0;
uint32_t dwellStart = 0;
uint32_t dwellMs    = 80;

// ─── Security types ───────────────────────────────────────────────────────────
enum SecType { SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA2, SEC_WPA3, SEC_UNKNOWN };
const char* secLabel(SecType s) {
  switch(s) {
    case SEC_OPEN:  return "OPEN";
    case SEC_WEP:   return "WEP";
    case SEC_WPA:   return "WPA";
    case SEC_WPA2:  return "WPA2";
    case SEC_WPA3:  return "WPA3";
    default:        return "?";
  }
}
uint16_t secColor(SecType s) {
  switch(s) {
    case SEC_OPEN:  return TFT_RED;
    case SEC_WEP:   return TFT_ORANGE;
    case SEC_WPA:   return TFT_YELLOW;
    case SEC_WPA2:  return TFT_GREEN;
    case SEC_WPA3:  return tft.color565(0,255,200);
    default:        return TFT_DARKGREY;
  }
}

// ─── Generic device table (shared by all three modes) ─────────────────────────
#define MAX_TRACKED  20
#define TOP_N         5

struct DeviceEntry {
  char    name[33];       // SSID or BT device name
  char    detail[33];     // MAC for WiFi/BLE, class for Classic
  int8_t  rssi;
  uint8_t channel;        // WiFi channel, or BLE adv type, or BT class
  uint8_t mac[6];
  SecType security;       // WiFi only
  bool    active;
  // BLE extras
  char    manufacturer[32];
  bool    connectable;
};

DeviceEntry wifiTable[MAX_TRACKED];
uint8_t     wifiCount = 0;
DeviceEntry bleTable[MAX_TRACKED];
uint8_t     bleCount  = 0;
DeviceEntry classicTable[MAX_TRACKED];
uint8_t     classicCount = 0;

uint8_t topList[TOP_N];
uint8_t topCount = 0;

// ─── Scan timing ──────────────────────────────────────────────────────────────
#define WIFI_SCAN_MS     10000
#define BLE_SCAN_MS       8000
#define CLASSIC_SCAN_MS  10000
uint32_t scanStart = 0;

// ─── Tracker state ────────────────────────────────────────────────────────────
uint8_t trackerIdx   = 0;
int8_t  trackerPeak  = -100;
float   bellCenter   = 160.0f;
float   bellVelocity = 0.0f;

#define RSSI_HISTORY 60
int8_t  rssiHistory[RSSI_HISTORY];
uint8_t rssiHistHead = 0;
bool    rssiHistFull = false;

// ─── Touch ────────────────────────────────────────────────────────────────────
uint32_t lastTouchMs = 0;
uint32_t touchDownMs = 0;
bool     touchWasDown = false;
bool     debugOverlay = false;
#define  TOUCH_DEBOUNCE  400
#define  LONG_PRESS_MS  1200

// ─── BLE scanner ──────────────────────────────────────────────────────────────
BLEScan* pBLEScan = nullptr;
bool     bleScanRunning = false;

// ─── Classic BT ───────────────────────────────────────────────────────────────
bool classicScanRunning = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────
uint16_t heatColor(uint8_t v) {
  if (v < 64)  { uint8_t t = v*4;       return tft.color565(0,   0,   t);     }
  if (v < 128) { uint8_t t = (v-64)*4;  return tft.color565(0,   t,   255-t); }
  if (v < 192) { uint8_t t = (v-128)*4; return tft.color565(t,   255, 0);     }
                 uint8_t t = (v-192)*4; return tft.color565(255, 255-t, 0);
}
uint8_t rssiToQuality(int8_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -30)  return 100;
  return (uint8_t)((rssi + 100) * (100.0f / 70.0f));
}
uint8_t getBatteryPct() {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < 10; i++) sum += analogRead(34);
  float v = (sum / 10.0f) * (3.3f / 4095.0f) * 2.0f;
  if (v >= 4.2f) return 100;
  if (v <= 3.0f) return 0;
  return (uint8_t)((v - 3.0f) / 1.2f * 100.0f);
}

// ─── Active table accessors ───────────────────────────────────────────────────
DeviceEntry* activeTable() {
  if (radioMode == RADIO_WIFI)    return wifiTable;
  if (radioMode == RADIO_BLE)     return bleTable;
  return classicTable;
}
uint8_t& activeCount() {
  if (radioMode == RADIO_WIFI)    return wifiCount;
  if (radioMode == RADIO_BLE)     return bleCount;
  return classicCount;
}
uint32_t scanDuration() {
  if (radioMode == RADIO_WIFI)    return WIFI_SCAN_MS;
  if (radioMode == RADIO_BLE)     return BLE_SCAN_MS;
  return CLASSIC_SCAN_MS;
}

// ─── Security parser ──────────────────────────────────────────────────────────
SecType parseSecurity(const uint8_t* payload, uint16_t len) {
  if (len < 36) return SEC_UNKNOWN;
  bool hasPrivacy = (payload[34] & 0x10) != 0;
  bool hasRSN = false, hasWPA = false, hasWPA3 = false;
  uint16_t pos = 36;
  while (pos + 2 <= len) {
    uint8_t id = payload[pos], eln = payload[pos+1];
    if (pos + 2 + eln > len) break;
    if (id == 48 && eln >= 4) {
      hasRSN = true;
      if (eln >= 12) {
        uint16_t pairCount = payload[pos+2+4] | (payload[pos+2+5] << 8);
        uint16_t akmStart  = pos + 2 + 4 + 2 + pairCount * 4 + 2;
        if (akmStart + 4 <= len) {
          uint8_t akmType = payload[akmStart+3];
          if (akmType == 8 || akmType == 9) hasWPA3 = true;
        }
      }
    }
    if (id == 221 && eln >= 4) {
      if (payload[pos+2]==0x00 && payload[pos+3]==0x50 &&
          payload[pos+4]==0xF2 && payload[pos+5]==0x01) hasWPA = true;
    }
    pos += 2 + eln;
  }
  if (hasWPA3) return SEC_WPA3;
  if (hasRSN)  return SEC_WPA2;
  if (hasWPA)  return SEC_WPA;
  if (hasPrivacy) return SEC_WEP;
  return SEC_OPEN;
}

// ─── Device upsert ────────────────────────────────────────────────────────────
void upsertDevice(DeviceEntry* table, uint8_t& count,
                  const char* name, int8_t rssi, uint8_t chan,
                  const uint8_t* mac, SecType sec,
                  const char* manufacturer = "", bool connectable = false) {
  // Match by MAC
  for (uint8_t i = 0; i < count; i++) {
    if (memcmp(table[i].mac, mac, 6) == 0) {
      // In scan mode keep peak RSSI, in tracker always use latest
      if (appState == STATE_TRACKER || rssi > table[i].rssi) {
        table[i].rssi    = rssi;
        table[i].channel = chan;
      }
      return;
    }
  }
  if (count >= MAX_TRACKED) return;
  strncpy(table[count].name, name[0] ? name : "Unknown", 32);
  table[count].name[32] = '\0';
  table[count].rssi     = rssi;
  table[count].channel  = chan;
  table[count].security = sec;
  table[count].active   = true;
  table[count].connectable = connectable;
  memcpy(table[count].mac, mac, 6);
  snprintf(table[count].detail, sizeof(table[count].detail),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  strncpy(table[count].manufacturer, manufacturer, 31);
  table[count].manufacturer[31] = '\0';
  count++;
}

void buildTopList() {
  DeviceEntry* table = activeTable();
  uint8_t      cnt   = activeCount();
  topCount = 0;
  bool used[MAX_TRACKED] = {false};
  for (uint8_t n = 0; n < TOP_N && n < cnt; n++) {
    int8_t best = -127; uint8_t bestIdx = 0;
    for (uint8_t i = 0; i < cnt; i++) {
      if (!used[i] && table[i].rssi > best) { best = table[i].rssi; bestIdx = i; }
    }
    used[bestIdx] = true;
    topList[topCount++] = bestIdx;
  }
}

// ─── WiFi sniffer ─────────────────────────────────────────────────────────────
void IRAM_ATTR wifiSniffCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const uint8_t* p   = pkt->payload;
  uint16_t       len = pkt->rx_ctrl.sig_len;
  int8_t         rssi = pkt->rx_ctrl.rssi;
  if (len < 38 || p[0] != 0x80) return;
  uint8_t ssidLen = p[37];
  if (ssidLen == 0 || ssidLen > 32 || (38u+ssidLen) >= len) return;
  char ssid[33]; memcpy(ssid, &p[38], ssidLen); ssid[ssidLen] = '\0';
  const uint8_t* mac = &p[16];
  SecType sec = parseSecurity(p, len);
  if (appState == STATE_SCAN)
    upsertDevice(wifiTable, wifiCount, ssid, rssi, currentChannel+1, mac, sec);
  else if (appState == STATE_TRACKER) {
    for (uint8_t i = 0; i < wifiCount; i++) {
      if (memcmp(wifiTable[i].mac, mac, 6) == 0) { wifiTable[i].rssi = rssi; break; }
    }
  }
}

void drawScanScreen(); // forward declaration

void advanceChannel() {
  currentChannel = (currentChannel + 1) % NUM_CH;
  esp_wifi_set_channel(currentChannel + 1, WIFI_SECOND_CHAN_NONE);
  dwellStart = millis();
}

// ─── BLE callback ─────────────────────────────────────────────────────────────
class BLECallback : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    uint8_t mac[6];
    uint64_t addr = *(uint64_t*)dev.getAddress().getNative();
    for (int i = 0; i < 6; i++) mac[i] = (addr >> (i*8)) & 0xFF;
    const char* name = dev.haveName() ? dev.getName().c_str() : "";
    const char* mfr  = dev.haveManufacturerData() ? "MFR" : "";
    upsertDevice(bleTable, bleCount, name, dev.getRSSI(), 0, mac,
                 SEC_UNKNOWN, mfr, false);
  }
};
BLECallback bleCb;


// ─── Classic BT callback ──────────────────────────────────────────────────────
void classicBtCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
  if (event == ESP_BT_GAP_DISC_RES_EVT) {
    uint8_t* mac = param->disc_res.bda;
    char name[33] = "Unknown";
    int8_t rssi = -100;
    uint32_t cod = 0;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
      esp_bt_gap_dev_prop_t* p = &param->disc_res.prop[i];
      if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
        uint8_t l = min((uint8_t)p->len, (uint8_t)32);
        memcpy(name, p->val, l); name[l] = '\0';
      }
      if (p->type == ESP_BT_GAP_DEV_PROP_RSSI) rssi = *(int8_t*)p->val;
      if (p->type == ESP_BT_GAP_DEV_PROP_COD)  cod  = *(uint32_t*)p->val;
    }
    upsertDevice(classicTable, classicCount, name, rssi, 0, mac, SEC_UNKNOWN);
  }
}

// ─── Radio init/deinit ────────────────────────────────────────────────────────
void stopWifi() {
  esp_wifi_set_promiscuous(false);
  esp_wifi_stop();
  esp_wifi_deinit();
  esp_event_loop_delete_default();
}

void startWifi() {
  esp_event_loop_create_default();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(wifiSniffCb);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  currentChannel = 0;
  dwellStart = millis();
}

void stopBLE() {
  if (pBLEScan) { pBLEScan->stop(); bleScanRunning = false; }
  BLEDevice::deinit(false);
  pBLEScan = nullptr;
}

void startBLE() {
  BLEDevice::init("");
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(&bleCb, true);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->start(BLE_SCAN_MS / 1000, true);  // async, non-blocking
  bleScanRunning = true;
}

void stopClassic() {
  esp_bt_gap_cancel_discovery();
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  classicScanRunning = false;
}

void startClassic() {
  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_bt_controller_init(&cfg);
  esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  esp_bluedroid_init();
  esp_bluedroid_enable();
  esp_bt_gap_register_callback(classicBtCb);
  esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
  esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
  classicScanRunning = true;
}

// ─── Mode switching ───────────────────────────────────────────────────────────
void switchToMode(RadioMode newMode) {
  radioMode = newMode;
  appState  = STATE_SCAN;
  topCount  = 0;

  // Draw scan screen immediately so user sees it while radio switches
  drawScanScreen();

  // Stop current radio
  if (radioMode == RADIO_WIFI)    stopWifi();
  if (radioMode == RADIO_BLE)     stopBLE();
  if (radioMode == RADIO_CLASSIC) stopClassic();

  // Start new radio — reset scanStart AFTER so timer is accurate
  if (radioMode == RADIO_WIFI)    startWifi();
  if (radioMode == RADIO_BLE)     startBLE();
  if (radioMode == RADIO_CLASSIC) startClassic();

  scanStart = millis();
}

// ─── UI shared helpers ────────────────────────────────────────────────────────
void drawBatteryIcon(int x, int y, uint8_t pct) {
  uint16_t bc = pct > 50 ? TFT_GREEN : pct > 20 ? TFT_YELLOW : TFT_RED;
  tft.drawRoundRect(x, y, 27, 14, 2, TFT_WHITE);
  tft.fillRect(x+27, y+4, 3, 6, TFT_WHITE);
  tft.fillRect(x+2, y+2, 23, 10, tft.color565(20,20,20));
  uint8_t fw = (uint8_t)(pct * 23 / 100);
  if (fw > 0) tft.fillRect(x+2, y+2, fw, 10, bc);
  tft.setTextColor(TFT_WHITE, tft.color565(0,20,42));
  char buf[5]; snprintf(buf, sizeof(buf), "%d%%", pct);
  tft.drawString(buf, x-28, y+2, 1);
}

void drawModeButtons(uint16_t headerBg) {
  // Three equal-width buttons across the bottom of the header
  // Each is 106px wide, 28px tall, at y=22
  #define BTN_Y    22
  #define BTN_H    26
  #define BTN_W   104

  // WiFi
  uint16_t wBg = radioMode == RADIO_WIFI ? tft.color565(0,70,120) : tft.color565(15,15,35);
  tft.fillRoundRect(2, BTN_Y, BTN_W, BTN_H, 4, wBg);
  if (radioMode == RADIO_WIFI) tft.drawRoundRect(2, BTN_Y, BTN_W, BTN_H, 4, TFT_CYAN);
  tft.setTextColor(radioMode == RADIO_WIFI ? TFT_CYAN : tft.color565(80,80,100), wBg);
  tft.drawCentreString("WiFi", 54, BTN_Y+7, 2);

  // BLE
  uint16_t bBg = radioMode == RADIO_BLE ? tft.color565(60,0,100) : tft.color565(15,15,35);
  tft.fillRoundRect(108, BTN_Y, BTN_W, BTN_H, 4, bBg);
  if (radioMode == RADIO_BLE) tft.drawRoundRect(108, BTN_Y, BTN_W, BTN_H, 4, tft.color565(180,100,255));
  tft.setTextColor(radioMode == RADIO_BLE ? tft.color565(180,100,255) : tft.color565(80,80,100), bBg);
  tft.drawCentreString("BLE", 160, BTN_Y+7, 2);

  // Classic
  uint16_t cBg = radioMode == RADIO_CLASSIC ? tft.color565(80,40,0) : tft.color565(15,15,35);
  tft.fillRoundRect(214, BTN_Y, BTN_W, BTN_H, 4, cBg);
  if (radioMode == RADIO_CLASSIC) tft.drawRoundRect(214, BTN_Y, BTN_W, BTN_H, 4, TFT_ORANGE);
  tft.setTextColor(radioMode == RADIO_CLASSIC ? TFT_ORANGE : tft.color565(80,80,100), cBg);
  tft.drawCentreString("Classic", 266, BTN_Y+7, 2);
}

void drawSignalBar(int x, int y, int w, int h, uint8_t pct, uint16_t col) {
  tft.drawRect(x, y, w, h, tft.color565(40,40,40));
  uint16_t fw = (uint16_t)(pct * (w-2) / 100);
  tft.fillRect(x+1, y+1, w-2, h-2, tft.color565(20,20,20));
  if (fw > 0) tft.fillRect(x+1, y+1, fw, h-2, col);
}

// ─── SCAN screen ──────────────────────────────────────────────────────────────
void drawScanScreen() {
  tft.fillScreen(tft.color565(6,6,14));
  tft.fillRect(0, 0, SCREEN_W, 54, tft.color565(0,20,42));
  tft.drawFastHLine(0, 54, SCREEN_W, TFT_CYAN);

  // Mode label at top
  tft.setTextColor(TFT_CYAN, tft.color565(0,20,42));
  const char* modeStr = radioMode == RADIO_WIFI ? "WiFi Scanner" :
                        radioMode == RADIO_BLE  ? "BLE Scanner" : "Classic BT Scanner";
  tft.drawCentreString(modeStr, SCREEN_W/2, 4, 2);
  drawModeButtons(tft.color565(0,20,42));
  drawBatteryIcon(SCREEN_W-36, 4, getBatteryPct());

  // Scan animation area
  tft.setTextColor(TFT_DARKGREY, tft.color565(6,6,14));
  tft.drawCentreString("Scanning...", SCREEN_W/2, 160, 4);
  tft.drawRoundRect(40, 220, 240, 24, 4, tft.color565(40,40,40));
}

void updateScanProgress(uint32_t elapsed) {
  uint32_t dur = scanDuration();
  uint16_t w   = min((uint16_t)(elapsed * 236 / dur), (uint16_t)236);
  tft.fillRoundRect(42, 222, w, 20, 3,
    radioMode == RADIO_WIFI    ? TFT_CYAN :
    radioMode == RADIO_BLE     ? tft.color565(180,100,255) : TFT_ORANGE);

  char buf[32];
  snprintf(buf, sizeof(buf), "%d devices found", activeCount());
  tft.setTextColor(TFT_WHITE, tft.color565(6,6,14));
  tft.drawCentreString(buf, SCREEN_W/2, 264, 2);

  if (radioMode == RADIO_WIFI) {
    tft.fillRect(60, 290, 200, 20, tft.color565(6,6,14));
    for (uint8_t i = 0; i < NUM_CH; i++) {
      uint16_t col = (i == currentChannel) ? TFT_CYAN : tft.color565(30,30,30);
      tft.fillRect(60 + i*14, 294, 10, 10, col);
    }
  }
}

// ─── LIST screen ──────────────────────────────────────────────────────────────
#define LIST_ITEM_H  78
#define LIST_TOP     54

void drawListScreen() {
  DeviceEntry* table = activeTable();
  tft.fillScreen(tft.color565(8,8,18));

  // Header
  uint16_t hdrBg = tft.color565(0,20,42);
  tft.fillRect(0, 0, SCREEN_W, LIST_TOP, hdrBg);
  tft.drawFastHLine(0, LIST_TOP, SCREEN_W, TFT_CYAN);

  // Title at top
  tft.setTextColor(TFT_CYAN, hdrBg);
  const char* title = radioMode == RADIO_WIFI ? "Top WiFi" :
                      radioMode == RADIO_BLE  ? "Top BLE" : "Top Classic BT";
  tft.drawCentreString(title, SCREEN_W/2, 4, 2);

  drawModeButtons(hdrBg);
  drawBatteryIcon(SCREEN_W-36, 4, getBatteryPct());

  if (topCount == 0) {
    tft.setTextColor(TFT_RED, tft.color565(8,8,18));
    tft.drawCentreString("No devices found", SCREEN_W/2, 240, 2);
    return;
  }

  for (uint8_t n = 0; n < topCount; n++) {
    uint8_t  idx = topList[n];
    int      y   = LIST_TOP + 1 + n * LIST_ITEM_H;
    uint16_t bg  = (n%2==0) ? tft.color565(12,12,28) : tft.color565(8,8,20);

    tft.fillRect(0, y, SCREEN_W, LIST_ITEM_H-1, bg);
    tft.drawFastHLine(0, y+LIST_ITEM_H-1, SCREEN_W, tft.color565(30,30,50));

    // Rank badge
    uint16_t badgeBg = radioMode == RADIO_WIFI    ? tft.color565(0,40,80) :
                       radioMode == RADIO_BLE      ? tft.color565(40,0,80) :
                                                     tft.color565(60,30,0);
    tft.fillRoundRect(6, y+8, 22, 22, 3, badgeBg);
    tft.setTextColor(TFT_WHITE, badgeBg);
    char rank[3]; snprintf(rank, sizeof(rank), "%d", n+1);
    tft.drawCentreString(rank, 17, y+11, 2);

    // Name
    tft.setTextColor(TFT_WHITE, bg);
    char nm[24]; strncpy(nm, table[idx].name, 23); nm[23]='\0';
    tft.drawString(nm, 36, y+8, 2);

    // MAC / detail
    tft.setTextColor(tft.color565(100,100,140), bg);
    tft.drawString(table[idx].detail, 36, y+30, 1);

    // Third line: channel for WiFi, connectable for BLE, blank for Classic
    tft.setTextColor(tft.color565(120,120,160), bg);
    if (radioMode == RADIO_WIFI) {
      char ch[8]; snprintf(ch, sizeof(ch), "CH %d", table[idx].channel);
      tft.drawString(ch, 36, y+44, 1);
    } else if (radioMode == RADIO_BLE) {
      tft.drawString(table[idx].connectable ? "Connectable" : "Broadcast only", 36, y+44, 1);
    } else {
      tft.drawString(table[idx].manufacturer[0] ? table[idx].manufacturer : "Classic BT", 36, y+44, 1);
    }

    // Right side: security badge (WiFi only) or mode badge (BLE/Classic)
    if (radioMode == RADIO_WIFI) {
      SecType sec = table[idx].security;
      uint16_t secBg = tft.color565(
        sec==SEC_OPEN ? 80 : 0,
        sec==SEC_WPA2||sec==SEC_WPA3 ? 60 : 20,
        sec==SEC_WPA3 ? 80 : 20);
      tft.fillRoundRect(SCREEN_W-62, y+8, 54, 18, 3, secBg);
      tft.setTextColor(secColor(sec), secBg);
      tft.drawCentreString(secLabel(sec), SCREEN_W-35, y+11, 1);
    } else {
      uint16_t modeBg = radioMode == RADIO_BLE ? tft.color565(40,0,80) : tft.color565(60,30,0);
      uint16_t modeCol = radioMode == RADIO_BLE ? tft.color565(180,100,255) : TFT_ORANGE;
      tft.fillRoundRect(SCREEN_W-62, y+8, 54, 18, 3, modeBg);
      tft.setTextColor(modeCol, modeBg);
      tft.drawCentreString(radioMode==RADIO_BLE ? "BLE" : "BT", SCREEN_W-35, y+11, 1);
    }

    // RSSI + signal bar
    int8_t rssi = table[idx].rssi;
    uint16_t rssiCol = rssi > -60 ? TFT_GREEN : rssi > -75 ? TFT_YELLOW : TFT_RED;
    tft.setTextColor(rssiCol, bg);
    char rs[8]; snprintf(rs, sizeof(rs), "%ddBm", rssi);
    tft.drawString(rs, SCREEN_W-62, y+44, 1);
    drawSignalBar(SCREEN_W-62, y+30, 54, 8, rssiToQuality(rssi), rssiCol);
  }
}

// ─── TRACKER screen ───────────────────────────────────────────────────────────
#define HEADER_H    110
#define BELL_TOP    HEADER_H
#define BELL_H      (SCREEN_H - HEADER_H - 80)
#define FOOTER_TOP  (SCREEN_H - 80)
#define BELL_COLS    48
#define GLOW_LAYERS   4

int16_t bellHeights[BELL_COLS];
int16_t prevBellHeights[BELL_COLS];

float gaussianH(float x, float center, float sigma, float peakH) {
  float d = x - center;
  return peakH * expf(-(d*d) / (2.0f*sigma*sigma));
}
void computeBell(float center, float peakH) {
  for (uint8_t i = 0; i < BELL_COLS; i++) {
    float bx = (i+0.5f) * (SCREEN_W / (float)BELL_COLS);
    bellHeights[i] = (int16_t)gaussianH(bx, center, 55.0f, peakH);
  }
}

void drawTrackerScreen(bool full) {
  DeviceEntry* table = activeTable();
  uint8_t idx  = topList[trackerIdx];
  int8_t  rssi = table[idx].rssi;
  if (rssi > trackerPeak) trackerPeak = rssi;

  static uint32_t lastHistUpdate = 0;
  if (millis() - lastHistUpdate > 500) {
    rssiHistory[rssiHistHead] = rssi;
    rssiHistHead = (rssiHistHead+1) % RSSI_HISTORY;
    if (!rssiHistFull && rssiHistHead == 0) rssiHistFull = true;
    lastHistUpdate = millis();
  }

  float delta = (float)(trackerPeak - rssi);
  if (delta > 3.0f) {
    bellVelocity += (random(-8,8) * 0.4f);
    bellVelocity *= 0.82f;
  } else {
    bellVelocity += (160.0f - bellCenter) * 0.08f;
    bellVelocity *= 0.65f;
  }
  bellCenter += bellVelocity;
  bellCenter  = constrain(bellCenter, 40.0f, 280.0f);

  float   peakH = (float)rssiToQuality(rssi) / 100.0f * (BELL_H - 8);
  uint8_t qual  = rssiToQuality(rssi);

  // Mode accent color
  uint16_t accentCol = radioMode == RADIO_WIFI    ? TFT_CYAN :
                       radioMode == RADIO_BLE      ? tft.color565(180,100,255) : TFT_ORANGE;

  if (full) {
    tft.fillScreen(tft.color565(6,6,14));
    tft.fillRect(0, 0, SCREEN_W, HEADER_H, tft.color565(0,20,42));
    tft.drawFastHLine(0, HEADER_H, SCREEN_W, accentCol);

    // Row 1 (y=2-20): Device name centered, battery top-right
    tft.setTextColor(TFT_WHITE, tft.color565(0,20,42));
    // Truncate name to leave room for battery
    char nm[20]; strncpy(nm, table[idx].name, 19); nm[19]='\0';
    tft.drawCentreString(nm, SCREEN_W/2 - 20, 3, 2);
    drawBatteryIcon(SCREEN_W-38, 3, getBatteryPct());

    // Row 2 (y=22-48): Mode buttons (full width)
    drawModeButtons(tft.color565(0,20,42));

    // Row 3 (y=52-62): MAC address
    tft.setTextColor(tft.color565(100,100,160), tft.color565(0,20,42));
    tft.drawCentreString(table[idx].detail, SCREEN_W/2, 52, 1);

    // Row 4 (y=64-74): Security/type + Back button
    if (radioMode == RADIO_WIFI) {
      char info[28];
      snprintf(info, sizeof(info), "%s  |  CH %d",
               secLabel(table[idx].security), table[idx].channel);
      tft.setTextColor(secColor(table[idx].security), tft.color565(0,20,42));
      tft.drawCentreString(info, SCREEN_W/2 + 30, 66, 1);
    } else if (radioMode == RADIO_BLE) {
      tft.setTextColor(accentCol, tft.color565(0,20,42));
      tft.drawCentreString(table[idx].connectable ? "Connectable" : "Broadcast", SCREEN_W/2 + 30, 66, 1);
    } else {
      tft.setTextColor(accentCol, tft.color565(0,20,42));
      tft.drawCentreString("Classic BT", SCREEN_W/2 + 30, 66, 1);
    }

    // Back button sits in row 4, left side
    tft.fillRoundRect(4, 62, 68, 24, 4, tft.color565(50,0,0));
    tft.setTextColor(tft.color565(255,80,80), tft.color565(50,0,0));
    tft.drawCentreString("< BACK", 38, 67, 2);

    // Footer bg
    tft.fillRect(0, FOOTER_TOP, SCREEN_W, 80, tft.color565(0,20,42));
    tft.drawFastHLine(0, FOOTER_TOP, SCREEN_W, accentCol);

    // Center tick marks
    for (uint8_t t = 0; t < 5; t++)
      tft.drawFastHLine(156, BELL_TOP + t*(BELL_H/4), 8, tft.color565(30,30,50));

    memset(prevBellHeights, 0, sizeof(prevBellHeights));
  }

  // ── Bell ──────────────────────────────────────────────────────────────────
  uint16_t colW = SCREEN_W / BELL_COLS;
  computeBell(bellCenter, peakH);

  for (uint8_t i = 0; i < BELL_COLS; i++) {
    int16_t h    = bellHeights[i];
    int16_t prev = prevBellHeights[i];
    int     bx   = i * colW;
    if (h == prev && !full) continue;
    tft.fillRect(bx, BELL_TOP, colW, BELL_H, tft.color565(6,6,14));
    if (h > 0) {
      int by = BELL_TOP + BELL_H - h;
      for (uint8_t g = GLOW_LAYERS; g > 0; g--) {
        float glowH = gaussianH(bx+colW/2.0f, bellCenter, 55.0f+g*18.0f, peakH);
        uint8_t a   = 40/g;
        int     gy  = BELL_TOP + BELL_H - (int16_t)glowH;
        if ((int16_t)glowH > h)
          tft.fillRect(bx, gy, colW, (int16_t)glowH-h, tft.color565(0,a,a*2));
      }
      uint8_t inten = (uint8_t)(h * 255 / BELL_H);
      tft.fillRect(bx+1, by, colW-2, h, heatColor(inten));
      tft.drawFastHLine(bx+1, by, colW-2, TFT_WHITE);
    }
    prevBellHeights[i] = h;
  }
  tft.drawFastVLine(160, BELL_TOP, BELL_H, tft.color565(40,40,60));

  // ── Footer ────────────────────────────────────────────────────────────────
  tft.fillRect(0, FOOTER_TOP+1, SCREEN_W, 79, tft.color565(0,20,42));

  uint16_t qCol = qual > 66 ? TFT_GREEN : qual > 33 ? TFT_YELLOW : TFT_RED;
  char qStr[8]; snprintf(qStr, sizeof(qStr), "%d%%", qual);
  tft.setTextColor(qCol, tft.color565(0,20,42));
  tft.drawCentreString(qStr, 70, FOOTER_TOP+4, 4);

  tft.setTextColor(tft.color565(160,160,200), tft.color565(0,20,42));
  char rssiStr[10]; snprintf(rssiStr, sizeof(rssiStr), "%d dBm", rssi);
  tft.drawCentreString(rssiStr, 70, FOOTER_TOP+44, 2);

  tft.setTextColor(tft.color565(80,80,120), tft.color565(0,20,42));
  char pkStr[16]; snprintf(pkStr, sizeof(pkStr), "peak %d", trackerPeak);
  tft.drawCentreString(pkStr, 70, FOOTER_TOP+62, 1);

  static uint32_t lastBatUpdate = 0;
  if (!full && millis() - lastBatUpdate > 30000) {
    lastBatUpdate = millis();
    drawBatteryIcon(SCREEN_W-38, 3, getBatteryPct());
  }
  if (full) lastBatUpdate = millis();

  // Mini RSSI graph
  uint8_t histCount = rssiHistFull ? RSSI_HISTORY : rssiHistHead;
  if (histCount > 1) {
    int gx=148, gy=FOOTER_TOP+4, gw=164, gh=68;
    tft.drawRect(gx, gy, gw, gh, tft.color565(30,30,60));
    for (uint8_t i = 1; i < histCount; i++) {
      uint8_t a  = (rssiHistHead+RSSI_HISTORY-histCount+i-1) % RSSI_HISTORY;
      uint8_t b  = (rssiHistHead+RSSI_HISTORY-histCount+i)   % RSSI_HISTORY;
      int8_t ra=rssiHistory[a], rb=rssiHistory[b];
      int x0=gx+1+(i-1)*(gw-2)/(histCount-1);
      int x1=gx+1+i*(gw-2)/(histCount-1);
      int y0=constrain(gy+gh-2-(int)((ra+100)*(gh-4)/70), gy+1, gy+gh-2);
      int y1=constrain(gy+gh-2-(int)((rb+100)*(gh-4)/70), gy+1, gy+gh-2);
      uint16_t lc = rssiToQuality(rb)>66 ? TFT_GREEN : rssiToQuality(rb)>33 ? TFT_YELLOW : TFT_RED;
      tft.drawLine(x0, y0, x1, y1, lc);
    }
  }

  // Centered label
  tft.fillRect(SCREEN_W/2-50, BELL_TOP+4, 100, 16, tft.color565(6,6,14));
  if (fabsf(bellCenter-160.0f) < 20.0f && delta < 4.0f) {
    tft.setTextColor(TFT_GREEN, tft.color565(6,6,14));
    tft.drawCentreString("[ CENTERED ]", SCREEN_W/2, BELL_TOP+4, 2);
  }
}

// ─── Debug overlay ────────────────────────────────────────────────────────────
void drawDebugOverlay() {
  uint16_t cMode = tft.color565(255, 255,   0);  // yellow — mode buttons
  uint16_t cBack = tft.color565(255,  80,  80);  // red    — back button
  uint16_t cList = tft.color565(  0, 200, 255);  // cyan   — list items
  uint16_t cNone = tft.color565(120, 120, 120);  // grey   — info

  // Mode button zones (y 7-37)
  tft.drawRect(10,  28, 105, 27, cMode);
  tft.drawRect(116, 28, 104, 27, cMode);
  tft.drawRect(221, 28,  97, 27, cMode);
  tft.setTextColor(cMode, TFT_BLACK);
  tft.fillRect(12,  30, 28, 10, TFT_BLACK);
  tft.fillRect(118, 30, 22, 10, TFT_BLACK);
  tft.fillRect(223, 30, 44, 10, TFT_BLACK);
  tft.drawString("WiFi", 12, 30, 1);
  tft.drawString("BLE", 118, 30, 1);
  tft.drawString("Classic", 223, 30, 1);

  if (appState == STATE_TRACKER) {
    // Back button zone (y 47-73, x 0-80)
    tft.drawRect(0, 47, 80, 26, cBack);
    tft.setTextColor(cBack, TFT_BLACK);
    tft.fillRect(2, 49, 30, 10, TFT_BLACK);
    tft.drawString("BACK", 2, 49, 1);
  }

  if (appState == STATE_LIST) {
    for (uint8_t n = 0; n < topCount; n++) {
      int itemY = LIST_TOP + 1 + n * LIST_ITEM_H + 10;
      tft.drawRect(0, itemY, SCREEN_W, LIST_ITEM_H - 1, cList);
      tft.setTextColor(cList, TFT_BLACK);
      char lbl[8]; snprintf(lbl, sizeof(lbl), "Item%d", n+1);
      tft.fillRect(2, itemY+2, 34, 10, TFT_BLACK);
      tft.drawString(lbl, 2, itemY+2, 1);
    }
  }

  // Footer info bar
  tft.fillRect(0, SCREEN_H-16, SCREEN_W, 16, TFT_BLACK);
  tft.setTextColor(cNone, TFT_BLACK);
  tft.drawString("LONG PRESS: hide overlay", 4, SCREEN_H-14, 1);
  char st[24]; snprintf(st, sizeof(st), "state=%d mode=%d", (int)appState, (int)radioMode);
  tft.drawString(st, 180, SCREEN_H-14, 1);
}

// ─── Touch ────────────────────────────────────────────────────────────────────
void handleTouch() {
  bool isDown = touch.touched();

  // Long press detection — toggle debug overlay
  if (isDown && !touchWasDown) {
    touchDownMs = millis();
  }
  if (isDown && touchWasDown && millis() - touchDownMs >= LONG_PRESS_MS) {
    uint32_t now = millis();
    if (now - lastTouchMs >= TOUCH_DEBOUNCE) {
      lastTouchMs = now;
      debugOverlay = !debugOverlay;
      if (debugOverlay) drawDebugOverlay();
      else {
        // Redraw current screen to clear overlay
        if      (appState == STATE_SCAN)    drawScanScreen();
        else if (appState == STATE_LIST)    drawListScreen();
        else                                drawTrackerScreen(true);
      }
      touchWasDown = isDown;
      return;
    }
  }
  touchWasDown = isDown;

  if (!isDown) return;
  uint32_t now = millis();
  if (now - lastTouchMs < TOUCH_DEBOUNCE) return;
  lastTouchMs = now;

  TS_Point p = touch.getPoint();
  int sx = constrain(map(p.x, 3527, 460, 0, SCREEN_W), 0, SCREEN_W-1);
  int sy = constrain(map(p.y, 3717, 273, 0, SCREEN_H), 0, SCREEN_H-1);

  // Mode buttons — y=22 to y=48 drawn, touch zone shifted up 15px for calibration
  if (sy >= 28 && sy <= 55) {
    if (sx >= 10  && sx <= 115 && radioMode != RADIO_WIFI)    { switchToMode(RADIO_WIFI);    if (debugOverlay) drawDebugOverlay(); return; }
    if (sx >= 116 && sx <= 220 && radioMode != RADIO_BLE)     { switchToMode(RADIO_BLE);     if (debugOverlay) drawDebugOverlay(); return; }
    if (sx >= 221 && sx <= 318 && radioMode != RADIO_CLASSIC) { switchToMode(RADIO_CLASSIC); if (debugOverlay) drawDebugOverlay(); return; }
  }

  // List: tap a device
  if (appState == STATE_LIST && topCount > 0) {
    for (uint8_t n = 0; n < topCount; n++) {
      int itemY = LIST_TOP + 1 + n * LIST_ITEM_H + 10; // shift down to match rows
      if (sy >= itemY && sy < itemY + LIST_ITEM_H - 1) {
        trackerIdx   = n;
        trackerPeak  = -100;
        bellCenter   = 160.0f;
        bellVelocity = 0.0f;
        rssiHistHead = 0;
        rssiHistFull = false;
        memset(rssiHistory, 0, sizeof(rssiHistory));
        appState = STATE_TRACKER;
        bleScanRunning = false;      // ensure continuous BLE scan starts fresh
        classicScanRunning = false;  // ensure continuous Classic scan starts fresh
        drawTrackerScreen(true);
        if (debugOverlay) drawDebugOverlay();
        return;
      }
    }
  }

  // Tracker: back button (y=62-86 drawn, shifted up 15 for touch)
  if (appState == STATE_TRACKER && sy >= 67 && sy <= 93 && sx < 80) {
    appState = STATE_LIST;
    drawListScreen();
    if (debugOverlay) drawDebugOverlay();
  }
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  randomSeed(analogRead(0));

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  SPI.begin(14, 12, 13, TOUCH_CS_PIN);
  touch.begin(SPI);
  touch.setRotation(0);

  memset(wifiTable,    0, sizeof(wifiTable));
  memset(bleTable,     0, sizeof(bleTable));
  memset(classicTable, 0, sizeof(classicTable));
  memset(bellHeights,     0, sizeof(bellHeights));
  memset(prevBellHeights, 0, sizeof(prevBellHeights));
  memset(rssiHistory, -100, sizeof(rssiHistory));

  nvs_flash_init();

  // Start in WiFi mode
  radioMode = RADIO_WIFI;
  startWifi();

  appState  = STATE_SCAN;
  scanStart = millis();
  drawScanScreen();
}

// ─── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
  handleTouch();

  // WiFi channel hop
  if (radioMode == RADIO_WIFI && millis() - dwellStart >= dwellMs)
    advanceChannel();

  if (appState == STATE_SCAN) {
    uint32_t elapsed = millis() - scanStart;
    updateScanProgress(elapsed);
    if (elapsed >= scanDuration()) {
      // BLE scan auto-stops via its own timer, just collect results
      if (radioMode == RADIO_BLE && pBLEScan) pBLEScan->stop();
      buildTopList();
      appState = STATE_LIST;
      drawListScreen();
    }
  } else if (appState == STATE_TRACKER) {
    // Keep BLE scanning continuously in tracker mode
    if (radioMode == RADIO_BLE && pBLEScan) {
      static uint32_t bleScanTimer = 0;
      if (!bleScanRunning || millis() - bleScanTimer > 1200) {
        if (bleScanRunning) pBLEScan->stop();
        pBLEScan->clearResults();
        pBLEScan->start(1, true);  // 1s non-blocking scan
        bleScanRunning = true;
        bleScanTimer = millis();
      }
    }

    // Keep Classic BT discovering continuously in tracker mode
    if (radioMode == RADIO_CLASSIC) {
      static uint32_t classicScanTimer = 0;
      if (!classicScanRunning || millis() - classicScanTimer > 12000) {
        if (classicScanRunning) esp_bt_gap_cancel_discovery();
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 5, 0);
        classicScanRunning = true;
        classicScanTimer = millis();
      }
    }

    static uint32_t lastUpdate = 0;
    if (millis() - lastUpdate > 150) {
      lastUpdate = millis();
      drawTrackerScreen(false);
    }
  }
}
