// =====================================================================
//  Heltec Shelf Lite
//  The Shelf, shrunk onto a Heltec WiFi LoRa 32 V3 / V4.
//  - Wi-Fi file hub on a microSD card (or the board's own flash)
//  - Music player: stream to your phone/PC, or play out a MAX98357A speaker
//  - Notes & links board
//  - OLED shows address, storage and what's playing
// =====================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <FFat.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <ESPAsyncWebServer.h>
#include <vector>
#include <algorithm>

#include "AudioFileSourceFS.h"
#include "AudioFileSourceBuffer.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"

#include "board.h"
#include "web_assets.h"

// ---------------------------------------------------------------------
//  Globals
// ---------------------------------------------------------------------
static AsyncWebServer server(80);
static DNSServer dns;
static Preferences prefs;
static SPIClass sdSPI(FSPI);
static U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, OLED_RST, OLED_SCL, OLED_SDA);

static fs::FS* store = nullptr;
static bool onSD = false;
static bool oledOK = false;

static String hostName;      // e.g. shelf-ab12  -> http://shelf-ab12.local
static String apName;        // e.g. Shelf-Lite-AB12
static const char* AP_PASS = "shelflite";
static String wifiSSID, wifiPass;
static bool apActive = false;
static bool staOK = false;
static bool mdnsOK = false;
static uint32_t apOffAt = 0;
static bool speakerEnabled = false;

static volatile bool rebootSoon = false;
static uint32_t rebootAt = 0;

static volatile bool statsDirty = true;
static uint64_t storeTotal = 0, storeUsed = 0;

static volatile bool musicDirty = true;
static volatile uint32_t musicChangedAt = 0;
static volatile bool musicScanning = false;
static volatile int musicCount = 0;

static const char* DATA_DIR   = "/.shelf";
static const char* BOARD_FILE = "/.shelf/board.json";
static const char* MUSIC_FILE = "/.shelf/music.json";
static const char* MUSIC_TMP  = "/.shelf/music.tmp";

static uint32_t lastActivity = 0;
static bool screenOn = true;

// ---------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------
static String jsonEsc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n";  break;
      case '\r': o += "\\r";  break;
      case '\t': o += "\\t";  break;
      default:
        if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += c;
    }
  }
  return o;
}

// Normalise a path from the browser. Returns false if it tries to escape.
static bool cleanPath(String& p) {
  p.trim();
  if (!p.startsWith("/")) p = "/" + p;
  if (p.indexOf("..") >= 0) return false;
  while (p.indexOf("//") >= 0) p.replace("//", "/");
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  return true;
}

static bool isHiddenName(const String& n) {
  return n.startsWith(".") || n == "System Volume Information";
}

static String baseName(const String& p) {
  int i = p.lastIndexOf('/');
  return i >= 0 ? p.substring(i + 1) : p;
}

static String parentDir(const String& p) {
  int i = p.lastIndexOf('/');
  if (i <= 0) return "/";
  return p.substring(0, i);
}

static String extOf(const String& p) {
  int i = p.lastIndexOf('.');
  if (i < 0) return "";
  String e = p.substring(i + 1); e.toLowerCase();
  return e;
}

static bool isAudioExt(const String& e) {
  return e == "mp3" || e == "m4a" || e == "aac" || e == "wav" || e == "flac" || e == "ogg" || e == "opus";
}
static bool speakerCanPlay(const String& e) { return e == "mp3" || e == "wav"; }

static const char* mimeFor(const String& p) {
  String e = extOf(p);
  if (e == "mp3") return "audio/mpeg";
  if (e == "m4a") return "audio/mp4";
  if (e == "aac") return "audio/aac";
  if (e == "wav") return "audio/wav";
  if (e == "flac") return "audio/flac";
  if (e == "ogg" || e == "opus") return "audio/ogg";
  if (e == "jpg" || e == "jpeg") return "image/jpeg";
  if (e == "png") return "image/png";
  if (e == "gif") return "image/gif";
  if (e == "webp") return "image/webp";
  if (e == "heic") return "image/heic";
  if (e == "svg") return "image/svg+xml";
  if (e == "mp4" || e == "m4v") return "video/mp4";
  if (e == "mov") return "video/quicktime";
  if (e == "pdf") return "application/pdf";
  if (e == "txt" || e == "md" || e == "log" || e == "ini" || e == "cfg") return "text/plain; charset=utf-8";
  if (e == "html" || e == "htm") return "text/html; charset=utf-8";
  if (e == "css") return "text/css";
  if (e == "js") return "text/javascript";
  if (e == "json") return "application/json";
  if (e == "zip") return "application/zip";
  return "application/octet-stream";
}

static String fmtBytes(uint64_t b) {
  char buf[24];
  if (b >= 1000000000ULL) snprintf(buf, sizeof buf, "%.1fGB", b / 1e9);
  else if (b >= 1000000ULL) snprintf(buf, sizeof buf, "%.1fMB", b / 1e6);
  else if (b >= 1000ULL) snprintf(buf, sizeof buf, "%.0fKB", b / 1e3);
  else snprintf(buf, sizeof buf, "%uB", (unsigned)b);
  return buf;
}

static void touchMusic() { musicDirty = true; musicChangedAt = millis(); statsDirty = true; }

// Delete a file or a whole folder.
static bool removeAll(const String& path) {
  File f = store->open(path);
  if (!f) return false;
  if (!f.isDirectory()) { f.close(); return store->remove(path); }
  std::vector<std::pair<String, bool>> kids;
  File c = f.openNextFile();
  while (c) { kids.push_back({String(c.path()), c.isDirectory()}); c.close(); c = f.openNextFile(); }
  f.close();
  for (auto& k : kids) { if (k.second) removeAll(k.first); else store->remove(k.first); }
  return store->rmdir(path);
}

// ---------------------------------------------------------------------
//  Speaker player (MAX98357A over I2S) - runs in its own task
// ---------------------------------------------------------------------
enum SpkState { SPK_STOPPED = 0, SPK_PLAYING = 1, SPK_PAUSED = 2 };
enum SpkCmd { CMD_NONE, CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_STOP, CMD_NEXT, CMD_PREV };

static SemaphoreHandle_t spkLock;
static SpkCmd pendingCmd = CMD_NONE;
static String pendingPath;
static volatile int spkVolume = 60;      // 0..100
static volatile SpkState spkState = SPK_STOPPED;
static std::vector<String> spkQueue;     // owned by the audio task (copied out under lock)
static int spkIndex = -1;
static String spkTitle, spkPath;         // read under lock

static AudioGenerator* gen = nullptr;
static AudioFileSourceFS* aSrc = nullptr;
static AudioFileSourceBuffer* aBuf = nullptr;
static AudioOutputI2S* aOut = nullptr;

static float gainFor(int v) { float x = v / 100.0f; return x * x * 1.0f; }

static void spkTeardown() {
  if (gen) { if (gen->isRunning()) gen->stop(); delete gen; gen = nullptr; }
  if (aBuf) { delete aBuf; aBuf = nullptr; }
  if (aSrc) { delete aSrc; aSrc = nullptr; }
  if (aOut) { delete aOut; aOut = nullptr; }
}

static bool spkStart(const String& path) {
  spkTeardown();
  String e = extOf(path);
  if (!speakerCanPlay(e) || !store->exists(path)) return false;
  aSrc = new AudioFileSourceFS(*store, path.c_str());
  aBuf = new AudioFileSourceBuffer(aSrc, 16 * 1024);
  aOut = new AudioOutputI2S(0, AudioOutputI2S::EXTERNAL_I2S);
  aOut->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  aOut->SetGain(gainFor(spkVolume));
  if (e == "wav") gen = new AudioGeneratorWAV();
  else gen = new AudioGeneratorMP3();
  if (!gen->begin(aBuf, aOut)) { spkTeardown(); return false; }
  xSemaphoreTake(spkLock, portMAX_DELAY);
  spkPath = path;
  spkTitle = baseName(path);
  int dot = spkTitle.lastIndexOf('.');
  if (dot > 0) spkTitle = spkTitle.substring(0, dot);
  xSemaphoreGive(spkLock);
  return true;
}

// Queue = every speaker-playable track in the same folder, in name order.
static void spkBuildQueue(const String& path) {
  spkQueue.clear();
  spkIndex = -1;
  File d = store->open(parentDir(path));
  if (d && d.isDirectory()) {
    File c = d.openNextFile();
    while (c) {
      String n = c.name();
      if (!c.isDirectory() && !isHiddenName(n) && speakerCanPlay(extOf(n))) spkQueue.push_back(String(c.path()));
      c.close();
      c = d.openNextFile();
    }
    d.close();
  }
  std::sort(spkQueue.begin(), spkQueue.end(), [](const String& a, const String& b) {
    String x = a, y = b; x.toLowerCase(); y.toLowerCase(); return x < y;
  });
  for (size_t i = 0; i < spkQueue.size(); i++) if (spkQueue[i] == path) spkIndex = i;
  if (spkIndex < 0) { spkQueue.insert(spkQueue.begin(), path); spkIndex = 0; }
}

static void spkPlayIndex(int i) {
  int tries = 0;
  while (i >= 0 && i < (int)spkQueue.size() && tries < 50) {
    if (spkStart(spkQueue[i])) { spkIndex = i; spkState = SPK_PLAYING; return; }
    i++; tries++;
  }
  spkTeardown();
  spkState = SPK_STOPPED;
  xSemaphoreTake(spkLock, portMAX_DELAY); spkTitle = ""; spkPath = ""; xSemaphoreGive(spkLock);
}

static void audioTask(void*) {
  int lastVol = -1;
  for (;;) {
    SpkCmd cmd = CMD_NONE; String path;
    xSemaphoreTake(spkLock, portMAX_DELAY);
    if (pendingCmd != CMD_NONE) { cmd = pendingCmd; path = pendingPath; pendingCmd = CMD_NONE; }
    xSemaphoreGive(spkLock);

    switch (cmd) {
      case CMD_PLAY:   spkBuildQueue(path); spkPlayIndex(spkIndex); break;
      case CMD_PAUSE:  if (spkState == SPK_PLAYING) spkState = SPK_PAUSED; break;   // DMA auto-clears to silence
      case CMD_RESUME: if (spkState == SPK_PAUSED) spkState = SPK_PLAYING; break;
      case CMD_STOP:   spkTeardown(); spkState = SPK_STOPPED;
                       xSemaphoreTake(spkLock, portMAX_DELAY); spkTitle = ""; spkPath = ""; xSemaphoreGive(spkLock); break;
      case CMD_NEXT:   if (!spkQueue.empty()) spkPlayIndex((spkIndex + 1) % spkQueue.size()); break;
      case CMD_PREV:   if (!spkQueue.empty()) spkPlayIndex(spkIndex > 0 ? spkIndex - 1 : spkQueue.size() - 1); break;
      default: break;
    }

    if (aOut && lastVol != spkVolume) { aOut->SetGain(gainFor(spkVolume)); lastVol = spkVolume; }

    if (spkState == SPK_PLAYING && gen) {
      if (gen->isRunning()) {
        if (!gen->loop()) {
          gen->stop();
          if (spkIndex + 1 < (int)spkQueue.size()) spkPlayIndex(spkIndex + 1);
          else { spkTeardown(); spkState = SPK_STOPPED;
                 xSemaphoreTake(spkLock, portMAX_DELAY); spkTitle = ""; spkPath = ""; xSemaphoreGive(spkLock); }
        }
      }
      vTaskDelay(1);
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

static void spkCommand(SpkCmd c, const String& path = "") {
  xSemaphoreTake(spkLock, portMAX_DELAY);
  pendingCmd = c; pendingPath = path;
  xSemaphoreGive(spkLock);
}

// ---------------------------------------------------------------------
//  Music index - written to /.shelf/music.json so it never sits in RAM
// ---------------------------------------------------------------------
static int scanInto(File& out, const String& dir, int depth, int count, bool& first) {
  if (depth > 8 || count >= 5000) return count;
  File d = store->open(dir);
  if (!d || !d.isDirectory()) return count;
  std::vector<String> subdirs;
  File c = d.openNextFile();
  while (c && count < 5000) {
    String n = c.name();
    if (!isHiddenName(n)) {
      if (c.isDirectory()) subdirs.push_back(String(c.path()));
      else {
        String e = extOf(n);
        if (isAudioExt(e)) {
          String title = n; int dot = title.lastIndexOf('.'); if (dot > 0) title = title.substring(0, dot);
          String folder = dir == "/" ? String("") : baseName(dir);
          out.printf("%s{\"p\":\"%s\",\"t\":\"%s\",\"f\":\"%s\",\"s\":%u,\"spk\":%d}",
                     first ? "" : ",", jsonEsc(String(c.path())).c_str(), jsonEsc(title).c_str(),
                     jsonEsc(folder).c_str(), (unsigned)c.size(), speakerCanPlay(e) ? 1 : 0);
          first = false;
          count++;
        }
      }
    }
    c.close();
    if ((count & 31) == 0) delay(1);
    c = d.openNextFile();
  }
  d.close();
  std::sort(subdirs.begin(), subdirs.end());
  for (auto& s : subdirs) count = scanInto(out, s, depth + 1, count, first);
  return count;
}

static void rebuildMusicIndex() {
  musicScanning = true;
  store->mkdir(DATA_DIR);
  File out = store->open(MUSIC_TMP, FILE_WRITE);
  if (!out) { musicScanning = false; return; }
  out.print("[");
  bool first = true;
  int n = scanInto(out, "/", 0, 0, first);
  out.print("]");
  out.close();
  store->remove(MUSIC_FILE);
  store->rename(MUSIC_TMP, MUSIC_FILE);
  musicCount = n;
  musicScanning = false;
}

// ---------------------------------------------------------------------
//  OLED
// ---------------------------------------------------------------------
static bool probeOLED() {
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW); delay(20); digitalWrite(OLED_RST, HIGH); delay(20);
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.beginTransmission(OLED_ADDR);
  bool ok = Wire.endTransmission() == 0;
  Wire.end();
  return ok;
}

static void powerOLED() {
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW); delay(60);
  bool found = probeOLED();
#if defined(SHELF_BOARD_V4)
  if (!found) {             // V4-R8: Vext lives on GPIO40 and GPIO36 belongs to PSRAM
    pinMode(PIN_VEXT, INPUT);
    pinMode(PIN_VEXT_ALT, OUTPUT); digitalWrite(PIN_VEXT_ALT, LOW); delay(60);
    found = probeOLED();
  }
#endif
  oledOK = true;   // even if the probe missed, try - U8g2 writes are harmless
  oled.begin();
  oled.setContrast(200);
}

static void oledMsg(const char* a, const char* b = "", const char* c = "", const char* d = "") {
  if (!oledOK) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB10_tf); oled.drawStr(0, 12, a);
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 30, b); oled.drawStr(0, 44, c); oled.drawStr(0, 58, d);
  oled.sendBuffer();
}

static int marqueeX = 0;
static void drawStatus() {
  if (!oledOK) return;
  if (!screenOn) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_helvB08_tf);
  oled.drawStr(0, 9, "THE SHELF");
  oled.setFont(u8g2_font_5x7_tf);
  oled.drawStr(56, 9, "lite");
  oled.drawStr(104, 9, onSD ? "SD" : "FLASH");
  if (onSD) {} // spacer
  oled.drawHLine(0, 12, 128);

  oled.setFont(u8g2_font_6x10_tf);
  if (staOK) {
    String ip = WiFi.localIP().toString();
    oled.drawStr(0, 24, ip.c_str());
    String h = hostName + ".local";
    oled.drawStr(0, 35, h.c_str());
  } else if (apActive) {
    String a = "Join " + apName;
    oled.drawStr(0, 24, a.c_str());
    oled.drawStr(0, 35, "pw shelflite > 4.1");
  } else {
    oled.drawStr(0, 24, "Connecting Wi-Fi...");
    oled.drawStr(0, 35, wifiSSID.c_str());
  }

  // storage bar
  if (storeTotal > 0) {
    int w = (int)((storeUsed * 76ULL) / storeTotal);
    oled.drawFrame(0, 39, 78, 7);
    oled.drawBox(1, 40, w, 5);
    String fr = fmtBytes(storeTotal - storeUsed) + " free";
    oled.setFont(u8g2_font_5x7_tf);
    oled.drawStr(82, 46, fr.c_str());
  }

  // bottom line: now playing or Wi-Fi strength
  oled.setFont(u8g2_font_6x10_tf);
  String title;
  xSemaphoreTake(spkLock, portMAX_DELAY); title = spkTitle; xSemaphoreGive(spkLock);
  if (spkState != SPK_STOPPED && title.length()) {
    String line = String(spkState == SPK_PAUSED ? "|| " : "> ") + title + "   ";
    int tw = oled.getStrWidth(line.c_str());
    if (tw <= 128) oled.drawStr(0, 62, line.c_str());
    else {
      marqueeX = (marqueeX + 3) % tw;
      oled.drawStr(-marqueeX, 62, line.c_str());
      oled.drawStr(tw - marqueeX, 62, line.c_str());
    }
  } else if (staOK) {
    String s = "Wi-Fi " + String(WiFi.RSSI()) + "dBm  " + String(musicCount) + " songs";
    oled.drawStr(0, 62, s.c_str());
  } else {
    oled.drawStr(0, 62, BOARD_NAME "  v" FW_VERSION);
  }
  oled.sendBuffer();
}

// ---------------------------------------------------------------------
//  Wi-Fi
// ---------------------------------------------------------------------
static void startAP() {
  if (apActive) return;
  WiFi.mode(wifiSSID.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(apName.c_str(), AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  apActive = true;
}

static void stopAP() {
  if (!apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apActive = false;
}

static void startMDNS() {
  if (mdnsOK) return;
  if (MDNS.begin(hostName.c_str())) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addServiceTxt("http", "tcp", "app", "shelf-lite");
    mdnsOK = true;
  }
}

static String sanitizeHost(String h) {
  h.toLowerCase();
  String o;
  for (size_t i = 0; i < h.length() && o.length() < 24; i++) {
    char c = h[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') o += c;
    else if (c == ' ' || c == '_') o += '-';
  }
  while (o.startsWith("-")) o.remove(0, 1);
  while (o.endsWith("-")) o.remove(o.length() - 1);
  return o;
}

// ---------------------------------------------------------------------
//  Web server
// ---------------------------------------------------------------------
static void sendJSON(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}
static void sendErr(AsyncWebServerRequest* r, int code, const char* msg) {
  sendJSON(r, code, String("{\"ok\":false,\"error\":\"") + msg + "\"}");
}
static void sendOK(AsyncWebServerRequest* r) { sendJSON(r, 200, "{\"ok\":true}"); }

static String param(AsyncWebServerRequest* r, const char* name) {
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  if (r->hasParam(name)) return r->getParam(name)->value();
  return "";
}

static void sendGz(AsyncWebServerRequest* r, const char* type, const uint8_t* data, size_t len, bool cache) {
  AsyncWebServerResponse* res = r->beginResponse(200, type, data, len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", cache ? "public, max-age=86400" : "no-cache");
  r->send(res);
}

// File download / streaming with HTTP Range support (iPhone Safari needs this for audio & video).
static void handleFile(AsyncWebServerRequest* r) {
  String p = param(r, "p");
  if (!cleanPath(p)) return sendErr(r, 400, "bad path");
  File f = store->open(p, FILE_READ);
  if (!f || f.isDirectory()) { if (f) f.close(); return sendErr(r, 404, "not found"); }
  size_t size = f.size();
  size_t start = 0, end = size ? size - 1 : 0;
  bool ranged = false;
  if (r->hasHeader("Range") && size > 0) {
    String rg = r->header("Range");           // bytes=START-END  or bytes=-SUFFIX
    if (rg.startsWith("bytes=")) {
      rg = rg.substring(6);
      int dash = rg.indexOf('-');
      String a = rg.substring(0, dash), b = rg.substring(dash + 1);
      int comma = b.indexOf(','); if (comma >= 0) b = b.substring(0, comma);
      if (a.length() == 0) { size_t suf = strtoul(b.c_str(), nullptr, 10); start = suf >= size ? 0 : size - suf; }
      else { start = strtoul(a.c_str(), nullptr, 10); if (b.length()) end = strtoul(b.c_str(), nullptr, 10); }
      if (end >= size) end = size - 1;
      if (start > end) {
        f.close();
        AsyncWebServerResponse* res = r->beginResponse(416, "text/plain", "range");
        res->addHeader("Content-Range", String("bytes */") + size);
        return r->send(res);
      }
      ranged = true;
    }
  }
  size_t len = size ? end - start + 1 : 0;
  File fh = f;
  size_t base = start;
  AsyncWebServerResponse* res = r->beginResponse(mimeFor(p), len,
    [fh, base, len](uint8_t* buf, size_t maxLen, size_t index) mutable -> size_t {
      if (index >= len) return 0;
      size_t want = std::min(maxLen, len - index);
      if (fh.position() != base + index) fh.seek(base + index);
      int got = fh.read(buf, want);
      return got > 0 ? (size_t)got : 0;
    });
  res->addHeader("Accept-Ranges", "bytes");
  res->addHeader("Cache-Control", "no-cache");
  if (ranged) {
    res->setCode(206);
    res->addHeader("Content-Range", String("bytes ") + start + "-" + end + "/" + size);
  }
  if (param(r, "dl") == "1") res->addHeader("Content-Disposition", String("attachment; filename=\"") + baseName(p) + "\"");
  r->send(res);
}

static void handleList(AsyncWebServerRequest* r) {
  String p = param(r, "path"); if (p == "") p = "/";
  if (!cleanPath(p)) return sendErr(r, 400, "bad path");
  File d = store->open(p);
  if (!d || !d.isDirectory()) { if (d) d.close(); return sendErr(r, 404, "no such folder"); }
  AsyncResponseStream* out = r->beginResponseStream("application/json");
  out->addHeader("Cache-Control", "no-store");
  out->printf("{\"ok\":true,\"path\":\"%s\",\"items\":[", jsonEsc(p).c_str());
  bool first = true; int n = 0;
  File c = d.openNextFile();
  while (c && n < 2000) {
    String nm = c.name();
    if (!isHiddenName(nm)) {
      out->printf("%s{\"n\":\"%s\",\"d\":%d,\"s\":%u,\"t\":%lu}", first ? "" : ",", jsonEsc(nm).c_str(),
                  c.isDirectory() ? 1 : 0, (unsigned)c.size(), (unsigned long)c.getLastWrite());
      first = false; n++;
    }
    c.close();
    c = d.openNextFile();
  }
  d.close();
  out->print("]}");
  r->send(out);
}

struct UploadCtx { uint8_t err; char path[200]; };   // err: 1 = bad name, 2 = write failed

static void handleUploadData(AsyncWebServerRequest* r, const String& filename, size_t index, uint8_t* data, size_t len, bool final) {
  if (index == 0) {
    UploadCtx* ctx = (UploadCtx*)malloc(sizeof(UploadCtx));
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    r->_tempObject = ctx;
    String dir = param(r, "dir"); if (dir == "") dir = "/";
    String name = baseName(filename);
    name.replace("\\", "_");
    String full = dir + "/" + name;
    if (!cleanPath(full) || name.length() == 0 || isHiddenName(name) || full.length() >= sizeof ctx->path) { ctx->err = 1; return; }
    strlcpy(ctx->path, full.c_str(), sizeof ctx->path);
    r->_tempFile = store->open(full, FILE_WRITE);
    if (!r->_tempFile) ctx->err = 2;
    lastActivity = millis();
  }
  UploadCtx* ctx = (UploadCtx*)r->_tempObject;
  if (!ctx || ctx->err) return;
  if (len && r->_tempFile.write(data, len) != len) {
    ctx->err = 2;
    r->_tempFile.close();
    store->remove(ctx->path);
    return;
  }
  if (final) { r->_tempFile.close(); touchMusic(); }
}

static void handleUploadDone(AsyncWebServerRequest* r) {
  UploadCtx* ctx = (UploadCtx*)r->_tempObject;
  if (!ctx) return sendErr(r, 400, "no file");
  if (ctx->err == 1) return sendErr(r, 400, "that file name can't be used");
  if (ctx->err) return sendErr(r, 507, onSD ? "upload failed - card full?" : "upload failed - flash full? add a microSD card");
  sendOK(r);
}

// Body collector for small JSON posts (board)
static void collectBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total > 96 * 1024) return;
  if (index == 0) { r->_tempObject = malloc(total + 1); if (r->_tempObject) ((char*)r->_tempObject)[total] = 0; }
  if (r->_tempObject) memcpy((uint8_t*)r->_tempObject + index, data, len);
}

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) { sendGz(r, "text/html; charset=utf-8", INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, false); });
  server.on("/icon.png", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "image/png", ICON_PNG, ICON_PNG_LEN);
    res->addHeader("Cache-Control", "public, max-age=604800"); r->send(res); });
  server.on("/manifest.webmanifest", HTTP_GET, [](AsyncWebServerRequest* r) {
    String m = String("{\"name\":\"The Shelf Lite\",\"short_name\":\"Shelf\",\"start_url\":\"/\",\"display\":\"standalone\","
                      "\"background_color\":\"#16130f\",\"theme_color\":\"#16130f\",\"icons\":[{\"src\":\"/icon.png\",\"sizes\":\"180x180\",\"type\":\"image/png\"}]}");
    r->send(200, "application/manifest+json", m); });

  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest* r) {
    String s = "{\"ok\":true";
    s += ",\"board\":\"" BOARD_NAME "\",\"boardId\":\"" BOARD_ID "\",\"fw\":\"" FW_VERSION "\"";
    s += ",\"storage\":\"" + String(onSD ? "microSD card" : "board flash") + "\",\"sd\":" + (onSD ? "true" : "false");
    s += ",\"total\":" + String((double)storeTotal, 0) + ",\"used\":" + String((double)storeUsed, 0);
    s += ",\"host\":\"" + jsonEsc(hostName) + "\",\"ip\":\"" + (staOK ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"";
    s += ",\"ssid\":\"" + jsonEsc(staOK ? WiFi.SSID() : String("")) + "\",\"rssi\":" + String(staOK ? WiFi.RSSI() : 0);
    s += ",\"ap\":" + String(apActive ? "true" : "false") + ",\"apName\":\"" + jsonEsc(apName) + "\"";
    s += ",\"uptime\":" + String(millis() / 1000) + ",\"heap\":" + String(ESP.getFreeHeap());
    s += ",\"speaker\":" + String(speakerEnabled ? "true" : "false");
    s += ",\"songs\":" + String(musicCount) + ",\"scanning\":" + String(musicScanning ? "true" : "false");
    s += "}";
    sendJSON(r, 200, s);
  });

  server.on("/api/list", HTTP_GET, handleList);
  server.on("/f", HTTP_GET, handleFile);
  server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);

  server.on("/api/mkdir", HTTP_POST, [](AsyncWebServerRequest* r) {
    String p = param(r, "path");
    if (!cleanPath(p) || p == "/" || isHiddenName(baseName(p))) return sendErr(r, 400, "bad name");
    if (store->exists(p)) return sendErr(r, 409, "already exists");
    store->mkdir(p) ? sendOK(r) : sendErr(r, 500, "could not create folder");
  });

  server.on("/api/delete", HTTP_POST, [](AsyncWebServerRequest* r) {
    String p = param(r, "path");
    if (!cleanPath(p) || p == "/" || p.startsWith(DATA_DIR)) return sendErr(r, 400, "bad path");
    if (!store->exists(p)) return sendErr(r, 404, "not found");
    bool ok = removeAll(p); touchMusic();
    ok ? sendOK(r) : sendErr(r, 500, "could not delete");
  });

  server.on("/api/rename", HTTP_POST, [](AsyncWebServerRequest* r) {
    String a = param(r, "from"), b = param(r, "to");
    if (!cleanPath(a) || !cleanPath(b) || a == "/" || b == "/" || isHiddenName(baseName(b))) return sendErr(r, 400, "bad name");
    if (!store->exists(a)) return sendErr(r, 404, "not found");
    if (store->exists(b)) return sendErr(r, 409, "a file with that name already exists");
    bool ok = store->rename(a, b); touchMusic();
    ok ? sendOK(r) : sendErr(r, 500, "could not rename");
  });

  // ---- music ----
  server.on("/api/music", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!store->exists(MUSIC_FILE)) return sendJSON(r, 200, "[]");
    AsyncWebServerResponse* res = r->beginResponse(*store, MUSIC_FILE, "application/json");
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  server.on("/api/rescan", HTTP_POST, [](AsyncWebServerRequest* r) { touchMusic(); musicChangedAt = 0; sendOK(r); });

  server.on("/api/speaker", HTTP_GET, [](AsyncWebServerRequest* r) {
    String t, p;
    xSemaphoreTake(spkLock, portMAX_DELAY); t = spkTitle; p = spkPath; xSemaphoreGive(spkLock);
    String s = "{\"ok\":true,\"enabled\":" + String(speakerEnabled ? "true" : "false");
    s += ",\"state\":" + String((int)spkState) + ",\"title\":\"" + jsonEsc(t) + "\",\"path\":\"" + jsonEsc(p) + "\"";
    s += ",\"vol\":" + String(spkVolume) + "}";
    sendJSON(r, 200, s);
  });
  server.on("/api/speaker", HTTP_POST, [](AsyncWebServerRequest* r) {
    String cmd = param(r, "cmd");
    String v = param(r, "vol");
    if (v.length()) { int x = constrain(v.toInt(), 0, 100); spkVolume = x; prefs.putInt("vol", x); }
    if (cmd == "play") {
      String p = param(r, "path");
      if (!cleanPath(p)) return sendErr(r, 400, "bad path");
      if (!speakerCanPlay(extOf(p))) return sendErr(r, 415, "the speaker plays MP3 and WAV files");
      spkCommand(CMD_PLAY, p);
    }
    else if (cmd == "pause") spkCommand(CMD_PAUSE);
    else if (cmd == "resume") spkCommand(CMD_RESUME);
    else if (cmd == "stop") spkCommand(CMD_STOP);
    else if (cmd == "next") spkCommand(CMD_NEXT);
    else if (cmd == "prev") spkCommand(CMD_PREV);
    lastActivity = millis();
    sendOK(r);
  });

  // ---- notes & links board ----
  server.on("/api/board", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!store->exists(BOARD_FILE)) return sendJSON(r, 200, "[]");
    AsyncWebServerResponse* res = r->beginResponse(*store, BOARD_FILE, "application/json");
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  server.on("/api/board", HTTP_POST, [](AsyncWebServerRequest* r) {
    const char* body = (const char*)r->_tempObject;
    if (!body || body[0] != '[') return sendErr(r, 400, "expected a list");
    store->mkdir(DATA_DIR);
    File f = store->open(BOARD_FILE, FILE_WRITE);
    if (!f) return sendErr(r, 500, "could not save");
    f.print(body); f.close();
    sendOK(r);
  }, nullptr, collectBody);

  // ---- Wi-Fi & settings ----
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
      if (!(WiFi.getMode() & WIFI_STA)) WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
      WiFi.scanNetworks(true);
      return sendJSON(r, 200, "{\"ok\":true,\"scanning\":true}");
    }
    if (n == WIFI_SCAN_RUNNING) return sendJSON(r, 200, "{\"ok\":true,\"scanning\":true}");
    String s = "{\"ok\":true,\"scanning\":false,\"nets\":[";
    for (int i = 0; i < n; i++) {
      if (i) s += ",";
      s += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"lock\":" + String(WiFi.encryptionType(i) != WIFI_AUTH_OPEN ? 1 : 0) + "}";
    }
    s += "]}";
    WiFi.scanDelete();
    sendJSON(r, 200, s);
  });
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    String ssid = param(r, "ssid"), pass = param(r, "pass");
    if (!ssid.length()) return sendErr(r, 400, "pick a network");
    prefs.putString("ssid", ssid); prefs.putString("pass", pass);
    sendOK(r);
    rebootSoon = true;
  });
  server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest* r) {
    prefs.remove("ssid"); prefs.remove("pass"); sendOK(r); rebootSoon = true; });
  server.on("/api/settings", HTTP_POST, [](AsyncWebServerRequest* r) {
    String h = param(r, "host");
    bool restart = false;
    if (h.length()) {
      h = sanitizeHost(h);
      if (h.length() < 2) return sendErr(r, 400, "name needs letters or numbers");
      if (h != hostName) { prefs.putString("host", h); restart = true; }
    }
    String spk = param(r, "speaker");
    if (spk.length()) { speakerEnabled = spk == "1"; prefs.putBool("spk", speakerEnabled); if (!speakerEnabled) spkCommand(CMD_STOP); }
    sendJSON(r, 200, String("{\"ok\":true,\"restart\":") + (restart ? "true" : "false") + "}");
    if (restart) rebootSoon = true;
  });
  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* r) { sendOK(r); rebootSoon = true; });

  // Captive portal: in setup mode send every unknown host to the app.
  server.onNotFound([](AsyncWebServerRequest* r) {
    if (apActive) {
      String host = r->host();
      String apIP = WiFi.softAPIP().toString();
      if (host != apIP && host != hostName + ".local") return r->redirect("http://" + apIP + "/");
    }
    if (r->method() == HTTP_OPTIONS) return r->send(204);
    r->send(404, "text/plain", "Not found");
  });

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  server.begin();
}

// ---------------------------------------------------------------------
//  Storage
// ---------------------------------------------------------------------
static void setupStorage() {
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI, 20000000, "/sd", 8)) onSD = true;
  else if (SD.begin(SD_CS, sdSPI, 4000000, "/sd", 8)) onSD = true;   // slower fallback for long wires
  if (onSD && SD.cardType() == CARD_NONE) { SD.end(); onSD = false; }

  if (onSD) { store = &SD; }
  else {
    oledMsg("THE SHELF lite", "No microSD found", "Using board flash", "(first boot formats)");
    if (!FFat.begin(true)) { oledMsg("Storage error", "Flash format failed", "Reflash firmware?"); }
    store = &FFat;
  }
  store->mkdir(DATA_DIR);
  store->remove(MUSIC_TMP);
}

static void refreshStats() {
  if (onSD) { storeTotal = SD.totalBytes(); storeUsed = SD.usedBytes(); }
  else { storeTotal = FFat.totalBytes(); storeUsed = FFat.usedBytes(); }
  statsDirty = false;
}

// ---------------------------------------------------------------------
//  Button: short press = next song (speaker) or wake screen; hold 6 s = forget Wi-Fi
// ---------------------------------------------------------------------
static uint32_t btnDownAt = 0;
static bool btnWasDown = false;

static void handleButton() {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !btnWasDown) { btnDownAt = now; btnWasDown = true; }
  if (down && btnWasDown) {
    uint32_t held = now - btnDownAt;
    if (held > 1500) {
      if (!screenOn) { screenOn = true; oled.setPowerSave(0); }
      int left = 6 - (int)(held / 1000);
      if (left <= 0) {
        oledMsg("Wi-Fi reset", "Forgetting network...", "Restarting in setup", "mode");
        prefs.remove("ssid"); prefs.remove("pass");
        delay(1200); ESP.restart();
      }
      char b[32]; snprintf(b, sizeof b, "Keep holding %d s", left);
      oledMsg("Forget Wi-Fi?", b, "Let go to cancel");
    }
  }
  if (!down && btnWasDown) {
    btnWasDown = false;
    uint32_t held = now - btnDownAt;
    lastActivity = now;
    if (held < 800) {
      if (!screenOn) { screenOn = true; oled.setPowerSave(0); }
      else if (spkState != SPK_STOPPED) spkCommand(CMD_NEXT);
    }
  }
}

// ---------------------------------------------------------------------
//  Setup & loop
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  spkLock = xSemaphoreCreateMutex();

  powerOLED();
  oledMsg("THE SHELF lite", BOARD_NAME, "Starting...");

  prefs.begin("shelf", false);
  uint64_t mac = ESP.getEfuseMac();
  char tag[5]; snprintf(tag, sizeof tag, "%02x%02x", (uint8_t)(mac >> 32), (uint8_t)(mac >> 40));
  String up = String(tag); up.toUpperCase();
  apName = "Shelf-Lite-" + up;
  hostName = prefs.getString("host", String("shelf-") + tag);
  wifiSSID = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  speakerEnabled = prefs.getBool("spk", false);
  spkVolume = prefs.getInt("vol", 60);

  setupStorage();
  refreshStats();

  WiFi.persistent(false);
  WiFi.setHostname(hostName.c_str());
  if (wifiSSID.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      char b[24]; snprintf(b, sizeof b, "%lus", (unsigned long)((millis() - t0) / 1000));
      oledMsg("Connecting", wifiSSID.c_str(), b);
      delay(400);
    }
    staOK = WiFi.status() == WL_CONNECTED;
  }
  if (staOK) { WiFi.setSleep(false); startMDNS(); }
  else startAP();   // keeps trying the saved network in the background too

  setupWeb();
  xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 3, nullptr, 1);
  lastActivity = millis();
}

void loop() {
  uint32_t now = millis();
  if (apActive) dns.processNextRequest();

  // Joined the home network while in setup mode -> drop the setup hotspot a bit later.
  if (!staOK && WiFi.status() == WL_CONNECTED) {
    staOK = true; WiFi.setSleep(false); startMDNS(); apOffAt = now + 90000;
  }
  if (staOK && WiFi.status() != WL_CONNECTED) staOK = false;   // auto-reconnect handles it
  if (apActive && apOffAt && now > apOffAt && staOK) { stopAP(); apOffAt = 0; }

  if (rebootSoon) { rebootSoon = false; rebootAt = now + 1500; }
  if (rebootAt && now > rebootAt) { oledMsg("Restarting..."); delay(200); ESP.restart(); }

  handleButton();

  static uint32_t lastStats = 0;
  if (statsDirty || now - lastStats > 60000) { refreshStats(); lastStats = now; }

  if (musicDirty && now - musicChangedAt > 3000) { musicDirty = false; rebuildMusicIndex(); }

  // Screen sleeps after 5 minutes of nothing happening (protects the OLED).
  if (spkState == SPK_PLAYING) lastActivity = now;
  if (screenOn && now - lastActivity > 300000 && !btnWasDown) { screenOn = false; oled.setPowerSave(1); }

  static uint32_t lastDraw = 0;
  if (!btnWasDown && now - lastDraw > 400) { drawStatus(); lastDraw = now; }
  delay(5);
}
