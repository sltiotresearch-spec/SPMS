#include <Arduino.h>
#include <math.h>
#include "esp_task_wdt.h"

// ===================== MODEM (A7680C) UART =====================
static const int MODEM_TX_PIN = 17;    // ESP32 TX -> Modem RX
static const int MODEM_RX_PIN = 16;    // ESP32 RX <- Modem TX
static const int MODEM_BAUD   = 115200;

// Your proven PWRKEY behavior: HIGH ~3s then LOW
static const int MODEM_PWRKEY_PIN = 12;

// APN (Mobitel)
static const char APN[] = "mobitel";

// ThingsBoard endpoint (token in URL)
static const char TB_URL[] =
  "http://thingsboard.cloud/api/v1/ju4ahw5881c13u2ipkvg/telemetry";

// ===================== TELEMETRY RULES =====================
// Every 15 minutes
static const uint32_t PERIODIC_INTERVAL_MS = 15UL * 60UL * 1000UL;

// Event send anti-spam for bouncing inputs
static const uint32_t MIN_EVENT_GAP_MS = 5000;

// EASY TO TUNE: voltage delta needed to trigger an immediate send (Volts)
static const float VOLT_EVENT_DELTA_V = 20;   // <-- change as you like

// Backoff
static const uint32_t BACKOFF_MAX_MS = 10UL * 60UL * 1000UL; // max 10 min
static uint8_t  failCount = 0;
static uint32_t nextRetryMs = 0;
static uint32_t backoffMs = 5000; // start 5s

// ===================== IO PINS =====================
#define VOLTAGE_PIN 4

#define P_TP1 18
#define P_TP2 19
#define P_TP3 36
#define P_TP4 39
#define P_TP5 34
#define P_TP6 35
#define P_TP7 32
#define P_TP8 33

// If your inputs are active LOW (pulled-up and grounded when active), set true
static const bool INPUT_ACTIVE_LOW = true;

// ===================== VOLTAGE CALIBRATION (MATCH ORIGINAL CODE) =====================
// Original formula:
//   voltage = ADC_avg * 220 / 2645.6
// Meaning: ADC_avg==2645.6 corresponds to 220V in your hardware chain.
static const float VOLT_CAL_REF_V   = 220.0f;
static const float VOLT_CAL_REF_ADC = 2645.6f;

// ===================== SERIAL =====================
HardwareSerial Modem(2);

// ===================== SNAPSHOT TYPE =====================
struct Snapshot {
  bool tp[8];
  float v;
};

static Snapshot lastSnap = {{false,false,false,false,false,false,false,false}, 0.0f};
static bool pendingSend = false;
static Snapshot pendingSnap;

// ===================== WATCHDOG =====================
static void initWDT() {
  esp_task_wdt_config_t cfg = {
    .timeout_ms = 120000, // 120s
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL);
}
static inline void feedWDT() { esp_task_wdt_reset(); }

// ===================== MODEM HELPERS =====================
static void flushModemRx() {
  while (Modem.available()) Modem.read();
}

static bool readUntil(char* out, size_t outLen, const char* token, uint32_t timeoutMs) {
  if (!out || outLen < 2) return false;
  out[0] = '\0';
  size_t idx = 0;

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    feedWDT();
    while (Modem.available()) {
      char c = (char)Modem.read();
      if (idx + 1 < outLen) {
        out[idx++] = c;
        out[idx] = '\0';
      }
      if (token && strstr(out, token)) return true;
      if (strstr(out, "ERROR")) return false;
    }
    delay(5);
  }
  return false;
}

static bool sendCmdWaitOK(const char* cmd, char* resp, size_t respLen, uint32_t timeoutMs) {
  flushModemRx();
  Modem.print(cmd);
  Modem.print("\r\n");
  bool got = readUntil(resp, respLen, "OK", timeoutMs);
  return got && (strstr(resp, "OK") != nullptr) && (strstr(resp, "ERROR") == nullptr);
}

static bool probeAT(uint8_t tries = 8) {
  char r[128];
  for (uint8_t i = 0; i < tries; i++) {
    feedWDT();
    flushModemRx();
    Modem.print("\r\n");      // wake
    delay(80);
    Modem.print("AT\r\n");
    if (readUntil(r, sizeof(r), "OK", 900) && strstr(r, "OK")) return true;
    delay(250);
  }
  return false;
}

static void modemPowerOnPulse() {
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(3000);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
}

static bool waitSIMReady(uint32_t totalMs) {
  char r[128];
  uint32_t t0 = millis();
  while (millis() - t0 < totalMs) {
    feedWDT();
    sendCmdWaitOK("AT+CPIN?", r, sizeof(r), 1500);
    if (strstr(r, "READY")) return true;
    delay(500);
  }
  return false;
}

static bool waitLTEReg(uint32_t totalMs) {
  char r[128];
  uint32_t t0 = millis();
  while (millis() - t0 < totalMs) {
    feedWDT();
    sendCmdWaitOK("AT+CEREG?", r, sizeof(r), 1500);
    if (strstr(r, ",1") || strstr(r, ",5")) return true;
    delay(1200);
  }
  return false;
}

static bool waitHTTPACTION(int &status, int &len, uint32_t timeoutMs) {
  char buf[512];
  buf[0] = '\0';
  size_t idx = 0;

  uint32_t t0 = millis();
  status = -1;
  len = 0;

  while (millis() - t0 < timeoutMs) {
    feedWDT();
    while (Modem.available()) {
      char c = (char)Modem.read();
      if (idx + 1 < sizeof(buf)) {
        buf[idx++] = c;
        buf[idx] = '\0';
      }

      char* p = strstr(buf, "+HTTPACTION:");
      if (p) {
        int method = 0;
        if (sscanf(p, "+HTTPACTION: %d,%d,%d", &method, &status, &len) == 3) return true;
      }

      if (idx > 420) {
        memmove(buf, buf + 200, idx - 200);
        idx -= 200;
        buf[idx] = '\0';
      }
    }
    delay(10);
  }
  return false;
}

static bool httpSendBody(const char* body, uint32_t inputTimeoutMs = 10000) {
  if (!body) return false;

  char r[256];
  int n = (int)strlen(body);

  char cmd[64];
  snprintf(cmd, sizeof(cmd), "AT+HTTPDATA=%d,%lu", n, (unsigned long)inputTimeoutMs);

  flushModemRx();
  Modem.print(cmd);
  Modem.print("\r\n");

  if (!readUntil(r, sizeof(r), "DOWNLOAD", 5000)) {
    Serial.println("HTTPDATA: no DOWNLOAD");
    Serial.println(r);
    return false;
  }

  Modem.write((const uint8_t*)body, n);

  if (!readUntil(r, sizeof(r), "OK", inputTimeoutMs + 5000)) {
    Serial.println("HTTPDATA: no OK after payload");
    Serial.println(r);
    return false;
  }

  return true;
}

static bool ensureDataSession() {
  char r[256];

  if (!probeAT(3)) return false;
  if (!waitSIMReady(15000)) return false;
  if (!waitLTEReg(60000))   return false;

  {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", APN);
    sendCmdWaitOK(cmd, r, sizeof(r), 2000);
  }

  sendCmdWaitOK("AT+CGATT=1", r, sizeof(r), 15000);

  if (!sendCmdWaitOK("AT+CGACT=1,1", r, sizeof(r), 15000)) {
    Serial.println("CGACT failed:");
    Serial.println(r);
    return false;
  }

  sendCmdWaitOK("AT+CGPADDR=1", r, sizeof(r), 3000);
  Serial.println(r);

  return true;
}

static bool postTelemetry(const char* json, int* outStatus = nullptr) {
  char r[256];
  int status = -1, len = 0;

  if (!ensureDataSession()) {
    if (outStatus) *outStatus = -1001;
    return false;
  }

  sendCmdWaitOK("AT+HTTPTERM", r, sizeof(r), 1500);

  if (!sendCmdWaitOK("AT+HTTPINIT", r, sizeof(r), 5000)) {
    if (outStatus) *outStatus = -1002;
    return false;
  }

  sendCmdWaitOK("AT+HTTPPARA=\"CID\",1", r, sizeof(r), 2000);

  {
    char cmd[280];
    snprintf(cmd, sizeof(cmd), "AT+HTTPPARA=\"URL\",\"%s\"", TB_URL);
    if (!sendCmdWaitOK(cmd, r, sizeof(r), 6000)) {
      sendCmdWaitOK("AT+HTTPTERM", r, sizeof(r), 2000);
      if (outStatus) *outStatus = -1003;
      return false;
    }
  }

  sendCmdWaitOK("AT+HTTPPARA=\"CONTENT\",\"application/json\"", r, sizeof(r), 2000);

  if (!httpSendBody(json, 10000)) {
    sendCmdWaitOK("AT+HTTPTERM", r, sizeof(r), 2000);
    if (outStatus) *outStatus = -1004;
    return false;
  }

  sendCmdWaitOK("AT+HTTPACTION=1", r, sizeof(r), 3000);

  if (!waitHTTPACTION(status, len, 20000)) {
    sendCmdWaitOK("AT+HTTPTERM", r, sizeof(r), 2000);
    if (outStatus) *outStatus = -1005;
    return false;
  }

  Serial.print("TB HTTP status=");
  Serial.print(status);
  Serial.print(" len=");
  Serial.println(len);

  sendCmdWaitOK("AT+HTTPTERM", r, sizeof(r), 2000);

  if (outStatus) *outStatus = status;
  return (status == 200 || status == 204);
}

// ===================== RECOVERY LADDER =====================
static bool modemRecoverStep(uint8_t step) {
  char r[256];

  if (step == 1) {
    sendCmdWaitOK("AT+HTTPTERM", r, sizeof(r), 2000);
    return probeAT(3);
  }

  if (step == 2) {
    sendCmdWaitOK("AT+CGACT=0,1", r, sizeof(r), 15000);
    delay(500);
    sendCmdWaitOK("AT+CGATT=1", r, sizeof(r), 15000);
    delay(500);
    return sendCmdWaitOK("AT+CGACT=1,1", r, sizeof(r), 15000);
  }

  if (step == 3) {
    sendCmdWaitOK("AT+CGATT=0", r, sizeof(r), 15000);
    delay(1000);
    return sendCmdWaitOK("AT+CGATT=1", r, sizeof(r), 20000);
  }

  if (step == 4) {
    sendCmdWaitOK("AT+CFUN=0", r, sizeof(r), 8000);
    delay(2000);
    sendCmdWaitOK("AT+CFUN=1", r, sizeof(r), 15000);
    delay(5000);
    return probeAT(10);
  }

  if (step == 5) {
    bool ok = sendCmdWaitOK("AT+CRESET", r, sizeof(r), 3000);
    if (!ok) sendCmdWaitOK("AT+CFUN=1,1", r, sizeof(r), 3000);
    delay(15000);
    return probeAT(25);
  }

  if (step >= 6) {
    ESP.restart();
  }

  return false;
}

// ===================== IO HELPERS =====================
static inline bool readTP(int pin) {
  int v = digitalRead(pin);
  return INPUT_ACTIVE_LOW ? (v == LOW) : (v == HIGH);
}

// FIXED: voltage calculation matches your original code exactly
static float readVoltage() {
  float adc = 0.0f;
  for (uint8_t i = 0; i < 10; i++) {
    adc += analogRead(VOLTAGE_PIN);
    delay(2);
  }
  adc /= 10.0f;

  float voltage = adc * VOLT_CAL_REF_V / VOLT_CAL_REF_ADC;
  if (voltage < 0.0f) voltage = 0.0f;
  return voltage;
}

static void readSnapshot(Snapshot &s) {
  s.tp[0] = readTP(P_TP1);
  s.tp[1] = readTP(P_TP2);
  s.tp[2] = readTP(P_TP3);
  s.tp[3] = readTP(P_TP4);
  s.tp[4] = readTP(P_TP5);
  s.tp[5] = readTP(P_TP6);
  s.tp[6] = readTP(P_TP7);
  s.tp[7] = readTP(P_TP8);
  s.v     = readVoltage();
}

static bool snapshotChanged(const Snapshot &cur, const Snapshot &prev) {
  for (int i = 0; i < 8; i++) {
    if (cur.tp[i] != prev.tp[i]) return true;
  }
  if (fabsf(cur.v - prev.v) >= VOLT_EVENT_DELTA_V) return true;
  return false;
}

static void buildTelemetryJson(char* out, size_t outLen, const Snapshot &s) {
  snprintf(out, outLen,
           "{\"V\":%.1f,"
           "\"1\":%s,\"2\":%s,\"3\":%s,\"4\":%s,"
           "\"5\":%s,\"6\":%s,\"7\":%s,\"8\":%s}",
           s.v,
           s.tp[0] ? "true" : "false",
           s.tp[1] ? "true" : "false",
           s.tp[2] ? "true" : "false",
           s.tp[3] ? "true" : "false",
           s.tp[4] ? "true" : "false",
           s.tp[5] ? "true" : "false",
           s.tp[6] ? "true" : "false",
           s.tp[7] ? "true" : "false");
}

static void initInputs() {
  pinMode(P_TP1, INPUT);
  pinMode(P_TP2, INPUT);
  pinMode(P_TP3, INPUT);
  pinMode(P_TP4, INPUT);
  pinMode(P_TP5, INPUT);
  pinMode(P_TP6, INPUT);
  pinMode(P_TP7, INPUT);
  pinMode(P_TP8, INPUT);

  analogReadResolution(12);
#if defined(ADC_11db)
  analogSetPinAttenuation(VOLTAGE_PIN, ADC_11db);
#endif
}

// ===================== SEND (ROBUST) =====================
static bool sendNowRobust(const Snapshot &s) {
  uint32_t now = millis();

  if (now < nextRetryMs) {
    pendingSend = true;
    pendingSnap = s;
    return false;
  }

  char payload[256];
  buildTelemetryJson(payload, sizeof(payload), s);
  Serial.print("POST payload: ");
  Serial.println(payload);

  int httpStatus = -1;
  bool ok = postTelemetry(payload, &httpStatus);

  if (ok) {
    Serial.println("✅ Telemetry sent");
    failCount = 0;
    backoffMs = 5000;
    nextRetryMs = 0;
    pendingSend = false;
    return true;
  }

  Serial.print("❌ Telemetry failed, httpStatus=");
  Serial.println(httpStatus);

  uint8_t step = 1;
  if (failCount >= 1) step = 2;
  if (failCount >= 2) step = 3;
  if (failCount >= 3) step = 4;
  if (failCount >= 4) step = 5;
  if (failCount >= 6) step = 6;

  Serial.print("Recovery step=");
  Serial.println(step);
  modemRecoverStep(step);

  failCount++;
  nextRetryMs = now + backoffMs;
  backoffMs = (backoffMs < BACKOFF_MAX_MS / 2) ? (backoffMs * 2) : BACKOFF_MAX_MS;

  pendingSend = true;
  pendingSnap = s;
  return false;
}

// ===================== MAIN =====================
static uint32_t lastPeriodicSendMs = 0;
static uint32_t lastEventSendMs    = 0;

void setup() {
  Serial.begin(115200);
  Modem.begin(MODEM_BAUD, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
  delay(2000);

  initWDT();
  initInputs();

  flushModemRx();
  delay(200);

  if (!probeAT(10)) {
    Serial.println("No AT OK -> powering modem ON...");
    modemPowerOnPulse();
    delay(6000);
    if (!probeAT(25)) {
      Serial.println("Still no AT OK. Check wiring/baud/power.");
      ESP.restart();
    }
  } else {
    Serial.println("Modem already ON.");
  }

  {
    char r[192];
    sendCmdWaitOK("ATE0", r, sizeof(r), 1500);
    sendCmdWaitOK("AT+CMEE=2", r, sizeof(r), 1500);
    sendCmdWaitOK("ATI", r, sizeof(r), 2000);
    Serial.println(r);
  }

  readSnapshot(lastSnap);
  sendNowRobust(lastSnap);

  lastPeriodicSendMs = millis();
  lastEventSendMs    = millis();
}

void loop() {
  feedWDT();
  uint32_t now = millis();

  if (pendingSend && now >= nextRetryMs) {
    sendNowRobust(pendingSnap);
  }

  Snapshot cur;
  readSnapshot(cur);

  if (snapshotChanged(cur, lastSnap) && (now - lastEventSendMs >= MIN_EVENT_GAP_MS)) {
    if (sendNowRobust(cur)) {
      lastSnap = cur;
      lastEventSendMs = now;
      lastPeriodicSendMs = now;
    }
  }

  if (now - lastPeriodicSendMs >= PERIODIC_INTERVAL_MS) {
    if (sendNowRobust(cur)) {
      lastSnap = cur;
      lastPeriodicSendMs = now;
    }
  }

  delay(200);
}