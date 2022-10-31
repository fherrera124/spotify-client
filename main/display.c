/* Includes ------------------------------------------------------------------*/
#include "buffer_callbacks.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "parseobjects.h"
#include "rotary_encoder.h"
#include "spotifyclient.h"
#include "strlib.h"
#include "u8g2_esp8266_hal.h"

/* Private macro -------------------------------------------------------------*/

/* Private types -------------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static void setup_display(u8g2_t* u8g2);
static void display_task(void* arg);
static void initial_menu_page(u8g2_t* u8g2);
static void currently_playing_page(u8g2_t* u8g2);
static void track_menu_context(u8g2_t* u8g2);
static void playlists_page(u8g2_t* u8g2);

/* Locally scoped variables --------------------------------------------------*/
QueueHandle_t playing_queue_hlr;
QueueHandle_t encoder_queue_hlr;
const char*   TAG = "ST7920";

/* Globally scoped variables -------------------------------------------------*/
TaskHandle_t MENU_TASK_HLR;
Playlists_t* PLAYLISTS = NULL;

/* Imported function prototypes ----------------------------------------------*/
uint8_t userInterfaceSelectionList(u8g2_t* u8g2, QueueHandle_t queue,
    const char* title, uint8_t start_pos,
    const char* sl);

/* Exported functions --------------------------------------------------------*/
esp_err_t display_init(UBaseType_t priority, QueueHandle_t encoder_q_hlr,
    QueueHandle_t playing_q_hlr)
{
    encoder_queue_hlr = encoder_q_hlr;
    playing_queue_hlr = playing_q_hlr;
    if (pdPASS == xTaskCreate(display_task, "display_task", 4096, NULL, priority, &MENU_TASK_HLR))
        return ESP_OK;
    return ESP_FAIL;
}

/* Private functions ---------------------------------------------------------*/
static void setup_display(u8g2_t* u8g2)
{
    u8g2_Setup_st7920_s_128x64_f(u8g2, U8G2_R0, u8x8_byte_esp8266_hw_spi,
        u8x8_gpio_and_delay_esp8266); // init u8g2 structure

    u8g2_InitDisplay(u8g2); // send init sequence to the display, display is in sleep mode after this
    u8g2_ClearDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0); // wake up display
    // u8g2_SetFont(u8g2, u8g2_font_tom_thumb_4x6_mr);
}

static void display_task(void* args)
{
    u8g2_t u8g2;
    setup_display(&u8g2);

    while (1) {
        initial_menu_page(&u8g2);
    }
    vTaskDelete(NULL);
}

static void initial_menu_page(u8g2_t* u8g2)
{
    uint8_t selection = 1;

    u8g2_SetFont(u8g2, u8g2_font_6x12_tr);

    do {
        selection = userInterfaceSelectionList(u8g2, encoder_queue_hlr,
            "Spotify", selection,
            "abcdef\nNow playing\nMy playlists");
        switch (selection) {
        case 1:
            break;
        case 2:
            return currently_playing_page(u8g2);
        case 3:
            return playlists_page(u8g2);
        default:
            break;
        }
    } while (1);
}

static void playlists_page(u8g2_t* u8g2)
{
    uint8_t selection = 1;

    assert(PLAYLISTS == NULL);

    PLAYLISTS = calloc(1, sizeof(*PLAYLISTS));
    assert(PLAYLISTS);

    PLAYLISTS->uris = calloc(1, sizeof(*PLAYLISTS->uris));
    assert(PLAYLISTS->uris);

    http_user_playlists();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    assert(PLAYLISTS->name_list);

    selection = userInterfaceSelectionList(u8g2, encoder_queue_hlr,
        "My Playlists", selection,
        PLAYLISTS->name_list);

    assert(selection <= PLAYLISTS->uris->count);

    StrListItem* uri = PLAYLISTS->uris->first;

    for (uint16_t i = 1; i < selection; i++) {
        uri = uri->next;
    }

    ESP_LOGI(TAG, "URI: %s", uri->str);

    http_play_context_uri(uri->str);

    free(PLAYLISTS->name_list);
    PLAYLISTS->name_list = NULL;
    strListClear(PLAYLISTS->uris);
    free(PLAYLISTS->uris);
    free(PLAYLISTS);
    PLAYLISTS = NULL;

    return initial_menu_page(u8g2);
}

static void currently_playing_page(u8g2_t* u8g2)
{
    static TrackInfo track = { .name = "Awaiting track..." };

    u8g2_SetFont(u8g2, u8g2_font_helvB18_tr);
    u8g2_uint_t x;
    u8g2_uint_t offset = 0;
    u8g2_uint_t width = 15 + u8g2_GetStrWidth(u8g2, track.name);

    while (1) {
        if (pdFALSE == xQueueReceive(playing_queue_hlr, &track, pdMS_TO_TICKS(50))) {
            /* No new track, keep scrolling current track */
            x = offset;
            u8g2_ClearBuffer(u8g2);

            /* Fake progress bar */
            u8g2_DrawFrame(u8g2, 20, u8g2->height - 5, u8g2->width - 20, 5);
            u8g2_DrawBox(u8g2, 20, u8g2->height - 5, 30, 5);

            do {
                u8g2_DrawStr(u8g2, x, 40, track.name);
                x += width;
            } while (x < u8g2->width);

            u8g2_SendBuffer(u8g2);

            offset -= 1; // scroll by one pixel
            if ((u8g2_uint_t)offset < (u8g2_uint_t)-width) {
                offset = 0; // start over again
            }
        } else { /* New track, reset variables */
            ESP_LOGW(TAG, "New track event");
            width = 10 + u8g2_GetStrWidth(u8g2, track.name);

            offset = 0;
        }

        rotary_encoder_event_t queue_event;

        if (pdTRUE == xQueueReceive(encoder_queue_hlr, &queue_event, 0)) {
            /* intercept the encoder event */

            if (queue_event.event_type == BUTTON_EVENT) {
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    player_cmd(&queue_event);
                    break;
                case MEDIUM_PRESS:
                    track_menu_context(u8g2);
                    break;
                case LONG_PRESS:
                    initial_menu_page(u8g2);
                    break;
                }
            } else { /* ROTARY_ENCODER_EVENT */
                player_cmd(&queue_event);
                /* Only interested for the first shift (left or rigth). Then, we
                 * put the task in suspention, while the ISR is overwriting the 1
                 * length queue in the lapse of your fingers rotating the encoder.
                 * When the task is active again, the task reset the queue,
                 * discarding the last move of the encoder */
                vTaskDelay(pdMS_TO_TICKS(500));
                xQueueReset(encoder_queue_hlr);
            }
        }
    }
}

static void track_menu_context(u8g2_t* u8g2)
{
    uint8_t selection = 1;

    const char* sl = "artist\nqueue\nas\nkauhs\nBack\nMain Menu";

    u8g2_SetFont(u8g2, u8g2_font_6x12_tr);

    do {
        selection = userInterfaceSelectionList(u8g2, encoder_queue_hlr,
            "Track options", selection,
            sl);
        switch (selection) {
        case 1:
            /* code */
            break;
        case 2:
            /* code */
            break;
        case 5:
            return currently_playing_page(u8g2);
        case 6:
            return initial_menu_page(u8g2);
        default:
            break;
        }

    } while (1);
}