/**
 * @file wifi.c
 * @author Francisco Herrara (fherrera@lifia.info.unlp.edu.ar)
 * @brief WiFi station. Once it got an ip, will try to
 *        connect to a SNTP server and fetch the time and update the C
 *        library runtime data for the new timezone.
 * @version 0.1
 * @date 2022-10-16
 *
 * @copyright Copyright (c) 2022
 *
 */

/* Includes ------------------------------------------------------------------*/
#include <string.h>
#include <time.h>

#include "credentials.h"
#include "display.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/apps/sntp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "spiffs_wifi.h"

/* Private macro -------------------------------------------------------------*/
#define ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD

#define MAXIMUM_RETRY    3
#define WAIT_AFTER_RETRY pdMS_TO_TICKS(5000) // WAIT_AFTER_RETRY must be smaller than WAIT_FOR_EVENT
#define WAIT_FOR_EVENT   pdMS_TO_TICKS(40000)
#define NOTIFY_TASK(event) \
    if (smartconfig_mode)  \
        xTaskNotify(task_handle, event, eSetValueWithOverwrite);

/* Private function prototypes -----------------------------------------------*/
static void smartconfig_task(void* parm);
static void event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data);
/* SNTP */
static void setting_up_time();
static void obtain_time();

/* Private variables ---------------------------------------------------------*/
static const char*   TAG = "wifi_station";
static int           retry_num = 0;
static bool          smartconfig_mode = false;
static wifi_config_t wifi_config = { 0 };
static TaskHandle_t  task_handle = NULL;

static const int CONNECTED_BIT = BIT0;
static const int ESPTOUCH_DONE_BIT = BIT1;
static const int RETRIED_BIT = BIT2;

/* Exported functions --------------------------------------------------------*/
void wifi_init_sta()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (CONFIG_FOUND == wifi_config_read(&wifi_config)) {
        ESP_LOGI(TAG, "Recovered credentials from flash memory");
        ESP_LOGD(TAG, "saved ssid: %s saved password: %s\n", wifi_config.sta.ssid, wifi_config.sta.password);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    } else {
        smartconfig_mode = true;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* Private functions ---------------------------------------------------------*/
static void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            smartconfig_mode ? xTaskCreate(smartconfig_task, "smartconfig", 4096, NULL, 3, &task_handle)
                             : esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (retry_num++ < MAXIMUM_RETRY) {
                ESP_LOGI(TAG, "retry to connect to the AP");
                esp_wifi_connect();
                vTaskDelay(WAIT_AFTER_RETRY);
                NOTIFY_TASK(RETRIED_BIT);
                break;
            }
            if (smartconfig_mode) {
/*                 send_err(10, 20, "Failed smartconfig"); */
                send_err(0, 20, "Wrong credentials or AP unreachable. Restarting");
                vTaskDelay(pdMS_TO_TICKS(3000));
                esp_restart();
            };
            smartconfig_mode = true;
            ESP_LOGE(TAG, "Wrong credentials or AP unreachable. Start smartconfig");
            xTaskCreate(smartconfig_task, "smartconfig_task", 4096, NULL, 3, &task_handle);
            break;
        }

    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            setting_up_time(); /* Got ip, now configure time */
            retry_num = 0;
            NOTIFY_TASK(CONNECTED_BIT);
        }

    } else if (event_base == SC_EVENT) {
        switch (event_id) {
        case SC_EVENT_SCAN_DONE:
            ESP_LOGI(TAG, "Scan done");
            break;
        case SC_EVENT_FOUND_CHANNEL:
            ESP_LOGI(TAG, "Found channel");
            break;
        case SC_EVENT_GOT_SSID_PSWD:
            ESP_LOGI(TAG, "Got SSID and password");

            smartconfig_event_got_ssid_pswd_t* evt = (smartconfig_event_got_ssid_pswd_t*)event_data;

            memset(&wifi_config, 0, sizeof(wifi_config_t));
            memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
            memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

            wifi_config.sta.bssid_set = evt->bssid_set;
            if (wifi_config.sta.bssid_set) {
                memcpy(wifi_config.sta.bssid, evt->bssid, sizeof(wifi_config.sta.bssid));
            }

            ESP_ERROR_CHECK(esp_wifi_disconnect());
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            esp_wifi_connect();
            break;
        case SC_EVENT_SEND_ACK_DONE:
            NOTIFY_TASK(ESPTOUCH_DONE_BIT);
            break;
        }
    }
}

static void setting_up_time()
{
    time_t    now;
    struct tm timeinfo;
    char      strftime_buf[64];

    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Getting time over NTP.");
        obtain_time();
    }
    // Set timezone to Argentina Standard Time
    setenv("TZ", "ART+3", 1);
    tzset(); /*update C library runtime data for the new timezone.*/
    time(&now);
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGE(TAG, "The current date/time error");
    } else {
        strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
        ESP_LOGI(TAG, "The current date/time in Argentina is: %s", strftime_buf);
    }
}

static void obtain_time()
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    // wait for time to be set
    time_t    now = 0;
    struct tm timeinfo = { 0 };
    int       retry = 0;
    const int retry_count = 10;

    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }
}

static void smartconfig_task(void* parm)
{
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    uint32_t notif;

    while (xTaskNotifyWait(pdFALSE, ULONG_MAX, &notif, WAIT_FOR_EVENT)) {
        switch (notif) {
        case CONNECTED_BIT:
            ESP_LOGI(TAG, "WiFi Connected to ap");
            wifi_config_write(&wifi_config);
            break;
        case ESPTOUCH_DONE_BIT:
            ESP_LOGI(TAG, "smartconfig over");
            esp_smartconfig_stop();
            smartconfig_mode = false;
            vTaskDelete(NULL);
            return;
        case RETRIED_BIT:
            break;
        }
    }
    ESP_LOGW(TAG, "Timeout waiting for connection. Restarting");
    esp_restart();
}
