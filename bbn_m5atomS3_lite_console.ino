/*
  Arduino-ESP32 3.3.5: interactive esp_console over USB console (Serial).
  Fixes prompt spam by NOT using stdin/getchar() (which is non-blocking / EOF in Arduino).

  Board options (what you already use):
    USBMode = hwcdc
    CDCOnBoot = cdc
*/

#define CONFIG_ESP_CONSOLE_USB_CDC 1
#define CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF 1
#define CONFIG_LIBC_STDIN_LINE_ENDING_CR 1

#include <Arduino.h>

#include <esp_console.h>
#include <esp_err.h>
#include <esp_log.h>
#include <argtable3/argtable3.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

// -------------------- Serial-backed printf/vprintf --------------------

static int serial_vprintf(const char *fmt, va_list ap) {
  // Keep buffers modest; if you print huge help text, it will be chunked.
  char buf[512];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0) return n;

  // vsnprintf returns required length (may exceed buffer).
  // If it didn't fit, send what we have.
  size_t to_write = (size_t)min(n, (int)sizeof(buf) - 1);

#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
  // Convert bare '\n' to "\r\n" for Windows terminals.
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

// -------------------- NVS --------------------

static void initialize_nvs(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// -------------------- Example command: heap --------------------

static int heap_size_cmd(int argc, char **argv) {
  (void)argc; (void)argv;
  uint32_t hs = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
  printf("min heap size: %u\n", (unsigned)hs);
  return 0;
}

static void register_heap(void) {
  const esp_console_cmd_t heap_cmd = {
    .command = "heap",
    .help    = "Get minimum size of free heap memory that was available during program execution",
    .hint    = NULL,
    .func    = &heap_size_cmd,
    .argtable= NULL,
  };
  ESP_ERROR_CHECK(esp_console_cmd_register(&heap_cmd));
}

// -------------------- Console init --------------------

static void initialize_console(void) {
  esp_console_config_t console_config = {
    .max_cmdline_length = 256,
    .max_cmdline_args   = 8,
#if CONFIG_LOG_COLORS
    .hint_color         = atoi(LOG_COLOR_CYAN),
#endif
  };
  ESP_ERROR_CHECK(esp_console_init(&console_config));

  // Register commands
  esp_console_register_help_command();
  register_heap();
}

static const char *prompt = "esp32s3> ";

// -------------------- Line input from Serial (blocking) --------------------

static bool read_line_serial(char *out, size_t out_cap) {
  if (!out || out_cap < 2) return false;

  size_t len = 0;

  while (true) {
    // Block until a byte arrives
    while (Serial.available() == 0) {
      delay(1);
    }

    int ch = Serial.read();
    if (ch < 0) continue;

    char c = (char)ch;

    // Handle CR/LF/CRLF
#if CONFIG_LIBC_STDIN_LINE_ENDING_CR
    if (c == '\r') {
      out[len] = 0;
      Serial.write("\r\n");
      return true;
    }
    if (c == '\n') {
      // ignore stray LF after CR, or treat as EOL if user sends LF-only
      if (len == 0) continue;
      out[len] = 0;
      Serial.write("\r\n");
      return true;
    }
#else
    if (c == '\r') continue;
    if (c == '\n') {
      out[len] = 0;
      Serial.write("\n");
      return true;
    }
#endif

    // Backspace (BS or DEL)
    if (c == '\b' || c == 0x7F) {
      if (len > 0) {
        len--;
        // erase: "\b \b"
        Serial.write("\b \b");
      }
      continue;
    }

    // Ignore other control chars
    if ((uint8_t)c < 0x20) continue;

    // Append if room
    if (len + 1 < out_cap) {
      out[len++] = c;
      Serial.write((uint8_t)c); // echo
    }
  }
}

// -------------------- Arduino entrypoints --------------------

void setup() {
  Serial.begin(115200);

  // Give host time to open the port; don’t block forever.
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 1500) delay(10);

  // Route printf/ESP-IDF console output to Serial
  esp_log_set_vprintf(&serial_vprintf);

  initialize_nvs();
  initialize_console();

  printf("\n"
         "ESP-IDF console (esp_console_run) over Serial/USB\n"
         "Type 'help' for commands. Try: heap\n\n");
}

void loop() {
  static char line[256];

  // Print prompt once per command
  Serial.print(prompt);

  if (!read_line_serial(line, sizeof(line))) {
    delay(2);
    return;
  }

  // Trim leading spaces
  char *p = line;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == 0) return;

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
