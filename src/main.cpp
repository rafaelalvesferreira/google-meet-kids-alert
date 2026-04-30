// Meet Alert — XIAO ESP32-C6 + WS2812 8x8
//
// Estados exibidos:
//   verde           = livre
//   amarelo         = em reunião solo (sem convidados)
//   vermelho        = em reunião com convidados
//   amarelo piscado = reunião vermelha começando em <= 5min
//   roxo            = estado desconhecido / erro persistente
//
// Um LED de canto indica a carga da bateria (verde > laranja > vermelho
// piscando).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <FastLED.h>
#include <time.h>

// =====================================================================
// HARDWARE
// =====================================================================
#define MATRIX_DATA_PIN        D6    // DIN da matriz WS2812
#define BATTERY_ADC_PIN        D0    // meio do divisor 100k + 100k
#define BOOT_BUTTON_PIN        9     // botão BOOT do XIAO (GPIO9)
#define EXT_BUTTON_PIN         D7    // botão externo (GPIO17) — mesma função do BOOT

#define NUM_LEDS               64
#define BATTERY_INDICATOR_LED  0     // índice do LED que mostra bateria
#define HEARTBEAT_LED          1     // LED de status de boot/WiFi (ao lado da bateria)

// =====================================================================
// COMPORTAMENTO
// =====================================================================
#define AP_SSID                "MeetAlert-Config"
#define WIFI_CONNECT_TIMEOUT   30    // segundos
#define CONFIG_PORTAL_TIMEOUT  300   // segundos (5 min)

#define DEFAULT_BRIGHTNESS     30
#define DEFAULT_POLL_INTERVAL  30    // segundos entre polls do Apps Script
#define DEFAULT_SLEEP_INTERVAL 300   // segundos de deep sleep fora do horário comercial

// Working hours
#define DEFAULT_TZ_OFFSET      -3    // UTC-3 (Brasília)
#define DEFAULT_WH_START       9     // hora de início do expediente
#define DEFAULT_WH_END         18    // hora de fim do expediente
#define DEFAULT_WH_DAYS        0x3E  // 0b0111110 = seg–sex

#define BTN_HOLD_CONFIG_MS     3000  // 3s = portal mantendo WiFi atual
#define BTN_HOLD_RESET_MS      9000  // 9s = apagar WiFi e reconfigurar

#define BLINK_PERIOD_MS        500   // meio segundo ligado, meio desligado

// Faixa de tensão da LiPo
#define BATTERY_EMPTY_V        3.00
#define BATTERY_FULL_V         4.20


// =====================================================================
// ESTADO GLOBAL
// =====================================================================
enum State {
  STATE_UNKNOWN,
  STATE_GREEN,
  STATE_YELLOW,
  STATE_RED,
  STATE_YELLOW_BLINK,
  STATE_SLEEP,   // fora do horário comercial — entra em deep sleep
  STATE_ERROR
};

struct Config {
  String   url;
  String   token;
  uint8_t  brightness;
  uint16_t pollInterval;
  uint16_t sleepInterval;
  int8_t   tzOffset;     // UTC offset em horas inteiras
  uint8_t  whStart;      // hora de início do expediente (0-23)
  uint8_t  whEnd;        // hora de fim do expediente (0-23)
  uint8_t  whDays;       // bitmask: bit0=Dom, bit1=Seg, …, bit6=Sáb
};

CRGB         leds[NUM_LEDS];
Preferences  prefs;
Config       cfg;
State        currentState = STATE_UNKNOWN;
unsigned long lastPollMs  = 0;
int          consecutiveErrors = 0;
bool         batteryTaskSent = false;
volatile bool gButtonActive   = false;
CRGB          gButtonColor   = CRGB::Black;
volatile int  gBatteryPercent = -1;

// WiFiManager guarda ponteiros para os parâmetros customizados.
WiFiManagerParameter* p_url      = nullptr;
WiFiManagerParameter* p_token    = nullptr;
WiFiManagerParameter* p_brightness = nullptr;
WiFiManagerParameter* p_interval = nullptr;
WiFiManagerParameter* p_sleep_iv = nullptr;
WiFiManagerParameter* p_tz       = nullptr;
WiFiManagerParameter* p_wh_start = nullptr;
WiFiManagerParameter* p_wh_end   = nullptr;
WiFiManagerParameter* p_wh_days  = nullptr;

// =====================================================================
// FORWARD DECLARATIONS
// =====================================================================
void   ledTask(void*);
void   loadConfig();
void   saveConfig();
void   setupWMParameters(WiFiManager& wm);
void   readWMParameters();
void   connectWiFi();
void   startConfigPortal(bool resetWifi);
void   syncNTP();
bool   isWithinWorkingHours();
State  fetchCalendarState();
void   renderMatrix();
void   drawPattern(const uint8_t* pattern, CRGB fg);
bool   handleButton();
int    readBatteryPercent();
CRGB   batteryColor(int percent);
void   sendBatteryTask();

// =====================================================================
// LED TASK — roda no core 0, independente do HTTP no core 1
// =====================================================================
void ledTask(void*) {
  State lastState      = STATE_UNKNOWN;
  bool  lastBtnActive  = false;

  for (;;) {
    bool btnChanged  = (gButtonActive != lastBtnActive);
    bool stateChanged = (currentState != lastState);
    bool isBlinking  = (currentState == STATE_YELLOW_BLINK) ||
                       (gBatteryPercent >= 0 && gBatteryPercent < 10);

    if (gButtonActive) {
      fill_solid(leds, NUM_LEDS, gButtonColor);
      FastLED.show();
    } else if (stateChanged || isBlinking || btnChanged) {
      renderMatrix();
      lastState = currentState;
    }

    lastBtnActive = gButtonActive;
    vTaskDelay(pdMS_TO_TICKS((isBlinking || gButtonActive) ? 50 : 200));
  }
}

// =====================================================================
// SETUP / LOOP
// =====================================================================
void setup() {
  setCpuFrequencyMhz(80);
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Meet Alert ===");

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(EXT_BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<WS2812, MATRIX_DATA_PIN, GRB>(leds, NUM_LEDS);

  loadConfig();
  FastLED.setBrightness(cfg.brightness);

  connectWiFi();
  syncNTP();
  WiFi.setSleep(true);
  xTaskCreatePinnedToCore(ledTask, "led", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
  // Botão tem prioridade — se está segurando, ele pinta o feedback.
  if (handleButton()) {
    return;
  }

  static uint8_t wifiRetries = 0;
  if (WiFi.status() != WL_CONNECTED) {
    gButtonColor  = CRGB(180, 60, 0);
    gButtonActive = true;
    WiFi.reconnect();
    delay(500);
    if (++wifiRetries > 20) ESP.restart();  // ~10s sem WiFi → reabre portal
    return;
  }
  wifiRetries = 0;

  unsigned long now = millis();

  static unsigned long lastHeartbeat = 0;
  if (now - lastHeartbeat >= 5000) {
    Serial.printf("[%lus] vivo | WiFi=%d estado=%d\n", now / 1000, WiFi.status(), currentState);
    lastHeartbeat = now;
  }

  if (now - lastPollMs > cfg.pollInterval * 1000UL || lastPollMs == 0) {
    Serial.printf("[%lus] Iniciando poll...\n", now / 1000);
    State s = fetchCalendarState();
    Serial.printf("[%lus] Poll concluído.\n", millis() / 1000);
    if (s == STATE_ERROR) {
      consecutiveErrors++;
      if (consecutiveErrors >= 3) currentState = STATE_ERROR;
    } else {
      consecutiveErrors = 0;
      if (s == STATE_GREEN && !isWithinWorkingHours()) s = STATE_SLEEP;
      currentState = s;
    }

    // Aproveita a janela de poll (já bloqueante) para enviar alerta de bateria
    gBatteryPercent = readBatteryPercent();
    if (gBatteryPercent < 0) Serial.println("Bateria: sem leitura");
    else                     Serial.printf("Bateria: %d%%\n", gBatteryPercent);
    if (gBatteryPercent >= 0 && gBatteryPercent < 10 && !batteryTaskSent) {
      sendBatteryTask();
      batteryTaskSent = true;
    } else if (gBatteryPercent < 0 || gBatteryPercent >= 15) {
      batteryTaskSent = false;
    }

    lastPollMs = now;
  }

  if (currentState == STATE_SLEEP) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    delay(50);
    Serial.printf("Fora do horário — deep sleep por %ds.\n", cfg.sleepInterval);
    Serial.flush();
    esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepInterval * 1000000ULL);
    esp_deep_sleep_start();
  }

  delay(10);
}

// =====================================================================
// CONFIG — persistência em NVS
// =====================================================================
void loadConfig() {
  prefs.begin("meetalert", true);  // read-only
  cfg.url           = prefs.getString("url", "");
  cfg.token         = prefs.getString("token", "");
  cfg.brightness    = prefs.getUChar("brightness", DEFAULT_BRIGHTNESS);
  cfg.pollInterval  = prefs.getUShort("interval", DEFAULT_POLL_INTERVAL);
  cfg.sleepInterval = prefs.getUShort("sleep_iv", DEFAULT_SLEEP_INTERVAL);
  cfg.tzOffset      = prefs.getChar("tz_offset", DEFAULT_TZ_OFFSET);
  cfg.whStart       = prefs.getUChar("wh_start",  DEFAULT_WH_START);
  cfg.whEnd         = prefs.getUChar("wh_end",    DEFAULT_WH_END);
  cfg.whDays        = prefs.getUChar("wh_days",   DEFAULT_WH_DAYS);
  prefs.end();

  Serial.printf("Config carregada: url=[%s] brilho=%d intervalo=%ds\n",
                cfg.url.c_str(), cfg.brightness, cfg.pollInterval);
}

void saveConfig() {
  prefs.begin("meetalert", false);  // read-write
  prefs.putString("url",      cfg.url);
  prefs.putString("token",    cfg.token);
  prefs.putUChar("brightness", cfg.brightness);
  prefs.putUShort("interval", cfg.pollInterval);
  prefs.putUShort("sleep_iv", cfg.sleepInterval);
  prefs.putChar("tz_offset",  cfg.tzOffset);
  prefs.putUChar("wh_start",  cfg.whStart);
  prefs.putUChar("wh_end",    cfg.whEnd);
  prefs.putUChar("wh_days",   cfg.whDays);
  prefs.end();
  Serial.println("Config salva na NVS.");
}

// =====================================================================
// WIFI MANAGER
// =====================================================================
void setupWMParameters(WiFiManager& wm) {
  char brightStr[5];
  char intervalStr[8];
  char sleepStr[8];
  char tzStr[5];
  char whStartStr[4];
  char whEndStr[4];
  char whDaysStr[8];
  snprintf(brightStr,   sizeof(brightStr),   "%d",  cfg.brightness);
  snprintf(intervalStr, sizeof(intervalStr), "%d",  cfg.pollInterval);
  snprintf(sleepStr,    sizeof(sleepStr),    "%d",  cfg.sleepInterval);
  snprintf(tzStr,       sizeof(tzStr),       "%d",  cfg.tzOffset);
  snprintf(whStartStr,  sizeof(whStartStr),  "%d",  cfg.whStart);
  snprintf(whEndStr,    sizeof(whEndStr),    "%d",  cfg.whEnd);
  for (int i = 0; i < 7; i++) whDaysStr[i] = (cfg.whDays & (1 << i)) ? '1' : '0';
  whDaysStr[7] = '\0';

  // Limpa ponteiros antigos se chamado mais de uma vez.
  delete p_url;         p_url         = new WiFiManagerParameter("url",      "URL Apps Script",          cfg.url.c_str(), 220);
  delete p_token;       p_token       = new WiFiManagerParameter("token",    "Token",                    cfg.token.c_str(), 40);
  delete p_brightness;  p_brightness  = new WiFiManagerParameter("brightness","Brilho 1-255",            brightStr,   4);
  delete p_interval;    p_interval    = new WiFiManagerParameter("interval", "Polling (seg)",            intervalStr, 6);
  delete p_sleep_iv;    p_sleep_iv    = new WiFiManagerParameter("sleep_iv", "Sleep fora horário (seg)", sleepStr,    6);
  delete p_tz;          p_tz          = new WiFiManagerParameter("tz",       "Fuso horário (ex: -3)",    tzStr,       4);
  delete p_wh_start;    p_wh_start    = new WiFiManagerParameter("wh_start", "Expediente início (h)",    whStartStr,  3);
  delete p_wh_end;      p_wh_end      = new WiFiManagerParameter("wh_end",   "Expediente fim (h)",       whEndStr,    3);
  delete p_wh_days;     p_wh_days     = new WiFiManagerParameter("wh_days",  "Dias úteis (DSTQQSS)",     whDaysStr,   8);

  wm.addParameter(p_url);
  wm.addParameter(p_token);
  wm.addParameter(p_brightness);
  wm.addParameter(p_interval);
  wm.addParameter(p_sleep_iv);
  wm.addParameter(p_tz);
  wm.addParameter(p_wh_start);
  wm.addParameter(p_wh_end);
  wm.addParameter(p_wh_days);
}

void readWMParameters() {
  if (p_url)        cfg.url           = p_url->getValue();
  if (p_token)      cfg.token         = p_token->getValue();
  if (p_brightness) cfg.brightness    = constrain(atoi(p_brightness->getValue()), 1, 255);
  if (p_interval)   cfg.pollInterval  = max(5, atoi(p_interval->getValue()));
  if (p_sleep_iv)   cfg.sleepInterval = max(60, atoi(p_sleep_iv->getValue()));
  if (p_tz)         cfg.tzOffset      = constrain(atoi(p_tz->getValue()), -12, 14);
  if (p_wh_start)   cfg.whStart       = constrain(atoi(p_wh_start->getValue()), 0, 23);
  if (p_wh_end)     cfg.whEnd         = constrain(atoi(p_wh_end->getValue()), 0, 23);
  if (p_wh_days) {
    const char* s = p_wh_days->getValue();
    uint8_t mask = 0;
    for (int i = 0; i < 7 && s[i]; i++) if (s[i] == '1') mask |= (1 << i);
    cfg.whDays = mask;
  }
}

void connectWiFi() {
  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFiManager wm;
  wm.setDebugOutput(false);
  setupWMParameters(wm);
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);

  fill_solid(leds, NUM_LEDS, CRGB::Black);
  leds[HEARTBEAT_LED] = CRGB::White;
  FastLED.setBrightness(10);
  FastLED.show();

  wm.setSaveParamsCallback([]() {
    readWMParameters();
    saveConfig();
    FastLED.setBrightness(cfg.brightness);
  });

  Serial.println("Tentando conectar WiFi ou abrir portal...");
  bool ok = wm.autoConnect(AP_SSID);

  FastLED.setBrightness(cfg.brightness);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  if (ok) {
    Serial.printf("Conectado: SSID=%s IP=%s\n",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  } else {
    Serial.println("Sem WiFi após timeout, reiniciando...");
    ESP.restart();
  }
}

void startConfigPortal(bool resetWifi) {
  Serial.printf("Portal de configuração (resetWifi=%d)\n", resetWifi);

  WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
  WiFiManager wm;
  if (resetWifi) wm.resetSettings();

  setupWMParameters(wm);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);
  wm.setSaveParamsCallback([]() {
    readWMParameters();
    saveConfig();
  });

  gButtonColor  = CRGB::Blue;
  gButtonActive = true;

  wm.startConfigPortal(AP_SSID);

  Serial.println("Portal encerrado, reiniciando...");
  ESP.restart();
}

// =====================================================================
// NTP / HORÁRIO
// =====================================================================
void syncNTP() {
  configTime((long)cfg.tzOffset * 3600, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  int retries = 0;
  while (!getLocalTime(&timeinfo) && retries < 20) {
    delay(500);
    retries++;
  }
  if (retries < 20)
    Serial.printf("NTP OK: %02d/%02d/%04d %02d:%02d\n",
                  timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                  timeinfo.tm_hour, timeinfo.tm_min);
  else
    Serial.println("NTP: timeout — working hours desabilitado.");
}

bool isWithinWorkingHours() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return true;  // sem hora → não dorme (safe)
  if (!(cfg.whDays & (1 << timeinfo.tm_wday))) return false;
  float h = timeinfo.tm_hour + timeinfo.tm_min / 60.0f;
  return h >= cfg.whStart && h < cfg.whEnd;
}

// =====================================================================
// APPS SCRIPT — consulta e parse
// =====================================================================
State parseStateString(const String& s) {
  if (s == "green")        return STATE_GREEN;
  if (s == "yellow")       return STATE_YELLOW;
  if (s == "red")          return STATE_RED;
  if (s == "yellow_blink") return STATE_YELLOW_BLINK;
  if (s == "sleep")        return STATE_SLEEP;
  return STATE_ERROR;
}

State fetchCalendarState() {
  if (cfg.url.length() == 0) {
    Serial.println("  [fetch] URL vazia, pulando.");
    return STATE_ERROR;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String fullUrl = cfg.url;
  fullUrl += (cfg.url.indexOf('?') >= 0 ? "&" : "?");
  fullUrl += "token=";
  fullUrl += cfg.token;

  Serial.println("  [fetch] http.begin...");
  if (!http.begin(client, fullUrl)) {
    Serial.println("  [fetch] http.begin FALHOU");
    return STATE_ERROR;
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  Serial.println("  [fetch] GET...");
  int code = http.GET();
  Serial.printf("  [fetch] HTTP %d\n", code);

  if (code != 200) {
    http.end();
    return STATE_ERROR;
  }

  String body = http.getString();
  body.trim();
  http.end();

  Serial.printf("  [fetch] resposta: [%s]\n", body.c_str());
  return parseStateString(body);
}

// =====================================================================
// MATRIZ — bitmaps 8x8 (bit 1 = LED ligado, bit 0 = apagado)
// =====================================================================
const uint8_t PATTERN_CIRCLE[8] = {
  0b00111100,
  0b01111110,
  0b11111111,
  0b11111111,
  0b11111111,
  0b11111111,
  0b01111110,
  0b00111100
};

void drawPattern(const uint8_t* pattern, CRGB fg) {
  for (uint8_t y = 0; y < 8; y++) {
    for (uint8_t x = 0; x < 8; x++) {
      bool on = pattern[y] & (1 << (7 - x));
      leds[y * 8 + x] = on ? fg : CRGB::Black;
    }
  }
}

void renderMatrix() {
  const uint8_t* pattern = PATTERN_CIRCLE;
  CRGB cor = CRGB::Black;

  switch (currentState) {
    case STATE_GREEN:
      cor = CRGB::DarkGreen;
      break;
    case STATE_YELLOW:
      cor = CRGB(180, 120, 0);
      break;
    case STATE_RED:
      cor = CRGB::DarkRed;
      break;
    case STATE_YELLOW_BLINK:
      cor = ((millis() / BLINK_PERIOD_MS) % 2 == 0) ? CRGB(255, 80, 0) : CRGB::Black;
      break;
    case STATE_ERROR:
      cor = CRGB::Purple;
      break;
    default:
      break;  // tudo preto
  }

  drawPattern(pattern, cor);
  leds[BATTERY_INDICATOR_LED] = batteryColor(gBatteryPercent);
  FastLED.show();
}

// =====================================================================
// BATERIA
// =====================================================================
int readBatteryPercent() {
  const int N = 10;
  uint32_t sum = 0;
  for (int i = 0; i < N; i++) sum += analogRead(BATTERY_ADC_PIN);
  float rawAvg = sum / (float)N;

  // analogRead default no ESP32-C6: 12 bits (0-4095), faixa 0-3.3V aprox.
  float vPin = (rawAvg / 4095.0f) * 3.3f;
  float vBat = vPin * 2.0f;          // desfaz o divisor 1:1

  if (vBat < BATTERY_EMPTY_V) return -1;  // ausente ou pino flutuando

  int pct = (int)((vBat - BATTERY_EMPTY_V) / (BATTERY_FULL_V - BATTERY_EMPTY_V) * 100);
  return constrain(pct, 0, 100);
}

CRGB batteryColor(int percent) {
  if (percent < 0) {
    // Sem bateria — deixa apagado pra não poluir.
    return CRGB::Black;
  }
  if (percent < 10) {
    // Crítico — piscando rápido.
    return (millis() / 200) % 2 == 0 ? CRGB::Red : CRGB::Black;
  }
  if (percent < 20) return CRGB::Red;
  if (percent < 50) return CRGB(255, 80, 0);  // laranja
  if (percent >= 98) return CRGB::Blue;       // carga completa
  return CRGB::Green;
}

// =====================================================================
// BOTÃO — long press durante operação
// =====================================================================
bool handleButton() {
  static unsigned long pressStart = 0;
  static bool wasPressed = false;
  static unsigned long lastCall = 0;

  unsigned long now = millis();
  // Se o loop ficou bloqueado por mais de 2s (ex: HTTP fetch), qualquer
  // pressStart gravado antes do bloqueio não representa um hold real.
  if (wasPressed && (now - lastCall) > 2000) {
    wasPressed    = false;
    pressStart    = 0;
    gButtonActive = false;
    lastCall      = now;
    return false;
  }
  lastCall = now;

  bool pressed = (digitalRead(BOOT_BUTTON_PIN) == LOW) || (digitalRead(EXT_BUTTON_PIN) == LOW);

  if (pressed && !wasPressed) {
    pressStart = millis();
    wasPressed = true;
  }

  if (!pressed && wasPressed) {
    unsigned long held = millis() - pressStart;
    wasPressed = false;

    if (held >= BTN_HOLD_RESET_MS) {
      startConfigPortal(true);   // não retorna (ESP.restart)
    } else if (held >= BTN_HOLD_CONFIG_MS) {
      startConfigPortal(false);
    }
    gButtonActive = false;
    return false;
  }

  if (pressed) {
    unsigned long held = millis() - pressStart;
    if (held >= BTN_HOLD_RESET_MS) {
      gButtonColor  = CRGB(255, 100, 0);
      gButtonActive = true;
      return true;
    }
    if (held >= BTN_HOLD_CONFIG_MS) {
      gButtonColor  = CRGB::Blue;
      gButtonActive = true;
      return true;
    }
  }
  gButtonActive = false;
  return false;
}

// =====================================================================
// TAREFA DE BATERIA — cria item no Google Tasks via Apps Script
// =====================================================================
void sendBatteryTask() {
  if (cfg.url.length() == 0) return;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = cfg.url;
  url += (url.indexOf('?') >= 0 ? "&" : "?");
  url += "action=battery&token=";
  url += cfg.token;

  if (!http.begin(client, url)) return;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  int code = http.GET();
  Serial.printf("sendBatteryTask: HTTP %d\n", code);
  http.end();
}
