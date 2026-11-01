// ============================================================
//  BLOOP - a TFT desk buddy (Display Week)
//  ESP32 DevKit + 3.5" ILI9488 SPI TFT (480x320, landscape)
//
//  Bloop blinks, looks around, reacts to his 5 buttons, sleeps
//  when ignored... and is useful: clock, Pomodoro study timer,
//  weather + air quality, phone message board, study streak,
//  countdown, water reminders and a decision oracle.
//
//  Screen wiring follows the Display Week guide's ESP32 table:
//    SCK=18  MOSI=23  CS=5  DC=16  RST=17  BL=4 (PWM dimming)
//    VCC=3V3  GND=GND   (3.3V logic only - never 5V data!)
//  Buttons (to GND, internal pull-ups):
//    LEFT=13  PLAY=14  RIGHT=27  UP=26  DOWN=25
//
//  Libraries: TFT_eSPI (with the supplied User_Setup.h),
//             ArduinoJson. Everything else is in the ESP32 core.
// ============================================================

#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

// ===================== CONFIG - EDIT THESE =====================
const char* WIFI_SSID = "YOUR_WIFI_NAME";     // your home WiFi
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
// Your city for weather + AQI (Google "my coordinates").
// Leave both 0 to switch weather off.
const float CITY_LAT = 0.0;
const float CITY_LON = 0.0;
// Countdown target (set YOUR date + label):
const int   CD_YEAR = 2026, CD_MONTH = 12, CD_DAY = 31;
const char* CD_LABEL = "NEW YEAR";
const int   POMO_MINUTES = 25;                // focus session length
// ===============================================================

#define BTN_POWER   13   // button 1: Bloop on/off
#define BTN_PLAY    14   // button 2: Pomodoro (short) / study streak (long)
#define BTN_WEATHER 27   // button 3: weather + AQI card
#define BTN_ORACLE  26   // button 4: decision oracle
#define BTN_GAMES   25   // button 5: games (Flappy Bird)

TFT_eSPI tft = TFT_eSPI();
WebServer server(80);
Preferences prefs;

// Colours (set in setup via color565)
uint16_t C_BG, C_INK, C_CHEEK, C_TEAL, C_SUN, C_CARD, C_NIGHT, C_WHITE;

// ---------------- Face state ----------------
enum Mood { M_NORMAL, M_HAPPY, M_LOVE, M_THINK, M_FOCUS };
Mood mood = M_NORMAL;
unsigned long moodUntil = 0;
int lookX = 0, lookY = 0;                      // pupil offset target
unsigned long nextBlink = 0, blinkOffAt = 0;
unsigned long nextLook = 0;
bool eyesClosed = false;
unsigned long lastInteract = 0;
bool sleeping = false;
int lastClockMin = -1;

// ---------------- Speech bubble ----------------
String bubbleText = "";
unsigned long bubbleUntil = 0;

// ---------------- Pomodoro ----------------
bool pomoRunning = false;
unsigned long pomoEnd = 0, pomoTotal = 0;
int lastPomoSec = -1;

// ---------------- Weather / AQI ----------------
bool wifiOn = false;
bool weatherOK = false;
float wTemp = 0; int wCode = -1; int aqiVal = -1;
unsigned long nextWeatherFetch = 0;
unsigned long nextWifiRetry = 0;

// ---------------- Streak ----------------
int streakCount = 0;
String streakLastDay = "";

// ---------------- Info rotation ----------------
int infoSlot = 0;
unsigned long nextInfoSwap = 0;

// ---------------- Misc timers ----------------
unsigned long nextWater = 3600000UL;           // first reminder after 1 h

// ============================================================
String nowTimeStr(bool withSec = false) {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--";
  char buf[12];
  if (withSec) strftime(buf, sizeof(buf), "%H:%M:%S", &t);
  else         strftime(buf, sizeof(buf), "%H:%M", &t);
  return String(buf);
}

String todayStr(int offsetDays = 0) {
  time_t now = time(nullptr) + (time_t)offsetDays * 86400;
  struct tm t; localtime_r(&now, &t);
  char buf[12];
  strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
  return String(buf);
}

long daysToCountdown() {
  struct tm target = {0};
  target.tm_year = CD_YEAR - 1900;
  target.tm_mon  = CD_MONTH - 1;
  target.tm_mday = CD_DAY;
  target.tm_isdst = -1;
  time_t tt = mktime(&target);
  time_t now = time(nullptr);
  if (tt < 0 || now < 0) return -1;
  return (long)((tt - now) / 86400) + 1;
}

String codeToText(int c) {
  if (c == 0) return "Clear sky";
  if (c == 1) return "Mostly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Foggy";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c >= 95) return "Thunderstorm";
  return "Weather";
}

void setBubble(const String& text, unsigned long ms) {
  bubbleText = text;
  bubbleUntil = millis() + ms;
}

// ============================================================
//  FACE DRAWING - only redraws the face region, never the
//  whole screen (that's what keeps the big screen smooth).
// ============================================================
void drawHeart(int x, int y, int s, uint16_t col) {
  tft.fillCircle(x - s / 2, y, s / 2, col);
  tft.fillCircle(x + s / 2, y, s / 2, col);
  tft.fillTriangle(x - s, y + 1, x + s, y + 1, x, y + s + 2, col);
}

void drawSmile(int cx, int cy, int r, bool big) {
  // arc of small segments, opening upward (a smile)
  int steps = 14;
  float a0 = big ? 0.25f : 0.45f, a1 = PI - a0;
  for (int i = 0; i < steps; i++) {
    float t1 = a0 + (a1 - a0) * i / steps;
    float t2 = a0 + (a1 - a0) * (i + 1) / steps;
    int x1 = cx + (int)(cos(t1) * r),        y1 = cy + (int)(sin(t1) * r * 0.62f);
    int x2 = cx + (int)(cos(t2) * r),        y2 = cy + (int)(sin(t2) * r * 0.62f);
    tft.drawLine(x1, y1, x2, y2, C_INK);
    tft.drawLine(x1, y1 + 1, x2, y2 + 1, C_INK);
  }
}

void drawFace() {
  const int CX = 240, CY = 172;               // face centre
  int bob = (millis() / 900) % 2 == 0 ? 0 : 3; // gentle idle bob
  if (mood == M_LOVE) bob = ((millis() / 160) % 2 == 0) ? -6 : 2; // excited bounce
  int cy = CY + bob;

  tft.fillRect(0, 38, 480, 282, C_BG);         // face region only

  int eyeY = cy - 28, eyeDX = 62;
  int eyeW = 40, eyeH = 52;
  int px = lookX * 9, py = lookY * 6;

  if (mood == M_HAPPY || mood == M_LOVE) {
    // happy ^^ eyes = two arcs
    for (int s = -1; s <= 1; s += 2) {
      int ex = CX + s * eyeDX;
      for (int i = 0; i < 12; i++) {
        float t1 = PI + (PI * i) / 12, t2 = PI + (PI * (i + 1)) / 12;
        tft.drawLine(ex + (int)(cos(t1) * 20), eyeY + (int)(sin(t1) * 16),
                     ex + (int)(cos(t2) * 20), eyeY + (int)(sin(t2) * 16), C_INK);
        tft.drawLine(ex + (int)(cos(t1) * 20), eyeY + 1 + (int)(sin(t1) * 16),
                     ex + (int)(cos(t2) * 20), eyeY + 1 + (int)(sin(t2) * 16), C_INK);
      }
    }
  } else if (eyesClosed) {
    tft.fillRoundRect(CX - eyeDX - eyeW / 2, eyeY - 2, eyeW, 6, 3, C_INK);
    tft.fillRoundRect(CX + eyeDX - eyeW / 2, eyeY - 2, eyeW, 6, 3, C_INK);
  } else {
    for (int s = -1; s <= 1; s += 2) {
      int ex = CX + s * eyeDX;
      tft.fillRoundRect(ex - eyeW / 2, eyeY - eyeH / 2, eyeW, eyeH, 14, C_WHITE);
      tft.fillCircle(ex + px, eyeY + py, 11, C_INK);
      tft.fillCircle(ex + px + 4, eyeY + py - 4, 4, C_WHITE);  // sparkle
    }
    if (mood == M_FOCUS) {  // determined brows
      tft.drawLine(CX - eyeDX - 20, eyeY - eyeH / 2 - 10, CX - eyeDX + 20, eyeY - eyeH / 2 - 3, C_INK);
      tft.drawLine(CX + eyeDX - 20, eyeY - eyeH / 2 - 3, CX + eyeDX + 20, eyeY - eyeH / 2 - 10, C_INK);
    }
  }

  // cheeks
  tft.fillCircle(CX - 108, cy + 18, 13, C_CHEEK);
  tft.fillCircle(CX + 108, cy + 18, 13, C_CHEEK);

  // mouth
  int my = cy + 34;
  if (mood == M_LOVE || mood == M_HAPPY) drawSmile(CX, my - 8, 30, true);
  else if (mood == M_THINK) tft.fillCircle(CX + 16, my + 4, 7, C_INK);  // "hmm" o, off-centre
  else if (mood == M_FOCUS) tft.fillRoundRect(CX - 14, my + 2, 28, 5, 2, C_INK);
  else drawSmile(CX, my - 6, 22, false);

  if (mood == M_LOVE) {  // floating hearts
    drawHeart(CX - 150, cy - 70, 12, C_CHEEK);
    drawHeart(CX + 150, cy - 84, 15, C_CHEEK);
    drawHeart(CX + 128, cy - 40, 9, C_SUN);
  }
}

// -------- Top strip: clock left, rotating info right --------
void drawTopStrip() {
  tft.fillRect(0, 0, 480, 36, C_TEAL);
  tft.setTextColor(C_WHITE, C_TEAL);
  tft.setTextFont(4);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(nowTimeStr(), 10, 6, 4);

  String info = "";
  if (infoSlot == 0 && weatherOK)      info = String(wTemp, 0) + "C " + codeToText(wCode);
  else if (infoSlot == 1 && aqiVal > 0) info = "AQI " + String(aqiVal);
  else if (infoSlot == 2) { long d = daysToCountdown(); info = (d >= 0) ? String(CD_LABEL) + " " + String(d) + "d" : String(""); }
  else if (infoSlot == 3) info = "Streak " + String(streakCount);
  else { struct tm t; if (getLocalTime(&t)) { char b[20]; strftime(b, sizeof(b), "%a %d %b", &t); info = String(b); } }
  tft.setTextDatum(TR_DATUM);
  tft.setTextFont(2);
  tft.drawString(info, 470, 10, 2);
  tft.setTextDatum(TL_DATUM);
}

// -------- Speech bubble (over the top of the face) --------
void drawBubble() {
  if (bubbleText.length() == 0) return;
  tft.fillRoundRect(90, 46, 300, 74, 12, C_WHITE);
  tft.drawRoundRect(90, 46, 300, 74, 12, C_INK);
  tft.fillTriangle(210, 120, 236, 120, 218, 138, C_WHITE);
  tft.setTextColor(C_INK, C_WHITE);
  tft.setTextFont(2);
  // simple word wrap, max 2 lines of ~30 chars
  String l1 = bubbleText, l2 = "";
  if (bubbleText.length() > 30) {
    int cut = bubbleText.lastIndexOf(' ', 30);
    if (cut < 5) cut = 30;
    l1 = bubbleText.substring(0, cut);
    l2 = bubbleText.substring(cut + 1);
    if (l2.length() > 30) l2 = l2.substring(0, 30);
  }
  tft.setTextDatum(TC_DATUM);
  tft.drawString(l1, 240, l2.length() ? 58 : 72, 2);
  if (l2.length()) tft.drawString(l2, 240, 82, 2);
  tft.setTextDatum(TL_DATUM);
}

// -------- Pomodoro overlay (bottom of the face region) --------
void drawPomo() {
  unsigned long remain = (pomoEnd > millis()) ? (pomoEnd - millis()) : 0;
  int sec = remain / 1000;
  tft.fillRect(0, 282, 480, 38, C_BG);
  // progress bar
  int w = 0;
  if (pomoTotal > 0) w = (int)(380.0f * (1.0f - (float)remain / (float)pomoTotal));
  tft.drawRoundRect(50, 300, 380, 10, 5, C_INK);
  if (w > 4) tft.fillRoundRect(50, 300, w, 10, 5, C_TEAL);
  tft.setTextColor(C_INK, C_BG);
  tft.setTextFont(4);
  tft.setTextDatum(TC_DATUM);
  char buf[8]; snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
  tft.drawString(buf, 240, 284, 4);
  tft.setTextDatum(TL_DATUM);
}

void drawSleepClock() {
  tft.fillScreen(C_NIGHT);
  tft.setTextColor(C_WHITE, C_NIGHT);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(nowTimeStr(), 240, 96, 7);           // big digits
  struct tm t; if (getLocalTime(&t)) { char b[24]; strftime(b, sizeof(b), "%a %d %b", &t); tft.drawString(b, 240, 190, 4); }
  // tiny sleeping face: two closed eyes + Zzz
  tft.fillRoundRect(196, 248, 34, 5, 2, C_WHITE);
  tft.fillRoundRect(250, 248, 34, 5, 2, C_WHITE);
  tft.drawString("Z", 306, 226, 2);
  tft.drawString("Z", 322, 210, 4);
  tft.setTextDatum(TL_DATUM);
}

void drawWeatherCard() {
  tft.fillRect(0, 38, 480, 282, C_CARD);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_INK, C_CARD);
  if (weatherOK) {
    tft.drawString(String(wTemp, 0) + " C", 240, 70, 7);
    tft.drawString(codeToText(wCode), 240, 170, 4);
    if (aqiVal > 0) tft.drawString("Air quality (US AQI): " + String(aqiVal), 240, 205, 2);
  } else {
    tft.drawString("No weather yet", 240, 120, 4);
    tft.drawString(wifiOn ? "Set CITY_LAT / CITY_LON in the code" : "WiFi not connected", 240, 160, 2);
  }
  tft.drawString("My address: " + (wifiOn ? WiFi.localIP().toString() : String("-")), 240, 262, 2);
  tft.drawString("Open it in a phone browser to leave me a message!", 240, 284, 2);
  tft.setTextDatum(TL_DATUM);
}

// ============================================================
//  WEATHER + AQI (Open-Meteo, free, no API key)
// ============================================================
bool fetchJSON(const String& url, JsonDocument& doc) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  bool ok = deserializeJson(doc, http.getString()) == DeserializationError::Ok;
  http.end();
  return ok;
}

void fetchWeather() {
  if (!wifiOn || CITY_LAT == 0.0f) return;
  JsonDocument doc;
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(CITY_LAT, 4) +
               "&longitude=" + String(CITY_LON, 4) + "&current=temperature_2m,weather_code&timezone=auto";
  if (fetchJSON(url, doc)) {
    wTemp = doc["current"]["temperature_2m"].as<float>();
    wCode = doc["current"]["weather_code"].as<int>();
    weatherOK = true;
  }
  JsonDocument doc2;
  String url2 = "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=" + String(CITY_LAT, 4) +
                "&longitude=" + String(CITY_LON, 4) + "&current=us_aqi";
  if (fetchJSON(url2, doc2)) aqiVal = doc2["current"]["us_aqi"].as<int>();
}

// ============================================================
//  PHONE MESSAGE BOARD
// ============================================================
void handleRoot() {
  server.send(200, "text/html",
    "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Bloop</title></head><body style='font-family:sans-serif;text-align:center;background:#FAF3E1'>"
    "<h2>Leave Bloop a message</h2>"
    "<form action='/msg' method='POST'>"
    "<input name='m' maxlength='55' style='font-size:18px;padding:8px;width:80%'><br><br>"
    "<button style='font-size:18px;padding:10px 24px'>Send</button></form></body></html>");
}

void handleMsg() {
  String m = server.arg("m");
  m.trim();
  if (m.length()) {
    setBubble(m, 30000);
    mood = M_HAPPY; moodUntil = millis() + 4000;
    lastInteract = millis();
    drawFace(); drawBubble();
  }
  server.send(200, "text/html", "<html><body style='font-family:sans-serif;text-align:center'><h3>Bloop got it!</h3></body></html>");
}

// ============================================================
//  BUTTONS
// ============================================================
bool btnPressed(int pin) { return digitalRead(pin) == LOW; }
unsigned long playDownAt = 0; bool playWasDown = false;

void markStudied() {
  String today = todayStr();
  if (streakLastDay == today) { setBubble("Already counted today. Proud of you!", 3500); return; }
  streakCount = (streakLastDay == todayStr(-1)) ? streakCount + 1 : 1;
  streakLastDay = today;
  prefs.begin("bloop", false);
  prefs.putInt("streak", streakCount);
  prefs.putString("sday", streakLastDay);
  prefs.end();
  mood = M_LOVE; moodUntil = millis() + 5000;
  setBubble("Study streak: " + String(streakCount) + " days!", 4500);
}

// ============================================================
//  FLAPPY BIRD (button 5) - PLAY / WEATHER / ORACLE flap,
//  button 5 exits, best score saved in Preferences.
// ============================================================
bool screenOff = false;
bool gameActive = false;
const int FL_TOP = 38, FL_GROUND = 300, FL_BIRD_X = 90, FL_BIRD_R = 11;
float flBirdY = 170, flVel = 0;
int flScore = 0, flBest = 0, flState = 0;      // 0 title, 1 playing, 2 game over
struct FlPipe { int x; int gapC; bool scored; };
FlPipe flPipes[3];
unsigned long flLastFrame = 0;

void flappyDrawField() {
  tft.fillRect(0, FL_TOP, 480, 320 - FL_TOP, C_CARD);
  tft.fillRect(0, FL_GROUND, 480, 320 - FL_GROUND, C_TEAL);
}
void flappyDrawPipes() {
  for (int i = 0; i < 3; i++) {
    FlPipe &p = flPipes[i];
    if (p.x < -60 || p.x > 484) continue;
    int gT = p.gapC - 52, gB = p.gapC + 52;
    tft.fillRect(p.x + 52, FL_TOP, 5, FL_GROUND - FL_TOP, C_CARD);       // erase trail
    tft.fillRect(p.x, FL_TOP, 52, gT - FL_TOP, C_TEAL);
    tft.fillRect(p.x, gT, 52, 8, C_INK);
    tft.fillRect(p.x, gB - 8, 52, 8, C_INK);
    tft.fillRect(p.x, gB, 52, FL_GROUND - gB, C_TEAL);
  }
}
void flappyDrawBird(int yOld, int yNew) {
  if (yOld >= 0) tft.fillCircle(FL_BIRD_X, yOld, FL_BIRD_R + 1, C_CARD);
  tft.fillCircle(FL_BIRD_X, yNew, FL_BIRD_R, C_SUN);
  tft.fillCircle(FL_BIRD_X + 4, yNew - 3, 2, C_INK);
  tft.fillTriangle(FL_BIRD_X + 9, yNew, FL_BIRD_X + 15, yNew + 2, FL_BIRD_X + 9, yNew + 4, C_INK);
}
void flappyScore() {
  tft.fillRect(200, FL_TOP + 2, 80, 30, C_CARD);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_INK, C_CARD);
  tft.drawString(String(flScore), 240, FL_TOP + 4, 4);
  tft.setTextDatum(TL_DATUM);
}
void flappyBeginRun() {
  flBirdY = 170; flVel = 0; flScore = 0;
  for (int i = 0; i < 3; i++) { flPipes[i].x = 500 + i * 200; flPipes[i].gapC = random(110, 240); flPipes[i].scored = false; }
  flappyDrawField();
  flState = 1;
  flappyDrawBird(-1, (int)flBirdY);
  flappyScore();
}
void flappyOver() {
  flState = 2;
  if (flScore > flBest) {
    flBest = flScore;
    prefs.begin("bloop", false); prefs.putInt("flap", flBest); prefs.end();
  }
  tft.fillRoundRect(90, 110, 300, 120, 10, C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_INK, C_BG);
  tft.drawString("GAME OVER", 240, 126, 4);
  tft.drawString("Score: " + String(flScore), 240, 160, 4);
  tft.drawString("Best: " + String(flBest), 240, 186, 2);
  tft.drawString("PLAY = retry    GAMES = exit", 240, 208, 2);
  tft.setTextDatum(TL_DATUM);
}
void startFlappy() {
  lastInteract = millis();
  if (sleeping) { sleeping = false; ledcWrite(0, 255); drawFace(); drawTopStrip(); return; }
  if (pomoRunning) { setBubble("Finish the Pomodoro first!", 2500); return; }
  gameActive = true; flState = 0;
  flappyDrawField();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(C_INK, C_CARD);
  tft.drawString("FLAPPY BIRD", 240, 120, 6);
  tft.drawString("PLAY = flap", 240, 180, 4);
  tft.drawString("Best: " + String(flBest), 240, 214, 2);
  tft.setTextDatum(TL_DATUM);
  flappyDrawBird(-1, 170);
}
void flappyFlap() {
  lastInteract = millis();
  if (flState == 0) { flappyBeginRun(); flVel = -7.6; }
  else if (flState == 1) flVel = -7.6;
  else if (flState == 2) flappyBeginRun();
}
void flappyTick() {
  if (flState != 1) return;
  unsigned long now = millis();
  if (now - flLastFrame < 33) return;
  flLastFrame = now;
  int oldY = (int)flBirdY;
  flVel = min(flVel + 0.45f, 9.0f);
  flBirdY += flVel;
  if (flBirdY < FL_TOP + FL_BIRD_R) { flBirdY = FL_TOP + FL_BIRD_R; flVel = 0; }
  for (int i = 0; i < 3; i++) {
    FlPipe &p = flPipes[i];
    p.x -= 4;
    if (p.x < -56) { p.x += 600; p.gapC = random(110, 240); p.scored = false; }
    if (!p.scored && p.x + 52 < FL_BIRD_X) { p.scored = true; flScore++; flappyScore(); }
    if (FL_BIRD_X + FL_BIRD_R > p.x && FL_BIRD_X - FL_BIRD_R < p.x + 52 &&
        (flBirdY - FL_BIRD_R < p.gapC - 52 || flBirdY + FL_BIRD_R > p.gapC + 52)) { flappyOver(); return; }
  }
  if (flBirdY + FL_BIRD_R >= FL_GROUND) { flappyOver(); return; }
  flappyDrawPipes();
  flappyDrawBird(oldY, (int)flBirdY);
  flappyScore();
}

void oracle() {
  mood = M_THINK; moodUntil = millis() + 900;
  drawFace();
  delay(900);
  const char* answers[] = {"YES. Go for it.", "No. Not today.", "Maybe... ask again.",
                           "Definitely yes.", "Hmm, risky. Wait.", "The stars say YES."};
  setBubble(answers[random(0, 6)], 4000);
  mood = M_NORMAL;
  drawFace(); drawBubble();
}

void onButton(int pin) {
  lastInteract = millis();
  if (sleeping) { sleeping = false; ledcWrite(0, 255); drawFace(); drawTopStrip(); return; }
  if (bubbleUntil > millis() && pin != BTN_PLAY) { bubbleUntil = 0; bubbleText = ""; drawFace(); return; } // tap clears reminders
  switch (pin) {
    case BTN_WEATHER: drawWeatherCard(); delay(6000); drawFace(); drawTopStrip(); break;
    case BTN_ORACLE:  oracle(); break;
  }
}

// ============================================================
void setup() {
  pinMode(BTN_POWER, INPUT_PULLUP);
  pinMode(BTN_PLAY, INPUT_PULLUP);
  pinMode(BTN_WEATHER, INPUT_PULLUP);
  pinMode(BTN_ORACLE, INPUT_PULLUP);
  pinMode(BTN_GAMES, INPUT_PULLUP);
  randomSeed(esp_random());

  C_BG = tft.color565(250, 243, 225);
  C_INK = tft.color565(43, 49, 60);
  C_CHEEK = tft.color565(255, 158, 158);
  C_TEAL = tft.color565(46, 166, 154);
  C_SUN = tft.color565(255, 183, 77);
  C_CARD = tft.color565(227, 242, 253);
  C_NIGHT = tft.color565(26, 30, 44);
  C_WHITE = TFT_WHITE;

  tft.init();
  tft.setRotation(1);                    // landscape 480x320
  tft.fillScreen(C_BG);
  ledcSetup(0, 5000, 8);                 // backlight PWM on GPIO4
  ledcAttachPin(4, 0);
  ledcWrite(0, 255);

  // boot splash
  tft.setTextColor(C_INK, C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("BLOOP", 240, 120, 6);
  tft.drawString("waking up...", 240, 180, 2);
  tft.setTextDatum(TL_DATUM);

  prefs.begin("bloop", true);
  streakCount = prefs.getInt("streak", 0);
  streakLastDay = prefs.getString("sday", "");
  flBest = prefs.getInt("flap", 0);
  prefs.end();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  drawFace(); drawTopStrip();
  lastInteract = millis();
  nextBlink = millis() + 2500;
  nextLook = millis() + 3000;
  nextInfoSwap = millis() + 8000;
  nextWater = millis() + 3600000UL;
}

void loop() {
  // ---- POWER button (button 1): Bloop on/off, works everywhere ----
  static bool pwrWas = false;
  bool pwr = btnPressed(BTN_POWER);
  if (pwr && !pwrWas) {
    if (!screenOff) {
      screenOff = true; gameActive = false; sleeping = false;
      ledcWrite(0, 0); tft.fillScreen(TFT_BLACK);
    } else {
      screenOff = false; ledcWrite(0, 255);
      lastInteract = millis(); drawFace(); drawTopStrip();
    }
  }
  pwrWas = pwr;
  if (screenOff) { delay(30); return; }

  server.handleClient();
  unsigned long now = millis();

  // ---- WiFi upkeep (non-blocking) ----
  if (WiFi.status() == WL_CONNECTED && !wifiOn) {
    wifiOn = true;
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", "IST-5:30", 1); tzset();          // India time - change if needed
    server.on("/", handleRoot);
    server.on("/msg", handleMsg);
    server.begin();
    nextWeatherFetch = now + 5000;
    setBubble("WiFi! I am online.", 3000);
  }
  if (WiFi.status() != WL_CONNECTED) {
    wifiOn = false;
    if (now > nextWifiRetry) { nextWifiRetry = now + 30000; WiFi.begin(WIFI_SSID, WIFI_PASS); }
  }
  if (wifiOn && now > nextWeatherFetch) { nextWeatherFetch = now + 1800000UL; fetchWeather(); drawTopStrip(); }

  // ---- PLAY button: short press = Pomodoro, long press = streak ----
  bool playDown = btnPressed(BTN_PLAY);
  if (playDown && !playWasDown) { playDownAt = now; if (gameActive) flappyFlap(); }
  if (!playDown && playWasDown) {
    unsigned long held = now - playDownAt;
    lastInteract = now;
    if (gameActive) { /* release does nothing in game */ }
    else if (sleeping) { sleeping = false; ledcWrite(0, 255); drawFace(); drawTopStrip(); }
    else if (held >= 1500) markStudied();
    else if (!pomoRunning) {
      pomoRunning = true;
      pomoTotal = (unsigned long)POMO_MINUTES * 60000UL;
      pomoEnd = now + pomoTotal;
      mood = M_FOCUS; lastPomoSec = -1;
      setBubble("Focus time. I am watching.", 3000);
    } else { pomoRunning = false; mood = M_NORMAL; setBubble("Pomodoro stopped.", 2500); }
    drawFace();
  }
  playWasDown = playDown;

  // ---- other buttons (edge detect) ----
  static bool pW = false, pO = false, pG = false;
  bool cW = btnPressed(BTN_WEATHER), cO = btnPressed(BTN_ORACLE), cG = btnPressed(BTN_GAMES);
  if (gameActive) {
    if ((cW && !pW) || (cO && !pO)) flappyFlap();
    if (cG && !pG) { gameActive = false; lastInteract = now; drawFace(); drawTopStrip(); }
  } else {
    if (cW && !pW) onButton(BTN_WEATHER);
    if (cO && !pO) onButton(BTN_ORACLE);
    if (cG && !pG) startFlappy();
  }
  pW = cW; pO = cO; pG = cG;

  // ---- game frame (skips face/idle behaviour while playing) ----
  if (gameActive) { flappyTick(); delay(10); return; }

  // ---- Pomodoro tick ----
  if (pomoRunning) {
    int sec = (pomoEnd > now) ? (int)((pomoEnd - now) / 1000) : 0;
    if (sec != lastPomoSec) { lastPomoSec = sec; drawPomo(); }
    if ((long)(now - pomoEnd) >= 0) {
      pomoRunning = false; mood = M_LOVE; moodUntil = now + 6000;
      setBubble("DONE! Take a break, legend.", 6000);
      drawFace(); drawBubble();
      tft.fillRect(0, 282, 480, 38, C_BG);
    }
  }

  // ---- Sleep ----
  if (!sleeping && !pomoRunning && now - lastInteract > 90000) {
    sleeping = true;
    ledcWrite(0, 60);                       // dim for the night
    lastClockMin = -1;
    drawSleepClock();
  }
  if (sleeping) {
    struct tm t;
    if (getLocalTime(&t) && t.tm_min != lastClockMin) { lastClockMin = t.tm_min; drawSleepClock(); }
    delay(40);
    return;                                  // rest of the brain naps too
  }

  // ---- Blink ----
  if (!eyesClosed && now > nextBlink) { eyesClosed = true; blinkOffAt = now + 140; drawFace(); if (bubbleUntil > now) drawBubble(); }
  if (eyesClosed && now > blinkOffAt) {
    eyesClosed = false;
    nextBlink = now + 2200 + random(0, 2400);
    drawFace(); if (bubbleUntil > now) drawBubble();
  }

  // ---- Look around ----
  if (now > nextLook) {
    nextLook = now + 2600 + random(0, 3000);
    lookX = random(-1, 2); lookY = random(-1, 2);
    if (!eyesClosed) { drawFace(); if (bubbleUntil > now) drawBubble(); }
  }

  // ---- Mood expiry ----
  if (mood != M_NORMAL && mood != M_FOCUS && now > moodUntil) { mood = M_NORMAL; drawFace(); if (bubbleUntil > now) drawBubble(); }

  // ---- Bubble expiry ----
  if (bubbleText.length() && (long)(now - bubbleUntil) >= 0) { bubbleText = ""; drawFace(); }

  // ---- Clock strip: update on the minute + rotate info ----
  {
    struct tm t;
    if (getLocalTime(&t) && t.tm_min != lastClockMin && !pomoRunning) { lastClockMin = t.tm_min; drawTopStrip(); }
  }
  if (now > nextInfoSwap) { nextInfoSwap = now + 8000; infoSlot = (infoSlot + 1) % 5; drawTopStrip(); }

  // ---- Water reminder ----
  if (now > nextWater) {
    nextWater = now + 3600000UL;
    setBubble("Psst... drink some water!", 20000);
    mood = M_HAPPY; moodUntil = now + 3000;
    drawFace(); drawBubble();
  }

  delay(25);
}
