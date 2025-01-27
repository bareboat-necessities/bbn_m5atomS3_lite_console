#define CONFIG_ESP_CONSOLE_USB_CDC 1

#include <M5AtomS3.h>
Console console;

void setup() {   
    //Initialize Serial port on UART0 (the onboard USB Serial Converter)
    Serial.begin(115200);
    
    //Enable ESP32Console on Pin 12 & 14 on UART1
    console.begin(115200, 12, 14, 1);


    console.registerSystemCommands();
    console.registerNetworkCommands();
}

void loop() {
    //We can use Serial on UART0 as usual, while ESP32Console is available on the Pins above.
    Serial.println("Test");
    delay(1000);
}
