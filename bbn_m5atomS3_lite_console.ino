/*
  Arduino-ESP32 3.3.5: Serial-backed esp_console with bash-like history/editing.

  - Transport: Serial (USB CDC or USB Serial/JTAG depending on board options)
  - Output: printf routed to Serial via esp_log_set_vprintf()
  - History: Up/Down arrows + Ctrl+P/Ctrl+N, Ctrl+R reverse search
  - Editing: left/right, backspace, delete, home/end

  Recommended AtomS3 board options:
    USBMode=hwcdc, CDCOnBoot=cdc
*/

#define CONFIG_ESP_CONSOLE_USB_CDC 1
#define CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF 1
#define CONFIG_LIBC_STDIN_LINE_ENDING_CR 1

#include <Arduino.h>

#include <esp_console.h>
#include <esp_err.h>
#include <esp_log.h>
#include <argtable3/argtable3.h>
#include <nvs_flash.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_timer.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// -------------------- Serial-backed printf/vprintf --------------------

static int serial_vprintf(const char *fmt, va_list ap) {
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0) return n;

  size_t to_write = (size_t)min(n, (int)sizeof(buf) - 1);

#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
  for (size_t i = 0; i < to_write; i++) {
    char c = buf[i];
    if (c == '\n') Serial.write('\r');
    Serial.write((uint8_t)c);
  }
#else
  Serial.write((const uint8_t*)buf, to_write);
#endif

  return n;
}

static inline void serial_newline() {
#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
  Serial.write("\r\n");
#else
  Serial.write("\n");
#endif
}

// -------------------- History + Editor constants --------------------
// NOTE: don't name this LINE_MAX (conflicts with toolchain macros)
static constexpr int CONSOLE_LINE_MAX = 256;
static constexpr int HIST_MAX = 32;

// -------------------- History (ring buffer) --------------------

struct History {
  char items[HIST_MAX][CONSOLE_LINE_MAX];
  int  count = 0;   // number of stored items (<= HIST_MAX)
  int  head  = 0;   // next write index
  int  nav   = 0;   // navigation offset (0=current, 1=most recent, ...)
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

  void add(const char* s) {
    if (!s || !*s) return;

    // Avoid duplicates of last command
    if (count > 0) {
      int last = (head - 1 + HIST_MAX) % HIST_MAX;
      if (strncmp(items[last], s, CONSOLE_LINE_MAX) == 0) return;
    }

    strncpy(items[head], s, CONSOLE_LINE_MAX - 1);
    items[head][CONSOLE_LINE_MAX - 1] = 0;
    head = (head + 1) % HIST_MAX;
    if (count < HIST_MAX) count++;
    nav = 0;
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

// Reverse search: first match from most recent backwards
static bool hist_reverse_search(const char* query, char* out, size_t out_cap) {
  if (!query) query = "";
  for (int k = 1; k <= g_hist.count; k++) {
    const char* s = g_hist.get_by_recent_index(k);
    if (s && strstr(s, query)) {
      strncpy(out, s, out_cap - 1);
      out[out_cap - 1] = 0;
      return true;
    }
  }
  return false;
}

// -------------------- Serial input helpers --------------------

static int read_byte_blocking() {
  while (Serial.available() == 0) delay(1);
  return Serial.read();
}

// Read rest of ANSI escape sequence after ESC.
// Returns true and NUL-terminates seq.
static bool read_esc_sequence(char* seq, size_t cap) {
  size_t n = 0;
  uint32_t t0 = millis();

  while (n + 1 < cap) {
    while (Serial.available() == 0) {
      if (millis() - t0 > 30) { // short timeout
        seq[n] = 0;
        return (n > 0);
      }
      delay(1);
    }
    char c = (char)Serial.read();
    seq[n++] = c;
    seq[n] = 0;

    // typical terminators
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
      return true;
    }
  }
  seq[cap - 1] = 0;
  return true;
}

static void redraw_line(const char* prompt, const char* buf, size_t len, size_t cursor) {
  // Go to line start, clear line, print prompt+buffer, then move cursor back if needed.
  Serial.write('\r');
  Serial.write("\x1b[2K"); // ANSI clear line

  Serial.print(prompt);
  if (len) Serial.write((const uint8_t*)buf, len);

  size_t back = len - cursor;
  if (back > 0) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "\x1b[%uD", (unsigned)back);
    Serial.write(tmp);
  }
}

// Line editor with bash-ish behavior.
// Returns true with out filled; false on error.
static bool read_line_edit(char* out, size_t out_cap, const char* prompt) {
  char buf[CONSOLE_LINE_MAX] = {0};
  size_t len = 0;
  size_t cursor = 0;

  g_hist.reset_nav();

  Serial.print(prompt);

  while (true) {
    int ch = read_byte_blocking();
    if (ch < 0) continue;
    char c = (char)ch;

    // Enter handling
#if CONFIG_LIBC_STDIN_LINE_ENDING_CR
    if (c == '\r' || c == '\n') {
      serial_newline();
      buf[len] = 0;
      strncpy(out, buf, out_cap - 1);
      out[out_cap - 1] = 0;
      return true;
    }
#else
    if (c == '\n') {
      serial_newline();
      buf[len] = 0;
      strncpy(out, buf, out_cap - 1);
      out[out_cap - 1] = 0;
      return true;
    }
    if (c == '\r') continue;
#endif

    // Control keys
    if ((uint8_t)c < 0x20) {
      switch (c) {
        case 0x01: // Ctrl+A (home)
          cursor = 0;
          redraw_line(prompt, buf, len, cursor);
          break;
        case 0x05: // Ctrl+E (end)
          cursor = len;
          redraw_line(prompt, buf, len, cursor);
          break;
        case 0x10: { // Ctrl+P (prev)
          const char* h = g_hist.older(buf);
          if (h) {
            strncpy(buf, h, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            len = strnlen(buf, sizeof(buf) - 1);
            cursor = len;
            redraw_line(prompt, buf, len, cursor);
          }
        } break;
        case 0x0E: { // Ctrl+N (next)
          const char* h = g_hist.newer();
          if (h) {
            strncpy(buf, h, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            len = strnlen(buf, sizeof(buf) - 1);
            cursor = len;
            redraw_line(prompt, buf, len, cursor);
          }
        } break;
        case 0x12: { // Ctrl+R reverse search
          char query[64] = {0};
          size_t qlen = 0;
          char match[CONSOLE_LINE_MAX] = {0};

          while (true) {
            Serial.write('\r');
            Serial.write("\x1b[2K");
            Serial.print("(reverse-i-search)`");
            if (qlen) Serial.write((const uint8_t*)query, qlen);
            Serial.print("': ");
            Serial.print(match);

            int k = read_byte_blocking();
            if (k < 0) continue;
            char kc = (char)k;

            if (kc == '\r' || kc == '\n') {
              if (match[0]) {
                strncpy(buf, match, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                len = strnlen(buf, sizeof(buf) - 1);
                cursor = len;
              }
              serial_newline();
              redraw_line(prompt, buf, len, cursor);
              break;
            }
            if ((uint8_t)kc == 0x1B) { // ESC cancels
              serial_newline();
              redraw_line(prompt, buf, len, cursor);
              break;
            }
            if (kc == '\b' || (uint8_t)kc == 0x7F) {
              if (qlen > 0) query[--qlen] = 0;
            } else if ((uint8_t)kc >= 0x20 && qlen + 1 < sizeof(query)) {
              query[qlen++] = kc;
              query[qlen] = 0;
            }

            match[0] = 0;
            (void)hist_reverse_search(query, match, sizeof(match));
          }
        } break;
        case 0x03: // Ctrl+C clears line
          serial_newline();
          buf[0] = 0;
          len = cursor = 0;
          Serial.print(prompt);
          break;
        default:
          // ignore other control keys
          break;
      }
      continue;
    }

    // Backspace / DEL
    if (c == '\b' || (uint8_t)c == 0x7F) {
      if (cursor > 0 && len > 0) {
        memmove(&buf[cursor - 1], &buf[cursor], len - cursor);
        len--;
        cursor--;
        buf[len] = 0;
        redraw_line(prompt, buf, len, cursor);
      }
      continue;
    }

    // ANSI escape sequences
    if ((uint8_t)c == 0x1B) {
      char seq[16] = {0};
      if (!read_esc_sequence(seq, sizeof(seq))) continue;

      if (strcmp(seq, "[A") == 0) { // Up
        const char* h = g_hist.older(buf);
        if (h) {
          strncpy(buf, h, sizeof(buf) - 1);
          buf[sizeof(buf) - 1] = 0;
          len = strnlen(buf, sizeof(buf) - 1);
          cursor = len;
          redraw_line(prompt, buf, len, cursor);
        }
      } else if (strcmp(seq, "[B") == 0) { // Down
        const char* h = g_hist.newer();
        if (h) {
          strncpy(buf, h, sizeof(buf) - 1);
          buf[sizeof(buf) - 1] = 0;
          len = strnlen(buf, sizeof(buf) - 1);
          cursor = len;
          redraw_line(prompt, buf, len, cursor);
        }
      } else if (strcmp(seq, "[C") == 0) { // Right
        if (cursor < len) {
          cursor++;
          redraw_line(prompt, buf, len, cursor);
        }
      } else if (strcmp(seq, "[D") == 0) { // Left
        if (cursor > 0) {
          cursor--;
          redraw_line(prompt, buf, len, cursor);
        }
      } else if (strcmp(seq, "[H") == 0 || strcmp(seq, "OH") == 0) { // Home
        cursor = 0;
        redraw_line(prompt, buf, len, cursor);
      } else if (strcmp(seq, "[F") == 0 || strcmp(seq, "OF") == 0) { // End
        cursor = len;
        redraw_line(prompt, buf, len, cursor);
      } else if (strcmp(seq, "[3~") == 0) { // Delete at cursor
        if (cursor < len) {
          memmove(&buf[cursor], &buf[cursor + 1], len - cursor - 1);
          len--;
          buf[len] = 0;
          redraw_line(prompt, buf, len, cursor);
        }
      }
      continue;
    }

    // Printable: insert at cursor
    if (len + 1 < sizeof(buf) && (uint8_t)c >= 0x20) {
      if (cursor == len) {
        buf[len++] = c;
        cursor++;
        buf[len] = 0;
        Serial.write((uint8_t)c); // echo
      } else {
        memmove(&buf[cursor + 1], &buf[cursor], len - cursor);
        buf[cursor] = c;
        len++;
        cursor++;
        buf[len] = 0;
        redraw_line(prompt, buf, len, cursor);
      }
    }
  }
}

// -------------------- Commands --------------------

// heap
static int cmd_heap(int, char**) {
  uint32_t hs = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
  printf("min heap size: %u\n", (unsigned)hs);
  return ESP_OK;
}

// free
static int cmd_free(int, char**) {
  printf("heap_free: %u\n", (unsigned)esp_get_free_heap_size());
  printf("heap_min_free: %u\n", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
  return ESP_OK;
}

// chip
static int cmd_chip(int, char**) {
  esp_chip_info_t info;
  esp_chip_info(&info);

  const char* model = "unknown";
  if (info.model == CHIP_ESP32S3) model = "S3";
  else if (info.model == CHIP_ESP32S2) model = "S2";
  else if (info.model == CHIP_ESP32C3) model = "C3";
  else if (info.model == CHIP_ESP32C6) model = "C6";

  printf("model: ESP32-%s\n", model);
  printf("cores: %d\n", info.cores);
  printf("revision: %d\n", info.revision);
  printf("features: WiFi%s BLE%s\n",
         (info.features & CHIP_FEATURE_WIFI_BGN) ? "+" : "-",
         (info.features & CHIP_FEATURE_BLE) ? "+" : "-");
  printf("flash size: %u bytes\n", (unsigned)ESP.getFlashChipSize());
  return ESP_OK;
}

// uptime
static int cmd_uptime(int, char**) {
  uint64_t us = (uint64_t)esp_timer_get_time();
  double s = (double)us * 1e-6;
  printf("uptime: %.3f s\n", s);
  return ESP_OK;
}

// reboot
static int cmd_reboot(int, char**) {
  printf("restarting...\n");
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
    arg_print_errors(stderr, echo_args.end, argv[0]);
    return ESP_ERR_INVALID_ARG;
  }

  if (echo_args.text && echo_args.text->count > 0) {
    for (int i = 0; i < echo_args.text->count; i++) {
      printf("%s%s", echo_args.text->sval[i],
             (i + 1 == echo_args.text->count) ? "\n" : " ");
    }
  } else {
    printf("\n");
  }
  return ESP_OK;
}

// loglevel <tag> <level>
static struct {
  struct arg_str* tag;
  struct arg_str* level;
  struct arg_end* end;
} loglevel_args;

static int cmd_loglevel(int argc, char** argv) {
  int nerrors = arg_parse(argc, argv, (void**)&loglevel_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, loglevel_args.end, argv[0]);
    return ESP_ERR_INVALID_ARG;
  }

  const char* tag = loglevel_args.tag->sval[0];
  const char* lvl = loglevel_args.level->sval[0];

  esp_log_level_t level = ESP_LOG_INFO;
  if      (!strcasecmp(lvl, "none"))    level = ESP_LOG_NONE;
  else if (!strcasecmp(lvl, "error"))   level = ESP_LOG_ERROR;
  else if (!strcasecmp(lvl, "warn"))    level = ESP_LOG_WARN;
  else if (!strcasecmp(lvl, "info"))    level = ESP_LOG_INFO;
  else if (!strcasecmp(lvl, "debug"))   level = ESP_LOG_DEBUG;
  else if (!strcasecmp(lvl, "verbose")) level = ESP_LOG_VERBOSE;
  else {
    printf("level must be one of: none,error,warn,info,debug,verbose\n");
    return ESP_ERR_INVALID_ARG;
  }

  esp_log_level_set(tag, level);
  printf("log level set: tag='%s' level=%s\n", tag, lvl);
  return ESP_OK;
}

// freq [hz]  (demo config variable)
static volatile double g_freq_hz = 0.30;

static struct {
  struct arg_dbl* hz;
  struct arg_end* end;
} freq_args;

static int cmd_freq(int argc, char** argv) {
  int nerrors = arg_parse(argc, argv, (void**)&freq_args);
  if (nerrors != 0) {
    arg_print_errors(stderr, freq_args.end, argv[0]);
    return ESP_ERR_INVALID_ARG;
  }

  if (freq_args.hz->count == 0) {
    printf("freq_hz: %.6f\n", g_freq_hz);
    return ESP_OK;
  }

  double v = freq_args.hz->dval[0];
  if (!(v > 0.0 && v < 1000.0)) {
    printf("hz out of range\n");
    return ESP_ERR_INVALID_ARG;
  }
  g_freq_hz = v;
  printf("freq_hz set to %.6f\n", g_freq_hz);
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
  // required argtables
  echo_args.text = arg_strn(nullptr, nullptr, "<text>", 0, 32, "Text to echo");
  echo_args.end  = arg_end(2);

  loglevel_args.tag   = arg_str1(nullptr, nullptr, "<tag>", "Log tag (use '*' for all)");
  loglevel_args.level = arg_str1(nullptr, nullptr, "<level>", "none|error|warn|info|debug|verbose");
  loglevel_args.end   = arg_end(2);

  freq_args.hz  = arg_dbl0(nullptr, nullptr, "<hz>", "Get/set demo frequency");
  freq_args.end = arg_end(2);

  // register
  esp_console_register_help_command();
  register_cmd("heap",     "Minimum free heap seen during execution", &cmd_heap);
  register_cmd("free",     "Current heap free + min free",            &cmd_free);
  register_cmd("chip",     "Chip info",                               &cmd_chip);
  register_cmd("uptime",   "Uptime",                                  &cmd_uptime);
  register_cmd("reboot",   "Restart the MCU",                         &cmd_reboot);
  register_cmd("echo",     "Echo arguments",                          &cmd_echo,     &echo_args);
  register_cmd("loglevel", "Set log level: loglevel <tag> <level>",   &cmd_loglevel, &loglevel_args);
  register_cmd("freq",     "Get/set demo frequency: freq [hz]",       &cmd_freq,     &freq_args);
}

// -------------------- Arduino setup/loop --------------------

static const char* kPrompt = "esp32s3> ";

void setup() {
  Serial.begin(115200);

  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  // Route printf() (and ESP_LOG*) to Serial
  esp_log_set_vprintf(&serial_vprintf);

  // NVS init (safe)
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  // esp_console init
  esp_console_config_t cfg = {};
  cfg.max_cmdline_length = CONSOLE_LINE_MAX;
  cfg.max_cmdline_args   = 8;
  ESP_ERROR_CHECK(esp_console_init(&cfg));

  register_commands();

  printf("\nESP console ready.\n");
  printf("History: Up/Down arrows, Ctrl+P/Ctrl+N. Reverse search: Ctrl+R.\n");
  printf("Try: help, heap, free, chip, uptime, loglevel * debug, freq 0.3\n\n");
}

void loop() {
  static char line[CONSOLE_LINE_MAX];

  if (!read_line_edit(line, sizeof(line), kPrompt)) {
    delay(2);
    return;
  }

  // Trim leading spaces
  char* p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == 0) return;

  // Store after entry (bash-like)
  g_hist.add(p);

  int ret = 0;
  esp_err_t err = esp_console_run(p, &ret);

  if (err == ESP_ERR_NOT_FOUND) {
    printf("Unrecognized command\n");
  } else if (err == ESP_ERR_INVALID_ARG) {
    // empty command
  } else if (err == ESP_OK && ret != ESP_OK) {
    printf("Command returned non-zero error code: 0x%x (%s)\n",
           (unsigned)ret, esp_err_to_name((esp_err_t)ret));
  } else if (err != ESP_OK) {
    printf("Internal error: %s\n", esp_err_to_name(err));
  }

  delay(1);
}
