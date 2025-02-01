# bbn_m5atomS3_lite_console
Console on esp32 example

## Console

Console unfortunately works only on UART. USB CDC and other OTG modes support in Arduino IDE would't work.
While IDF supports it, it requires whole recompilation of IDF esp32 libraries with different options.
And Arduino IDE doesn't support it. So console via regular USB you connect your esp32 doesn't work as of now.

You would need USB to UART TTL adapter with CH340 chip or similar. 
