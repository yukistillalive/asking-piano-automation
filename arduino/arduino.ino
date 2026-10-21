// PianoAutomata — solenoid oracle (ESP32)
//
// Built for long exhibition runs with hundreds of questions:
//  - memory stays bounded (only the last few questions are kept)
//  - the request JSON is built with ArduinoJson, so any question text is safe
//    (quotes, backslashes, emoji, å/ä/ö, Japanese...)
//  - every sequence is checked and stretched/shrunk to 10–16 s on the device
//  - if Gemini is slow, rate-limited or offline, a locally generated sequence
//    plays instead, so the installation never goes silent
//  - Wi-Fi reconnects on its own; the board restarts itself between questions
//    if memory ever gets fragmented
//
// The serial lines "Thinking...", "Playing..." and "Waiting for input..."
// are read by oracle.py — keep them unchanged.

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ═══════════════════════ SETTINGS ═══════════════════════

// Wi-Fi networks, best signal wins. Add the venue network if it has a
// normal password (eduroam / WPA2-Enterprise will NOT work with this code).
const char* WIFI_NETWORKS[][2] = {
  // {"VENUE_WIFI", "VENUE_PASSWORD"},
};

const char* API_KEY = GEMINI_API_KEY;
const char* MODEL   = GEMINI_MODEL;

// Solenoid GPIOs, in physical order along the row.
const int SOLENOID_PINS[] = {13, 14, 16, 17, 18};

// Limits enforced on every sequence (ms)
const unsigned long MIN_TOTAL_MS = 10000;
const unsigned long MAX_TOTAL_MS = 16000;
const int MIN_ON_MS  = 60;
const int MAX_ON_MS  = 500;    // also protects the coils from long ON times
const int MIN_OFF_MS = 60;
const int MAX_OFF_MS = 1500;
const int MAX_PULSES = 60;

const int HISTORY_SIZE = 3;                     // recent questions sent as context
const unsigned long HISTORY_RESET_MS = 120000;  // 2 min quiet = new visitor
const int MAX_HISTORY_ITEM_LEN = 100;
const int MAX_QUESTION_BYTES = 600;
const uint16_t HTTP_TIMEOUT_MS = 35000;         // keep below 65000
const uint32_t MIN_CONTIGUOUS_HEAP = 45000;     // TLS needs a big free block

// ════════════════════════════════════════════════════════

const int NUM_SOLENOIDS = sizeof(SOLENOID_PINS) / sizeof(SOLENOID_PINS[0]);
const int NUM_NETWORKS  = sizeof(WIFI_NETWORKS) / sizeof(WIFI_NETWORKS[0]);

struct Pulse {
  uint8_t  solenoid;
  uint16_t onMs;
  uint16_t offMs;
};

Pulse seq[MAX_PULSES];
int seqLen = 0;

String history[HISTORY_SIZE];
int historyCount = 0;
unsigned long lastQuestionAt = 0;
unsigned long questionCount = 0;

WiFiMulti wifiMulti;
String systemPrompt;

// ───────────────────────── solenoids ─────────────────────────

void allSolenoidsOff() {
  for (int i = 0; i < NUM_SOLENOIDS; i++) digitalWrite(SOLENOID_PINS[i], LOW);
}

void playSequence() {
  for (int i = 0; i < seqLen; i++) {
    const Pulse& p = seq[i];
    Serial.printf("  solenoid %d (GPIO%d): ON %ums -> pause %ums\n",
                  p.solenoid, SOLENOID_PINS[p.solenoid], p.onMs, p.offMs);
    digitalWrite(SOLENOID_PINS[p.solenoid], HIGH);
    delay(p.onMs);
    digitalWrite(SOLENOID_PINS[p.solenoid], LOW);
    delay(p.offMs);
  }
  allSolenoidsOff();
}

// ───────────────────────── sequence shaping ─────────────────────────

unsigned long onTotal() {
  unsigned long t = 0;
  for (int i = 0; i < seqLen; i++) t += seq[i].onMs;
  return t;
}

unsigned long offTotal() {
  unsigned long t = 0;
  for (int i = 0; i < seqLen; i++) t += seq[i].offMs;
  return t;
}

void scaleOff(float factor) {
  for (int i = 0; i < seqLen; i++) {
    bool last = (i == seqLen - 1);
    long v = lround(seq[i].offMs * factor);
    seq[i].offMs = (uint16_t)constrain(v, last ? 0L : (long)MIN_OFF_MS, (long)MAX_OFF_MS);
  }
}

// Force the sequence into MIN_TOTAL_MS..MAX_TOTAL_MS, keeping its rhythm.
void normalizeTiming() {
  if (seqLen == 0) return;

  for (int i = 0; i < seqLen - 1; i++) {
    if (seq[i].offMs < MIN_OFF_MS) seq[i].offMs = MIN_OFF_MS;
  }

  // Far too much ON time: drop pulses from the end.
  while (seqLen > 6 && onTotal() > MAX_TOTAL_MS * 6 / 10) seqLen--;

  unsigned long on = onTotal(), off = offTotal();
  if (off > 0 && on + off < MIN_TOTAL_MS) {
    scaleOff((float)(MIN_TOTAL_MS - on) / off);
  } else if (off > 0 && on + off > MAX_TOTAL_MS) {
    float room = (on < MAX_TOTAL_MS) ? (float)(MAX_TOTAL_MS - on) : 0.0f;
    scaleOff(room / off);
  }

  // Pauses hit their cap and it is still short: hold the final silence.
  unsigned long total = onTotal() + offTotal();
  if (total < MIN_TOTAL_MS) seq[seqLen - 1].offMs += (uint16_t)(MIN_TOTAL_MS - total);
}

void addPulse(int solenoid, int onMs, int offMs) {
  if (seqLen >= MAX_PULSES) return;
  seq[seqLen++] = { (uint8_t)solenoid,
                    (uint16_t)constrain(onMs, MIN_ON_MS, MAX_ON_MS),
                    (uint16_t)constrain(offMs, 0, MAX_OFF_MS) };
}

// Used when the model can't answer: same clause structure as the prompt,
// seeded from the question so different questions sound different.
void generateFallback(const String& q) {
  uint32_t h = 2166136261u;
  for (unsigned int i = 0; i < q.length(); i++) { h ^= (uint8_t)q[i]; h *= 16777619u; }
  randomSeed(h ^ questionCount);

  seqLen = 0;
  int pos = random(NUM_SOLENOIDS);
  int dir = random(2) ? 1 : -1;
  addPulse(pos, random(300, 501), random(300, 601));

  int clauses = random(5, 8);
  for (int c = 0; c < clauses; c++) {
    int n = random(6, 10);
    int baseOn = random(80, 260);
    for (int i = 0; i < n; i++) {
      pos += dir;
      if (pos < 0 || pos >= NUM_SOLENOIDS) { dir = -dir; pos += 2 * dir; }
      pos = constrain(pos, 0, NUM_SOLENOIDS - 1);
      bool lastInClause = (i == n - 1);
      addPulse(pos, baseOn + random(-40, 41),
               lastInClause ? random(400, 801) : random(80, 251));
    }
    if (random(3) == 0) dir = -dir;
  }
  addPulse(pos, random(300, 501), 0);
}

// ───────────────────────── Gemini ─────────────────────────

void buildSystemPrompt() {
  String last = String(NUM_SOLENOIDS - 1);
  systemPrompt =
    "You are a solenoid oracle in an art installation. Visitors type questions; "
    "you answer only in rhythm. " + String(NUM_SOLENOIDS) +
    " solenoids sit in a row, indexed 0-" + last + ", firing one at a time. "
    "Output ONLY a JSON array of objects "
    "{\"solenoid\":0-" + last + ",\"duration_ms\":60-500,\"delay_ms\":80-800}. "
    "The whole sequence (sum of all duration_ms + delay_ms) MUST last 10000-16000 ms. "
    "Each clause, including the gap after it, should last roughly 1800-2800 ms. "
    "36-50 pulses, grouped into 5-7 clauses of 6-9 pulses. "
    "Within a clause: delay_ms 80-250, moving across the row; "
    "a clause may turn back once or repeat a solenoid for emphasis. "
    "Between clauses: one delay_ms 400-800 gap. "
    "Start and end on one slow pulse (duration_ms 300-500). "
    "Sad/heavy: longer duration_ms, wider delays, fewer clauses. "
    "Light/joyful: short duration_ms, tight delays, more clauses.";
}

String buildUserText(const String& q) {
  String t;
  if (historyCount > 0) {
    t = "Earlier questions from visitors, oldest first: ";
    for (int i = 0; i < historyCount; i++) t += "\"" + history[i] + "\" ";
    t += ". Make this answer feel distinct from those. ";
  }
  t += "Question: \"" + q + "\"";
  return t;
}

void addHistory(const String& q) {
  String item = q.length() > MAX_HISTORY_ITEM_LEN ? q.substring(0, MAX_HISTORY_ITEM_LEN) : q;
  if (historyCount < HISTORY_SIZE) {
    history[historyCount++] = item;
  } else {
    for (int i = 1; i < HISTORY_SIZE; i++) history[i - 1] = history[i];
    history[HISTORY_SIZE - 1] = item;
  }
}

// Returns the HTTP code (negative = connection problem); text goes to outText.
int askGemini(const String& question, String& outText) {
  outText = "";

  String body;
  {
    String userText = buildUserText(question);
    DynamicJsonDocument req(2048 + 2 * (systemPrompt.length() + userText.length()));
    req["systemInstruction"]["parts"][0]["text"] = systemPrompt;
    req["contents"][0]["role"] = "user";
    req["contents"][0]["parts"][0]["text"] = userText;
    req["generationConfig"]["responseMimeType"] = "application/json";
    req["generationConfig"]["temperature"] = 1.0;
    req["generationConfig"]["maxOutputTokens"] = 2048;
    // Gemini 3.x models "think" before answering, which can cost 20+ s.
    // For 2.5 models use ["thinkingBudget"] = 0 instead.
    req["generationConfig"]["thinkingConfig"]["thinkingLevel"] = "minimal";
    serializeJson(req, body);
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://generativelanguage.googleapis.com/v1beta/models/") +
               MODEL + ":generateContent?key=" + API_KEY;

  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed");
    return -1;
  }
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(10000);
  http.setTimeout(HTTP_TIMEOUT_MS);

  unsigned long t0 = millis();
  int code = http.POST(body);
  body = "";
  Serial.printf("API response code: %d (%.1f s)\n", code, (millis() - t0) / 1000.0);

  if (code == 200) {
    String raw = http.getString();
    StaticJsonDocument<128> filter;
    filter["candidates"][0]["content"]["parts"][0]["text"] = true;
    DynamicJsonDocument doc(raw.length() + 1024);
    DeserializationError err =
      deserializeJson(doc, raw, DeserializationOption::Filter(filter));
    if (err) {
      Serial.println("Response parse error: " + String(err.c_str()));
    } else {
      for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
        outText += (const char*)(part["text"] | "");
      }
    }
  } else if (code > 0) {
    String err = http.getString();
    if (code == 429) Serial.println("Rate limited by Gemini.");
    Serial.println("API error: " + err.substring(0, 300));
  } else {
    Serial.println("HTTP error: " + http.errorToString(code));
  }

  http.end();
  return code;
}

// Read the model's JSON into seq[]. Returns false if unusable.
bool parsePulses(String text) {
  int a = text.indexOf('[');
  int b = text.lastIndexOf(']');
  if (a < 0 || b <= a) return false;
  text = text.substring(a, b + 1);

  DynamicJsonDocument doc(2 * text.length() + 4096);
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    Serial.println("Pulse JSON error: " + String(err.c_str()));
    return false;
  }

  seqLen = 0;
  for (JsonObject p : doc.as<JsonArray>()) {
    int s = p["solenoid"] | -1;
    if (s < 0 || s >= NUM_SOLENOIDS) continue;
    addPulse(s, p["duration_ms"] | 150, p["delay_ms"] | 200);
  }
  return seqLen >= 6;
}

// ───────────────────────── Wi-Fi ─────────────────────────

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("WiFi lost, reconnecting");
  unsigned long t = millis();
  while (wifiMulti.run(5000) != WL_CONNECTED && millis() - t < 15000) Serial.print(".");
  Serial.println();
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) Serial.println("WiFi back: " + WiFi.SSID());
  else    Serial.println("WiFi still down");
  return ok;
}

// ───────────────────────── main flow ─────────────────────────

void handleQuestion(String q) {
  Serial.println("--------------------------------------------");
  Serial.println("You: " + q);
  Serial.println("Thinking...");

  if (millis() - lastQuestionAt > HISTORY_RESET_MS) historyCount = 0;

  bool ok = false;
  if (ensureWifi()) {
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      String text;
      int code = askGemini(q, text);
      if (code == 200) {
        ok = parsePulses(text);
        if (!ok) Serial.println("Model answer unusable.");
      }
      // Retry only quick failures; a timeout already cost the visitor 35 s.
      bool retry = (code == 200 && !ok) || code >= 500 ||
                   (code < 0 && code != HTTPC_ERROR_READ_TIMEOUT);
      if (ok || !retry) break;
      delay(800);
    }
  }

  if (!ok) {
    Serial.println("Fallback: playing a locally generated sequence.");
    generateFallback(q);
  }
  normalizeTiming();

  Serial.printf("Sequence: %d pulses, %.1f s%s\n", seqLen,
                (onTotal() + offTotal()) / 1000.0, ok ? "" : " (fallback)");
  Serial.println("Playing...");
  playSequence();

  addHistory(q);
  lastQuestionAt = millis();
  questionCount++;

  Serial.printf("Done. Questions so far: %lu | free heap %lu | largest block %lu\n",
                questionCount, (unsigned long)ESP.getFreeHeap(),
                (unsigned long)ESP.getMaxAllocHeap());
  Serial.println("--------------------------------------------");

  if (ESP.getMaxAllocHeap() < MIN_CONTIGUOUS_HEAP) {
    Serial.println("Memory fragmented, restarting...");
    delay(200);
    ESP.restart();   // setup() prints "Waiting for input..." again
  }
  Serial.println("Waiting for input...");
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(1000);

  for (int i = 0; i < NUM_SOLENOIDS; i++) {
    pinMode(SOLENOID_PINS[i], OUTPUT);
    digitalWrite(SOLENOID_PINS[i], LOW);
  }

  buildSystemPrompt();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  for (int i = 0; i < NUM_NETWORKS; i++) wifiMulti.addAP(WIFI_NETWORKS[i][0], WIFI_NETWORKS[i][1]);

  Serial.print("Connecting to WiFi");
  unsigned long t = millis();
  while (wifiMulti.run(5000) != WL_CONNECTED && millis() - t < 30000) Serial.print(".");
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected to " + WiFi.SSID() + ", IP " + WiFi.localIP().toString());
  } else {
    Serial.println("No WiFi yet: answering with fallback sequences until it connects.");
  }

  Serial.println("============================================");
  Serial.println("PianoAutomata - solenoid oracle");
  Serial.printf("%d solenoid(s) on GPIO: ", NUM_SOLENOIDS);
  for (int i = 0; i < NUM_SOLENOIDS; i++) {
    Serial.print(SOLENOID_PINS[i]);
    if (i < NUM_SOLENOIDS - 1) Serial.print(", ");
  }
  Serial.println();
  Serial.println("============================================");
  Serial.println("Waiting for input...");
}

void loop() {
  if (Serial.available()) {
    String q = Serial.readStringUntil('\n');
    q.trim();
    if (q.length() > 0) {
      if (q.length() > MAX_QUESTION_BYTES) q = q.substring(0, MAX_QUESTION_BYTES);
      handleQuestion(q);
      while (Serial.available()) Serial.read();  // drop anything sent while playing
    }
  }
  delay(10);
}
