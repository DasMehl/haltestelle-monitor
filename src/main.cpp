#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <TFT_eSPI.h>
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
constexpr const char *STOP_LABEL = "Steinstr./Koenigsallee";
constexpr const char *STOP_ID = "20018234";
constexpr const char *VRR_DM_URL = "https://openservice-test.vrr.de/static03/XML_DM_REQUEST";

constexpr uint16_t SCREEN_W = 320;
constexpr uint16_t SCREEN_H = 240;

constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
constexpr uint32_t DATA_REFRESH_INTERVAL_MS = 60000;
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
constexpr uint16_t COLOR_RED_DARK = 0x4000;

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
int lastAppliedElapsedMinutes = -1;

SemaphoreHandle_t dataMutex = nullptr;
volatile bool dataDirty = false;

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

DirectionView snapshotActiveView() {
  DirectionView copy{};
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  copy = modeViews[activeMode].directions[activeDirection];
  xSemaphoreGive(dataMutex);
  return copy;
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
  uint16_t dotColor = COLOR_RED_DARK;
  if (ageMs < DATA_REFRESH_INTERVAL_MS * 2) {
    uint32_t phase = millis() % 2000;
    uint8_t pulse = phase < 1000 ? phase / 20 : (2000 - phase) / 20;
    uint8_t r = 10 + pulse;
    uint8_t g = pulse / 10;
    uint8_t b = pulse / 12;
    dotColor = tft.color565(r, g, b);
  }
  tft.fillCircle(dotX, statusY, 3, dotColor);
  tft.setTextColor(COLOR_MUTED, COLOR_BG);
  tft.setTextDatum(TR_DATUM);
  if (showLive) {
    tft.drawString("Live", statusRight, statusY, 1);
  } else {
    tft.drawString(stamp, stampRight, statusY, 1);
  }
}

void drawHeader() {
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.setTextDatum(TL_DATUM);
  tft.fillRect(0, 0, 258, 28, COLOR_BG);
  tft.drawString(STOP_LABEL, 10, 10, 2);
  tft.drawString(STOP_LABEL, 11, 10, 2);
  tft.drawString(STOP_LABEL, 10, 11, 2);
  tft.drawString(STOP_LABEL, 11, 11, 2);
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

bool fetchLiveData() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(VRR_DM_URL) +
               "?language=de&outputFormat=JSON&coordOutputFormat=WGS84%5BDD.ddddd%5D"
               "&type_dm=stop&name_dm=" +
               STOP_ID + "&mode=direct&useRealtime=1&limit=40";

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
  WiFiClient *stream = http.getStreamPtr();
  DeserializationError error = deserializeJson(doc, *stream);
  int contentLength = http.getSize();
  http.end();
  if (error) {
    Serial.printf("JSON parse failed: %s (contentLength=%d)\n", error.c_str(), contentLength);
    return false;
  }

  xSemaphoreTake(dataMutex, portMAX_DELAY);

  clearLiveData();

  const char *stopInfoText = firstInfoText(doc["dm"]["points"]["point"]["infos"]);
  if (stopInfoText && stopInfoText[0]) {
    for (ModeView &mode : modeViews) {
      for (DirectionView &direction : mode.directions) {
        copyText(direction.liveHint, sizeof(direction.liveHint), stopInfoText);
      }
    }
  }

  JsonArrayConst departures = doc["departureList"].as<JsonArrayConst>();
  for (JsonVariantConst departure : departures) {
    int modeIndex = modeIndexForDeparture(departure);
    if (modeIndex < 0) {
      continue;
    }

    int directionIndex = directionIndexForDeparture(departure);
    appendDeparture(modeViews[modeIndex].directions[directionIndex], departure);
  }

  markDataUpdated();
  xSemaphoreGive(dataMutex);

  dataDirty = true;
  Serial.println("Live departure data updated from VRR");
  return true;
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

void connectWifi(bool forceSetupPortal) {
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  WiFiManager wifiManager;
  wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
  wifiManager.setConfigPortalTimeout(180);

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
  connectWifi(forceWifiSetup);

  dataMutex = xSemaphoreCreateMutex();
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
}
