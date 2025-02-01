#include <M5AtomS3.h>
#include <ESP32Console.h>

using namespace ESP32Console;

Console console;

void setup() {
  auto cfg = M5.config();
  AtomS3.begin(cfg);

  //Serial2.begin(9600, SERIAL_8N1, G6, G5);  // UART pins when USB-to-TTL is plugged into port C of ATOMIC PortABC Extension Base
  //Serial2.begin(9600, SERIAL_8N1, G1, G2);  // UART pins when USB-to-TTL is plugged directly into grove port of M5 AtomS3-lite

  // Initialize Serial port on UART0 (the onboard USB Serial Converter)
  Serial.begin(115200);

  // Enable ESP32Console on Pin 6 & 5 on UART1
  console.begin(9600, G6, G5, 1);

  console.registerSystemCommands();
  //console.registerNetworkCommands();
}

void loop() {
  // We can use Serial on UART0 as usual, while ESP32Console is available on the Pins above.
  Serial.println("Test");
  delay(1000);
}
