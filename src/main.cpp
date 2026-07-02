#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
constexpr const char *VRR_DM_URL = "https://openservice-test.vrr.de/static03/XML_DM_REQUEST";
constexpr const char *VRR_STOPFINDER_URL =
    "https://openservice-test.vrr.de/static03/XML_STOPFINDER_REQUEST";

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
constexpr uint32_t HEADER_REFRESH_INTERVAL_MS = 1000;
constexpr uint32_t TICKER_FRAME_INTERVAL_MS = 45;

constexpr uint16_t COLOR_BG = TFT_BLACK;
constexpr uint16_t COLOR_PANEL = 0x10A2;
constexpr uint16_t COLOR_PANEL_SOFT = 0x18E4;
constexpr uint16_t COLOR_BORDER = 0x31A6;
constexpr uint16_t COLOR_TEXT = 0xEF7D;
constexpr uint16_t COLOR_MUTED = 0x7BEF;
constexpr uint16_t COLOR_TRAM = 0xFEE0;
constexpr uint16_t COLOR_UBAHN = 0x44BF;
constexpr uint16_t COLOR_WARN = 0xFDF0;

struct Departure {
  char line[8];
  char destination[40];
  int minutesAtFetch;
  bool valid;
};

struct DirectionView {
  char platformLabel[20];
  char liveHint[180];
  Departure rows[4];
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

Rect modeTapArea{0, 0, 320, 72};
Rect tableArea{8, 78, 304, 154};
Rect departuresTapArea{8, 78, 304, 154};

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

void drawRoundedPanel(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t fill) {
  tft.fillRoundRect(x, y, w, h, 10, fill);
  tft.drawRoundRect(x, y, w, h, 10, COLOR_BORDER);
}

void drawHeaderStatus() {
  char stamp[20];
  uint32_t ageMs = lastUpdateAt == 0 ? UINT32_MAX : millis() - lastUpdateAt;
  bool showLive = false;
  if (lastUpdateAt == 0) {
    copyText(stamp, sizeof(stamp), "vor -- Min");
  } else {
    uint32_t ageMinutes = ageMs / 60000;
    if (ageMinutes == 0) {
      copyText(stamp, sizeof(stamp), "Live");
      showLive = true;
    } else {
      snprintf(stamp, sizeof(stamp), "vor %lu Min", ageMinutes);
    }
  }

  constexpr int statusRight = 314;
  constexpr int statusY = 13;
  constexpr int dotX = 244;
  constexpr int stampRight = 312;
  tft.fillRect(232, 4, 82, 18, COLOR_BG);
  uint16_t dotColor = COLOR_MUTED;
  if (ageMs < DATA_REFRESH_INTERVAL_MS * 2) {
    uint32_t phase = millis() % 1600;
    uint8_t pulse = phase < 800 ? (phase * 255) / 800 : ((1600 - phase) * 255) / 800;
    uint8_t g = 90 + (pulse * 165 / 255);
    dotColor = tft.color565(20, g, 60);
  }
  tft.fillCircle(dotX, statusY, 4, dotColor);
  tft.setTextColor(COLOR_MUTED, COLOR_BG);
  tft.setTextDatum(TR_DATUM);
  if (showLive) {
    tft.drawString("Live", statusRight, statusY, 1);
  } else {
    tft.drawString(stamp, stampRight, statusY, 1);
  }
}

void drawHeader() {
  const char *activeStopLabel = activeMode == 0 ? tramStopLabel : ubahnStopLabel;
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  tft.fillRect(0, 0, SCREEN_W, 28, COLOR_BG);

  constexpr int labelMaxWidth = 214;
  String label = activeStopLabel;
  while (label.length() > 0 && tft.textWidth(label + "...", 2) > labelMaxWidth) {
    label.remove(label.length() - 1);
  }
  if (label != activeStopLabel) {
    label += "...";
  }

  tft.drawString(label, 10, 10, 2);
  tft.drawString(label, 11, 10, 2);
  tft.drawString(label, 10, 11, 2);
  tft.drawString(label, 11, 11, 2);
  drawHeaderStatus();
}

void drawTabs() {
  auto drawTab = [](int x, const char *label, bool active, uint16_t color) {
    uint16_t fill = active ? color : COLOR_PANEL_SOFT;
    uint16_t text = active ? COLOR_BG : COLOR_TEXT;
    tft.fillRoundRect(x, 34, 148, 34, 12, fill);
    tft.drawRoundRect(x, 34, 148, 34, 12, COLOR_BORDER);
    tft.setTextColor(text, fill);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(label, x + 74, 52, 2);
  };

  drawTab(8, "Strassenbahn", activeMode == 0, COLOR_TRAM);
  drawTab(164, "U-Bahn", activeMode == 1, COLOR_UBAHN);
}

void drawRows() {
  DirectionView view = snapshotActiveView();
  uint32_t elapsedMinutes = lastDataRefreshAt == 0 ? 0 : (millis() - lastDataRefreshAt) / 60000;
  drawRoundedPanel(tableArea.x, tableArea.y, tableArea.w, tableArea.h, COLOR_PANEL);

  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Li", 18, 84, 1);
  tft.drawString("Ziel", 74, 84, 1);
  tft.drawString("Min", 274, 84, 1);

  constexpr int rowTop = 94;
  constexpr int rowHeight = 18;
  constexpr int rowGap = 5;
  for (int i = 0; i < 4; ++i) {
    int y = rowTop + i * (rowHeight + rowGap);
    if (i > 0) {
      tft.drawFastHLine(16, y - 3, 288, COLOR_BORDER);
    }

    const Departure &row = view.rows[i];
    int liveMinutes = row.valid ? max(0, row.minutesAtFetch - (int)elapsedMinutes) : -1;
    uint16_t chipColor = activeMode == 0 ? COLOR_TRAM : COLOR_UBAHN;
    tft.fillRoundRect(14, y, 44, 18, 6, chipColor);
    tft.setTextColor(COLOR_BG, chipColor);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(row.line, 36, y + 9, 2);

    tft.setTextColor(COLOR_TEXT, COLOR_PANEL);
    tft.setTextDatum(TL_DATUM);
    char minutesLabel[8] = "";
    int minutesFont = 4;
    if (row.valid) {
      if (liveMinutes <= 0) {
        copyText(minutesLabel, sizeof(minutesLabel), "sofort");
        minutesFont = 2;
      } else {
        snprintf(minutesLabel, sizeof(minutesLabel), "%d", liveMinutes);
      }
    }

    int minutesWidth = row.valid ? tft.textWidth(minutesLabel, minutesFont) : 0;
    int destLeft = 68;
    int destRight = row.valid ? (296 - minutesWidth - 10) : 296;
    int destWidth = max(12, destRight - destLeft);
    String destText = row.destination;
    while (destText.length() > 0 && tft.textWidth(destText + "...", 2) > destWidth) {
      destText.remove(destText.length() - 1);
    }
    if (destText != row.destination) {
      destText += "...";
    }
    tft.drawString(destText, destLeft, y + 3, 2);

    if (row.valid) {
      tft.setTextDatum(TR_DATUM);
      tft.drawString(minutesLabel, 296, y - 1, minutesFont);
    }
  }

  tft.drawFastHLine(16, 188, 288, COLOR_BORDER);
  tft.setTextColor(COLOR_WARN, COLOR_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("Live-Hinweis", 16, 198, 1);
  tft.setTextColor(COLOR_MUTED, COLOR_PANEL);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(view.platformLabel, 296, 198, 1);
}

void drawFooter() {
  DirectionView view = snapshotActiveView();
  int textWidth = tickerSprite.textWidth(view.liveHint, 2);
  int loopWidth = max(1, textWidth + 24);
  int textX = -(tickerOffset % loopWidth);

  tickerSprite.fillSprite(COLOR_PANEL);
  tickerSprite.setTextColor(COLOR_TEXT, COLOR_PANEL);
  tickerSprite.setTextDatum(TL_DATUM);
  tickerSprite.drawString(view.liveHint, textX, 0, 2);
  if (textX + textWidth < 288) {
    tickerSprite.drawString(view.liveHint, textX + loopWidth, 0, 2);
  }
  tickerSprite.pushSprite(16, 212);
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
  for (int i = 0; i < 4; ++i) {
    if (!view.rows[i].valid || candidate.minutesAtFetch < view.rows[i].minutesAtFetch) {
      insertAt = i;
      break;
    }
  }

  if (insertAt >= 0) {
    for (int i = 3; i > insertAt; --i) {
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

bool fetchStopDepartures(const char *stopIdForRequest, int modeIndex) {
  if (!stopIdForRequest[0]) {
    return false;
  }

  MutexGuard networkLock(networkMutex);

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(VRR_DM_URL) +
               "?language=de&outputFormat=JSON&coordOutputFormat=WGS84%5BDD.ddddd%5D"
               "&type_dm=stop&name_dm=" +
               stopIdForRequest + "&mode=direct&useRealtime=1&limit=40";

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

  DynamicJsonDocument doc(98304);
  DeserializationError error = deserializeJson(doc, *http.getStreamPtr());
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
  Serial.printf("Live departure data updated (tram=%s, ubahn=%s)\n", tramOk ? "ok" : "fail",
                ubahnOk ? "ok" : "fail");
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

  WiFiClientSecure client;
  client.setInsecure();

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

  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, *http.getStreamPtr());
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
    lastDataRefreshAt = 0;

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
  for (;;) {
    bool dueForFetch =
        lastDataRefreshAt == 0 || millis() - lastDataRefreshAt >= DATA_REFRESH_INTERVAL_MS;
    if (WiFi.status() == WL_CONNECTED && dueForFetch) {
      fetchLiveData();
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
  drawFooter();
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
  tickerSprite.createSprite(288, 16);

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
  checkForNewData();
  updateCountdownIfNeeded();
  if (stopServerRunning) {
    stopServer.handleClient();
  }
}
