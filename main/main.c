/* SPOTIFY HTTP Client

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/* Includes ------------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "rotary_encoder.h"
#include "spotifyclient.h"

/* External variables --------------------------------------------------------*/

/* Private typedef -----------------------------------------------------------*/

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/
static const char* TAG = "MAIN";

rotary_encoder_info_t info = {0};

/* Imported function prototypes ----------------------------------------------*/
void wifi_init_sta(void);

esp_err_t display_init(UBaseType_t   priority,
                       QueueHandle_t encoder_q_hlr,
                       QueueHandle_t playing_q_hlr);

esp_err_t rotary_encoder_default_init(rotary_encoder_info_t* info);

/**/

void app_main(void) {
    QueueHandle_t playing_queue_hlr;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    wifi_init_sta();

    ESP_ERROR_CHECK(rotary_encoder_default_init(&info));
    ESP_ERROR_CHECK(spotify_client_init(5, &playing_queue_hlr));
    ESP_ERROR_CHECK(display_init(5, info.queue, playing_queue_hlr));
}
