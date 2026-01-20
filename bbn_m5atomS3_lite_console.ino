/*
  Arduino-ESP32 3.3.5 console + WiFi + WebServer for PuTTY (M5AtomS3 / ESP32-S3)
  - ANSI line editor with mid-line cursor
  - Persistent history in NVS (survives reboot)
  - PuTTY arrows: ESC parser + fallback for missing ESC (treat "[A" etc as arrows)
  - Auto-connect WiFi on boot if creds exist
  - WebController runs WebServer in its own FreeRTOS task (so console blocking input doesn't starve HTTP)

  Commands:
    help
    history [n]
    uptime | chip | free | reboot
    wifi scan [n]
    wifi choose <index> [pass]
    wifi set "ssid" "pass"
    wifi connect ["ssid" "pass"]
    wifi status | wifi disconnect | wifi clear
    ping <host> [count] [timeout_ms]
    web start [port] | web stop | web status

  Web:
    GET /         welcome page
    GET /status   basic status
*/

#define CONFIG_ESP_CONSOLE_USB_CDC 1
#define CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF 1
#define CONFIG_LIBC_STDIN_LINE_ENDING_CR 1

#include <Arduino.h>

#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <WiFi.h>
#include <WebServer.h>

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <string>
#include <vector>
#include <functional>
#include <algorithm>
#include <memory>

// ------------------------------ small utils ------------------------------

static inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static inline std::string to_lower(std::string s) {
  for (char &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// argv split: quotes "..." and backslash escapes.
static std::vector<std::string> split_argv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quotes = false;
  bool esc = false;

  auto push = [&]() {
    if (!cur.empty()) out.push_back(cur);
    cur.clear();
  };

  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (esc) { cur.push_back(c); esc = false; continue; }
    if (c == '\\') { esc = true; continue; }
    if (c == '"') { in_quotes = !in_quotes; continue; }

    if (!in_quotes && is_space(c)) {
      push();
      while (i + 1 < line.size() && is_space(line[i + 1])) i++;
      continue;
    }
    cur.push_back(c);
  }
  push();
  return out;
}

static inline void trim_in_place(std::string& s) {
  size_t a = 0;
  while (a < s.size() && is_space(s[a])) a++;
  size_t b = s.size();
  while (b > a && is_space(s[b - 1])) b--;
  s = s.substr(a, b - a);
}

static inline int clampi(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

// ------------------------------ ConsoleHistory ------------------------------

class ConsoleHistory {
public:
  static constexpr int kMaxItems = 60;
  static constexpr size_t kMaxLine = 256;

  explicit ConsoleHistory(std::string nvs_ns = "console")
  : ns_(std::move(nvs_ns)) {
    items_.assign(kMaxItems, "");
  }

  bool load() {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READONLY, &h) != ESP_OK) return false;

    int32_t head = 0, count = 0;
    (void)nvs_get_i32(h, "h_head", &head);
    (void)nvs_get_i32(h, "h_count", &count);
    head_  = clampi(head,  0, kMaxItems - 1);
    count_ = clampi(count, 0, kMaxItems);

    for (int i = 0; i < kMaxItems; i++) {
      items_[i].clear();
      char key[8];
      snprintf(key, sizeof(key), "h%02d", i);
      size_t required = 0;
      if (nvs_get_str(h, key, nullptr, &required) == ESP_OK && required > 0 && required <= kMaxLine) {
        std::string tmp(required, '\0');
        if (nvs_get_str(h, key, tmp.data(), &required) == ESP_OK) {
          if (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
          items_[i] = tmp;
        }
      }
    }

    nvs_close(h);
    reset_nav();
    return true;
  }

  void add(const std::string& line) {
    if (line.empty()) return;

    if (count_ > 0) {
      int last = (head_ - 1 + kMaxItems) % kMaxItems;
      if (items_[last] == line) { reset_nav(); return; }
    }

    items_[head_] = truncate(line, kMaxLine - 1);
    head_ = (head_ + 1) % kMaxItems;
    if (count_ < kMaxItems) count_++;
    reset_nav();
    persist_last();
  }

  void reset_nav() { nav_ = 0; scratch_.clear(); }

  const std::string* older(const std::string& current_line) {
    if (count_ == 0) return nullptr;
    if (nav_ == 0) scratch_ = current_line;
    if (nav_ < count_) nav_++;
    return get_recent(nav_);
  }

  const std::string* newer() {
    if (count_ == 0) return nullptr;
    if (nav_ > 0) nav_--;
    if (nav_ == 0) return &scratch_;
    return get_recent(nav_);
  }

  int size() const { return count_; }

  std::vector<std::string> last_n(int n) const {
    n = clampi(n, 0, count_);
    std::vector<std::string> out;
    out.reserve(n);
    for (int k = n; k >= 1; k--) {
      const std::string* s = get_recent(k);
      if (s && !s->empty()) out.push_back(*s);
    }
    return out;
  }

private:
  std::string ns_;
  std::vector<std::string> items_;
  int head_ = 0;
  int count_ = 0;

  int nav_ = 0;
  std::string scratch_;

  static std::string truncate(const std::string& s, size_t max_len) {
    return (s.size() <= max_len) ? s : s.substr(0, max_len);
  }

  const std::string* get_recent(int k) const {
    if (k <= 0 || k > count_) return nullptr;
    int idx = (head_ - k + kMaxItems) % kMaxItems;
    return &items_[idx];
  }

  void persist_last() {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READWRITE, &h) != ESP_OK) return;

    int slot = (head_ - 1 + kMaxItems) % kMaxItems;
    char key[8];
    snprintf(key, sizeof(key), "h%02d", slot);

    (void)nvs_set_str(h, key, items_[slot].c_str());
    (void)nvs_set_i32(h, "h_head", head_);
    (void)nvs_set_i32(h, "h_count", count_);
    (void)nvs_commit(h);
    nvs_close(h);
  }
};

// ------------------------------ CommandsRegistry ------------------------------

class CommandsRegistry {
public:
  using Handler = std::function<int(const std::vector<std::string>& args)>;

  void add(std::string name, Handler handler) {
    name = to_lower(std::move(name));
    for (auto &x : cmds_) {
      if (x.name == name) { x.handler = std::move(handler); return; }
    }
    cmds_.push_back({name, std::move(handler)});
    std::sort(cmds_.begin(), cmds_.end(),
              [](const Cmd& a, const Cmd& b){ return a.name < b.name; });
  }

  int run_line(const std::string& line) const {
    auto argv = split_argv(line);
    if (argv.empty()) return 0;
    std::string key = to_lower(argv[0]);
    for (const auto& c : cmds_) {
      if (c.name == key) return c.handler(argv);
    }
    return err_not_found;
  }

  static constexpr int err_not_found = 127;

private:
  struct Cmd {
    std::string name;
    Handler handler;
  };
  std::vector<Cmd> cmds_;
};

// ------------------------------ WifiManager ------------------------------

class WifiManager {
public:
  struct ScanEntry {
    std::string ssid;
    int32_t rssi = 0;
    uint8_t enc = 0;
    int32_t channel = 0;
  };

  explicit WifiManager(std::string nvs_ns = "wifi") : ns_(std::move(nvs_ns)) {}

  bool load_creds() {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READONLY, &h) != ESP_OK) return false;
    ssid_ = nvs_get_str_or_empty(h, "ssid");
    pass_ = nvs_get_str_or_empty(h, "pass");
    nvs_close(h);
    return !ssid_.empty();
  }

  bool save_creds(const std::string& ssid, const std::string& pass) {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READWRITE, &h) != ESP_OK) return false;
    (void)nvs_set_str(h, "ssid", ssid.c_str());
    (void)nvs_set_str(h, "pass", pass.c_str());
    (void)nvs_commit(h);
    nvs_close(h);
    ssid_ = ssid;
    pass_ = pass;
    return true;
  }

  bool clear_creds() {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READWRITE, &h) != ESP_OK) return false;
    (void)nvs_erase_key(h, "ssid");
    (void)nvs_erase_key(h, "pass");
    (void)nvs_commit(h);
    nvs_close(h);
    ssid_.clear();
    pass_.clear();
    return true;
  }

  const std::string& ssid() const { return ssid_; }
  const std::string& pass() const { return pass_; }
  bool has_creds() const { return !ssid_.empty(); }

  int scan(int max_results = 15) {
    last_scan_.clear();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true, true);
    delay(80);

    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) return n;

    for (int i = 0; i < n && (int)last_scan_.size() < max_results; i++) {
      ScanEntry e;
      e.ssid = WiFi.SSID(i).c_str();
      e.rssi = WiFi.RSSI(i);
      e.enc = (uint8_t)WiFi.encryptionType(i);
      e.channel = WiFi.channel(i);
      last_scan_.push_back(std::move(e));
    }
    return (int)last_scan_.size();
  }

  const std::vector<ScanEntry>& last_scan() const { return last_scan_; }

  bool choose_from_scan(int index_1based, const std::string& pass) {
    if (index_1based <= 0 || index_1based > (int)last_scan_.size()) return false;
    const auto& e = last_scan_[index_1based - 1];
    return save_creds(e.ssid, pass);
  }

  bool connect_using(const std::string& ssid, const std::string& pass, uint32_t timeout_ms = 15000) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    delay(80);

    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
      delay(120);
    }
    return WiFi.status() == WL_CONNECTED;
  }

  bool connect_stored(uint32_t timeout_ms = 15000) {
    if (!has_creds()) return false;
    return connect_using(ssid_, pass_, timeout_ms);
  }

  void disconnect() { WiFi.disconnect(true, true); }

  std::string status_line() const {
    wl_status_t st = WiFi.status();
    const char* s = "unknown";
    switch (st) {
      case WL_IDLE_STATUS: s = "idle"; break;
      case WL_NO_SSID_AVAIL: s = "no-ssid"; break;
      case WL_SCAN_COMPLETED: s = "scan-done"; break;
      case WL_CONNECTED: s = "connected"; break;
      case WL_CONNECT_FAILED: s = "connect-failed"; break;
      case WL_CONNECTION_LOST: s = "lost"; break;
      case WL_DISCONNECTED: s = "disconnected"; break;
      default: break;
    }

    String ip = WiFi.localIP().toString();
    String gw = WiFi.gatewayIP().toString();
    String ss = WiFi.SSID();

    char buf[256];
    snprintf(buf, sizeof(buf),
             "wifi=%s ssid=%s ip=%s gw=%s rssi=%d",
             s,
             ss.length() ? ss.c_str() : "-",
             ip.c_str(),
             gw.c_str(),
             (int)WiFi.RSSI());
    return buf;
  }

private:
  std::string ns_;
  std::string ssid_;
  std::string pass_;
  std::vector<ScanEntry> last_scan_;

  static std::string nvs_get_str_or_empty(nvs_handle_t h, const char* key) {
    size_t required = 0;
    if (nvs_get_str(h, key, nullptr, &required) != ESP_OK || required == 0) return {};
    std::string tmp(required, '\0');
    if (nvs_get_str(h, key, tmp.data(), &required) != ESP_OK) return {};
    if (!tmp.empty() && tmp.back() == '\0') tmp.pop_back();
    return tmp;
  }
};

// ------------------------------ WebController ------------------------------

class WebController {
public:
  WebController() = default;

  void set_default_port(uint16_t p) { default_port_ = p; }

  bool is_running() const { return running_; }
  uint16_t port() const { return port_; }

  bool start(uint16_t port = 0) {
    if (running_) return true;
    if (port == 0) port = default_port_;
    if (!WiFi.isConnected()) return false;

    port_ = port;
    server_ = std::make_unique<WebServer>(port_);

    setup_routes(*server_);

    server_->begin();
    running_ = true;

    if (task_ == nullptr) {
      xTaskCreatePinnedToCore(&WebController::task_thunk,
                              "websrv",
                              4096,
                              this,
                              1,
                              &task_,
                              0);
    }
    return true;
  }

  void stop() {
    running_ = false;
    if (server_) {
      server_->stop();
      server_.reset();
    }
  }

  std::string status_line() const {
    char buf[128];
    snprintf(buf, sizeof(buf), "web=%s port=%u",
             running_ ? "running" : "stopped",
             (unsigned)port_);
    return buf;
  }

private:
  uint16_t default_port_ = 80;
  uint16_t port_ = 80;
  std::unique_ptr<WebServer> server_;
  volatile bool running_ = false;
  TaskHandle_t task_ = nullptr;

  static void task_thunk(void* arg) {
    static_cast<WebController*>(arg)->task_loop();
  }

  void task_loop() {
    while (true) {
      if (running_ && server_) {
        server_->handleClient();
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  void setup_routes(WebServer& s) {
    s.on("/", HTTP_GET, [this, &s]() {
      const String ip   = WiFi.localIP().toString();
      const String ssid = WiFi.SSID();
    
      String html;
      html.reserve(900);
    
      html += "<!doctype html><meta charset=utf-8>"
              "<meta name=viewport content='width=device-width,initial-scale=1'>"
              "<title>ESP32S3</title>"
              "<style>"
              ":root{color-scheme:dark}"
              "body{margin:0;font:16px system-ui;background:#0b0f14;color:#e6edf3}"
              "main{max-width:640px;margin:0 auto;padding:16px}"
              "section{background:#0f1620;border:1px solid #223044;border-radius:12px;padding:14px}"
              "h1{margin:0 0 10px;font-size:18px}"
              "p{margin:8px 0;color:#c9d6e4}"
              "a{color:#8ab4ff;text-decoration:none}a:hover{text-decoration:underline}"
              "code{background:#111b28;border:1px solid #223044;border-radius:8px;padding:2px 6px}"
              "</style>"
              "<main><section><h1>ESP32S3</h1>";
    
      html += "<p>SSID: <code>" + ssid + "</code><br>IP: <code>" + ip + "</code></p>"
              "<p><a href=/status>/status</a></p>"
              "<p style='margin-top:12px;color:#9fb2c7;font-size:13px'>Use serial console for commands.</p>"
              "</section></main>";
    
      s.send(200, "text/html", html);
    });

    s.on("/status", HTTP_GET, [this, &s]() {
      String ip = WiFi.localIP().toString();
      String ssid = WiFi.SSID();
      uint32_t heap = esp_get_free_heap_size();
      uint64_t us = esp_timer_get_time();

      String json;
      json.reserve(256);
      json += "{";
      json += "\"ssid\":\"" + ssid + "\",";
      json += "\"ip\":\"" + ip + "\",";
      json += "\"heap_free\":" + String(heap) + ",";
      json += "\"uptime_s\":" + String((double)us * 1e-6, 3) + ",";
      json += "\"web_port\":" + String(port_) + "";
      json += "}";
      s.send(200, "application/json", json);
    });

    s.onNotFound([&s]() {
      s.send(404, "text/plain", "404");
    });
  }
};

// ------------------------------ Console (editor + io) ------------------------------

class Console {
public:
  struct Config {
    std::string prompt = "esp32s3> ";
    bool ansi = true;
    bool putty_bracket_fallback = true;
    size_t max_line = 256;
    uint32_t esc_seq_timeout_ms = 2500;
    uint32_t bracket_peek_timeout_ms = 60;
  };

  Console(Config cfg, ConsoleHistory& hist, CommandsRegistry& reg)
  : cfg_(std::move(cfg)), hist_(hist), reg_(reg) {}

  void begin(uint32_t baud = 115200) {
    Serial.begin(baud);
    Serial.setRxBufferSize(4096);

    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 1500) delay(10);

    crlf();
    line("Ready. Type 'help'.");
  }

  void loop_once() {
    std::string in;
    if (!read_line(in)) return;

    trim_in_place(in);
    if (in.empty()) return;

    hist_.add(in);

    crlf();
    int rc = reg_.run_line(in);
    if (rc == CommandsRegistry::err_not_found) {
      line("Unknown command. Type 'help'.");
    } else if (rc != 0) {
      printf("Error: %d\n", rc);
    }
  }

  void crlf() { Serial.print("\r\n"); }
  void line(const char* s) { if (s) Serial.print(s); crlf(); }
  void line(const std::string& s) { Serial.print(s.c_str()); crlf(); }

  void printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    for (int i = 0; i < n && i < (int)sizeof(buf); i++) {
      char c = buf[i];
      if (c == '\n') Serial.write('\r');
      Serial.write((uint8_t)c);
    }
  }

private:
  enum class KeyType {
    Char, Enter,
    Backspace, Delete,
    Left, Right, Up, Down,
    Home, End,
    CtrlA, CtrlE, CtrlU, CtrlK, CtrlW, CtrlL, CtrlC,
    Unknown
  };
  struct Key { KeyType type; char ch; };

  Config cfg_;
  ConsoleHistory& hist_;
  CommandsRegistry& reg_;

  std::string buf_;
  size_t cursor_ = 0;

  enum class EscState { Idle, GotEsc, CSI, SS3 };
  EscState esc_state_ = EscState::Idle;
  uint32_t esc_deadline_ = 0;
  char esc_params_[24] = {0};
  int esc_p_ = 0;

  bool read_byte_nonblocking(uint8_t& out) {
    if (Serial.available() == 0) return false;
    int v = Serial.read();
    if (v < 0) return false;
    out = (uint8_t)v;
    return true;
  }

  uint8_t read_byte_blocking() {
    while (Serial.available() == 0) delay(1);
    int v = Serial.read();
    return (v < 0) ? 0 : (uint8_t)v;
  }

  bool read_byte_until(uint8_t& out, uint32_t deadline_ms_abs) {
    while (Serial.available() == 0) {
      if ((int32_t)(millis() - deadline_ms_abs) >= 0) return false;
      delay(1);
    }
    int v = Serial.read();
    if (v < 0) return false;
    out = (uint8_t)v;
    return true;
  }

  static Key map_arrow_final(uint8_t final) {
    switch (final) {
      case 'A': return {KeyType::Up, 0};
      case 'B': return {KeyType::Down, 0};
      case 'C': return {KeyType::Right, 0};
      case 'D': return {KeyType::Left, 0};
      case 'H': return {KeyType::Home, 0};
      case 'F': return {KeyType::End, 0};
      default:  return {KeyType::Unknown, 0};
    }
  }

  Key read_key() {
    while (true) {
      if (esc_state_ != EscState::Idle) {
        if ((int32_t)(millis() - esc_deadline_) >= 0) {
          esc_state_ = EscState::Idle;
          esc_p_ = 0;
          esc_params_[0] = 0;
          continue;
        }

        uint8_t b;
        if (!read_byte_nonblocking(b)) { delay(1); continue; }

        if (esc_state_ == EscState::GotEsc) {
          if (b == '[') { esc_state_ = EscState::CSI; esc_p_ = 0; esc_params_[0] = 0; continue; }
          if (b == 'O') { esc_state_ = EscState::SS3; continue; }
          esc_state_ = EscState::Idle;
          continue;
        }

        if (esc_state_ == EscState::SS3) {
          esc_state_ = EscState::Idle;
          return map_arrow_final(b);
        }

        if (b >= 0x40 && b <= 0x7E) {
          Key k = map_arrow_final(b);
          if (b == '~') {
            int code = atoi(esc_params_);
            if (code == 3) k = {KeyType::Delete, 0};
            else if (code == 1 || code == 7) k = {KeyType::Home, 0};
            else if (code == 4 || code == 8) k = {KeyType::End, 0};
            else k = {KeyType::Unknown, 0};
          }
          esc_state_ = EscState::Idle;
          esc_p_ = 0;
          esc_params_[0] = 0;
          return k;
        } else {
          if (esc_p_ + 1 < (int)sizeof(esc_params_)) {
            esc_params_[esc_p_++] = (char)b;
            esc_params_[esc_p_] = 0;
          }
          continue;
        }
      }

      uint8_t b = read_byte_blocking();

      if (b == '\r' || b == '\n') return {KeyType::Enter, 0};
      if (b == 0x7F || b == '\b') return {KeyType::Backspace, 0};

      if (b < 0x20) {
        switch (b) {
          case 0x01: return {KeyType::CtrlA, 0};
          case 0x05: return {KeyType::CtrlE, 0};
          case 0x15: return {KeyType::CtrlU, 0};
          case 0x0B: return {KeyType::CtrlK, 0};
          case 0x17: return {KeyType::CtrlW, 0};
          case 0x0C: return {KeyType::CtrlL, 0};
          case 0x03: return {KeyType::CtrlC, 0};
          default:   return {KeyType::Unknown, 0};
        }
      }

      if (b == 0x1B) {
        esc_state_ = EscState::GotEsc;
        esc_deadline_ = millis() + cfg_.esc_seq_timeout_ms;
        esc_p_ = 0;
        esc_params_[0] = 0;
        continue;
      }

      // PuTTY fallback: treat bare [A etc as arrows if ESC is missing.
      if (cfg_.putty_bracket_fallback && b == '[') {
        uint8_t next = 0;
        uint32_t dl = millis() + cfg_.bracket_peek_timeout_ms;
        if (read_byte_until(next, dl)) {
          if (next == 'A') return {KeyType::Up, 0};
          if (next == 'B') return {KeyType::Down, 0};
          if (next == 'C') return {KeyType::Right, 0};
          if (next == 'D') return {KeyType::Left, 0};
          if (next == 'H') return {KeyType::Home, 0};
          if (next == 'F') return {KeyType::End, 0};

          if (next >= '0' && next <= '9') {
            char tmp[8] = { (char)next, 0 };
            int tp = 1;
            uint8_t c = 0;
            while (tp + 1 < (int)sizeof(tmp) && read_byte_until(c, dl)) {
              if (c == '~') { tmp[tp] = 0; break; }
              if (c < '0' || c > '9') break;
              tmp[tp++] = (char)c;
              tmp[tp] = 0;
            }
            int code = atoi(tmp);
            if (code == 3) return {KeyType::Delete, 0};
            if (code == 1 || code == 7) return {KeyType::Home, 0};
            if (code == 4 || code == 8) return {KeyType::End, 0};
          }
          return {KeyType::Char, '['};
        }
        return {KeyType::Char, '['};
      }

      if (b >= 0x20) return {KeyType::Char, (char)b};
      return {KeyType::Unknown, 0};
    }
  }

  void redraw() {
    if (!cfg_.ansi) return;
    Serial.write('\r');
    Serial.write("\x1b[2K");
    Serial.print(cfg_.prompt.c_str());
    if (!buf_.empty()) Serial.write((const uint8_t*)buf_.data(), buf_.size());
    size_t back = buf_.size() - cursor_;
    if (back > 0) {
      char tmp[24];
      snprintf(tmp, sizeof(tmp), "\x1b[%uD", (unsigned)back);
      Serial.write(tmp);
    }
  }

  void set_buffer(const std::string& s) {
    buf_ = s;
    if (buf_.size() >= cfg_.max_line) buf_.resize(cfg_.max_line - 1);
    cursor_ = buf_.size();
    redraw();
  }

  void insert_char(char c) {
    if (buf_.size() + 1 >= cfg_.max_line) return;
    if (!cfg_.ansi) {
      buf_.push_back(c);
      cursor_ = buf_.size();
      Serial.write((uint8_t)c);
      return;
    }
    if (cursor_ == buf_.size()) {
      buf_.push_back(c);
      cursor_++;
      Serial.write((uint8_t)c);
    } else {
      buf_.insert(buf_.begin() + (long)cursor_, c);
      cursor_++;
      redraw();
    }
  }

  void backspace() {
    if (cursor_ == 0) return;
    buf_.erase(buf_.begin() + (long)cursor_ - 1);
    cursor_--;
    if (!cfg_.ansi) Serial.write("\b \b");
    else redraw();
  }

  void del() {
    if (cursor_ >= buf_.size()) return;
    buf_.erase(buf_.begin() + (long)cursor_);
    redraw();
  }

  void left() {
    if (cursor_ == 0) return;
    cursor_--;
    if (cfg_.ansi) Serial.write("\x1b[1D");
  }

  void right() {
    if (cursor_ >= buf_.size()) return;
    cursor_++;
    if (cfg_.ansi) Serial.write("\x1b[1C");
  }

  void home() { cursor_ = 0; redraw(); }
  void end()  { cursor_ = buf_.size(); redraw(); }

  void kill_to_start() {
    if (cursor_ == 0) return;
    buf_.erase(0, cursor_);
    cursor_ = 0;
    redraw();
  }

  void kill_to_end() {
    if (cursor_ >= buf_.size()) return;
    buf_.erase(cursor_);
    redraw();
  }

  void delete_prev_word() {
    if (cursor_ == 0) return;
    size_t i = cursor_;
    while (i > 0 && is_space(buf_[i - 1])) i--;
    while (i > 0 && !is_space(buf_[i - 1])) i--;
    buf_.erase(i, cursor_ - i);
    cursor_ = i;
    redraw();
  }

  bool read_line(std::string& out_line) {
    buf_.clear();
    cursor_ = 0;
    hist_.reset_nav();

    Serial.print(cfg_.prompt.c_str());

    while (true) {
      Key k = read_key();

      if (k.type == KeyType::Enter) {
        crlf();
        out_line = buf_;
        return true;
      }

      if (k.type == KeyType::Char) { insert_char(k.ch); continue; }

      switch (k.type) {
        case KeyType::Backspace: backspace(); break;
        case KeyType::Delete:    del(); break;
        case KeyType::Left:      left(); break;
        case KeyType::Right:     right(); break;
        case KeyType::Home:      home(); break;
        case KeyType::End:       end(); break;

        case KeyType::Up: {
          const std::string* h = hist_.older(buf_);
          if (h) set_buffer(*h);
        } break;

        case KeyType::Down: {
          const std::string* h = hist_.newer();
          if (h) set_buffer(*h);
        } break;

        case KeyType::CtrlA: home(); break;
        case KeyType::CtrlE: end(); break;
        case KeyType::CtrlU: kill_to_start(); break;
        case KeyType::CtrlK: kill_to_end(); break;
        case KeyType::CtrlW: delete_prev_word(); break;

        case KeyType::CtrlL:
          if (cfg_.ansi) { Serial.write("\x1b[2J\x1b[H"); redraw(); }
          break;

        case KeyType::CtrlC:
          crlf();
          buf_.clear();
          cursor_ = 0;
          Serial.print(cfg_.prompt.c_str());
          break;

        default:
          break;
      }
    }
  }
};

// ------------------------------ ping (TCP timing) ------------------------------

static bool resolve_host(const std::string& host, IPAddress& out_ip) {
  if (!WiFi.isConnected()) return false;
  return WiFi.hostByName(host.c_str(), out_ip);
}

static int tcp_connect_ms(const IPAddress& ip, uint16_t port, uint32_t timeout_ms) {
  WiFiClient client;
  uint32_t t0 = millis();
  bool ok = client.connect(ip, port, timeout_ms);
  uint32_t dt = millis() - t0;
  client.stop();
  return ok ? (int)dt : -1;
}

// ------------------------------ globals + setup ------------------------------

static ConsoleHistory g_history("console");
static CommandsRegistry g_cmds;
static WifiManager g_wifi("wifi");
static WebController g_web;
static Console* g_console = nullptr;

static constexpr bool kAutoConnectWiFiOnBoot = true;
static constexpr bool kAutoStartWebOnWiFi = true;
static constexpr uint16_t kDefaultWebPort = 80;

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    (void)nvs_flash_erase();
    (void)nvs_flash_init();
  }
}

static void register_commands() {
  g_cmds.add("help", [](const std::vector<std::string>&) -> int {
    if (!g_console) return 1;
    g_console->line("Commands: help history uptime chip free reboot wifi ping web");
    g_console->line("WiFi: wifi scan|choose|set|connect|status|disconnect|clear");
    g_console->line("Web:  web start [port] | web stop | web status  (GET /, /status)");
    return 0;
  });

  g_cmds.add("history", [](const std::vector<std::string>& args) -> int {
    if (!g_console) return 1;
    int n = g_history.size();
    if (args.size() == 2) n = clampi(atoi(args[1].c_str()), 0, g_history.size());
    auto lines = g_history.last_n(n);
    int idx0 = g_history.size() - (int)lines.size() + 1;
    for (size_t i = 0; i < lines.size(); i++) {
      g_console->printf("%4d  %s\n", idx0 + (int)i, lines[i].c_str());
    }
    return 0;
  });

  g_cmds.add("uptime", [](const std::vector<std::string>&) -> int {
    if (!g_console) return 1;
    double s = (double)esp_timer_get_time() * 1e-6;
    g_console->printf("uptime=%.3fs\n", s);
    return 0;
  });

  g_cmds.add("chip", [](const std::vector<std::string>&) -> int {
    if (!g_console) return 1;
    esp_chip_info_t info;
    esp_chip_info(&info);
    const char* model = "unknown";
    if (info.model == CHIP_ESP32S3) model = "S3";
    else if (info.model == CHIP_ESP32S2) model = "S2";
    else if (info.model == CHIP_ESP32C3) model = "C3";
    else if (info.model == CHIP_ESP32C6) model = "C6";
    g_console->printf("ESP32-%s cores=%d rev=%d flash=%u\n",
                      model, info.cores, info.revision, (unsigned)ESP.getFlashChipSize());
    return 0;
  });

  g_cmds.add("free", [](const std::vector<std::string>&) -> int {
    if (!g_console) return 1;
    g_console->printf("heap_free=%u heap_min_free=%u\n",
                      (unsigned)esp_get_free_heap_size(),
                      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    return 0;
  });

  g_cmds.add("reboot", [](const std::vector<std::string>&) -> int {
    if (!g_console) return 1;
    g_console->line("restarting...");
    delay(50);
    ESP.restart();
    return 0;
  });

  g_cmds.add("wifi", [](const std::vector<std::string>& args) -> int {
    if (!g_console) return 1;
    if (args.size() < 2) {
      g_console->line("usage: wifi scan|choose|set|connect|status|disconnect|clear");
      return 2;
    }
    std::string sub = to_lower(args[1]);

    if (sub == "scan") {
      int n = 15;
      if (args.size() >= 3) n = clampi(atoi(args[2].c_str()), 1, 30);
      g_console->line("scanning...");
      int found = g_wifi.scan(n);
      if (found <= 0) { g_console->line("no networks"); return 0; }
      const auto& v = g_wifi.last_scan();
      for (int i = 0; i < (int)v.size(); i++) {
        g_console->printf("%2d) %-24s rssi=%4d ch=%2d enc=%u\n",
                          i + 1, v[i].ssid.c_str(), (int)v[i].rssi, (int)v[i].channel, (unsigned)v[i].enc);
      }
      g_console->line("use: wifi choose <index> <pass>");
      return 0;
    }

    if (sub == "choose") {
      if (args.size() < 3) { g_console->line("usage: wifi choose <index> [pass]"); return 2; }
      int idx = atoi(args[2].c_str());
      std::string pass = (args.size() >= 4) ? args[3] : "";
      if (!g_wifi.choose_from_scan(idx, pass)) { g_console->line("bad index (scan first)"); return 2; }
      g_console->printf("saved ssid=%s (pass %s)\n", g_wifi.ssid().c_str(), pass.empty() ? "empty" : "set");
      return 0;
    }

    if (sub == "set") {
      if (args.size() != 4) { g_console->line("usage: wifi set \"ssid\" \"pass\""); return 2; }
      if (!g_wifi.save_creds(args[2], args[3])) { g_console->line("save failed"); return 3; }
      g_console->printf("saved ssid=%s (pass %s)\n", g_wifi.ssid().c_str(), args[3].empty() ? "empty" : "set");
      return 0;
    }

    if (sub == "connect") {
      bool ok = false;
      if (args.size() == 2) {
        g_wifi.load_creds();
        if (!g_wifi.has_creds()) { g_console->line("no saved ssid; use wifi set or wifi choose"); return 2; }
        g_console->printf("connecting ssid=%s ...\n", g_wifi.ssid().c_str());
        ok = g_wifi.connect_stored();
      } else if (args.size() == 4) {
        g_console->printf("connecting ssid=%s ...\n", args[2].c_str());
        ok = g_wifi.connect_using(args[2], args[3]);
        if (ok) (void)g_wifi.save_creds(args[2], args[3]);
      } else {
        g_console->line("usage: wifi connect [\"ssid\" \"pass\"]");
        return 2;
      }
      g_console->line(ok ? "connected" : "connect failed");
      if (ok) {
        g_console->line(g_wifi.status_line());
        if (kAutoStartWebOnWiFi && !g_web.is_running()) {
          bool ws = g_web.start(kDefaultWebPort);
          if (ws) g_console->printf("web started: http://%s/\n", WiFi.localIP().toString().c_str());
          else g_console->line("web start failed");
        }
      }
      return ok ? 0 : 4;
    }

    if (sub == "status") {
      g_console->line(g_wifi.status_line());
      g_wifi.load_creds();
      if (g_wifi.has_creds()) g_console->printf("saved_ssid=%s\n", g_wifi.ssid().c_str());
      else g_console->line("saved_ssid=-");
      return 0;
    }

    if (sub == "disconnect") {
      g_wifi.disconnect();
      g_console->line("disconnected");
      return 0;
    }

    if (sub == "clear") {
      g_wifi.clear_creds();
      g_console->line("cleared saved creds");
      return 0;
    }

    g_console->line("usage: wifi scan|choose|set|connect|status|disconnect|clear");
    return 2;
  });

  g_cmds.add("ping", [](const std::vector<std::string>& args) -> int {
    if (!g_console) return 1;
    if (args.size() < 2) { g_console->line("usage: ping <host> [count] [timeout_ms]"); return 2; }
    if (!WiFi.isConnected()) { g_console->line("wifi not connected"); return 3; }

    std::string host = args[1];
    int count = (args.size() >= 3) ? clampi(atoi(args[2].c_str()), 1, 20) : 4;
    int timeout_ms = (args.size() >= 4) ? clampi(atoi(args[3].c_str()), 100, 8000) : 1000;

    IPAddress ip;
    if (!resolve_host(host, ip)) { g_console->line("dns failed"); return 4; }

    g_console->printf("ping %s (%s) count=%d timeout=%dms\n",
                      host.c_str(), ip.toString().c_str(), count, timeout_ms);

    int ok = 0;
    for (int i = 0; i < count; i++) {
      int ms = tcp_connect_ms(ip, 80, (uint32_t)timeout_ms);
      if (ms < 0) ms = tcp_connect_ms(ip, 443, (uint32_t)timeout_ms);

      if (ms >= 0) { ok++; g_console->printf("%d: %dms\n", i + 1, ms); }
      else         { g_console->printf("%d: timeout\n", i + 1); }

      delay(60);
    }
    g_console->printf("ok=%d/%d\n", ok, count);
    g_console->line("note: TCP connect timing (not ICMP).");
    return (ok > 0) ? 0 : 6;
  });

  g_cmds.add("web", [](const std::vector<std::string>& args) -> int {
    if (!g_console) return 1;
    if (args.size() < 2) {
      g_console->line("usage: web start [port] | web stop | web status");
      return 2;
    }
    std::string sub = to_lower(args[1]);

    if (sub == "status") {
      g_console->line(g_web.status_line());
      if (g_web.is_running()) g_console->printf("url: http://%s:%u/\n",
                                                WiFi.localIP().toString().c_str(),
                                                (unsigned)g_web.port());
      return 0;
    }

    if (sub == "stop") {
      g_web.stop();
      g_console->line("web stopped");
      return 0;
    }

    if (sub == "start") {
      uint16_t port = kDefaultWebPort;
      if (args.size() >= 3) port = (uint16_t)clampi(atoi(args[2].c_str()), 1, 65535);
      if (!WiFi.isConnected()) { g_console->line("wifi not connected"); return 3; }
      bool ok = g_web.start(port);
      if (ok) g_console->printf("web started: http://%s:%u/\n", WiFi.localIP().toString().c_str(), (unsigned)port);
      else g_console->line("web start failed");
      return ok ? 0 : 4;
    }

    g_console->line("usage: web start [port] | web stop | web status");
    return 2;
  });
}

void setup() {
  init_nvs();
  (void)g_history.load();
  (void)g_wifi.load_creds();

  // WiFi off until we decide to connect
  WiFi.mode(WIFI_OFF);

  register_commands();

  Console::Config cfg;
  cfg.prompt = "esp32s3> ";
  cfg.ansi = true;
  cfg.putty_bracket_fallback = true;
  cfg.max_line = 256;

  static Console console(cfg, g_history, g_cmds);
  g_console = &console;

  g_web.set_default_port(kDefaultWebPort);

  console.begin(115200);

  // Auto-connect WiFi on boot if creds exist
  if (kAutoConnectWiFiOnBoot) {
    g_wifi.load_creds();
    if (g_wifi.has_creds()) {
      console.printf("auto wifi connect ssid=%s ...\n", g_wifi.ssid().c_str());
      bool ok = g_wifi.connect_stored(15000);
      console.line(ok ? "connected" : "connect failed");
      if (ok) {
        console.line(g_wifi.status_line());
        if (kAutoStartWebOnWiFi) {
          bool ws = g_web.start(kDefaultWebPort);
          if (ws) console.printf("web started: http://%s/\n", WiFi.localIP().toString().c_str());
          else console.line("web start failed");
        }
      }
    }
  }
}

void loop() {
  if (g_console) g_console->loop_once();  // blocks on input; web server runs in its own task
  delay(1);
}
