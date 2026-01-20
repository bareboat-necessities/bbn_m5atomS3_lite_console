/*
  Arduino-ESP32 3.3.5 console for PuTTY
  - ANSI line editor with mid-line cursor
  - Robust arrow keys via stateful ESC parser (FSM) so [A never leaks
  - Persistent command history in NVS (survives reboot)
  - Modern C++: ConsoleHistory, CommandsRegistry, Console; lambdas for commands

  Keys:
    Up/Down     history
    Left/Right  cursor
    Home/End    start/end
    Backspace   delete left
    Delete      delete at cursor
    Ctrl+A/E    start/end
    Ctrl+U/K    kill to start/end
    Ctrl+W      delete previous word
    Ctrl+L      clear screen + redraw
    Ctrl+C      cancel line
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

// ------------------------------ ConsoleHistory ------------------------------

class ConsoleHistory {
public:
  static constexpr int kMaxItems = 50;
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
    head_  = clamp(head,  0, kMaxItems - 1);
    count_ = clamp(count, 0, kMaxItems);

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
    n = clamp(n, 0, count_);
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

  static int clamp(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi) ? hi : v; }

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

  struct Command {
    std::string name;
    std::string help;
    std::string usage;
    Handler handler;
  };

  void add(std::string name, std::string help, std::string usage, Handler handler) {
    name = to_lower(std::move(name));
    Command c{std::move(name), std::move(help), std::move(usage), std::move(handler)};

    for (auto &x : cmds_) {
      if (x.name == c.name) { x = std::move(c); return; }
    }
    cmds_.push_back(std::move(c));
    std::sort(cmds_.begin(), cmds_.end(),
              [](const Command& a, const Command& b){ return a.name < b.name; });
  }

  const Command* find(const std::string& name) const {
    std::string key = to_lower(name);
    for (const auto& c : cmds_) if (c.name == key) return &c;
    return nullptr;
  }

  int run_line(const std::string& line) const {
    auto argv = split_argv(line);
    if (argv.empty()) return 0;
    const Command* cmd = find(argv[0]);
    if (!cmd) return err_not_found;
    return cmd->handler(argv);
  }

  std::vector<Command> list() const { return cmds_; }

  static constexpr int err_not_found = 127;

private:
  std::vector<Command> cmds_;
};

// ------------------------------ Console (editor + io) ------------------------------

class Console {
public:
  struct Config {
    std::string prompt = "esp32s3> ";
    bool ansi = true;
    size_t max_line = 256;

    // Total time allowed for an ESC sequence to complete.
    // This must be "large" to tolerate USB CDC fragmentation.
    uint32_t esc_seq_timeout_ms = 2000;
  };

  Console(Config cfg, ConsoleHistory& hist, CommandsRegistry& reg)
  : cfg_(std::move(cfg)), hist_(hist), reg_(reg) {}

  void begin(uint32_t baud = 115200) {
    Serial.begin(baud);
    Serial.setRxBufferSize(4096);

    uint32_t t0 = millis();
    while (!Serial && (millis() - t0) < 1500) delay(10);

    crlf();
    line("Ready. Type 'help' or 'help <cmd>'.");
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

  // ---- output helpers ----
  void print(const char* s) { if (s) Serial.print(s); }
  void print(const std::string& s) { Serial.print(s.c_str()); }
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

  void set_ansi(bool on) { cfg_.ansi = on; }

private:
  enum class KeyType {
    Char, Enter,
    Backspace, Delete,
    Left, Right, Up, Down,
    Home, End,
    CtrlA, CtrlE,
    CtrlU, CtrlK,
    CtrlW,
    CtrlL,
    CtrlC,
    Unknown,
    None  // internal: no key yet (waiting for ESC completion)
  };

  struct Key { KeyType type; char ch; };

  Config cfg_;
  ConsoleHistory& hist_;
  CommandsRegistry& reg_;

  std::string buf_;
  size_t cursor_ = 0;

  // -------- ANSI parser FSM --------
  enum class EscState { Idle, GotEsc, CSI, SS3 };
  EscState esc_state_ = EscState::Idle;
  uint32_t esc_deadline_ = 0;
  char esc_params_[24] = {0};
  int esc_p_ = 0;

  // ---- serial primitives ----
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

  // Map CSI/SS3 final bytes to keys
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

  // Stateful key reader: consumes ESC sequences across fragmented reads.
  Key read_key() {
    while (true) {
      // If we're in escape parsing mode, try to finish it using whatever bytes arrive.
      if (esc_state_ != EscState::Idle) {
        // timeout?
        if ((int32_t)(millis() - esc_deadline_) >= 0) {
          // Drop the partial sequence completely (do NOT leak bytes as text)
          esc_state_ = EscState::Idle;
          esc_p_ = 0;
          esc_params_[0] = 0;
          // continue to read next normal key
          continue;
        }

        uint8_t b;
        if (!read_byte_nonblocking(b)) {
          delay(1);
          continue;
        }

        if (esc_state_ == EscState::GotEsc) {
          if (b == '[') {
            esc_state_ = EscState::CSI;
            esc_p_ = 0;
            esc_params_[0] = 0;
            continue;
          }
          if (b == 'O') {
            esc_state_ = EscState::SS3;
            continue;
          }
          // Unknown lead-in: discard ESC sequence and continue (do not leak this byte)
          esc_state_ = EscState::Idle;
          continue;
        }

        if (esc_state_ == EscState::SS3) {
          esc_state_ = EscState::Idle;
          return map_arrow_final(b);
        }

        // CSI
        if (esc_state_ == EscState::CSI) {
          // parameter bytes until final @-~
          if (b >= 0x40 && b <= 0x7E) {
            // final byte
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
            // accumulate params (digits, ';', etc.)
            if (esc_p_ + 1 < (int)sizeof(esc_params_)) {
              esc_params_[esc_p_++] = (char)b;
              esc_params_[esc_p_] = 0;
            }
            continue;
          }
        }
      }

      // Normal (idle) state: blocking read one byte and interpret.
      uint8_t b = read_byte_blocking();

      // Enter
      if (b == '\r' || b == '\n') return {KeyType::Enter, 0};

      // Backspace
      if (b == 0x7F || b == '\b') return {KeyType::Backspace, 0};

      // Ctrl keys
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

      // ESC starts a sequence; do NOT return yet. Switch state and keep reading.
      if (b == 0x1B) {
        esc_state_ = EscState::GotEsc;
        esc_deadline_ = millis() + cfg_.esc_seq_timeout_ms;
        esc_p_ = 0;
        esc_params_[0] = 0;
        // loop to continue parsing, do not leak
        continue;
      }

      // Printable
      if (b >= 0x20) return {KeyType::Char, (char)b};

      return {KeyType::Unknown, 0};
    }
  }

  // ---- editor render ----
  void redraw() {
    if (!cfg_.ansi) return;
    Serial.write('\r');
    Serial.write("\x1b[2K");
    Serial.print(cfg_.prompt.c_str());
    if (!buf_.empty()) Serial.write((const uint8_t*)buf_.data(), buf_.size());
    const size_t back = buf_.size() - cursor_;
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

// ------------------------------ wiring ------------------------------

static ConsoleHistory g_history("console");
static CommandsRegistry g_cmds;
static Console* g_console = nullptr;

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    (void)nvs_flash_erase();
    (void)nvs_flash_init();
  }
}

static void register_commands() {
  g_cmds.add("help",
             "Show command list or help for one command",
             "help [command]",
             [](const std::vector<std::string>& args) -> int {
               if (!g_console) return 1;

               if (args.size() == 1) {
                 auto list = g_cmds.list();
                 size_t w = 0;
                 for (const auto& c : list) w = std::max(w, c.name.size());
                 w = std::min<size_t>(w, 12);

                 g_console->line("Commands:");
                 for (const auto& c : list) {
                   g_console->printf("  %-*s  %s\n", (int)w, c.name.c_str(), c.help.c_str());
                 }
                 g_console->line("Keys: Up/Down hist, Left/Right, Home/End, Del/BS, ^A/^E, ^U/^K, ^W, ^L.");
                 return 0;
               }

               const auto* c = g_cmds.find(args[1]);
               if (!c) { g_console->line("No such command."); return 2; }
               g_console->printf("%s - %s\n", c->name.c_str(), c->help.c_str());
               g_console->printf("Usage: %s\n", c->usage.c_str());
               return 0;
             });

  g_cmds.add("history",
             "Print persistent history",
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
                 g_console->printf("%5d  %s\n", idx0 + (int)i, lines[i].c_str());
               }
               return 0;
             });

  g_cmds.add("echo",
             "Echo arguments",
             "echo [text...]",
             [](const std::vector<std::string>& args) -> int {
               if (!g_console) return 1;
               for (size_t i = 1; i < args.size(); i++) {
                 g_console->print(args[i]);
                 if (i + 1 < args.size()) g_console->print(" ");
               }
               g_console->crlf();
               return 0;
             });

  g_cmds.add("uptime",
             "Print uptime",
             "uptime",
             [](const std::vector<std::string>&) -> int {
               if (!g_console) return 1;
               double s = (double)esp_timer_get_time() * 1e-6;
               g_console->printf("uptime=%.3fs\n", s);
               return 0;
             });

  g_cmds.add("chip",
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
               g_console->printf("ESP32-%s cores=%d rev=%d flash=%u\n",
                                 model, info.cores, info.revision, (unsigned)ESP.getFlashChipSize());
               return 0;
             });

  g_cmds.add("free",
             "Print heap free/min",
             "free",
             [](const std::vector<std::string>&) -> int {
               if (!g_console) return 1;
               g_console->printf("heap_free=%u heap_min_free=%u\n",
                                 (unsigned)esp_get_free_heap_size(),
                                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
               return 0;
             });

  g_cmds.add("reboot",
             "Restart the MCU",
             "reboot",
             [](const std::vector<std::string>&) -> int {
               if (!g_console) return 1;
               g_console->line("restarting...");
               delay(50);
               ESP.restart();
               return 0;
             });
}

void setup() {
  init_nvs();
  (void)g_history.load();
  register_commands();

  Console::Config cfg;
  cfg.prompt = "esp32s3> ";
  cfg.ansi = true;
  cfg.max_line = 256;
  cfg.esc_seq_timeout_ms = 2000;

  static Console console(cfg, g_history, g_cmds);
  g_console = &console;
  console.begin(115200);
}

void loop() {
  if (g_console) g_console->loop_once();
  delay(1);
}
