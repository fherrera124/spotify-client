## Spotify-display

#Steps:

https://www.rinconingenieril.es/el-boton-iot/

https://experiments.rinconingenieril.es/boton-spotify/

Create an app on spotify


create credentials.h file, put it inside include folder, define the macros

#define REFRESH_TOKEN "..."
#define AUTH_TOKEN    "..."

#define CONFIG_ESP_WIFI_SSID     "..."
#define CONFIG_ESP_WIFI_PASSWORD "..."
#define CONFIG_ESP_MAXIMUM_RETRY 5

## Display section
Used st7920 in SPI mode

Tested on ESP8266 LoLin V3 NodeMCU

|LCD pin|LCD pin name|ESP32|ESP8266|
|--|--|--|--|
|#01| GND| GND|
|#02| VCC |VCC (5V)|
|#04| RS |  D10/CS or any pin|
|#05| R/W|  D11/MOSI|
|#06| E  |  D13/SCK|
|#15| PSB|  GND (for SPI mode)|
|#19| BLA | D9, VCC or any pin via 300ohm resistor|
|#20| BLK | GND|

## Rotary encoder
