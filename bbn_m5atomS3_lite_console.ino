/*
  Arduino-ESP32 3.3.5 console for PuTTY over USB CDC/Serial-JTAG:
   - Modern C++ structure: ConsoleHistory, CommandsRegistry, Console
   - ANSI/VT100 line editor with mid-line cursor, insert/delete
   - Up/Down history (PuTTY ESC [ A / ESC [ B and ESC O A / ESC O B)
   - Persistent history in NVS (survives reboot)
   - Clean help: help, help <cmd>

  Board options recommended:
    USBMode=hwcdc, CDCOnBoot=cdc
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

#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <string>
#include <vector>
#include <functional>
#include <algorithm>

// --------------------------------- Utilities ---------------------------------

static inline bool is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static inline std::string to_lower(std::string s) {
  for (char &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

// Basic shell-like argv parsing: supports quotes ("...") and backslash escapes.
static std::vector<std::string> split_argv(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  bool in_quotes = false;
  bool esc = false;

  auto push_cur = [&]() {
    if (!cur.empty()) out.push_back(cur);
    cur.clear();
  };

  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (esc) {
      cur.push_back(c);
      esc = false;
      continue;
    }
    if (c == '\\') { esc = true; continue; }
    if (c == '"') { in_quotes = !in_quotes; continue; }

    if (!in_quotes && is_space(c)) {
      push_cur();
      while (i + 1 < line.size() && is_space(line[i + 1])) i++;
      continue;
    }
    cur.push_back(c);
  }
  push_cur();
  return out;
}

// -------------------------------- ConsoleHistory --------------------------------

class ConsoleHistory {
public:
  static constexpr int kMaxItems = 50;
  static constexpr size_t kMaxLine = 256;

  explicit ConsoleHistory(std::string nvs_ns = "console")
  : ns_(std::move(nvs_ns)) {
    for (auto &s : items_) s.reserve(64);
  }

  bool load() {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READONLY, &h) != ESP_OK) return false;

    int32_t head = 0, count = 0;
    (void)nvs_get_i32(h, "h_head", &head);
    (void)nvs_get_i32(h, "h_count", &count);

    head_  = clamp_int(head,  0, kMaxItems - 1);
    count_ = clamp_int(count, 0, kMaxItems);

    items_.assign(kMaxItems, "");
    for (int i = 0; i < kMaxItems; i++) {
      char key[8];
      snprintf(key, sizeof(key), "h%02d", i);

      size_t required = 0;
      if (nvs_get_str(h, key, nullptr, &required) == ESP_OK && required > 0 && required <= kMaxLine) {
        std::string tmp(required, '\0');
        if (nvs_get_str(h, key, tmp.data(), &required) == ESP_OK) {
          // nvs includes null terminator in required
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

    // avoid duplicates of last entry
    if (count_ > 0) {
      int last = (head_ - 1 + kMaxItems) % kMaxItems;
      if (items_[last] == line) {
        reset_nav();
        return;
      }
    }

    items_[head_] = truncate(line, kMaxLine - 1);
    head_ = (head_ + 1) % kMaxItems;
    if (count_ < kMaxItems) count_++;
    reset_nav();

    persist_last();
  }

  // Navigation: "older" means earlier (Up arrow)
  const std::string* older(const std::string& current_line) {
    if (count_ == 0) return nullptr;

    if (nav_ == 0) scratch_ = current_line;
    if (nav_ < count_) nav_++;

    return get_recent(nav_);
  }

  // Navigation: "newer" means later (Down arrow)
  const std::string* newer() {
    if (count_ == 0) return nullptr;
    if (nav_ > 0) nav_--;
    if (nav_ == 0) return &scratch_;
    return get_recent(nav_);
  }

  void reset_nav() { nav_ = 0; scratch_.clear(); }

  int size() const { return count_; }

  // For "history" command: return from oldest->newest for last N entries
  std::vector<std::string> last_n(int n) const {
    n = clamp_int(n, 0, count_);
    std::vector<std::string> out;
    out.reserve(n);

    // oldest among last n is k = n, newest is k = 1
    for (int k = n; k >= 1; k--) {
      const std::string* s = get_recent(k);
      if (s && !s->empty()) out.push_back(*s);
    }
    return out;
  }

private:
  std::string ns_;
  std::vector<std::string> items_ = std::vector<std::string>(kMaxItems);
  int head_ = 0;
  int count_ = 0;

  // navigation state
  int nav_ = 0;
  std::string scratch_;

  static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
  }

  static std::string truncate(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    return s.substr(0, max_len);
  }

  const std::string* get_recent(int k) const {
    // k=1 newest, k=count oldest
    if (k <= 0 || k > count_) return nullptr;
    int idx = (head_ - k + kMaxItems) % kMaxItems;
    return &items_[idx];
  }

  void persist_last() {
    nvs_handle_t h;
    if (nvs_open(ns_.c_str(), NVS_READWRITE, &h) != ESP_OK) return;

    // the newest item is at head_-1
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

// -------------------------------- CommandsRegistry --------------------------------

class CommandsRegistry {
public:
  using Handler = std::function<int(const std::vector<std::string>& args)>;

  struct Command {
    std::string name;
    std::string help;
    std::string usage;
    Handler handler;
  };

  bool add(std::string name, std::string help, std::string usage, Handler handler) {
    if (name.empty() || !handler) return false;
    name = to_lower(name);

    // replace if exists
    for (auto &c : cmds_) {
      if (c.name == name) {
        c.help = std::move(help);
        c.usage = std::move(usage);
        c.handler = std::move(handler);
        return true;
      }
    }
    cmds_.push_back(Command{std::move(name), std::move(help), std::move(usage), std::move(handler)});
    std::sort(cmds_.begin(), cmds_.end(), [](const Command& a, const Command& b){ return a.name < b.name; });
    return true;
  }

  const Command* find(const std::string& name) const {
    std::string key = to_lower(name);
    for (const auto &c : cmds_) if (c.name == key) return &c;
    return nullptr;
  }

  int run_line(const std::string& line) const {
    auto argv = split_argv(line);
    if (argv.empty()) return 0;

    const Command* cmd = find(argv[0]);
    if (!cmd) return err_not_found;

    // args passed include argv[0] as command name (bash-like convention optional).
    return cmd->handler(argv);
  }

  std::vector<Command> list() const { return cmds_; }

  static constexpr int err_not_found = 127;

private:
  std::vector<Command> cmds_;
};

// -------------------------------- Console (I/O + line editor) --------------------------------

class Console {
public:
  struct Config {
    std::string prompt = "esp32s3> ";
    bool ansi = true;            // PuTTY: true
    size_t max_line = 256;       // editor buffer cap
    uint32_t esc_timeout_ms = 80; // CSI parse timeout
  };

  Console(Config cfg, ConsoleHistory& hist, CommandsRegistry& reg)
  : cfg_(std::move(cfg)), hist_(hist), reg_(reg) {}

  void begin(uint32_t baud = 115200) {
    Serial.begin(baud);
    Serial.setRxBufferSize(2048);

    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 1500) delay(10);

    print_crlf();
    print_line("Ready. Type 'help' or 'help <cmd>'.");
  }

  void loop_once() {
    std::string line;
    if (!read_line(line)) return;

    // trim
    trim_in_place(line);
    if (line.empty()) return;

    hist_.add(line);

    // Always begin command output on clean line
    print_crlf();

    int rc = reg_.run_line(line);
    if (rc == CommandsRegistry::err_not_found) {
      print_line("Unknown command. Type 'help'.");
    } else if (rc != 0) {
      printf("Error: %d", rc);
      print_crlf();
    }
  }

  // -------- output helpers (CRLF correct for PuTTY) --------

  void print(const char* s) { if (s) Serial.print(s); }
  void print(const std::string& s) { Serial.print(s.c_str()); }

  void print_crlf() { Serial.print("\r\n"); }

  void print_line(const char* s) {
    if (s) Serial.print(s);
    print_crlf();
  }

  void printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    // Ensure CRLF on any '\n'
    for (int i = 0; i < n && i < (int)sizeof(buf); i++) {
      char c = buf[i];
      if (c == '\n') Serial.write('\r');
      Serial.write((uint8_t)c);
    }
  }

  void set_ansi(bool on) { cfg_.ansi = on; }
  bool ansi() const { return cfg_.ansi; }

private:
  enum class KeyType {
    Char,
    Enter,
    Backspace,
    Delete,
    Left, Right, Up, Down,
    Home, End,
    CtrlA, CtrlE,
    CtrlU, CtrlK,
    CtrlW,
    CtrlL,
    CtrlC,
    Unknown
  };

  struct Key {
    KeyType type = KeyType::Unknown;
    char ch = 0; // valid if Char
  };

  Config cfg_;
  ConsoleHistory& hist_;
  CommandsRegistry& reg_;

  // editor state
  std::string buf_;
  size_t cursor_ = 0;

  static void trim_in_place(std::string& s) {
    size_t a = 0;
    while (a < s.size() && is_space(s[a])) a++;
    size_t b = s.size();
    while (b > a && is_space(s[b - 1])) b--;
    s = s.substr(a, b - a);
  }

  // ------------ Read key from Serial, decode ANSI/VT100 ------------

  int read_byte_blocking() {
    while (Serial.available() == 0) delay(1);
    return Serial.read();
  }

  bool read_byte_with_timeout(uint8_t& out, uint32_t timeout_ms) {
    uint32_t t0 = millis();
    while (Serial.available() == 0) {
      if (millis() - t0 >= timeout_ms) return false;
      delay(1);
    }
    int v = Serial.read();
    if (v < 0) return false;
    out = (uint8_t)v;
    return true;
  }

  Key read_key() {
    int v = read_byte_blocking();
    if (v < 0) return {KeyType::Unknown, 0};
    uint8_t b = (uint8_t)v;

    // Enter
    if (b == '\r' || b == '\n') return {KeyType::Enter, 0};

    // Backspace / DEL
    if (b == 0x7F || b == '\b') return {KeyType::Backspace, 0};

    // Ctrl keys
    if (b < 0x20) {
      switch (b) {
        case 0x01: return {KeyType::CtrlA, 0}; // ^A
        case 0x05: return {KeyType::CtrlE, 0}; // ^E
        case 0x15: return {KeyType::CtrlU, 0}; // ^U
        case 0x0B: return {KeyType::CtrlK, 0}; // ^K
        case 0x17: return {KeyType::CtrlW, 0}; // ^W
        case 0x0C: return {KeyType::CtrlL, 0}; // ^L
        case 0x03: return {KeyType::CtrlC, 0}; // ^C
        default:   return {KeyType::Unknown, 0};
      }
    }

    // ESC sequences
    if (b == 0x1B) {
      // PuTTY sends: ESC [ A ... or ESC O A ...
      uint8_t b1 = 0;
      if (!read_byte_with_timeout(b1, cfg_.esc_timeout_ms)) return {KeyType::Unknown, 0};

      if (b1 == '[') {
        // CSI: ESC [ ... final
        // Read params (digits/;), then final byte in @-~
        char params[16] = {0};
        int p = 0;
        uint8_t c = 0;

        // Read until final
        while (true) {
          if (!read_byte_with_timeout(c, cfg_.esc_timeout_ms)) return {KeyType::Unknown, 0};
          if (c >= 0x40 && c <= 0x7E) break; // final
          if (p + 1 < (int)sizeof(params)) params[p++] = (char)c;
        }

        // Arrow / Home/End
        if (c == 'A') return {KeyType::Up, 0};
        if (c == 'B') return {KeyType::Down, 0};
        if (c == 'C') return {KeyType::Right, 0};
        if (c == 'D') return {KeyType::Left, 0};
        if (c == 'H') return {KeyType::Home, 0};
        if (c == 'F') return {KeyType::End, 0};

        // Tilde sequences like [3~ [1~ [4~ [7~ [8~
        if (c == '~') {
          int code = atoi(params);
          if (code == 3) return {KeyType::Delete, 0};
          if (code == 1 || code == 7) return {KeyType::Home, 0};
          if (code == 4 || code == 8) return {KeyType::End, 0};
        }

        return {KeyType::Unknown, 0};
      }

      if (b1 == 'O') {
        // SS3: ESC O A/B/C/D/H/F
        uint8_t c = 0;
        if (!read_byte_with_timeout(c, cfg_.esc_timeout_ms)) return {KeyType::Unknown, 0};
        if (c == 'A') return {KeyType::Up, 0};
        if (c == 'B') return {KeyType::Down, 0};
        if (c == 'C') return {KeyType::Right, 0};
        if (c == 'D') return {KeyType::Left, 0};
        if (c == 'H') return {KeyType::Home, 0};
        if (c == 'F') return {KeyType::End, 0};
        return {KeyType::Unknown, 0};
      }

      return {KeyType::Unknown, 0};
    }

    // Printable
    if (b >= 0x20) return {KeyType::Char, (char)b};

    return {KeyType::Unknown, 0};
  }

  // ------------ Editor actions ------------

  void redraw() {
    if (!cfg_.ansi) return;

    Serial.write('\r');
    Serial.write("\x1b[2K");   // clear full line
    Serial.print(cfg_.prompt.c_str());
    if (!buf_.empty()) Serial.write((const uint8_t*)buf_.data(), buf_.size());

    // move cursor back from end to cursor_
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
    if (cursor_ == 0 || buf_.empty()) return;

    if (!cfg_.ansi) {
      buf_.erase(buf_.begin() + (long)cursor_ - 1);
      cursor_--;
      // dumb-mode minimal erase
      Serial.write("\b \b");
      return;
    }

    buf_.erase(buf_.begin() + (long)cursor_ - 1);
    cursor_--;
    redraw();
  }

  void del() {
    if (cursor_ >= buf_.size()) return;
    buf_.erase(buf_.begin() + (long)cursor_);
    redraw();
  }

  void move_left() {
    if (cursor_ == 0) return;
    cursor_--;
    if (!cfg_.ansi) return;
    Serial.write("\x1b[1D");
  }

  void move_right() {
    if (cursor_ >= buf_.size()) return;
    cursor_++;
    if (!cfg_.ansi) return;
    Serial.write("\x1b[1C");
  }

  void home() {
    cursor_ = 0;
    redraw();
  }

  void end() {
    cursor_ = buf_.size();
    redraw();
  }

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

    // skip spaces
    while (i > 0 && is_space(buf_[i - 1])) i--;
    // skip word
    while (i > 0 && !is_space(buf_[i - 1])) i--;

    buf_.erase(i, cursor_ - i);
    cursor_ = i;
    redraw();
  }

  // ------------ Read a full edited line ------------

  bool read_line(std::string& out_line) {
    buf_.clear();
    cursor_ = 0;
    hist_.reset_nav();

    // prompt
    Serial.print(cfg_.prompt.c_str());

    while (true) {
      Key k = read_key();

      if (k.type == KeyType::Enter) {
        print_crlf();
        out_line = buf_;
        return true;
      }

      if (k.type == KeyType::Char) {
        insert_char(k.ch);
        continue;
      }

      switch (k.type) {
        case KeyType::Backspace: backspace(); break;
        case KeyType::Delete:    del(); break;
        case KeyType::Left:      move_left(); break;
        case KeyType::Right:     move_right(); break;
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

        case KeyType::CtrlL: {
          // Clear screen + redraw prompt+line
          if (cfg_.ansi) {
            Serial.write("\x1b[2J\x1b[H"); // clear & home
            redraw();
          }
        } break;

        case KeyType::CtrlC: {
          // Cancel line
          print_crlf();
          buf_.clear();
          cursor_ = 0;
          Serial.print(cfg_.prompt.c_str());
        } break;

        default:
          break;
      }
    }
  }
};

// --------------------------------- Application ---------------------------------

static ConsoleHistory g_history("console");
static CommandsRegistry g_cmds;
static Console* g_console = nullptr;

static void init_nvs_or_die() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    (void)nvs_flash_erase();
    err = nvs_flash_init();
  }
  // If this fails, continuing is pointless for persistent history.
  if (err != ESP_OK) {
    // last resort: still allow console, but history won't persist.
  }
}

static void register_commands() {
  // help / help <cmd>
  g_cmds.add(
    "help",
    "Show command list or help for one command",
    "help [command]",
    [](const std::vector<std::string>& args) -> int {
      if (!g_console) return 1;

      if (args.size() == 1) {
        auto list = g_cmds.list();

        // Format: name + help (aligned)
        size_t w = 0;
        for (const auto &c : list) w = std::max(w, c.name.size());
        w = std::min<size_t>(w, 16);

        g_console->print_line("Commands (help <cmd> for details):");
        for (const auto &c : list) {
          // Keep lines short & readable
          std::string name = c.name;
          if (name.size() > w) name.resize(w);

          g_console->printf("  %-*s  %s", (int)w, name.c_str(), c.help.c_str());
          g_console->print_crlf();
        }
        return 0;
      }

      const auto* cmd = g_cmds.find(args[1]);
      if (!cmd) {
        g_console->print_line("No such command.");
        return 2;
      }

      g_console->printf("%s - %s", cmd->name.c_str(), cmd->help.c_str());
      g_console->print_crlf();
      if (!cmd->usage.empty()) {
        g_console->printf("Usage: %s", cmd->usage.c_str());
        g_console->print_crlf();
      }
      return 0;
    }
  );

  g_cmds.add(
    "term",
    "Set terminal mode",
    "term ansi|dumb",
    [](const std::vector<std::string>& args) -> int {
      if (!g_console) return 1;
      if (args.size() != 2) {
        g_console->print_line("Usage: term ansi|dumb");
        return 2;
      }
      std::string m = to_lower(args[1]);
      if (m == "ansi") {
        g_console->set_ansi(true);
        g_console->print_line("term=ansi");
        return 0;
      }
      if (m == "dumb") {
        g_console->set_ansi(false);
        g_console->print_line("term=dumb");
        return 0;
      }
      g_console->print_line("Usage: term ansi|dumb");
      return 2;
    }
  );

  g_cmds.add(
    "history",
    "Print command history (persistent)",
    "history [n]",
    [](const std::vector<std::string>& args) -> int {
      if (!g_console) return 1;
      int n = g_history.size();
      if (args.size() == 2) {
        n = atoi(args[1].c_str());
        if (n < 0) n = 0;
        if (n > g_history.size()) n = g_history.size();
      }
      auto lines = g_history.last_n(n);
      int idx0 = g_history.size() - (int)lines.size() + 1;
      for (size_t i = 0; i < lines.size(); i++) {
        g_console->printf("%5d  %s", idx0 + (int)i, lines[i].c_str());
        g_console->print_crlf();
      }
      return 0;
    }
  );

  g_cmds.add(
    "echo",
    "Echo arguments",
    "echo [text...]",
    [](const std::vector<std::string>& args) -> int {
      if (!g_console) return 1;
      if (args.size() <= 1) {
        g_console->print_crlf();
        return 0;
      }
      for (size_t i = 1; i < args.size(); i++) {
        g_console->print(args[i]);
        if (i + 1 < args.size()) g_console->print(" ");
      }
      g_console->print_crlf();
      return 0;
    }
  );

  g_cmds.add(
    "uptime",
    "Print uptime",
    "uptime",
    [](const std::vector<std::string>&) -> int {
      if (!g_console) return 1;
      double s = (double)esp_timer_get_time() * 1e-6;
      g_console->printf("uptime=%.3fs", s);
      g_console->print_crlf();
      return 0;
    }
  );

  g_cmds.add(
    "chip",
    "Print chip info",
    "chip",
    [](const std::vector<std::string>&) -> int {
      if (!g_console) return 1;
      esp_chip_info_t info;
      esp_chip_info(&info);
      const char* model = "unknown";
      if (info.model == CHIP_ESP32S3) model = "S3";
      else if (info.model == CHIP_ESP32S2) model = "S2";
      else if (info.model == CHIP_ESP32C3) model = "C3";
      else if (info.model == CHIP_ESP32C6) model = "C6";

      g_console->printf("ESP32-%s cores=%d rev=%d flash=%u",
                        model, info.cores, info.revision, (unsigned)ESP.getFlashChipSize());
      g_console->print_crlf();
      return 0;
    }
  );

  g_cmds.add(
    "free",
    "Print heap free/min",
    "free",
    [](const std::vector<std::string>&) -> int {
      if (!g_console) return 1;
      g_console->printf("heap_free=%u heap_min_free=%u",
                        (unsigned)esp_get_free_heap_size(),
                        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
      g_console->print_crlf();
      return 0;
    }
  );

  g_cmds.add(
    "reboot",
    "Restart the MCU",
    "reboot",
    [](const std::vector<std::string>&) -> int {
      if (!g_console) return 1;
      g_console->print_line("restarting...");
      delay(50);
      ESP.restart();
      return 0;
    }
  );

  // Diagnostic: show raw key bytes for 5 seconds
  g_cmds.add(
    "keys",
    "Dump raw key bytes for 5 seconds",
    "keys",
    [](const std::vector<std::string>&) -> int {
      if (!g_console) return 1;
      g_console->print_line("Key dump for 5 seconds. Press keys now...");
      uint32_t t0 = millis();
      while (millis() - t0 < 5000) {
        while (Serial.available()) {
          uint8_t b = (uint8_t)Serial.read();
          g_console->printf("%02X ", (unsigned)b);
        }
        delay(5);
      }
      g_console->print_crlf();
      g_console->print_line("Done.");
      return 0;
    }
  );
}

// -------------------------------- Arduino entrypoints --------------------------------

void setup() {
  init_nvs_or_die();
  (void)g_history.load();

  register_commands();

  Console::Config cfg;
  cfg.prompt = "esp32s3> ";
  cfg.ansi = true;             // PuTTY supports ANSI
  cfg.max_line = 256;
  cfg.esc_timeout_ms = 80;

  static Console console(cfg, g_history, g_cmds);
  g_console = &console;
  console.begin(115200);
}

void loop() {
  if (g_console) g_console->loop_once();
  delay(1);
}
