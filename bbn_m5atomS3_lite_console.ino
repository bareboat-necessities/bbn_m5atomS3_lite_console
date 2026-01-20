/*
  Arduino-ESP32 3.3.5 USB CDC console with:
   - ANSI line editor (mid-line cursor, insert/delete)
   - Up/Down history (PuTTY ESC [ A / ESC [ B)
   - Persistent history in NVS across reboot
   - Compact, stable help (no corruption)

  Commands:
    help, free, chip, uptime, reboot, echo, history, keys, term
*/

#define CONFIG_ESP_CONSOLE_USB_CDC 1
#define CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF 1
#define CONFIG_LIBC_STDIN_LINE_ENDING_CR 1

#include <Arduino.h>

#include <esp_console.h>
#include <esp_err.h>
#include <argtable3/argtable3.h>

#include <nvs.h>
#include <nvs_flash.h>

#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// -------------------- CRLF-safe output (DO NOT use printf) --------------------

static void out_crlf() {
  Serial.print("\r\n");
}

static void out_vprintf(const char* fmt, va_list ap) {
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0) return;

  // Ensure CRLF on any '\n' in the formatted text.
  for (int i = 0; i < n && i < (int)sizeof(buf); i++) {
    char c = buf[i];
    if (c == '\n') Serial.write('\r');
    Serial.write((uint8_t)c);
  }
}

static void out_printf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  out_vprintf(fmt, ap);
  va_end(ap);
}

static void out_printfln(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  out_vprintf(fmt, ap);
  va_end(ap);
  out_crlf();
}

// -------------------- Constants --------------------

static constexpr int CONSOLE_LINE_MAX = 256;
static constexpr int HIST_MAX = 32;

// -------------------- Terminal mode --------------------

static bool g_term_ansi = true; // PuTTY supports ANSI

// -------------------- Persistent history in NVS --------------------

static constexpr const char* NVS_NS = "console";
static constexpr const char* KEY_HEAD  = "h_head";
static constexpr const char* KEY_COUNT = "h_count";

static void nvs_key_for_slot(char* out, size_t cap, int slot) {
  snprintf(out, cap, "h%02d", slot); // h00..h31
}

static bool nvs_read_i32(nvs_handle_t h, const char* key, int32_t& outv) {
  return nvs_get_i32(h, key, &outv) == ESP_OK;
}

static void nvs_write_i32(nvs_handle_t h, const char* key, int32_t v) {
  (void)nvs_set_i32(h, key, v);
}

static bool nvs_read_str(nvs_handle_t h, const char* key, char* out, size_t out_cap) {
  size_t required = 0;
  esp_err_t err = nvs_get_str(h, key, nullptr, &required);
  if (err != ESP_OK || required == 0 || required > out_cap) return false;
  return nvs_get_str(h, key, out, &required) == ESP_OK;
}

static void nvs_write_str(nvs_handle_t h, const char* key, const char* s) {
  (void)nvs_set_str(h, key, s ? s : "");
}

// -------------------- History ring --------------------

struct History {
  char items[HIST_MAX][CONSOLE_LINE_MAX];
  int  count = 0;
  int  head  = 0;
  int  nav   = 0;
  char scratch[CONSOLE_LINE_MAX];

  void reset_nav() { nav = 0; }

  void begin_nav(const char* current_line) {
    if (nav == 0) {
      strncpy(scratch, current_line ? current_line : "", CONSOLE_LINE_MAX - 1);
      scratch[CONSOLE_LINE_MAX - 1] = 0;
    }
  }

  const char* get_by_recent_index(int k) const {
    if (k <= 0 || k > count) return nullptr;
    int idx = (head - k + HIST_MAX) % HIST_MAX;
    return items[idx];
  }

  void load_from_nvs() {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    int32_t n_head = 0, n_count = 0;
    if (!nvs_read_i32(h, KEY_HEAD, n_head)) n_head = 0;
    if (!nvs_read_i32(h, KEY_COUNT, n_count)) n_count = 0;

    if (n_head < 0 || n_head >= HIST_MAX) n_head = 0;
    if (n_count < 0 || n_count > HIST_MAX) n_count = 0;

    head = (int)n_head;
    count = (int)n_count;
    nav = 0;
    scratch[0] = 0;

    for (int i = 0; i < HIST_MAX; i++) {
      items[i][0] = 0;
      char key[8];
      nvs_key_for_slot(key, sizeof(key), i);
      (void)nvs_read_str(h, key, items[i], sizeof(items[i]));
    }
    nvs_close(h);
  }

  void persist_add(const char* s) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    int slot = (head + HIST_MAX - 1) % HIST_MAX;
    char key[8];
    nvs_key_for_slot(key, sizeof(key), slot);
    nvs_write_str(h, key, s);

    nvs_write_i32(h, KEY_HEAD, (int32_t)head);
    nvs_write_i32(h, KEY_COUNT, (int32_t)count);

    (void)nvs_commit(h);
    nvs_close(h);
  }

  void add(const char* s) {
    if (!s || !*s) return;

    if (count > 0) {
      int last = (head - 1 + HIST_MAX) % HIST_MAX;
      if (strncmp(items[last], s, CONSOLE_LINE_MAX) == 0) return;
    }

    strncpy(items[head], s, CONSOLE_LINE_MAX - 1);
    items[head][CONSOLE_LINE_MAX - 1] = 0;

    head = (head + 1) % HIST_MAX;
    if (count < HIST_MAX) count++;
    nav = 0;

    persist_add(s);
  }

  const char* older(const char* current_line) {
    if (count == 0) return nullptr;
    begin_nav(current_line);
    if (nav < count) nav++;
    return get_by_recent_index(nav);
  }

  const char* newer() {
    if (count == 0) return nullptr;
    if (nav > 0) nav--;
    if (nav == 0) return scratch;
    return get_by_recent_index(nav);
  }
};

static History g_hist;

// -------------------- Serial input helpers --------------------

static int read_byte_blocking() {
  while (Serial.available() == 0) delay(1);
  return Serial.read();
}

static bool read_esc_sequence(char* seq, size_t cap) {
  size_t n = 0;
  uint32_t t0 = millis();
  const uint32_t timeout_ms = 250;

  while (n + 1 < cap) {
    while (Serial.available() == 0) {
      if (millis() - t0 > timeout_ms) { seq[n] = 0; return (n > 0); }
      delay(1);
    }
    char c = (char)Serial.read();
    seq[n++] = c;
    seq[n] = 0;

    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') return true;
  }

  seq[cap - 1] = 0;
  return true;
}

static bool seq_ends_with(const char* s, char c) {
  if (!s) return false;
  size_t L = strlen(s);
  return L > 0 && s[L - 1] == c;
}

static bool seq_is_up(const char* s)   { return s && (strcmp(s,"[A")==0 || strcmp(s,"OA")==0 || seq_ends_with(s,'A')); }
static bool seq_is_down(const char* s) { return s && (strcmp(s,"[B")==0 || strcmp(s,"OB")==0 || seq_ends_with(s,'B')); }
static bool seq_is_right(const char* s){ return s && (strcmp(s,"[C")==0 || strcmp(s,"OC")==0 || seq_ends_with(s,'C')); }
static bool seq_is_left(const char* s) { return s && (strcmp(s,"[D")==0 || strcmp(s,"OD")==0 || seq_ends_with(s,'D')); }
static bool seq_is_home(const char* s) { return s && (strcmp(s,"[H")==0 || strcmp(s,"OH")==0 || strcmp(s,"[1~")==0); }
static bool seq_is_end(const char* s)  { return s && (strcmp(s,"[F")==0 || strcmp(s,"OF")==0 || strcmp(s,"[4~")==0); }
static bool seq_is_del(const char* s)  { return s && (strcmp(s,"[3~")==0); }

// -------------------- Redraw with stable ANSI behavior --------------------

static void redraw_line(const char* prompt, const char* buf, size_t len, size_t cursor) {
  Serial.write('\r');
  if (g_term_ansi) Serial.write("\x1b[2K"); // clear full line

  Serial.print(prompt);
  if (len) Serial.write((const uint8_t*)buf, len);

  if (g_term_ansi) {
    size_t back = len - cursor;
    if (back > 0) {
      char tmp[24];
      snprintf(tmp, sizeof(tmp), "\x1b[%uD", (unsigned)back);
      Serial.write(tmp);
    }
  }
}

// -------------------- Line editor (mid-line) --------------------

static bool read_line_edit(char* out, size_t out_cap, const char* prompt) {
  char buf[CONSOLE_LINE_MAX] = {0};
  size_t len = 0;
  size_t cursor = 0;

  g_hist.reset_nav();
  Serial.print(prompt);

  while (true) {
    int ch = read_byte_blocking();
    if (ch < 0) continue;
    uint8_t uc = (uint8_t)ch;
    char c = (char)uc;

#if CONFIG_LIBC_STDIN_LINE_ENDING_CR
    if (c == '\r' || c == '\n') {
      out_crlf();
      buf[len] = 0;
      strncpy(out, buf, out_cap - 1);
      out[out_cap - 1] = 0;
      return true;
    }
#else
    if (c == '\n') {
      out_crlf();
      buf[len] = 0;
      strncpy(out, buf, out_cap - 1);
      out[out_cap - 1] = 0;
      return true;
    }
    if (c == '\r') continue;
#endif

    // Control keys
    if (uc < 0x20) {
      switch (uc) {
        case 0x01: // Ctrl+A
          cursor = 0;
          redraw_line(prompt, buf, len, cursor);
          break;
        case 0x05: // Ctrl+E
          cursor = len;
          redraw_line(prompt, buf, len, cursor);
          break;
        case 0x10: { // Ctrl+P
          const char* h = g_hist.older(buf);
          if (h) {
            strncpy(buf, h, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            len = strnlen(buf, sizeof(buf) - 1);
            cursor = len;
            redraw_line(prompt, buf, len, cursor);
          }
        } break;
        case 0x0E: { // Ctrl+N
          const char* h = g_hist.newer();
          if (h) {
            strncpy(buf, h, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            len = strnlen(buf, sizeof(buf) - 1);
            cursor = len;
            redraw_line(prompt, buf, len, cursor);
          }
        } break;
        case 0x03: // Ctrl+C clears line
          out_crlf();
          buf[0] = 0;
          len = cursor = 0;
          Serial.print(prompt);
          break;
        default:
          break;
      }
      continue;
    }

    // Backspace
    if (c == '\b' || uc == 0x7F) {
      if (cursor > 0 && len > 0) {
        memmove(&buf[cursor - 1], &buf[cursor], len - cursor);
        len--;
        cursor--;
        buf[len] = 0;
        redraw_line(prompt, buf, len, cursor);
      }
      continue;
    }

    // ESC sequences
    if (uc == 0x1B) {
      char seq[24] = {0};
      if (!read_esc_sequence(seq, sizeof(seq))) continue;

      if (seq_is_up(seq)) {
        const char* h = g_hist.older(buf);
        if (h) {
          strncpy(buf, h, sizeof(buf) - 1);
          buf[sizeof(buf) - 1] = 0;
          len = strnlen(buf, sizeof(buf) - 1);
          cursor = len;
          redraw_line(prompt, buf, len, cursor);
        }
      } else if (seq_is_down(seq)) {
        const char* h = g_hist.newer();
        if (h) {
          strncpy(buf, h, sizeof(buf) - 1);
          buf[sizeof(buf) - 1] = 0;
          len = strnlen(buf, sizeof(buf) - 1);
          cursor = len;
          redraw_line(prompt, buf, len, cursor);
        }
      } else if (seq_is_left(seq)) {
        if (cursor > 0) { cursor--; redraw_line(prompt, buf, len, cursor); }
      } else if (seq_is_right(seq)) {
        if (cursor < len) { cursor++; redraw_line(prompt, buf, len, cursor); }
      } else if (seq_is_home(seq)) {
        cursor = 0;
        redraw_line(prompt, buf, len, cursor);
      } else if (seq_is_end(seq)) {
        cursor = len;
        redraw_line(prompt, buf, len, cursor);
      } else if (seq_is_del(seq)) {
        if (cursor < len) {
          memmove(&buf[cursor], &buf[cursor + 1], len - cursor - 1);
          len--;
          buf[len] = 0;
          redraw_line(prompt, buf, len, cursor);
        }
      }
      continue;
    }

    // Printable insert at cursor
    if (uc >= 0x20 && len + 1 < sizeof(buf)) {
      if (cursor == len) {
        buf[len++] = c;
        cursor++;
        buf[len] = 0;
        Serial.write((uint8_t)c);
      } else {
        memmove(&buf[cursor + 1], &buf[cursor], len - cursor);
        buf[cursor] = c;
        len++;
        cursor++;
        buf[len] = 0;
        redraw_line(prompt, buf, len, cursor);
      }
      continue;
    }
  }
}

// -------------------- Commands (Serial output only) --------------------

static int cmd_help(int, char**) {
  out_printfln("Commands: help free chip uptime reboot echo history keys term");
  out_printfln("Keys: Up/Down hist, Ctrl+P/N hist. term ansi|dumb");
  return ESP_OK;
}

static int cmd_free(int, char**) {
  out_printfln("heap_free=%u heap_min_free=%u",
               (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
  return ESP_OK;
}

static int cmd_chip(int, char**) {
  esp_chip_info_t info;
  esp_chip_info(&info);
  const char* model = "unknown";
  if (info.model == CHIP_ESP32S3) model = "S3";
  else if (info.model == CHIP_ESP32S2) model = "S2";
  else if (info.model == CHIP_ESP32C3) model = "C3";
  else if (info.model == CHIP_ESP32C6) model = "C6";

  out_printfln("ESP32-%s cores=%d rev=%d flash=%u",
               model, info.cores, info.revision, (unsigned)ESP.getFlashChipSize());
  return ESP_OK;
}

static int cmd_uptime(int, char**) {
  double s = (double)esp_timer_get_time() * 1e-6;
  out_printfln("uptime=%.3fs", s);
  return ESP_OK;
}

static int cmd_reboot(int, char**) {
  out_printfln("restarting...");
  delay(50);
  ESP.restart();
  return ESP_OK;
}

// echo [text...]
static struct {
  struct arg_str* text;
  struct arg_end* end;
} echo_args;

static int cmd_echo(int argc, char** argv) {
  int nerrors = arg_parse(argc, argv, (void**)&echo_args);
  if (nerrors != 0) {
    out_printfln("usage: echo [text...]");
    return ESP_ERR_INVALID_ARG;
  }
  if (echo_args.text && echo_args.text->count > 0) {
    for (int i = 0; i < echo_args.text->count; i++) {
      Serial.print(echo_args.text->sval[i]);
      if (i + 1 < echo_args.text->count) Serial.print(' ');
    }
    out_crlf();
  } else {
    out_crlf();
  }
  return ESP_OK;
}

// history [n]
static struct {
  struct arg_int* n;
  struct arg_end* end;
} history_args;

static int cmd_history(int argc, char** argv) {
  int nerrors = arg_parse(argc, argv, (void**)&history_args);
  if (nerrors != 0) {
    out_printfln("usage: history [n]");
    return ESP_ERR_INVALID_ARG;
  }

  int want = g_hist.count;
  if (history_args.n->count > 0) {
    want = history_args.n->ival[0];
    if (want < 0) want = 0;
    if (want > g_hist.count) want = g_hist.count;
  }

  int line_no = g_hist.count - want + 1;
  for (int k = want; k >= 1; k--) {
    const char* s = g_hist.get_by_recent_index(k);
    if (!s) continue;
    out_printfln("%5d  %s", line_no++, s);
  }
  return ESP_OK;
}

static int cmd_keys(int, char**) {
  out_printfln("Key dump 5s. Press keys:");
  uint32_t t0 = millis();
  while (millis() - t0 < 5000) {
    while (Serial.available()) {
      uint8_t b = (uint8_t)Serial.read();
      out_printf("%02X ", (unsigned)b);
    }
    delay(5);
  }
  out_crlf();
  out_printfln("Done.");
  return ESP_OK;
}

// term <ansi|dumb>
static struct {
  struct arg_str* mode;
  struct arg_end* end;
} term_args;

static int cmd_term(int argc, char** argv) {
  int nerrors = arg_parse(argc, argv, (void**)&term_args);
  if (nerrors != 0) {
    out_printfln("usage: term ansi|dumb");
    return ESP_ERR_INVALID_ARG;
  }
  const char* m = term_args.mode->sval[0];
  if (!strcasecmp(m, "ansi")) {
    g_term_ansi = true;
    out_printfln("term=ansi");
  } else if (!strcasecmp(m, "dumb")) {
    g_term_ansi = false;
    out_printfln("term=dumb");
  } else {
    out_printfln("usage: term ansi|dumb");
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

// -------------------- Register commands --------------------

static void register_cmd(const char* name, const char* help, esp_console_cmd_func_t fn, void* argtable = nullptr) {
  esp_console_cmd_t cmd = {};
  cmd.command = name;
  cmd.help = help;
  cmd.hint = nullptr;
  cmd.func = fn;
  cmd.argtable = argtable;
  ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static void register_commands() {
  echo_args.text = arg_strn(nullptr, nullptr, "<text>", 0, 32, "Text to echo");
  echo_args.end  = arg_end(2);

  history_args.n  = arg_int0(nullptr, nullptr, "<n>", "Print last n history entries");
  history_args.end = arg_end(2);

  term_args.mode = arg_str1(nullptr, nullptr, "<mode>", "ansi|dumb");
  term_args.end  = arg_end(2);

  register_cmd("help",    "Show commands (compact)",           &cmd_help);
  register_cmd("free",    "Heap free/min",                     &cmd_free);
  register_cmd("chip",    "Chip info",                         &cmd_chip);
  register_cmd("uptime",  "Uptime",                            &cmd_uptime);
  register_cmd("reboot",  "Restart MCU",                       &cmd_reboot);
  register_cmd("echo",    "Echo arguments",                    &cmd_echo,    &echo_args);
  register_cmd("history", "Show history: history [n]",         &cmd_history, &history_args);
  register_cmd("keys",    "Dump raw key bytes (diagnostic)",   &cmd_keys);
  register_cmd("term",    "Terminal mode: term ansi|dumb",     &cmd_term,    &term_args);
}

// -------------------- Arduino setup/loop --------------------

static const char* kPrompt = "esp32s3> ";

void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(2048);

  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  // NVS init
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    (void)nvs_flash_erase();
    (void)nvs_flash_init();
  }

  g_hist.load_from_nvs();

  esp_console_config_t cfg = {};
  cfg.max_cmdline_length = CONSOLE_LINE_MAX;
  cfg.max_cmdline_args   = 8;
  ESP_ERROR_CHECK(esp_console_init(&cfg));

  register_commands();

  out_printfln("");
  out_printfln("Ready. Type 'help'.");
}

void loop() {
  static char line[CONSOLE_LINE_MAX];

  if (!read_line_edit(line, sizeof(line), kPrompt)) {
    delay(2);
    return;
  }

  char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == 0) return;

  g_hist.add(p);

  // Start command output on a clean line
  out_crlf();

  int ret = 0;
  esp_err_t e = esp_console_run(p, &ret);

  if (e == ESP_ERR_NOT_FOUND) {
    out_printfln("Unrecognized command");
  } else if (e == ESP_ERR_INVALID_ARG) {
    // empty
  } else if (e == ESP_OK && ret != ESP_OK) {
    out_printfln("cmd error: 0x%x", (unsigned)ret);
  } else if (e != ESP_OK) {
    out_printfln("internal error: %s", esp_err_to_name(e));
  }

  delay(1);
}
