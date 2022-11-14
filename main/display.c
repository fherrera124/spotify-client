/* Includes ------------------------------------------------------------------*/
#include "esp_log.h"

#include "display.h"
#include "handler_callbacks.h"
#include "spotifyclient.h"
#include "strlib.h"
#include "u8g2_esp8266_hal.h"

/* Private macro -------------------------------------------------------------*/
#define MS_TO_SEC(ms) ((ms) / 1000) // no need for ms precision

/* Private types -------------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static void setup_display(u8g2_t* u8g2);
static void display_task(void* arg);
static void initial_menu_page(u8g2_t* u8g2);
static void now_playing_page(u8g2_t* u8g2);
static void track_menu_context(u8g2_t* u8g2);
static void playlists_page(u8g2_t* u8g2);

/* Locally scoped variables --------------------------------------------------*/
QueueHandle_t encoder_queue_hlr;
const char*   TAG = "ST7920";

/* Globally scoped variables definitions -------------------------------------*/
TaskHandle_t MENU_TASK = NULL;
Playlists_t* PLAYLISTS = NULL;

/* Imported function prototypes ----------------------------------------------*/
uint8_t userInterfaceSelectionList(u8g2_t* u8g2, QueueHandle_t queue,
    const char* title, uint8_t start_pos,
    const char* sl);

/* Exported functions --------------------------------------------------------*/
esp_err_t display_init(UBaseType_t priority, QueueHandle_t encoder_q_hlr)
{
    assert(PLAYING_TASK);

    encoder_queue_hlr = encoder_q_hlr;
    if (pdPASS == xTaskCreate(display_task, "display_task", 4096, NULL, priority, &MENU_TASK))
        return ESP_OK;
    return ESP_FAIL;
}

/* Private functions ---------------------------------------------------------*/
static void setup_display(u8g2_t* u8g2)
{
    u8g2_esp8266_hal_t u8g2_esp8266_hal = {
        .mosi = GPIO_NUM_13,
        .clk = GPIO_NUM_14,
        .cs = GPIO_NUM_15
    };
    u8g2_esp8266_hal_init(u8g2_esp8266_hal);
    u8g2_Setup_st7920_s_128x64_f(u8g2, U8G2_R0, u8g2_esp8266_spi_byte_cb,
        u8g2_esp8266_gpio_and_delay_cb); // init u8g2 structure

    u8g2_InitDisplay(u8g2); // send init sequence to the display, display is in sleep mode after this
    u8g2_ClearDisplay(u8g2);
    u8g2_SetPowerSave(u8g2, 0); // wake up display
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
            return now_playing_page(u8g2);
        case 3:
            return playlists_page(u8g2);
        default:
            break;
        }
    } while (1);
}

static void playlists_page(u8g2_t* u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_tom_thumb_4x6_mr);
    u8g2_DrawStr(u8g2, 10, 20, "Retrieving user playlists...");
    u8g2_SendBuffer(u8g2);

    uint8_t selection = 1;

    assert(PLAYLISTS == NULL);

    PLAYLISTS = calloc(1, sizeof(*PLAYLISTS));
    assert(PLAYLISTS);

    PLAYLISTS->uris = calloc(1, sizeof(*PLAYLISTS->uris));
    assert(PLAYLISTS->uris);

    http_user_playlists();
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (!PLAYLISTS->name_list || !PLAYLISTS->uris) {
        u8g2_ClearBuffer(u8g2);
        u8g2_DrawStr(u8g2, 0, 20, "Error getting playlists");
        u8g2_SendBuffer(u8g2);
        vTaskDelay(pdMS_TO_TICKS(3000));
        goto quit;
    }

    u8g2_SetFont(u8g2, u8g2_font_6x12_tr);
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
    vTaskDelay(50);
    UNBLOCK_PLAYING_TASK;

quit:
    if (PLAYLISTS->name_list) {
        free(PLAYLISTS->name_list);
        PLAYLISTS->name_list = NULL;
    }
    if (PLAYLISTS->uris) {
        strListClear(PLAYLISTS->uris);
        free(PLAYLISTS->uris);
    }
    free(PLAYLISTS);
    PLAYLISTS = NULL;

    return now_playing_page(u8g2);
}

static void now_playing_page(u8g2_t* u8g2)
{
    ENABLE_PLAYING_TASK;

    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_tom_thumb_4x6_mr);
    u8g2_DrawStr(u8g2, 10, 20, "Retrieving player state...");
    u8g2_SendBuffer(u8g2);

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    assert(TRACK);
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_helvB18_tr);
    u8g2_uint_t track_width = 10 + u8g2_GetStrWidth(u8g2, TRACK->name);
    u8g2_uint_t offset = 0, x = 0;

    TickType_t start, finish;
    time_t     pr, last_progress = 0, progress_sec = 0;
    char       m_str[3], s_str[3];
    uint32_t   notif;

    enum {
        paused,
        playing,
        toBePaused,
        toBeUnpaused,
    } track_state;

    pr = TRACK->progress_ms ? MS_TO_SEC(TRACK->progress_ms) : 0;

    strcpy(m_str, u8x8_u8toa(pr / 60, 2));
    strcpy(s_str, u8x8_u8toa(pr % 60, 2));

    start = xTaskGetTickCount();
    track_state = TRACK->isPlaying ? playing : paused;

    while (1) {

        /* Intercept any encoder event -----------------------------------------------*/

        rotary_encoder_event_t queue_event;

        if (pdTRUE == xQueueReceive(encoder_queue_hlr, &queue_event, 0)) {
            if (queue_event.event_type == BUTTON_EVENT) {
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    track_state = TRACK->isPlaying ? toBePaused : toBeUnpaused;
                    player_cmd(&queue_event);
                    break;
                case MEDIUM_PRESS:
                    DISABLE_PLAYING_TASK;
                    return track_menu_context(u8g2);
                    break;
                case LONG_PRESS:
                    DISABLE_PLAYING_TASK;
                    return initial_menu_page(u8g2);
                    break;
                }
            } else { /* ROTARY_ENCODER_EVENT intercepted */
                player_cmd(&queue_event);
                /* now block the task to ignore the values the ISR is putting
                 * in the queue while the rotary encoder is still moving */
                vTaskDelay(pdMS_TO_TICKS(500));
                /* The task is active again. Reset the queue to discard
                 * the last move of the encoder */
                xQueueReset(encoder_queue_hlr);
            }
        }

        /* Wait for track event ------------------------------------------------------*/

        if (pdPASS == xTaskNotifyWait(0, ULONG_MAX, &notif, pdMS_TO_TICKS(50))) {
            pr = MS_TO_SEC(TRACK->progress_ms);

            if ((notif & SAME_TRACK) != 0) {
                ESP_LOGW(TAG, "Same track event");
            } else if ((notif & NEW_TRACK) != 0) {
                ESP_LOGW(TAG, "New track event");
                last_progress = offset = 0;
                u8g2_SetFont(u8g2, u8g2_font_helvB18_tr);
                track_width = 20 + u8g2_GetStrWidth(u8g2, TRACK->name);
            }
            if (TRACK->isPlaying) {
                start = xTaskGetTickCount();
                track_state = playing;
            } else {
                track_state = paused;
            }
        } else { /* xTicksToWait expired */
            switch (track_state) {
            case playing:
                finish = xTaskGetTickCount();

                progress_sec = pr + pdTICKS_TO_MS((finish - start)) / 1000;
                if (progress_sec > MS_TO_SEC(TRACK->duration_ms)) {
                    ESP_LOGW(TAG, "End of track, unblock playing task");
                    vTaskDelay(50);
                    UNBLOCK_PLAYING_TASK;
                    continue;
                }
                break;
            case paused:
                progress_sec = pr;
                break;
            case toBePaused:
                ESP_LOGW(TAG, "EARLY PAUSE");
                track_state = paused;
                progress_sec = pr = last_progress;
                break;
            case toBeUnpaused:
                ESP_LOGW(TAG, "EARLY UNPAUSE");
                track_state = playing;
                start = xTaskGetTickCount();
                break;
            default:
                break;
            }
            strcpy(m_str, u8x8_u8toa(progress_sec / 60, 2));

            if (progress_sec != last_progress) { /* seconds incremented */
                last_progress = progress_sec;
                strcpy(s_str, u8x8_u8toa(progress_sec % 60, 2));
                ESP_LOGI(TAG, "Time: %s:%s", m_str, s_str);
            }
        }

        /* Display scrolling text and time progress ----------------------------------*/

        x = offset;

        u8g2_SetFont(u8g2, u8g2_font_helvB18_tr);
        u8g2_ClearBuffer(u8g2);

        /* Scrolling track name */
        do {
            u8g2_DrawStr(u8g2, x, 35, TRACK->name);
            x += track_width;
        } while (x < u8g2->width);

        /* Artists */
        /* IMPLEMENT */

        /* Time progress */
        u8g2_SetFont(u8g2, u8g2_font_tom_thumb_4x6_mr);
        u8g2_DrawStr(u8g2, 0, u8g2->height, m_str);
        u8g2_DrawStr(u8g2, u8g2_GetStrWidth(u8g2, m_str) - 1, u8g2->height, ":");
        u8g2_DrawStr(u8g2, u8g2_GetStrWidth(u8g2, m_str) + 3, u8g2->height, s_str);

        /* Progress bar */
        const uint16_t time_bar_width = u8g2->width - 20;
        u8g2_DrawFrame(u8g2, 20, u8g2->height - 5, time_bar_width, 5);
        float progress_percent = ((float)progress_sec) / (MS_TO_SEC(TRACK->duration_ms));
        int   bar_width = progress_percent * time_bar_width;
        u8g2_DrawBox(u8g2, 20, u8g2->height - 5, bar_width, 5);

        u8g2_SendBuffer(u8g2);

        offset -= 1; // scroll by one pixel
        if ((u8g2_uint_t)offset < (u8g2_uint_t)-track_width) {
            offset = 0; // start over again
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
            return now_playing_page(u8g2);
        case 6:
            return initial_menu_page(u8g2);
        default:
            break;
        }

    } while (1);
}