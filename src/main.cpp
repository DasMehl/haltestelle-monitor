#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {
constexpr uint8_t PIN_LCD_BL = 21;
constexpr uint8_t PIN_TOUCH_CS = 33;
constexpr uint8_t PIN_TOUCH_IRQ = 36;
constexpr uint8_t PIN_TOUCH_MOSI = 32;
constexpr uint8_t PIN_TOUCH_MISO = 39;
constexpr uint8_t PIN_TOUCH_SCLK = 25;

constexpr const char *WIFI_SETUP_AP_NAME = "HaltestelleMonitor-Setup";
constexpr const char *DEFAULT_STOP_LABEL = "Steinstr./Koenigsallee";
constexpr const char *DEFAULT_STOP_ID = "20018234";
// Plain HTTP on purpose: the data is public, no credentials are involved, and
// mbedtls-over-802.11 proved fragile for these ~100KB bodies (frequent
// mid-stream aborts) while costing ~45KB of heap per connection.
constexpr const char *VRR_DM_URL = "http://openservice-test.vrr.de/static03/XML_DM_REQUEST";
constexpr const char *VRR_STOPFINDER_URL =
    "http://openservice-test.vrr.de/static03/XML_STOPFINDER_REQUEST";

constexpr const char *PORTAL_CUSTOM_CSS =
    "<style>"
    "div>a[href='#p']{display:block;box-sizing:border-box;width:100%;padding:14px 16px;"
    "margin:0 0 10px 0;border-radius:10px;background:#f0f0f0;color:#111;font-size:18px;"
    "font-weight:600;}"
    "div>a[href='#p']:active{background:#dcdcdc;}"
    "</style>";

constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 240;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t DATA_REFRESH_INTERVAL_MS = 30000;
constexpr uint32_t FETCH_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t HEADER_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t TICKER_FRAME_INTERVAL_MS = 45;
constexpr uint32_t STALE_WIFI_RECONNECT_MS = 5 * 60 * 1000;
constexpr uint32_t STALE_REBOOT_MS = 10 * 60 * 1000;

constexpr int STORED_ROWS = 6;
constexpr int DISPLAY_ROWS = 4;

// Font "number" that selects the currently set GFX free font in drawString;
// same convention as TFT_eSPI's Free_Fonts.h example header.
constexpr uint8_t GFXFF = 1;

// RGB565 palette; comments give the approximate RGB888 source values.
constexpr uint16_t COLOR_BG = 0x0841;         // deep blue-black (8,10,14)
constexpr uint16_t COLOR_PANEL = 0x10C4;      // (22,26,34)
constexpr uint16_t COLOR_PANEL_SOFT = 0x2126; // (32,38,48)
constexpr uint16_t COLOR_BORDER = 0x31E9;     // (52,60,74)
constexpr uint16_t COLOR_SEPARATOR = 0x2146;  // (36,42,52)
constexpr uint16_t COLOR_TEXT = 0xEF7E;       // (235,238,242)
constexpr uint16_t COLOR_MUTED = 0x8CB4;      // (140,150,165)
constexpr uint16_t COLOR_TRAM = 0xFE60;       // (255,205,0)
constexpr uint16_t COLOR_UBAHN = 0x44DF;      // (68,153,255)
constexpr uint16_t COLOR_LIVE = 0x2E6E;       // (46,204,113)
constexpr uint16_t COLOR_STALE = 0xFD47;      // amber (255,170,56)
constexpr uint16_t COLOR_HINT_DOT = 0xFDA5;   // (255,180,40)

struct Departure {
  char line[8];
  char destination[40];
  int minutesAtFetch;
  bool valid;
};

struct DirectionView {
  char platformLabel[20];
  char liveHint[180];
  Departure rows[STORED_ROWS];
};

struct ModeView {
  const char *label;
  DirectionView directions[2];
};

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite tickerSprite = TFT_eSprite(&tft);
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(PIN_TOUCH_CS, PIN_TOUCH_IRQ);

ModeView modeViews[2] = {
    {"Strassenbahn", {}},
    {"U-Bahn", {}},
};

int activeMode = 0;
int activeDirection = 0;
int tickerOffset = 0;
uint32_t lastUpdateAt = 0;
uint32_t lastHeaderRefreshAt = 0;
uint32_t lastTickerShiftAt = 0;
uint32_t lastWifiRetryAt = 0;
uint32_t lastDataRefreshAt = 0;
uint32_t lastFetchAttemptAt = 0;
uint32_t stopChangeRequestedAt = 0;
int lastAppliedElapsedMinutes = -1;

SemaphoreHandle_t dataMutex = nullptr;
SemaphoreHandle_t networkMutex = nullptr;
volatile bool dataDirty = false;

struct MutexGuard {
  SemaphoreHandle_t sem;
  explicit MutexGuard(SemaphoreHandle_t s) : sem(s) { xSemaphoreTake(sem, portMAX_DELAY); }
  ~MutexGuard() { xSemaphoreGive(sem); }
};

char tramStopId[16];
char tramStopLabel[64];
char ubahnStopId[16];
char ubahnStopLabel[64];

constexpr const char *STOP_SETUP_HOSTNAME = "haltestelle";
WebServer stopServer(80);
bool stopServerRunning = false;

struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

Rect modeTapArea{0, 0, 320, 68};
Rect departuresTapArea{0, 68, 320, 134};

bool pointInRect(int16_t x, int16_t y, const Rect &rect) {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

void normalizeGermanText(char *text, size_t size) {
  String src = text;
  src.replace("ä", "ae");
  src.replace("ö", "oe");
  src.replace("ü", "ue");
  src.replace("Ä", "Ae");
  src.replace("Ö", "Oe");
  src.replace("Ü", "Ue");
  src.replace("ß", "ss");
  src.replace("é", "e");
  src.replace("è", "e");
  src.replace("–", "-");
  src.replace("’", "'");
  strncpy(text, src.c_str(), size - 1);
  text[size - 1] = '\0';
}

void copyText(char *dest, size_t destSize, const char *src) {
  if (!destSize) {
    return;
  }
  strncpy(dest, src ? src : "", destSize - 1);
  dest[destSize - 1] = '\0';
  normalizeGermanText(dest, destSize);
}

void clearDirectionView(DirectionView &view) {
  copyText(view.platformLabel, sizeof(view.platformLabel), "--");
  copyText(view.liveHint, sizeof(view.liveHint), "Keine Live-Daten");
  for (Departure &row : view.rows) {
    copyText(row.line, sizeof(row.line), "--");
    copyText(row.destination, sizeof(row.destination), "");
    row.minutesAtFetch = -1;
    row.valid = false;
  }
}

void clearLiveData() {
  for (ModeView &mode : modeViews) {
    for (DirectionView &direction : mode.directions) {
      clearDirectionView(direction);
    }
  }
}

void markDataUpdated() {
  lastUpdateAt = millis();
  lastHeaderRefreshAt = lastUpdateAt;
  lastDataRefreshAt = lastUpdateAt;
}

void loadStopConfig() {
  Preferences prefs;
  prefs.begin("haltmon", true);
  String savedTramId = prefs.getString("tramStopId", DEFAULT_STOP_ID);
  String savedTramLabel = prefs.getString("tramStopLabel", DEFAULT_STOP_LABEL);
  String savedUbahnId = prefs.getString("ubahnStopId", DEFAULT_STOP_ID);
  String savedUbahnLabel = prefs.getString("ubahnStopLabel", DEFAULT_STOP_LABEL);
  prefs.end();
  copyText(tramStopId, sizeof(tramStopId), savedTramId.c_str());
  copyText(tramStopLabel, sizeof(tramStopLabel), savedTramLabel.c_str());
  copyText(ubahnStopId, sizeof(ubahnStopId), savedUbahnId.c_str());
  copyText(ubahnStopLabel, sizeof(ubahnStopLabel), savedUbahnLabel.c_str());
}

void saveStopConfig() {
  Preferences prefs;
  prefs.begin("haltmon", false);
  prefs.putString("tramStopId", tramStopId);
  prefs.putString("tramStopLabel", tramStopLabel);
  prefs.putString("ubahnStopId", ubahnStopId);
  prefs.putString("ubahnStopLabel", ubahnStopLabel);
  prefs.end();
}

DirectionView snapshotActiveView() {
  DirectionView copy{};
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  copy = modeViews[activeMode].directions[activeDirection];
  xSemaphoreGive(dataMutex);
  return copy;
}

void drawSetupStatus(const char *line1, const char *line2 = nullptr, const char *line3 = nullptr,
                      const char *line4 = nullptr) {
  tft.fillScreen(COLOR_BG);
  tft.setTextFont(1);
  tft.setTextDatum(MC_DATUM);
  int y = SCREEN_H / 2 - 30;
  const char *lines[4] = {line1, line2, line3, line4};
  for (const char *line : lines) {
    if (!line) {
      continue;
    }
    tft.setTextColor(line == line1 ? COLOR_TEXT : COLOR_MUTED, COLOR_BG);
    tft.drawString(line, SCREEN_W / 2, y, line == line1 ? 2 : 1);
    y += line == line1 ? 26 : 18;
  }
}

void drawHeaderStatus() {
  char stamp[20];
  uint32_t ageMs = lastUpdateAt == 0 ? UINT32_MAX : millis() - lastUpdateAt;
  bool fresh = false;
  if (lastUpdateAt == 0) {
    copyText(stamp, sizeof(stamp), "vor -- Min");
  } else {
    uint32_t ageMinutes = ageMs / 60000;
    if (ageMinutes == 0) {
      copyText(stamp, sizeof(stamp), "Live");
      fresh = true;
    } else {
      snprintf(stamp, sizeof(stamp), "vor %lu Min", ageMinutes);
    }
  }

  uint16_t stateColor = COLOR_STALE;
  if (fresh) {
    uint32_t phase = millis() % 1600;
    uint8_t pulse = phase < 800 ? (phase * 255) / 800 : ((1600 - phase) * 255) / 800;
    uint8_t g = 120 + (pulse * 84 / 255);
    stateColor = tft.color565(30, g, 90);
  }

  // Chip is right-anchored and sized to its content: pad + dot + gap + text
  // + pad, so "Live" sits centered and longer stale text grows leftward.
  // setTextFont(1) clears any active free font, otherwise "font 1" would
  // render the free font instead of the small 8px system font.
  tft.setTextFont(1);
  constexpr int chipRight = 308;
  constexpr int chipTop = 6;
  constexpr int chipH = 18;
  int textW = tft.textWidth(stamp, 1);
  int chipW = 7 + 6 + 5 + textW + 7;
  int chipLeft = chipRight - chipW;

  tft.fillRect(chipRight - 104, chipTop - 2, 108, chipH + 4, COLOR_BG);
  tft.drawRoundRect(chipLeft, chipTop, chipW, chipH, 9, stateColor);
  tft.fillCircle(chipLeft + 10, chipTop + chipH / 2, 3, stateColor);
  tft.setTextColor(stateColor, COLOR_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(stamp, chipLeft + 18, chipTop + chipH / 2 + 1, 1);
}

void drawHeader() {
  const char *activeStopLabel = activeMode == 0 ? tramStopLabel : ubahnStopLabel;
  tft.fillRect(0, 0, SCREEN_W, 34, COLOR_BG);

  tft.setFreeFont(&FreeSansBold9pt7b);
  constexpr int labelMaxWidth = 196;
  String label = activeStopLabel;
  while (label.length() > 0 && tft.textWidth(label + "...", GFXFF) > labelMaxWidth) {
    label.remove(label.length() - 1);
  }
  if (label != activeStopLabel) {
    label += "...";
  }

  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(label, 12, 8, GFXFF);
  tft.drawFastHLine(12, 31, 296, COLOR_BORDER);
  drawHeaderStatus();
}

void drawTabs() {
  uint16_t activeColor = activeMode == 0 ? COLOR_TRAM : COLOR_UBAHN;
  tft.fillRoundRect(12, 38, 296, 28, 14, COLOR_PANEL_SOFT);
  if (activeMode == 0) {
    tft.fillRoundRect(14, 40, 146, 24, 12, activeColor);
  } else {
    tft.fillRoundRect(160, 40, 146, 24, 12, activeColor);
  }

  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(activeMode == 0 ? COLOR_BG : COLOR_MUTED,
                   activeMode == 0 ? activeColor : COLOR_PANEL_SOFT);
  tft.drawString("Strassenbahn", 87, 53, GFXFF);
  tft.setTextColor(activeMode == 1 ? COLOR_BG : COLOR_MUTED,
                   activeMode == 1 ? activeColor : COLOR_PANEL_SOFT);
  tft.drawString("U-Bahn", 234, 53, GFXFF);
}

void drawRows() {
  DirectionView view = snapshotActiveView();
  uint32_t elapsedMinutes = lastDataRefreshAt == 0 ? 0 : (millis() - lastDataRefreshAt) / 60000;
  tft.fillRect(0, 68, SCREEN_W, 134, COLOR_BG);

  tft.setTextFont(1);
  tft.setTextColor(COLOR_MUTED, COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("ABFAHRTEN", 12, 74, 1);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(view.platformLabel, 308, 74, 1);

  // Drop trains that are more than a minute past "sofort" so stale data keeps
  // predicting: a departed train disappears and the next stored one shifts up,
  // even when no fresh fetch has arrived.
  Departure displayRows[DISPLAY_ROWS];
  int displayMinutes[DISPLAY_ROWS];
  int shown = 0;
  for (const Departure &stored : view.rows) {
    if (shown >= DISPLAY_ROWS) {
      break;
    }
    if (!stored.valid) {
      continue;
    }
    int liveMinutes = stored.minutesAtFetch - (int)elapsedMinutes;
    if (liveMinutes < 0) {
      continue;
    }
    displayRows[shown] = stored;
    displayMinutes[shown] = liveMinutes;
    ++shown;
  }

  uint16_t chipColor = activeMode == 0 ? COLOR_TRAM : COLOR_UBAHN;
  constexpr int rowTop = 90;
  constexpr int rowStride = 28;
  for (int i = 0; i < DISPLAY_ROWS; ++i) {
    int y = rowTop + i * rowStride;
    if (i > 0) {
      tft.drawFastHLine(12, y - 5, 296, COLOR_SEPARATOR);
    }
    if (i >= shown) {
      continue;
    }

    const Departure &row = displayRows[i];
    int liveMinutes = displayMinutes[i];

    // Bitmap font 2 digits center exactly within the chip; the FreeSans
    // glyphs used previously overflowed its corners.
    tft.fillRoundRect(12, y, 42, 20, 10, chipColor);
    tft.setTextColor(COLOR_BG, chipColor);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(row.line, 33, y + 10, 2);

    int destRight;
    if (liveMinutes <= 0) {
      tft.setFreeFont(&FreeSans9pt7b);
      tft.setTextColor(COLOR_LIVE, COLOR_BG);
      tft.setTextDatum(TR_DATUM);
      tft.drawString("sofort", 308, y + 2, GFXFF);
      destRight = 308 - tft.textWidth("sofort", GFXFF) - 8;
    } else {
      char minutesLabel[8];
      snprintf(minutesLabel, sizeof(minutesLabel), "%d", liveMinutes);
      tft.setTextFont(1);
      int suffixW = tft.textWidth("min", 1);
      tft.setTextColor(COLOR_MUTED, COLOR_BG);
      tft.setTextDatum(TR_DATUM);
      tft.drawString("min", 308, y + 10, 1);
      tft.setFreeFont(&FreeSansBold12pt7b);
      tft.setTextColor(COLOR_TEXT, COLOR_BG);
      tft.drawString(minutesLabel, 308 - suffixW - 3, y + 1, GFXFF);
      destRight = 308 - suffixW - 3 - tft.textWidth(minutesLabel, GFXFF) - 8;
    }

    tft.setFreeFont(&FreeSans9pt7b);
    constexpr int destLeft = 64;
    int destWidth = max(12, destRight - destLeft);
    String destText = row.destination;
    while (destText.length() > 0 && tft.textWidth(destText + "...", GFXFF) > destWidth) {
      destText.remove(destText.length() - 1);
    }
    if (destText != row.destination) {
      destText += "...";
    }
    tft.setTextColor(COLOR_TEXT, COLOR_BG);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(destText, destLeft, y + 2, GFXFF);
  }
}

void drawTicker() {
  DirectionView view = snapshotActiveView();
  int textWidth = tickerSprite.textWidth(view.liveHint, 2);
  int loopWidth = max(1, textWidth + 24);
  int textX = -(tickerOffset % loopWidth);

  tickerSprite.fillSprite(COLOR_PANEL);
  tickerSprite.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tickerSprite.setTextDatum(TL_DATUM);
  tickerSprite.drawString(view.liveHint, textX, 0, 2);
  if (textX + textWidth < 264) {
    tickerSprite.drawString(view.liveHint, textX + loopWidth, 0, 2);
  }
  tickerSprite.pushSprite(34, 210);
}

void drawFooter() {
  tft.fillRoundRect(12, 206, 296, 24, 12, COLOR_PANEL);
  tft.fillCircle(24, 218, 3, COLOR_HINT_DOT);
  drawTicker();
}

void redrawDynamicPanels() {
  drawHeaderStatus();
  drawRows();
  drawFooter();
}

void renderScreen(bool fullRefresh = false) {
  if (fullRefresh) {
    tft.fillScreen(COLOR_BG);
  }
  drawHeader();
  drawTabs();
  drawRows();
  drawFooter();
}

const char *extractInfoText(JsonVariantConst infoNode) {
  if (infoNode.isNull()) {
    return nullptr;
  }
  const char *subtitle = infoNode["infoText"]["subtitle"] | "";
  if (subtitle[0]) {
    return subtitle;
  }
  const char *subject = infoNode["infoText"]["subject"] | "";
  if (subject[0]) {
    return subject;
  }
  const char *wmlText = infoNode["infoText"]["wmlText"] | "";
  if (wmlText[0]) {
    return wmlText;
  }
  return nullptr;
}

const char *firstInfoText(JsonVariantConst node) {
  JsonVariantConst info = node["info"];
  if (info.is<JsonObjectConst>()) {
    return extractInfoText(info);
  }
  if (info.is<JsonArrayConst>()) {
    for (JsonVariantConst entry : info.as<JsonArrayConst>()) {
      const char *text = extractInfoText(entry);
      if (text && text[0]) {
        return text;
      }
    }
  }
  return nullptr;
}

int modeIndexForDeparture(JsonVariantConst departure) {
  const char *number = departure["servingLine"]["number"] | "";
  const char *motType = departure["servingLine"]["motType"] | "";
  if (number[0] == 'U') {
    return 1;
  }
  if (strcmp(motType, "4") == 0) {
    return 0;
  }
  return -1;
}

int directionIndexForDeparture(JsonVariantConst departure) {
  const char *directionCode = departure["servingLine"]["liErgRiProj"]["direction"] | "H";
  return strcmp(directionCode, "R") == 0 ? 1 : 0;
}

int minutesForDeparture(JsonVariantConst departure) {
  const char *countdown = departure["countdown"] | "0";
  return max(0, atoi(countdown));
}

bool appendDeparture(DirectionView &view, JsonVariantConst departure) {
  Departure candidate{};
  copyText(candidate.line, sizeof(candidate.line), departure["servingLine"]["number"] | "--");
  copyText(candidate.destination, sizeof(candidate.destination), departure["servingLine"]["direction"] | "");
  candidate.minutesAtFetch = minutesForDeparture(departure);
  candidate.valid = true;

  int insertAt = -1;
  for (int i = 0; i < STORED_ROWS; ++i) {
    if (!view.rows[i].valid || candidate.minutesAtFetch < view.rows[i].minutesAtFetch) {
      insertAt = i;
      break;
    }
  }

  if (insertAt >= 0) {
    for (int i = STORED_ROWS - 1; i > insertAt; --i) {
      view.rows[i] = view.rows[i - 1];
    }
    view.rows[insertAt] = candidate;
  }

  if (strcmp(view.platformLabel, "--") == 0) {
    char platformLabel[20];
    const char *platform = departure["platformName"] | departure["platform"] | "--";
    snprintf(platformLabel, sizeof(platformLabel), "Steig %s", platform);
    copyText(view.platformLabel, sizeof(view.platformLabel), platformLabel);
  }

  JsonVariantConst lineInfos = departure["lineInfos"];
  if (strcmp(view.liveHint, "Keine Live-Daten") == 0 && !lineInfos.isNull()) {
    if (lineInfos.is<JsonArrayConst>()) {
      for (JsonVariantConst lineInfo : lineInfos.as<JsonArrayConst>()) {
        const char *text = extractInfoText(lineInfo);
        if (text && text[0]) {
          copyText(view.liveHint, sizeof(view.liveHint), text);
          break;
        }
      }
    } else if (lineInfos.is<JsonObjectConst>()) {
      const char *text = extractInfoText(lineInfos);
      if (text && text[0]) {
        copyText(view.liveHint, sizeof(view.liveHint), text);
      }
    }
  }
  return insertAt >= 0;
}

// Reader for ArduinoJson that yields the CPU while waiting for network data.
// Plain WiFiClient::read() returns immediately when the RX buffer is empty,
// so parsing straight from the stream busy-spins during transfer gaps; on
// core 0 that starves the idle task and trips the task watchdog (device
// reboot). vTaskDelay in the wait loop keeps the watchdog fed.
class YieldingReader {
 public:
  YieldingReader(WiFiClient &client, uint32_t gapTimeoutMs)
      : client_(client), gapTimeoutMs_(gapTimeoutMs) {}

  int read() {
    uint32_t waitStart = millis();
    for (;;) {
      int c = client_.read();
      if (c >= 0) {
        return c;
      }
      if (!client_.connected() && client_.available() == 0) {
        return -1;
      }
      if (millis() - waitStart >= gapTimeoutMs_) {
        return -1;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  size_t readBytes(char *buffer, size_t length) {
    size_t count = 0;
    uint32_t waitStart = millis();
    while (count < length) {
      int avail = client_.available();
      if (avail > 0) {
        size_t want = length - count;
        int n = client_.read(reinterpret_cast<uint8_t *>(buffer) + count,
                             want < (size_t)avail ? want : (size_t)avail);
        if (n > 0) {
          count += n;
          waitStart = millis();
          continue;
        }
      }
      if (!client_.connected() && client_.available() == 0) {
        break;
      }
      if (millis() - waitStart >= gapTimeoutMs_) {
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
    }
    return count;
  }

 private:
  WiFiClient &client_;
  uint32_t gapTimeoutMs_;
};

bool fetchStopDepartures(const char *stopIdForRequest, int modeIndex) {
  if (!stopIdForRequest[0]) {
    return false;
  }

  MutexGuard networkLock(networkMutex);

  WiFiClient client;

  HTTPClient http;
  // includedMeans restricts the response to U-Bahn (MOT 2) and tram (MOT 4)
  // departures, so buses and trains don't eat up the limited result slots.
  String url = String(VRR_DM_URL) +
               "?language=de&outputFormat=JSON&coordOutputFormat=WGS84%5BDD.ddddd%5D"
               "&type_dm=stop&name_dm=" +
               stopIdForRequest +
               "&mode=direct&useRealtime=1&limit=20"
               "&includedMeans=checkbox&inclMOT_2=1&inclMOT_4=1";

  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed: %d\n", httpCode);
    http.end();
    return false;
  }

  // The full VRR response is ~150KB, which overflowed the old fixed parse
  // buffer and made parsing fragile. Filter down to only the fields we read
  // so the parsed document stays a few KB regardless of response size.
  JsonDocument filter;
  filter["dm"]["points"]["point"]["infos"] = true;
  JsonObject departureFilter = filter["departureList"].add<JsonObject>();
  departureFilter["countdown"] = true;
  departureFilter["platform"] = true;
  departureFilter["platformName"] = true;
  departureFilter["lineInfos"] = true;
  departureFilter["servingLine"]["number"] = true;
  departureFilter["servingLine"]["motType"] = true;
  departureFilter["servingLine"]["direction"] = true;
  departureFilter["servingLine"]["liErgRiProj"]["direction"] = true;

  JsonDocument doc;
  YieldingReader reader(*http.getStreamPtr(), 10000);
  DeserializationError error =
      deserializeJson(doc, reader, DeserializationOption::Filter(filter));
  int contentLength = http.getSize();
  http.end();
  if (error) {
    Serial.printf("JSON parse failed: %s (contentLength=%d)\n", error.c_str(), contentLength);
    return false;
  }

  xSemaphoreTake(dataMutex, portMAX_DELAY);

  clearDirectionView(modeViews[modeIndex].directions[0]);
  clearDirectionView(modeViews[modeIndex].directions[1]);

  const char *stopInfoText = firstInfoText(doc["dm"]["points"]["point"]["infos"]);
  if (stopInfoText && stopInfoText[0]) {
    copyText(modeViews[modeIndex].directions[0].liveHint,
             sizeof(modeViews[modeIndex].directions[0].liveHint), stopInfoText);
    copyText(modeViews[modeIndex].directions[1].liveHint,
             sizeof(modeViews[modeIndex].directions[1].liveHint), stopInfoText);
  }

  JsonArrayConst departures = doc["departureList"].as<JsonArrayConst>();
  for (JsonVariantConst departure : departures) {
    if (modeIndexForDeparture(departure) != modeIndex) {
      continue;
    }

    int directionIndex = directionIndexForDeparture(departure);
    appendDeparture(modeViews[modeIndex].directions[directionIndex], departure);
  }

  xSemaphoreGive(dataMutex);
  return true;
}

bool fetchLiveData() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  char tramIdSnapshot[16];
  char ubahnIdSnapshot[16];
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  strncpy(tramIdSnapshot, tramStopId, sizeof(tramIdSnapshot));
  strncpy(ubahnIdSnapshot, ubahnStopId, sizeof(ubahnIdSnapshot));
  xSemaphoreGive(dataMutex);

  bool tramOk = fetchStopDepartures(tramIdSnapshot, 0);
  bool ubahnOk = fetchStopDepartures(ubahnIdSnapshot, 1);

  if (!tramOk && !ubahnOk) {
    return false;
  }

  xSemaphoreTake(dataMutex, portMAX_DELAY);
  markDataUpdated();
  xSemaphoreGive(dataMutex);

  dataDirty = true;
  Serial.printf("Live departure data updated (tram=%s, ubahn=%s, heap=%u)\n",
                tramOk ? "ok" : "fail", ubahnOk ? "ok" : "fail", ESP.getFreeHeap());
  return true;
}

String urlEncode(const char *value) {
  String encoded;
  char buf[4];
  for (const char *p = value; *p; ++p) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else if (c == ' ') {
      encoded += '+';
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

struct StopCandidate {
  char id[16];
  char name[64];
};

int searchStops(const char *query, StopCandidate *results, int maxResults) {
  if (WiFi.status() != WL_CONNECTED || !query || !query[0]) {
    return 0;
  }

  MutexGuard networkLock(networkMutex);

  WiFiClient client;

  HTTPClient http;
  String url = String(VRR_STOPFINDER_URL) +
               "?language=de&outputFormat=JSON&type_sf=stop&anyObjFilter_sf=2&name_sf=" +
               urlEncode(query);

  http.useHTTP10(true);
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, url)) {
    Serial.println("Stop search: HTTP begin failed");
    return 0;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Stop search: HTTP GET failed: %d\n", httpCode);
    http.end();
    return 0;
  }

  JsonDocument doc;
  YieldingReader reader(*http.getStreamPtr(), 10000);
  DeserializationError error = deserializeJson(doc, reader);
  http.end();
  if (error) {
    Serial.printf("Stop search: JSON parse failed: %s\n", error.c_str());
    return 0;
  }

  JsonVariantConst pointsNode = doc["stopFinder"]["points"];
  int count = 0;

  auto addPoint = [&](JsonVariantConst point) {
    if (count >= maxResults) {
      return;
    }
    const char *id = point["stateless"] | "";
    const char *name = point["name"] | "";
    if (!id[0]) {
      return;
    }
    copyText(results[count].id, sizeof(results[count].id), id);
    copyText(results[count].name, sizeof(results[count].name), name);
    ++count;
  };

  if (pointsNode.is<JsonArrayConst>()) {
    for (JsonVariantConst p : pointsNode.as<JsonArrayConst>()) {
      if (count >= maxResults) {
        break;
      }
      addPoint(p);
    }
  } else if (pointsNode.is<JsonObjectConst>()) {
    JsonVariantConst inner = pointsNode["point"];
    if (inner.is<JsonArrayConst>()) {
      for (JsonVariantConst p : inner.as<JsonArrayConst>()) {
        if (count >= maxResults) {
          break;
        }
        addPoint(p);
      }
    } else if (inner.is<JsonObjectConst>()) {
      addPoint(inner);
    }
  }

  return count;
}

void handleStopRoot() {
  String page =
      "<!doctype html><html><head><meta name='viewport' content='width=device-width,"
      "initial-scale=1'><meta charset='utf-8'><title>Haltestelle</title><style>"
      "body{font-family:sans-serif;max-width:480px;margin:24px auto;padding:0 16px;color:#222;}"
      "input{width:100%;padding:12px;font-size:18px;box-sizing:border-box;margin:8px 0;}"
      "button{width:100%;padding:14px;font-size:18px;background:#1fa3ec;color:#fff;border:0;"
      "border-radius:8px;}"
      ".status{color:#2a2;font-weight:bold;}"
      ".section{margin-bottom:32px;padding-bottom:16px;border-bottom:1px solid #ddd;}"
      ".result{display:block;width:100%;box-sizing:border-box;padding:12px 14px;margin:6px 0;"
      "border-radius:8px;background:#f0f0f0;color:#111;text-align:left;border:0;font-size:16px;}"
      ".result:active{background:#dcdcdc;}"
      ".saved{color:#2a2;font-weight:bold;}"
      "</style></head><body>"
      "<p class='status'>WLAN: verbunden</p>"

      "<div class='section'>"
      "<h2>Strassenbahn</h2>"
      "<p>Aktuell: <b id='tram-current'>" +
      String(tramStopLabel) +
      "</b></p>"
      "<input id='tram-q' placeholder='Haltestelle suchen...'>"
      "<button onclick=\"doSearch('tram')\">Suchen</button>"
      "<div id='tram-results'></div>"
      "</div>"

      "<div class='section'>"
      "<h2>U-Bahn</h2>"
      "<p>Aktuell: <b id='ubahn-current'>" +
      String(ubahnStopLabel) +
      "</b></p>"
      "<input id='ubahn-q' placeholder='Haltestelle suchen...'>"
      "<button onclick=\"doSearch('ubahn')\">Suchen</button>"
      "<div id='ubahn-results'></div>"
      "</div>"

      "<script>"
      "function doSearch(mode){"
      "var q=document.getElementById(mode+'-q').value;"
      "fetch('/search?q='+encodeURIComponent(q)).then(function(r){return r.json();})"
      ".then(function(list){"
      "var el=document.getElementById(mode+'-results');"
      "el.innerHTML='';"
      "if(list.length===0){el.innerHTML='<p>Keine Treffer</p>';return;}"
      "list.forEach(function(item){"
      "var btn=document.createElement('button');"
      "btn.className='result';"
      "btn.textContent=item.name;"
      "btn.onclick=function(){selectStop(mode,item.id,item.name);};"
      "el.appendChild(btn);"
      "});"
      "});"
      "}"
      "function pollStatus(el,tries){"
      "fetch('/status').then(function(r){return r.json();}).then(function(s){"
      "if(s.updated){"
      "el.innerHTML=\"<p class='saved'>Gespeichert und aktualisiert!</p>\";"
      "}else if(tries<10){"
      "setTimeout(function(){pollStatus(el,tries+1);},700);"
      "}else{"
      "el.innerHTML=\"<p class='saved'>Gespeichert (Update dauert laenger als erwartet)</p>\";"
      "}"
      "});"
      "}"
      "function selectStop(mode,id,name){"
      "var body='mode='+encodeURIComponent(mode)+'&id='+encodeURIComponent(id)"
      "+'&label='+encodeURIComponent(name);"
      "var el=document.getElementById(mode+'-results');"
      "el.innerHTML='<p>Wird gespeichert...</p>';"
      "fetch('/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
      "body:body}).then(function(){"
      "document.getElementById(mode+'-current').textContent=name;"
      "el.innerHTML='<p>Wird aktualisiert...</p>';"
      "setTimeout(function(){pollStatus(el,0);},500);"
      "});"
      "}"
      "</script>"
      "</body></html>";
  stopServer.send(200, "text/html; charset=utf-8", page);
}

void handleStopSearch() {
  String query = stopServer.arg("q");
  query.trim();

  StopCandidate results[8];
  int count = query.length() > 0 ? searchStops(query.c_str(), results, 8) : 0;

  String json = "[";
  for (int i = 0; i < count; ++i) {
    if (i > 0) {
      json += ",";
    }
    String name = results[i].name;
    name.replace("\\", "\\\\");
    name.replace("\"", "\\\"");
    json += "{\"id\":\"" + String(results[i].id) + "\",\"name\":\"" + name + "\"}";
  }
  json += "]";
  stopServer.send(200, "application/json; charset=utf-8", json);
}

void handleStopSet() {
  String mode = stopServer.arg("mode");
  String id = stopServer.arg("id");
  String label = stopServer.arg("label");
  id.trim();
  label.trim();

  // VRR names stops as "City, Stop". The web page shows the full name so the
  // right city can be picked, but the 320px header can't fit both, and the
  // city is already known at selection time — keep only the stop part.
  int citySep = label.indexOf(", ");
  if (citySep >= 0 && citySep + 2 < (int)label.length()) {
    label = label.substring(citySep + 2);
  }

  bool ok = id.length() > 0 && (mode == "tram" || mode == "ubahn");
  if (ok) {
    int modeIndex = mode == "tram" ? 0 : 1;

    xSemaphoreTake(dataMutex, portMAX_DELAY);
    if (modeIndex == 0) {
      copyText(tramStopId, sizeof(tramStopId), id.c_str());
      copyText(tramStopLabel, sizeof(tramStopLabel), label.c_str());
    } else {
      copyText(ubahnStopId, sizeof(ubahnStopId), id.c_str());
      copyText(ubahnStopLabel, sizeof(ubahnStopLabel), label.c_str());
    }
    xSemaphoreGive(dataMutex);
    saveStopConfig();
    Serial.printf("%s stop set: %s (ID %s)\n", mode.c_str(), label.c_str(), id.c_str());
    drawHeader();

    // Signal the background fetch task to refresh immediately instead of
    // waiting up to DATA_REFRESH_INTERVAL_MS. Deliberately non-blocking here:
    // the actual network fetch happens on fetchTaskLoop's core, not this
    // request handler, so the UI never freezes while it runs.
    stopChangeRequestedAt = millis();
    lastFetchAttemptAt = 0;

    stopServer.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  } else {
    stopServer.send(400, "application/json; charset=utf-8", "{\"ok\":false}");
  }
}

void handleStopStatus() {
  bool updated = lastDataRefreshAt != 0 && lastDataRefreshAt >= stopChangeRequestedAt;
  String json = String("{\"updated\":") + (updated ? "true" : "false") + "}";
  stopServer.send(200, "application/json; charset=utf-8", json);
}

void startStopServer() {
  if (MDNS.begin(STOP_SETUP_HOSTNAME)) {
    Serial.printf("mDNS started: http://%s.local\n", STOP_SETUP_HOSTNAME);
  } else {
    Serial.println("mDNS start failed");
  }
  stopServer.on("/", HTTP_GET, handleStopRoot);
  stopServer.on("/search", HTTP_GET, handleStopSearch);
  stopServer.on("/set", HTTP_POST, handleStopSet);
  stopServer.on("/status", HTTP_GET, handleStopStatus);
  stopServer.begin();
  stopServerRunning = true;
  Serial.println("Stop-selector web server started");
}

void fetchTaskLoop(void *) {
  bool lastFetchOk = true;
  for (;;) {
    // Gate on the last *attempt*, not the last success, so a failing fetch
    // can't retry every second and hammer VRR's server. After a failure the
    // next try comes sooner than the normal interval so stale data recovers
    // quickly, but still with enough spacing to stay polite.
    uint32_t waitMs = lastFetchOk ? DATA_REFRESH_INTERVAL_MS : FETCH_RETRY_INTERVAL_MS;
    bool dueForFetch = lastFetchAttemptAt == 0 || millis() - lastFetchAttemptAt >= waitMs;
    if (WiFi.status() == WL_CONNECTED && dueForFetch) {
      lastFetchAttemptAt = millis();
      lastFetchOk = fetchLiveData();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

bool connectWifi(bool forceSetupPortal) {
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  WiFiManager wifiManager;
  wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
  wifiManager.setConfigPortalTimeout(600);
  wifiManager.setCustomHeadElement(PORTAL_CUSTOM_CSS);

  bool connected;
  if (forceSetupPortal) {
    Serial.println("Opening WiFi setup portal (forced)...");
    wifiManager.resetSettings();
    connected = wifiManager.startConfigPortal(WIFI_SETUP_AP_NAME);
  } else {
    Serial.println("Connecting to saved WiFi, or opening setup portal if none saved...");
    connected = wifiManager.autoConnect(WIFI_SETUP_AP_NAME);
  }

  if (connected) {
    Serial.printf("WiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi connect/setup portal timed out");
  }
  return connected;
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }
  if (millis() - lastWifiRetryAt < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiRetryAt = millis();
  Serial.println("Retrying WiFi with saved credentials...");
  WiFi.disconnect();
  WiFi.begin();
}

// Self-healing for the "connected but broken" state: WiFi reports connected
// yet no fetch has succeeded for a long time (stale sockets, exhausted heap,
// half-dead AP association). A reboot is known to fix it, so do that
// automatically instead of showing hours-old departures.
void maintainFreshness() {
  uint32_t sinceSuccess = millis() - lastUpdateAt;  // lastUpdateAt==0 -> since boot
  if (sinceSuccess < STALE_WIFI_RECONNECT_MS) {
    return;
  }

  if (sinceSuccess >= STALE_REBOOT_MS) {
    Serial.printf("No successful update for %lu min, restarting device\n",
                  STALE_REBOOT_MS / 60000);
    ESP.restart();
  }

  static uint32_t lastStaleReconnectAt = 0;
  if (millis() - lastStaleReconnectAt < STALE_WIFI_RECONNECT_MS) {
    return;
  }
  lastStaleReconnectAt = millis();
  Serial.printf("No successful update for %lu min, forcing WiFi reconnect (heap=%u)\n",
                sinceSuccess / 60000, ESP.getFreeHeap());
  WiFi.disconnect();
  WiFi.begin();
}

void checkForNewData() {
  if (!dataDirty) {
    return;
  }
  dataDirty = false;
  lastAppliedElapsedMinutes = 0;
  redrawDynamicPanels();
}

void updateCountdownIfNeeded() {
  if (lastDataRefreshAt == 0) {
    return;
  }
  int elapsedMinutes = (int)((millis() - lastDataRefreshAt) / 60000);
  if (elapsedMinutes == lastAppliedElapsedMinutes) {
    return;
  }
  lastAppliedElapsedMinutes = elapsedMinutes;
  drawRows();
}

bool readTouchPoint(int16_t &x, int16_t &y) {
  if (!touch.touched()) {
    return false;
  }

  TS_Point p = touch.getPoint();
  if (p.z < 150) {
    return false;
  }

  // Landscape touch mapping flipped 180 degrees from the previous orientation.
  x = map(p.x, 300, 3850, SCREEN_W - 1, 0);
  y = map(p.y, 370, 3800, SCREEN_H - 1, 0);
  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);
  Serial.printf("touch raw=(%d,%d,%d) mapped=(%d,%d)\n", p.x, p.y, p.z, x, y);
  return true;
}

void handleTouch() {
  static uint32_t lastTouchAt = 0;
  if (millis() - lastTouchAt < 250) {
    return;
  }

  int16_t x = 0;
  int16_t y = 0;
  if (!readTouchPoint(x, y)) {
    return;
  }

  if (pointInRect(x, y, modeTapArea)) {
    activeMode = activeMode == 0 ? 1 : 0;
    activeDirection = 0;
    tickerOffset = 0;
    renderScreen(false);
    lastTouchAt = millis();
    return;
  }

  if (pointInRect(x, y, departuresTapArea)) {
    activeDirection = activeDirection == 0 ? 1 : 0;
    tickerOffset = 0;
    renderScreen(false);
    lastTouchAt = millis();
  }
}

void updateTicker() {
  if (millis() - lastTickerShiftAt < TICKER_FRAME_INTERVAL_MS) {
    return;
  }

  lastTickerShiftAt = millis();
  tickerOffset += 1;
  drawTicker();
}

void updateHeaderStatusIfNeeded() {
  if (millis() - lastHeaderRefreshAt < HEADER_REFRESH_INTERVAL_MS) {
    return;
  }

  lastHeaderRefreshAt = millis();
  drawHeaderStatus();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  clearLiveData();
  loadStopConfig();

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  tft.init();
  tft.setRotation(3);
  tft.invertDisplay(true);
  tft.fillScreen(COLOR_BG);

  tickerSprite.setColorDepth(16);
  tickerSprite.createSprite(264, 16);

  touchSPI.begin(PIN_TOUCH_SCLK, PIN_TOUCH_MISO, PIN_TOUCH_MOSI, PIN_TOUCH_CS);
  touch.begin(touchSPI);

  bool forceWifiSetup = touch.touched();
  if (forceWifiSetup) {
    Serial.println("Touch held at boot: forcing WiFi setup portal");
  }

  drawSetupStatus("Schritt 1: WLAN", "Nicht verbunden",
                   "Falls noetig, verbinde dich mit WLAN:", WIFI_SETUP_AP_NAME);
  bool wifiConnected = connectWifi(forceWifiSetup);
  dataMutex = xSemaphoreCreateMutex();
  networkMutex = xSemaphoreCreateMutex();

  if (wifiConnected) {
    startStopServer();
    drawSetupStatus("Schritt 2: Haltestelle", "WLAN verbunden",
                     "Haltestelle waehlen unter:", "http://haltestelle.local");
    Serial.printf("Stop selector: http://%s.local or http://%s\n", STOP_SETUP_HOSTNAME,
                  WiFi.localIP().toString().c_str());
    delay(4000);
  } else {
    drawSetupStatus("WLAN fehlgeschlagen", "Bitte neu starten und", "erneut versuchen");
    delay(3000);
  }

  fetchLiveData();
  renderScreen(true);
  xTaskCreatePinnedToCore(fetchTaskLoop, "fetchTask", 8192, nullptr, 1, nullptr, 0);

  Serial.println("ESP32-2432S028 live departure monitor booted");
}

void loop() {
  handleTouch();
  updateTicker();
  updateHeaderStatusIfNeeded();
  maintainWifi();
  maintainFreshness();
  checkForNewData();
  updateCountdownIfNeeded();
  if (stopServerRunning) {
    stopServer.handleClient();
  }
}
