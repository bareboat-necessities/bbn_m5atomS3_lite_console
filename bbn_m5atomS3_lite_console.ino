#define CONFIG_ESP_CONSOLE_USB_CDC 1

#include <Arduino.h>
#include <USB.h>
#include <Esp.h>
#include <esp_console.h>
#include <linenoise/linenoise.h>
#include <argtable3/argtable3.h>

// Configuration
#define MAX_LINE_LENGTH 256
#define MAX_HISTORY 50
static const char* PROMPT = "\033[1;32mesp32s3>\033[0m\r ";
//static const char* PROMPT = "";

// Function prototypes
static int help_command(int argc, char** argv);
static int version_command(int argc, char** argv);
static int echo_command(int argc, char** argv);
static int reboot_command(int argc, char** argv);
void register_commands();
void console_task(void* parameter);

void setup() {
  // Start USB serial (CDC)
  //USB.begin();
  Serial.begin(115200);
  while (!Serial) delay(10);

  // Initialize console
  esp_console_config_t console_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT(); //ESP_CONSOLE_CONFIG_DEFAULT();
  console_config.max_cmdline_length = MAX_LINE_LENGTH;
  ESP_ERROR_CHECK(esp_console_init(&console_config));

  // Configure line editing library
  linenoiseSetMultiLine(0);
  linenoiseSetCompletionCallback(&esp_console_get_completion);
  linenoiseAllowEmpty(false);
  linenoiseHistorySetMaxLen(MAX_HISTORY);
  //linenoiseSetPrompt(PROMPT);

  // Register commands
  register_commands();

  // Start console task
  xTaskCreate(console_task, "console_task", 4096, NULL, 1, NULL);

  Serial.println("\nESP32-S3 USB Console Ready (Arduino-ESP32 3.1)");
  Serial.println("Type 'help' for command list");
}

void loop() {
  // Main loop can be used for other tasks
  delay(1000);
}

// Console task running in background
void console_task(void* parameter) {
  while (true) {
    if (Serial.available() <= 0) {
      continue;
    }
    // Get command line
    char* line = linenoise(PROMPT); //linenoise(linenoiseGetPrompt());
    if (line == NULL) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }
    //Serial.print(line);

    // Add to history
    if (strlen(line) > 0) {
      linenoiseHistoryAdd(line);
    }

    // Execute command
    int ret;
    esp_err_t err = esp_console_run(line, &ret);
    if (err == ESP_ERR_NOT_FOUND) {
      //Serial.println("Error: Unrecognized command");
    } else if (err == ESP_ERR_INVALID_ARG) {
      // Empty command
    } else if (err == ESP_OK && ret != ESP_OK) {
      Serial.printf("Command failed (0x%x)\n", ret);
    } else if (err != ESP_OK) {
      Serial.printf("Internal error: %s\n", esp_err_to_name(err));
    }

    int len = strlen(line);
    while (Serial.available() > 0) {
      char c = Serial.read();
      //Serial.printf("%c\n", c);
    }
    // Free memory
    linenoiseFree(line);
    //linenoisePrintPrompt();
  }
}

// ======= Command Implementations ======= //

static int help_command(int argc, char** argv) {
  Serial.printf("Help\n");
  //esp_console_print_help();
  return ESP_OK;
}

static int version_command(int argc, char** argv) {
  //Serial.printf("Arduino-ESP32 Version: %s\n", ESP.getArduinoVersion());
  Serial.printf("SDK Version: %s\n", ESP.getSdkVersion());
  Serial.printf("Chip Model: %s\n", ESP.getChipModel());
  Serial.printf("CPU Cores: %d\n", ESP.getChipCores());
  Serial.printf("Flash Size: %dMB\n", ESP.getFlashChipSize() / (1024 * 1024));
  Serial.printf("PSRAM Size: %dMB\n", ESP.getPsramSize() / (1024 * 1024));
  return ESP_OK;
}

static int echo_command(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    if (i > 1) Serial.print(" ");
    Serial.print(argv[i]);
  }
  Serial.println();
  return ESP_OK;
}

static int reboot_command(int argc, char** argv) {
  Serial.println("Rebooting...");
  delay(100);
  ESP.restart();
  return ESP_OK;
}

// ======= Command Registration ======= //

void register_commands() {
  const esp_console_cmd_t help_cmd = {
    .command = "help",
    .help = "Print list of commands",
    .hint = NULL,
    .func = &help_command,
    .argtable = NULL
  };
  esp_console_cmd_register(&help_cmd);

  const esp_console_cmd_t version_cmd = {
    .command = "version",
    .help = "Get system version information",
    .hint = NULL,
    .func = &version_command,
    .argtable = NULL
  };
  esp_console_cmd_register(&version_cmd);

  const esp_console_cmd_t echo_cmd = {
    .command = "echo",
    .help = "Echo the input arguments",
    .hint = NULL,
    .func = &echo_command,
    .argtable = NULL
  };
  esp_console_cmd_register(&echo_cmd);

  const esp_console_cmd_t reboot_cmd = {
    .command = "reboot",
    .help = "Reboot the system",
    .hint = NULL,
    .func = &reboot_command,
    .argtable = NULL
  };
  esp_console_cmd_register(&reboot_cmd);
}
