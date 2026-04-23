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

// =====================================================================
// HARDWARE
// =====================================================================
#define MATRIX_DATA_PIN        D6    // DIN da matriz WS2812
#define BATTERY_ADC_PIN        D0    // meio do divisor 100k + 100k
#define BOOT_BUTTON_PIN        9     // botão BOOT do XIAO (GPIO9)

#define NUM_LEDS               64
#define BATTERY_INDICATOR_LED  0     // índice do LED que mostra bateria

// =====================================================================
// COMPORTAMENTO
// =====================================================================
#define AP_SSID                "MeetAlert-Config"
#define WIFI_CONNECT_TIMEOUT   30    // segundos
#define CONFIG_PORTAL_TIMEOUT  300   // segundos (5 min)

#define DEFAULT_BRIGHTNESS     30
#define DEFAULT_POLL_INTERVAL  30    // segundos entre polls do Apps Script

#define BTN_HOLD_RESET_MS      3000  // 3s = apagar WiFi e reconfigurar
#define BTN_HOLD_CONFIG_MS     5000  // 5s = portal mantendo WiFi atual

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
  STATE_ERROR
};

struct Config {
  String   url;
  String   token;
  uint8_t  brightness;
  uint16_t pollInterval;
};

CRGB         leds[NUM_LEDS];
Preferences  prefs;
Config       cfg;
State        currentState = STATE_UNKNOWN;
unsigned long lastPollMs  = 0;
int          consecutiveErrors = 0;
bool         batteryTaskSent = false;

// WiFiManager guarda ponteiros para os parâmetros customizados.
WiFiManagerParameter* p_url = nullptr;
WiFiManagerParameter* p_token = nullptr;
WiFiManagerParameter* p_brightness = nullptr;
WiFiManagerParameter* p_interval = nullptr;

// =====================================================================
// FORWARD DECLARATIONS
// =====================================================================
void   loadConfig();
void   saveConfig();
void   setupWMParameters(WiFiManager& wm);
void   readWMParameters();
void   connectWiFi();
void   startConfigPortal(bool resetWifi);
State  fetchCalendarState();
void   renderMatrix();
void   drawPattern(const uint8_t* pattern, CRGB fg);
bool   handleButton();
int    readBatteryPercent();
CRGB   batteryColor(int percent);
void   sendBatteryTask();

// =====================================================================
// SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Meet Alert ===");

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<WS2812, MATRIX_DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(DEFAULT_BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  loadConfig();
  FastLED.setBrightness(cfg.brightness);

  connectWiFi();
}

void loop() {
  // Botão tem prioridade — se está segurando, ele pinta o feedback.
  if (handleButton()) {
    return;
  }

  static uint8_t wifiRetries = 0;
  if (WiFi.status() != WL_CONNECTED) {
    uint8_t v = sin8(millis() / 4);
    fill_solid(leds, NUM_LEDS, CRGB(v, v / 3, 0));
    FastLED.show();
    WiFi.reconnect();
    delay(500);
    if (++wifiRetries > 20) ESP.restart();  // ~10s sem WiFi → reabre portal
    return;
  }
  wifiRetries = 0;

  unsigned long now = millis();
  if (now - lastPollMs > cfg.pollInterval * 1000UL || lastPollMs == 0) {
    State s = fetchCalendarState();
    if (s == STATE_ERROR) {
      consecutiveErrors++;
      if (consecutiveErrors >= 3) currentState = STATE_ERROR;
    } else {
      consecutiveErrors = 0;
      currentState = s;
    }

    // Aproveita a janela de poll (já bloqueante) para enviar alerta de bateria
    int batPct = readBatteryPercent();
    if (batPct >= 0 && batPct < 10 && !batteryTaskSent) {
      sendBatteryTask();
      batteryTaskSent = true;
    } else if (batPct < 0 || batPct >= 15) {
      batteryTaskSent = false;
    }

    lastPollMs = now;
  }

  renderMatrix();
  delay(50);  // ~20 fps, suficiente para o blink suave
}

// =====================================================================
// CONFIG — persistência em NVS
// =====================================================================
void loadConfig() {
  prefs.begin("meetalert", true);  // read-only
  cfg.url          = prefs.getString("url", "");
  cfg.token        = prefs.getString("token", "");
  cfg.brightness   = prefs.getUChar("brightness", DEFAULT_BRIGHTNESS);
  cfg.pollInterval = prefs.getUShort("interval", DEFAULT_POLL_INTERVAL);
  prefs.end();

  Serial.printf("Config carregada: url=[%s] brilho=%d intervalo=%ds\n",
                cfg.url.c_str(), cfg.brightness, cfg.pollInterval);
}

void saveConfig() {
  prefs.begin("meetalert", false);  // read-write
  prefs.putString("url", cfg.url);
  prefs.putString("token", cfg.token);
  prefs.putUChar("brightness", cfg.brightness);
  prefs.putUShort("interval", cfg.pollInterval);
  prefs.end();
  Serial.println("Config salva na NVS.");
}

// =====================================================================
// WIFI MANAGER
// =====================================================================
void setupWMParameters(WiFiManager& wm) {
  char brightStr[5];
  char intervalStr[8];
  snprintf(brightStr,   sizeof(brightStr),   "%d", cfg.brightness);
  snprintf(intervalStr, sizeof(intervalStr), "%d", cfg.pollInterval);

  // Limpa ponteiros antigos se chamado mais de uma vez.
  delete p_url;         p_url         = new WiFiManagerParameter("url",      "URL Apps Script",  cfg.url.c_str(),   220);
  delete p_token;       p_token       = new WiFiManagerParameter("token",    "Token",            cfg.token.c_str(), 40);
  delete p_brightness;  p_brightness  = new WiFiManagerParameter("brightness","Brilho 1-255",    brightStr,         4);
  delete p_interval;    p_interval    = new WiFiManagerParameter("interval", "Polling (seg)",   intervalStr,       6);

  wm.addParameter(p_url);
  wm.addParameter(p_token);
  wm.addParameter(p_brightness);
  wm.addParameter(p_interval);
}

void readWMParameters() {
  if (p_url)        cfg.url          = p_url->getValue();
  if (p_token)      cfg.token        = p_token->getValue();
  if (p_brightness) cfg.brightness   = constrain(atoi(p_brightness->getValue()), 1, 255);
  if (p_interval)   cfg.pollInterval = max(5, atoi(p_interval->getValue()));
}

void connectWiFi() {
  WiFiManager wm;
  wm.setDebugOutput(false);
  setupWMParameters(wm);
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);

  // Feedback visual enquanto tenta conectar: branco pulsante.
  fill_solid(leds, NUM_LEDS, CRGB(40, 40, 40));
  FastLED.show();

  wm.setSaveParamsCallback([]() {
    readWMParameters();
    saveConfig();
    FastLED.setBrightness(cfg.brightness);
  });

  Serial.println("Tentando conectar WiFi ou abrir portal...");
  bool ok = wm.autoConnect(AP_SSID);

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

  WiFiManager wm;
  if (resetWifi) wm.resetSettings();

  setupWMParameters(wm);
  wm.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT);
  wm.setSaveParamsCallback([]() {
    readWMParameters();
    saveConfig();
  });

  // Azul sólido enquanto portal está aberto.
  fill_solid(leds, NUM_LEDS, CRGB::Blue);
  FastLED.show();

  wm.startConfigPortal(AP_SSID);

  Serial.println("Portal encerrado, reiniciando...");
  ESP.restart();
}

// =====================================================================
// APPS SCRIPT — consulta e parse
// =====================================================================
State parseStateString(const String& s) {
  if (s == "green")        return STATE_GREEN;
  if (s == "yellow")       return STATE_YELLOW;
  if (s == "red")          return STATE_RED;
  if (s == "yellow_blink") return STATE_YELLOW_BLINK;
  return STATE_ERROR;
}

State fetchCalendarState() {
  if (cfg.url.length() == 0) {
    Serial.println("URL vazia, pule.");
    return STATE_ERROR;
  }

  // Apps Script é HTTPS. setInsecure() pula validação de certificado — não é um
  // problema aqui porque a autenticação é pelo token na query string.
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String fullUrl = cfg.url;
  fullUrl += (cfg.url.indexOf('?') >= 0 ? "&" : "?");
  fullUrl += "token=";
  fullUrl += cfg.token;

  if (!http.begin(client, fullUrl)) {
    Serial.println("http.begin falhou");
    return STATE_ERROR;
  }
  // Apps Script responde 302 pra googleusercontent.com. Sem isto, não chegamos no corpo.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP %d\n", code);
    http.end();
    return STATE_ERROR;
  }

  String body = http.getString();
  body.trim();
  http.end();

  Serial.printf("Estado bruto: [%s]\n", body.c_str());
  return parseStateString(body);
}

// =====================================================================
// MATRIZ — bitmaps 8x8 (bit 1 = LED ligado, bit 0 = apagado)
// =====================================================================
// Círculo cheio — verde, amarelo, erro (roxo)
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

// Círculo com X apagado no miolo — vermelho
const uint8_t PATTERN_CIRCLE_X[8] = {
  0b00111100,
  0b01111110,
  0b11011011,
  0b11100111,
  0b11100111,
  0b11011011,
  0b01111110,
  0b00111100
};

// Círculo com "!" apagado de 2 colunas — amarelo blink (5min warning)
const uint8_t PATTERN_CIRCLE_BANG[8] = {
  0b00111100,
  0b01100110,
  0b11100111,
  0b11100111,
  0b11100111,
  0b11111111,
  0b01100110,
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
      cor = CRGB::Green;
      break;
    case STATE_YELLOW:
      cor = CRGB::Yellow;
      break;
    case STATE_RED:
      pattern = PATTERN_CIRCLE_X;
      cor = CRGB::Red;
      break;
    case STATE_YELLOW_BLINK:
      pattern = PATTERN_CIRCLE_BANG;
      cor = ((millis() / BLINK_PERIOD_MS) % 2 == 0) ? CRGB::Yellow : CRGB::Black;
      break;
    case STATE_ERROR:
      cor = CRGB::Purple;
      break;
    default:
      break;  // tudo preto
  }

  drawPattern(pattern, cor);

  // LED de bateria sobrescreve um único LED
  int pct = readBatteryPercent();
  leds[BATTERY_INDICATOR_LED] = batteryColor(pct);

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
  return CRGB::Green;
}

// =====================================================================
// BOTÃO — long press durante operação
// =====================================================================
bool handleButton() {
  static unsigned long pressStart = 0;
  static bool wasPressed = false;

  bool pressed = (digitalRead(BOOT_BUTTON_PIN) == LOW);

  if (pressed && !wasPressed) {
    pressStart = millis();
    wasPressed = true;
  }

  if (!pressed && wasPressed) {
    unsigned long held = millis() - pressStart;
    wasPressed = false;

    if (held >= BTN_HOLD_CONFIG_MS) {
      startConfigPortal(false);  // não retorna (ESP.restart)
    } else if (held >= BTN_HOLD_RESET_MS) {
      startConfigPortal(true);
    }
    return false;
  }

  if (pressed) {
    unsigned long held = millis() - pressStart;
    if (held >= BTN_HOLD_CONFIG_MS) {
      fill_solid(leds, NUM_LEDS, CRGB::Blue);     // solta agora = portal mantendo WiFi
      FastLED.show();
      return true;
    }
    if (held >= BTN_HOLD_RESET_MS) {
      fill_solid(leds, NUM_LEDS, CRGB(255, 100, 0));  // solta agora = reset WiFi
      FastLED.show();
      return true;
    }
  }
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
