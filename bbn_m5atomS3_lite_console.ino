#include <M5AtomS3.h>

//#define CONFIG_ESP_CONSOLE_USB_CDC
//#undef CONFIG_ESP_CONSOLE_UART_DEFAULT 
//#undef CONFIG_ESP_CONSOLE_UART_CUSTOM 

#include <M5Unified.h>
#include <ESP32Console.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ESP32Console/Helpers/PWDHelpers.h>

using namespace ESP32Console;

Console console;

void setup() {
  auto cfg = M5.config();
  AtomS3.begin(cfg);
  
  //Initalize SPIFFS and mount it on /spiffs
  //SPIFFS.begin(true, "/spiffs");

  //Modify prompt to show current file path (%pwd% get replaced by the filepath)
  //console.setPrompt("ESP32 %pwd%> ");
  console.setPrompt("m5> ");

  //Set HOME env for easier navigating (type cd to jump to home)
  setenv("HOME", "/spiffs", 1);
  //Set PWD to env
  //console_chdir("/spiffs");

  //Initialize Serial port on UART0 (the onboard USB Serial Converter)
  //Serial.begin(115200);

  //console.begin(115200, G2, G3, 0);
  console.begin(115200);
  //Serial.begin(115200);
  //Serial.begin(115200, G2, G3, 0);

  //Enable the saving of our command history to SPIFFS. You will be able to see it, when you type ls in your console.
  //console.enablePersistentHistory("/spiffs/.history.txt");

  console.registerSystemCommands();

  //Register the VFS specific commands
  //console.registerVFSCommands();

  //delay(2000);
  //Serial.flush();
}

void loop() {
  AtomS3.update();
  delay(100);
}
