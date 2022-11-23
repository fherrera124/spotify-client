/* Includes ------------------------------------------------------------------*/
#include "esp_log.h"

#include "display.h"
#include "handler_callbacks.h"
#include "spotifyclient.h"
#include "strlib.h"
#include "u8g2_esp8266_hal.h"

#include "selection_list.h"

/* Private macro -------------------------------------------------------------*/
#define TICKSTOWAIT pdMS_TO_TICKS(50)

#define MARGIN_RIGHT 5

#define DRAW_STR(u8g2, x, y, font, str) \
    u8g2_ClearBuffer(u8g2);             \
    u8g2_SetFont(u8g2, font);           \
    u8g2_DrawStr(u8g2, x, y, str);      \
    u8g2_SendBuffer(u8g2)

/* Private types -------------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
static void setup_display(u8g2_t* u8g2);
static void display_task(void* arg);
static void initial_menu_page(u8g2_t* u8g2);
static void now_playing_page(u8g2_t* u8g2);
static void now_playing_context_menu(u8g2_t* u8g2);
static void playlists_page(u8g2_t* u8g2);
static void available_devices_page(u8g2_t* u8g2);

/* Locally scoped variables --------------------------------------------------*/
static QueueHandle_t encoder;
static const char*   TAG = "DISPLAY";

/* Globally scoped variables definitions -------------------------------------*/
TaskHandle_t DISPLAY_TASK = NULL;

/* Exported functions --------------------------------------------------------*/
void display_init(UBaseType_t priority, QueueHandle_t encoder_queue_hlr)
{
    encoder = encoder_queue_hlr;
    int res = xTaskCreate(display_task, "display_task", 4096, NULL, priority, &DISPLAY_TASK);
    assert((res == pdPASS) && "Error creating task");
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
        selection = userInterfaceSelectionList(u8g2, encoder,
            "Spotify", selection,
            "Available devices\nNow playing\nMy playlists",
            portMAX_DELAY);
        switch (selection) {
        case 1:
            return available_devices_page(u8g2);
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
    DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "Retrieving user playlists...");

    uint8_t selection = 1;

    assert(PLAYLISTS == NULL);

    PLAYLISTS = calloc(1, sizeof(*PLAYLISTS));
    assert(PLAYLISTS && "Error allocating memory");

    PLAYLISTS->values = calloc(1, sizeof(*PLAYLISTS->values));
    assert(PLAYLISTS->values && "Error allocating memory");

    http_user_playlists();
    uint32_t notif;
    xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

    if (notif == PLAYLISTS_EMPTY) {
        DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "User doesn't have playlists");
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else if (notif == PLAYLISTS_OK) {
        u8g2_ClearBuffer(u8g2);
        u8g2_SetFont(u8g2, u8g2_font_6x12_tr);
        selection = userInterfaceSelectionList(u8g2, encoder,
            "My Playlists", selection,
            PLAYLISTS->items_string,
            portMAX_DELAY);

        StrListItem* uri = PLAYLISTS->values->first;

        for (uint16_t i = 1; i < selection; i++) {
            uri = uri->next;
        }

        ESP_LOGI(TAG, "URI: %s", uri->str);

        http_play_context_uri(uri->str);
        vTaskDelay(50);
        UNBLOCK_PLAYING_TASK;
    }
    /* cleanup */
    if (PLAYLISTS->items_string) {
        free(PLAYLISTS->items_string);
        PLAYLISTS->items_string = NULL;
    }
    if (PLAYLISTS->values) {
        strListClear(PLAYLISTS->values);
        free(PLAYLISTS->values);
        PLAYLISTS->values = NULL;
    }
    free(PLAYLISTS);
    PLAYLISTS = NULL;

    return now_playing_page(u8g2);
}

static void now_playing_page(u8g2_t* u8g2)
{
    ENABLE_PLAYING_TASK;
    DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "Retrieving player state...");

    /* wait for the current track */

    uint32_t notif;
    xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

    if (notif == LAST_DEVICE_FAILED) {
        DISABLE_PLAYING_TASK;
        ESP_LOGD(TAG, "No device playing");
        DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "No device playing");
        vTaskDelay(pdMS_TO_TICKS(3000));
        return available_devices_page(u8g2);
    }
    // else...
    u8g2_SetFont(u8g2, u8g2_font_helvB14_te);
    u8g2_uint_t track_width = u8g2_GetUTF8Width(u8g2, TRACK->name) + MARGIN_RIGHT;
    u8g2_uint_t offset = 0;
    TickType_t  finished_scroll = 0;
    TickType_t  start = xTaskGetTickCount();
    time_t      progress_base = TRACK->progress_ms;
    time_t      last_progress = 0, progress_ms = 0;
    char        mins[3], secs[3];
    strcpy(mins, u8x8_u8toa(progress_base / 60000, 2));
    strcpy(secs, u8x8_u8toa((progress_base / 1000) % 60, 2));
    enum {
        paused,
        playing,
        toBePaused,
        toBeUnpaused,
    } track_state
        = TRACK->isPlaying ? playing : paused;

    while (1) {

        /* Intercept any encoder event -----------------------------------------------*/

        rotary_encoder_event_t queue_event;
        if (pdTRUE == xQueueReceive(encoder, &queue_event, 0)) {
            if (queue_event.event_type == BUTTON_EVENT) {
                switch (queue_event.btn_event) {
                case SHORT_PRESS:
                    track_state = TRACK->isPlaying ? toBePaused : toBeUnpaused;
                    player_cmd(&queue_event);
                    break;
                case MEDIUM_PRESS:
                    DISABLE_PLAYING_TASK;
                    return now_playing_context_menu(u8g2);
                    break;
                case LONG_PRESS:
                    DISABLE_PLAYING_TASK;
                    return initial_menu_page(u8g2);
                    break;
                }
            } else { /* ROTARY_ENCODER_EVENT intercepted */
                player_cmd(&queue_event);
                /* now block the task to ignore the values the ISR is storing
                 * in the queue while the rotary encoder is still moving */
                vTaskDelay(pdMS_TO_TICKS(500));
                /* The task is active again. Reset the queue to discard
                 * the last move of the rotary encoder */
                xQueueReset(encoder);
            }
        }

        /* Wait for track event ------------------------------------------------------*/

        if (pdPASS == xTaskNotifyWait(0, ULONG_MAX, &notif, TICKSTOWAIT)) {
            start = xTaskGetTickCount();
            progress_base = TRACK->progress_ms;

            if (notif == SAME_TRACK) {
                ESP_LOGD(TAG, "Same track event");
            } else if (notif == NEW_TRACK) {
                ESP_LOGD(TAG, "New track event");
                last_progress = offset = 0;
                u8g2_SetFont(u8g2, u8g2_font_helvB14_te);
                track_width = u8g2_GetUTF8Width(u8g2, TRACK->name) + MARGIN_RIGHT;
            } else if (notif == LAST_DEVICE_FAILED) {
                DISABLE_PLAYING_TASK;
                ESP_LOGW(TAG, "Last device failed");
                DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "Device disconected...");
                vTaskDelay(pdMS_TO_TICKS(3000));
                return available_devices_page(u8g2);
            }

            track_state = TRACK->isPlaying ? playing : paused;

        } else { /* TICKSTOWAIT expired */
            TickType_t finish = xTaskGetTickCount();
            switch (track_state) {
            case playing:;
                time_t prg = progress_base + pdTICKS_TO_MS(finish - start);
                /* track finished, early unblock of PLAYING_TASK */
                if (prg > TRACK->duration_ms) {
                    /* only notify once */
                    if (progress_ms != TRACK->duration_ms) {
                        progress_ms = TRACK->duration_ms;
                        vTaskDelay(50);
                        ESP_LOGW(TAG, "End of track, unblock playing task");
                        UNBLOCK_PLAYING_TASK;
                    }
                } else {
                    progress_ms = prg;
                }
                break;
            case paused:
                progress_ms = progress_base;
                break;
            case toBePaused:
                track_state = paused;
                progress_base = progress_ms;
                break;
            case toBeUnpaused:
                track_state = playing;
                start = xTaskGetTickCount();
                break;
            default:
                break;
            }
            strcpy(mins, u8x8_u8toa(progress_ms / 60000, 2));
            /* detect a second increment */
            if ((progress_ms / 1000) != (last_progress / 1000)) {
                last_progress = progress_ms;
                strcpy(secs, u8x8_u8toa((progress_ms / 1000) % 60, 2));
                ESP_LOGD(TAG, "Time: %s:%s", mins, secs);
            }
        }

        /* Display track information -------------------------------------------------*/

        u8g2_SetFont(u8g2, u8g2_font_helvB14_te);
        u8g2_ClearBuffer(u8g2);

        /* Track name */
        u8g2_DrawUTF8(u8g2, offset, 35, TRACK->name);

        if ((track_width - MARGIN_RIGHT) > u8g2->width) {
            /* wait 500ms before start scrolling again */
            if ((xTaskGetTickCount() - finished_scroll) > pdMS_TO_TICKS(500)) {
                offset -= 1; // scroll by one pixel
                if ((u8g2_uint_t)offset < (u8g2_uint_t)(u8g2->width - track_width)) {
                    offset = 0; // start over again
                    finished_scroll = xTaskGetTickCount();
                }
            }
        }
        /* Track artists */
        /* IMPLEMENT */

        /* Time progress */
        u8g2_SetFont(u8g2, u8g2_font_tom_thumb_4x6_mr);
        u8g2_DrawStr(u8g2, 0, u8g2->height, mins);
        u8g2_DrawStr(u8g2, u8g2_GetStrWidth(u8g2, mins) - 1, u8g2->height, ":");
        u8g2_DrawStr(u8g2, u8g2_GetStrWidth(u8g2, mins) + 3, u8g2->height, secs);

        /* Progress bar */
        const uint16_t max_bar_width = u8g2->width - 20;
        u8g2_DrawFrame(u8g2, 20, u8g2->height - 5, max_bar_width, 5);
        float progress_percent = ((float)(progress_ms)) / TRACK->duration_ms;
        long  bar_width = progress_percent * max_bar_width;
        u8g2_DrawBox(u8g2, 20, u8g2->height - 5, (u8g2_uint_t)bar_width, 5);

        u8g2_SendBuffer(u8g2);
    }
}

static void now_playing_context_menu(u8g2_t* u8g2)
{
    uint8_t selection = 1;

    const char* sl = "artist\nqueue\nas\nkauhs\nBack\nMain Menu";

    u8g2_SetFont(u8g2, u8g2_font_6x12_tr);

    do {
        selection = userInterfaceSelectionList(u8g2, encoder,
            "Track options", selection,
            sl, portMAX_DELAY);
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

static void available_devices_page(u8g2_t* u8g2)
{
    DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "Retrieving available devices...");
    uint8_t selection;
update_list:
    selection = 1;

    DEVICES = calloc(1, sizeof(*DEVICES));
    assert(DEVICES && "Error allocating memory for DEVICES");

    DEVICES->values = calloc(1, sizeof(*DEVICES->values));
    assert(DEVICES->values && "Error allocating memory for DEVICES->values");

    http_available_devices();
    uint32_t notif;
    xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

    if (notif == ACTIVE_DEVICES_FOUND) {
        u8g2_SetFont(u8g2, u8g2_font_6x12_tr);
        selection = userInterfaceSelectionList(u8g2, encoder,
            "Select a device", selection,
            DEVICES->items_string,
            pdMS_TO_TICKS(10000));

        if (selection == MENU_EVENT_TIMEOUT)
            goto cleanup;

        StrListItem* device = DEVICES->values->first;

        for (uint16_t i = 1; i < selection; i++) {
            device = device->next;
        }

        ESP_LOGI(TAG, "DEVICE ID: %s", device->str);

        http_set_device(device->str);
        u8g2_SetFont(u8g2, u8g2_font_tom_thumb_4x6_mr);
        xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);
        u8g2_ClearBuffer(u8g2);

        if (notif == PLAYBACK_TRANSFERRED_OK) {
            u8g2_DrawStr(u8g2, 0, 20, "Playback transferred to device");
        } else if (notif == PLAYBACK_TRANSFERRED_FAIL) {
            u8g2_DrawStr(u8g2, 0, 20, "Device failed");
        }
        u8g2_SendBuffer(u8g2);
        vTaskDelay(pdMS_TO_TICKS(3000));

    } else if (notif == NO_ACTIVE_DEVICES) {
        DRAW_STR(u8g2, 0, 20, u8g2_font_tom_thumb_4x6_mr, "No devices found :c");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
cleanup:
    if (DEVICES->items_string) {
        free(DEVICES->items_string);
        DEVICES->items_string = NULL;
    }
    if (DEVICES->values) {
        strListClear(DEVICES->values);
        free(DEVICES->values);
    }
    free(DEVICES);
    DEVICES = NULL;

    if (selection == MENU_EVENT_TIMEOUT)
        goto update_list;
    return now_playing_page(u8g2); // TODO: make dynamic
}