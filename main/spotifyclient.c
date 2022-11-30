/* Includes ------------------------------------------------------------------*/
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "limits.h"

#include "credentials.h"
#include "display.h"
#include "handler_callbacks.h"
#include "spotifyclient.h"

/* Private macro -------------------------------------------------------------*/
#define PLAYER              "/me/player"
#define TOKEN_URL           "https://accounts.spotify.com/api/token"
#define PLAYING             PLAYER "?market=AR&additional_types=episode"
#define PLAY                PLAYER "/play"
#define PAUSE               PLAYER "/pause"
#define PREV                PLAYER "/previous"
#define NEXT                PLAYER "/next"
#define PLAYERURL(ENDPOINT) "https://api.spotify.com/v1" ENDPOINT
#define ACQUIRE_LOCK(mux)   xSemaphoreTake(mux, portMAX_DELAY)
#define RELEASE_LOCK(mux)   xSemaphoreGive(mux)
#define RETRIES_ERR_CONN    3
#define SPRINTF_BUF_SIZE    100

/* -"204" on "GET /me/player" means the actual device is inactive
 * -"204" on "PUT /me/player" means playback sucessfuly transfered
 *   to an active device (although my Sangean returns 202) */
#define DEVICE_INACTIVE(state) (                  \
    !strcmp(s_state.endpoint, PLAYERURL(PLAYING)) \
    && s_state.method == HTTP_METHOD_GET && s_state.status_code == 204)

#define PLAYBACK_TRANSFERED(state) (             \
    !strcmp(s_state.endpoint, PLAYERURL(PLAYER)) \
    && s_state.method == HTTP_METHOD_PUT         \
    && (s_state.status_code == 204 || s_state.status_code == 202))

#define PREPARE_CLIENT(state, AUTH, TYPE)                              \
    esp_http_client_set_url(s_state.client, s_state.endpoint);         \
    esp_http_client_set_method(s_state.client, s_state.method);        \
    esp_http_client_set_header(s_state.client, "Authorization", AUTH); \
    esp_http_client_set_header(s_state.client, "Content-Type", TYPE)

#define SWAP_PTRS(pt1, pt2) \
    TrackInfo* temp = pt1;  \
    pt1 = pt2;              \
    pt2 = temp

/* DRY macros */
#define CALLOC(var, size)  \
    var = calloc(1, size); \
    assert((var) && "Error allocating memory")

/* Private types -------------------------------------------------------------*/
typedef void (*handler_cb_t)(char*, esp_http_client_event_t*);

typedef struct {
    Tokens                   tokens; /*!<*/
    const char*              endpoint; /*!<*/
    int                      status_code; /*!<*/
    esp_err_t                err; /*!<*/
    esp_http_client_method_t method; /*!<*/
    esp_http_client_handle_t client; /*!<*/
    handler_cb_t             handler_cb; /*!< Callback function to handle http events */
} Client_state_t;

/* Locally scoped variables --------------------------------------------------*/
static const char*       TAG = "SPOTIFY_CLIENT";
static char              http_buffer[MAX_HTTP_BUFFER];
static char              sprintf_buf[SPRINTF_BUF_SIZE];
static SemaphoreHandle_t client_lock = NULL; /* Mutex to manage access to the http client handle */
static uint8_t           s_retries = 0; /* number of retries on error connections */
static Client_state_t    s_state = { .tokens.access_token = { 'B', 'e', 'a', 'r', 'e', 'r', ' ', '\0' } };
static const char*       HTTP_METHOD_LOOKUP[] = { "GET", "POST", "PUT" };

/* Globally scoped variables definitions -------------------------------------*/
TaskHandle_t PLAYING_TASK = NULL;
TrackInfo*   TRACK = &(TrackInfo) { 0 }; /* pointer to an unnamed object, constructed in place
by the the COMPOUND LITERAL expression "(TrackInfo) { 0 }". NOTE: Although the syntax of a compound
literal is similar to a cast, the important distinction is that a cast is a non-lvalue expression
while a compound literal is an lvalue */

/* External variables --------------------------------------------------------*/
extern const char spotify_cert_pem_start[] asm("_binary_spotify_cert_pem_start");
extern const char spotify_cert_pem_end[] asm("_binary_spotify_cert_pem_end");

/* Private function prototypes -----------------------------------------------*/
static esp_err_t validate_token();
static esp_err_t _http_event_handler(esp_http_client_event_t* evt);
static void      now_playing_task(void* pvParameters);
static void      free_track(TrackInfo* track);
static void      handle_track_fetched(TrackInfo** new_track);
static void      handle_err_connection();
static void      debug_mem();

/* Exported functions --------------------------------------------------------*/
void spotify_client_init(UBaseType_t priority)
{
    esp_http_client_config_t config = {
        .url = "https://api.spotify.com/v1",
        .event_handler = _http_event_handler,
        .cert_pem = spotify_cert_pem_start,
    };

    // strcpy(s_state.tokens.access_token, "Bearer ");
    CALLOC(TRACK->name, 1);

    s_state.client = esp_http_client_init(&config);
    assert(s_state.client && "Error on esp_http_client_init()");

    client_lock = xSemaphoreCreateMutex();
    assert(client_lock && "Error on xSemaphoreCreateMutex()");

    s_state.handler_cb = default_http_event_handler;

    init_functions_cb();

    int res = xTaskCreate(now_playing_task, "now_playing_task", 4096, NULL, priority, &PLAYING_TASK);
    assert((res == pdPASS) && "Error creating task");
}

void player_cmd(rotary_encoder_event_t* event)
{
    Player_cmd_t cmd;

    if (event->event_type == BUTTON_EVENT) {
        cmd = cmdToggle;
    } else {
        cmd = event->re_state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE ? cmdNext : cmdPrev;
    }
    switch (cmd) {
    case cmdToggle:
        s_state.method = HTTP_METHOD_PUT;
        s_state.endpoint = TRACK->isPlaying ? PLAYERURL(PAUSE) : PLAYERURL(PLAY);
        break;
    case cmdPrev:
        s_state.method = HTTP_METHOD_POST;
        s_state.endpoint = PLAYERURL(PREV);
        break;
    case cmdNext:
        s_state.method = HTTP_METHOD_POST;
        s_state.endpoint = PLAYERURL(NEXT);
        break;
    default:
        ESP_LOGE(TAG, "unknow command");
        return;
    }

    ACQUIRE_LOCK(client_lock);
    validate_token();
    s_state.handler_cb = default_http_event_handler;

    PREPARE_CLIENT(s_state, s_state.tokens.access_token, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", s_state.endpoint);
    s_state.err = esp_http_client_perform(s_state.client);
    s_state.status_code = esp_http_client_get_status_code(s_state.client);
    int length = esp_http_client_get_content_length(s_state.client);

    if (s_state.err == ESP_OK) {
        s_retries = 0;
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", s_state.status_code, length);
        if (cmd == cmdToggle) {
            /* If for any reason, we dont have the actual state
             * of the player, then when sending play command when
             * paused, or viceversa, we receive error 403. */
            if (s_state.status_code == 403) {
                if (strcmp(s_state.endpoint, PLAYERURL(PLAY)) == 0) {
                    s_state.endpoint = PLAYERURL(PAUSE);
                } else {
                    s_state.endpoint = PLAYERURL(PLAY);
                }
                esp_http_client_set_url(s_state.client, s_state.endpoint);
                goto retry; // add max number of retries maybe
            } else { /* all ok?? */
                TRACK->isPlaying = !TRACK->isPlaying;
            }
        } else {
            /* The command was prev or next, change track in progress */
            vTaskDelay(pdMS_TO_TICKS(1000)); /* wait for the server to update the current track */
            UNBLOCK_PLAYING_TASK; /* unblock task before reach MS_NOTIF_POLLING timeout */
        }
    } else {
        handle_err_connection();
        goto retry;
    }

    RELEASE_LOCK(client_lock);

    ESP_LOGD(TAG, "[PLAYER-TASK]: stack watermark: %d", uxTaskGetStackHighWaterMark(NULL));
}

void http_user_playlists()
{
    ACQUIRE_LOCK(client_lock);
    validate_token();
    s_state.handler_cb = playlists_handler;
    s_state.method = HTTP_METHOD_GET;
    s_state.endpoint = PLAYERURL("/me/playlists?offset=0&limit=50");

    PREPARE_CLIENT(s_state, s_state.tokens.access_token, "application/json");
retry:
    s_state.err = esp_http_client_perform(s_state.client);
    s_state.status_code = esp_http_client_get_status_code(s_state.client);
    if (s_state.err == ESP_OK) {
        s_retries = 0;
    } else {
        handle_err_connection();
        goto retry;
    }
    RELEASE_LOCK(client_lock);
}

void http_available_devices()
{
    ACQUIRE_LOCK(client_lock);
    validate_token();
    s_state.handler_cb = default_http_event_handler;
    s_state.endpoint = PLAYERURL(PLAYER "/devices");
    s_state.method = HTTP_METHOD_GET;
    PREPARE_CLIENT(s_state, s_state.tokens.access_token, "application/json");

    s_state.err = esp_http_client_perform(s_state.client);
    s_state.status_code = esp_http_client_get_status_code(s_state.client);
    esp_http_client_set_post_field(s_state.client, NULL, 0);

    ESP_LOGW(TAG, "Active devices:\n%s", http_buffer);

    esp_err_t err = parse_available_devices(http_buffer);

    RELEASE_LOCK(client_lock);
    (ESP_OK == err) ? NOTIFY_DISPLAY(ACTIVE_DEVICES_FOUND)
                    : NOTIFY_DISPLAY(NO_ACTIVE_DEVICES);
}

void http_set_device(const char* dev_id)
{
    ACQUIRE_LOCK(client_lock);
    int str_len = sprintf(sprintf_buf, "{\"device_ids\":[\"%s\"],\"play\":true}", dev_id); // TODO: true if now playing, else false
    assert((str_len <= SPRINTF_BUF_SIZE) && "Device id too long");
    validate_token();
    s_state.handler_cb = default_http_event_handler;
    s_state.method = HTTP_METHOD_PUT;
    s_state.endpoint = PLAYERURL(PLAYER);
    esp_http_client_set_post_field(s_state.client, sprintf_buf, str_len);

    PREPARE_CLIENT(s_state, s_state.tokens.access_token, "application/json");
retry:
    s_state.err = esp_http_client_perform(s_state.client);
    s_state.status_code = esp_http_client_get_status_code(s_state.client);
    esp_http_client_set_post_field(s_state.client, NULL, 0); /* Clear post field */
    if (s_state.err == ESP_OK) {
        s_retries = 0;
        if (PLAYBACK_TRANSFERED(s_state)) {
            ESP_LOGI(TAG, "Playback transfered to: %s", dev_id);
            NOTIFY_DISPLAY(PLAYBACK_TRANSFERRED_OK);
        } else {
            NOTIFY_DISPLAY(PLAYBACK_TRANSFERRED_FAIL);
        }
    } else {
        handle_err_connection();
        goto retry;
    }
    RELEASE_LOCK(client_lock);
}

void http_play_context_uri(const char* uri)
{
    ACQUIRE_LOCK(client_lock);
    int str_len = sprintf(sprintf_buf, "{\"context_uri\":\"%s\"}", uri);
    assert((str_len <= SPRINTF_BUF_SIZE) && "uri too long");
    validate_token();
    s_state.handler_cb = default_http_event_handler;
    s_state.method = HTTP_METHOD_PUT;
    s_state.endpoint = PLAYERURL(PLAY);

    esp_http_client_set_post_field(s_state.client, sprintf_buf, str_len);
    PREPARE_CLIENT(s_state, s_state.tokens.access_token, "application/json");
    s_state.err = esp_http_client_perform(s_state.client);
    s_state.status_code = esp_http_client_get_status_code(s_state.client);
    esp_http_client_set_post_field(s_state.client, NULL, 0);
    RELEASE_LOCK(client_lock);
}

/* Private functions ---------------------------------------------------------*/

static esp_err_t validate_token()
{
    /* client_lock lock already must be aquired */

    if ((s_state.tokens.expiresIn - 10) > time(0))
        return ESP_OK;

    ESP_LOGD(TAG, "Access Token expired or expiring soon. Fetching a new one.");
    s_state.handler_cb = default_http_event_handler;
    s_state.method = HTTP_METHOD_POST;
    s_state.endpoint = TOKEN_URL;
    PREPARE_CLIENT(s_state, "Basic " AUTH_TOKEN, "application/x-www-form-urlencoded");

    const char* post_data = "grant_type=refresh_token&refresh_token=" REFRESH_TOKEN;
    esp_http_client_set_post_field(s_state.client, post_data, strlen(post_data));
    s_state.err = esp_http_client_perform(s_state.client);
    s_state.status_code = esp_http_client_get_status_code(s_state.client);
    esp_http_client_set_post_field(s_state.client, NULL, 0); /* Clear post field */

    if (s_state.err != ESP_OK || s_state.status_code != 200) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s, status code: %d",
            esp_err_to_name(s_state.err), s_state.status_code);
        ESP_LOGE(TAG, "The answer was:\n%s", http_buffer);
        return ESP_FAIL;
    }

    parseTokens(http_buffer, &s_state.tokens);

    ESP_LOGW(TAG, "Access Token obtained:\n%s", &s_state.tokens.access_token[8]);
    ESP_LOGW(TAG, "Access Token length:\n%d", strlen(s_state.tokens.access_token));
    return ESP_OK;
}

static inline void handle_track_fetched(TrackInfo** new_track)
{
    parseTrackInfo(http_buffer, *new_track);

    SWAP_PTRS(*new_track, TRACK);

    if (0 == strcmp(TRACK->name, (*new_track)->name)) {
        free_track(*new_track);
        NOTIFY_DISPLAY(SAME_TRACK);
    } else {
        free_track(*new_track);
        ESP_LOGI(TAG, "New track");
        ESP_LOGI(TAG, "Title: %s", TRACK->name);
        StrListItem* artist = TRACK->artists.first;
        while (artist) {
            ESP_LOGI(TAG, "Artist: %s", artist->str);
            artist = artist->next;
        }
        ESP_LOGI(TAG, "Album: %s", TRACK->album);
        NOTIFY_DISPLAY(NEW_TRACK);
    }
}

static inline void handle_err_connection()
{
    ESP_LOGE(TAG, "HTTP %s request failed: %s",
        HTTP_METHOD_LOOKUP[s_state.method],
        esp_err_to_name(s_state.err));
    assert((++s_retries <= RETRIES_ERR_CONN) && "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "Retrying %d/%d...", s_retries, RETRIES_ERR_CONN);
    debug_mem();
}

static esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    s_state.handler_cb(http_buffer, evt);
    return ESP_OK;
}

static void now_playing_task(void* pvParameters)
{
    TrackInfo* new_track = &(TrackInfo) { 0 };
    CALLOC(new_track->name, 1);

    while (1) {
        bool     first_try = true;
        uint32_t notif;

        xTaskNotifyWait(
            pdFALSE, /* bits to clear on entry (if there are no pending notifications) */
            ULONG_MAX, /* bits to clear on exit (if there are pending notifications) */
            &notif, /* Stores the notified value */
            portMAX_DELAY); /* xTicksToWait */

        if (notif == ENABLE_TASK)
            do {
                ACQUIRE_LOCK(client_lock);
                validate_token();
                s_state.handler_cb = default_http_event_handler;
                s_state.method = HTTP_METHOD_GET;
                s_state.endpoint = PLAYERURL(PLAYING);

            prepare:
                PREPARE_CLIENT(s_state, s_state.tokens.access_token, "application/json");

            retry:
                s_state.err = esp_http_client_perform(s_state.client);

                s_state.status_code = esp_http_client_get_status_code(s_state.client);
                esp_http_client_set_post_field(s_state.client, NULL, 0); /* Clear post field */
                if (s_state.err == ESP_OK) {
                    s_retries = 0;
                    ESP_LOGD(TAG, "Received:\n%s", http_buffer);
                    if (s_state.status_code == 200) {
                        handle_track_fetched(&new_track);
                        goto exit;
                    }
                    if (s_state.status_code == 401) { /* bad token or expired */
                        ESP_LOGW(TAG, "Token expired, getting a new one");
                        goto prepare;
                    }
                    if (DEVICE_INACTIVE(s_state)) { /* Playback not available or active */
                        ESP_LOGW(TAG, "Device inactive");
                        if (first_try && TRACK->device.id) {
                            first_try = false;
                            int str_len = sprintf(sprintf_buf, "{\"device_ids\":[\"%s\"],\"play\":false}", TRACK->device.id);
                            assert((str_len <= SPRINTF_BUF_SIZE) && "device id too long");
                            validate_token();
                            s_state.handler_cb = default_http_event_handler;
                            s_state.method = HTTP_METHOD_PUT;
                            s_state.endpoint = PLAYERURL(PLAYER);
                            esp_http_client_set_post_field(s_state.client, sprintf_buf, str_len);
                            goto prepare;
                        } else {
                            ESP_LOGW(TAG, "Failed to reconnect with the device");
                            first_try = true;
                            NOTIFY_DISPLAY(LAST_DEVICE_FAILED);
                            goto exit;
                        }
                    }
                    if (PLAYBACK_TRANSFERED(s_state)) {
                        ESP_LOGI(TAG, "Reconnected with device: %s", TRACK->device.id);
                        first_try = true;
                        goto exit;
                    }
                    /* Unhandled status_code follows */
                    ESP_LOGE(TAG, "ENDPOINT: %s, METHOD: %s, STATUS_CODE: %d", s_state.endpoint,
                        HTTP_METHOD_LOOKUP[s_state.method], s_state.status_code);
                    if (*http_buffer) {
                        ESP_LOGE(TAG, "%s", http_buffer);
                    }
                    goto exit;
                } else {
                    handle_err_connection();
                    goto retry;
                }
            exit:
                RELEASE_LOCK(client_lock);
                debug_mem();
                xTaskNotifyWait(pdFALSE, ULONG_MAX, &notif, pdMS_TO_TICKS(MS_NOTIF_POLLING));
            } while (notif != DISABLE_TASK);
    }
    assert(false && "Unexpected exit of infinite task loop");
}

static inline void debug_mem()
{
    /* uxTaskGetStackHighWaterMark() returns the minimum amount of remaining
     * stack space that was available to the task since the task started
     * executing - that is the amount of stack that remained unused when the
     * task stack was at its greatest (deepest) value. This is what is referred
     * to as the stack 'high water mark'.
     * */
    ESP_LOGI(TAG, "[NOW_PLAYING]: stack high water mark: %d", uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "[NOW_PLAYING]: minimum free heap size: %d", esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "[NOW_PLAYING]: free heap size: %d", esp_get_free_heap_size());
}

static inline void free_track(TrackInfo* track)
{
    free(track->name);
    free(track->album);

    strListClear(&track->artists);

    free(track->device.id);
    free(track->device.name);
    free(track->device.type);
    free(track->device.volume_percent);
}