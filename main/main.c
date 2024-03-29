/* SPOTIFY HTTP Client

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Includes ------------------------------------------------------------------*/
#include "esp_log.h"
#include "nvs_flash.h"

#include "display.h"
#include "rotary_encoder.h"
#include "spotifyclient.h"
#include "esp_system.h"

/* External variables --------------------------------------------------------*/

/* Private typedef -----------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
static const char*    TAG = "MAIN";
rotary_encoder_info_t info = { 0 };

/* Imported function prototypes ----------------------------------------------*/
extern void wifi_init_sta(void);

/**/

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rotary_encoder_default_init(&info));
    display_init(5, info.queue);
    wifi_init_sta();
    spotify_client_init(5);
}
