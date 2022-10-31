/* Includes ------------------------------------------------------------------*/
#include "spotifyclient.h"

#include <string.h>

#include "buffer_callbacks.h"
#include "credentials.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "parseobjects.h"
#include "rotary_encoder.h"

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
#define MAX_HTTP_BUFFER     8192
#define RETRIES_ERR_CONN    2

/* DRY macros */
#define CALLOC_ESP_FAIL(var, size)                 \
    var = calloc(1, size);                         \
    if (var == NULL) {                             \
        ESP_LOGE(TAG, "Error allocating memmory"); \
        return ESP_FAIL;                           \
    }

#define CALLOC_LABEL(var, size, label)             \
    var = calloc(1, size);                         \
    if (var == NULL) {                             \
        ESP_LOGE(TAG, "Error allocating memmory"); \
        goto label;                                \
    }

/* Private function prototypes -----------------------------------------------*/
static esp_err_t   _validate_token();
static inline void get_active_devices(StrList*);
static inline void free_track(TrackInfo* track);
static inline void handle_new_track(TrackInfo** new_track);
static inline void handle_err_connection();
static esp_err_t   _http_event_handler(esp_http_client_event_t* evt);
static void        now_playing_task(void* pvParameters);

/* Private variables ---------------------------------------------------------*/
static const char*       TAG = "SPOTIFY_CLIENT";
char*                    buffer;
static SemaphoreHandle_t client_lock = NULL; /* Mutex to manage access to the http client handle */
short                    retries = 0; /* number of retries on error connections */
static Tokens*           tokens;
TrackInfo*               track;
Client_state_t           state = { 0 };
QueueHandle_t            playing_queue_hlr;
TaskHandle_t             playing_task_hlr; // for notify the task
QueueHandle_t*           encoder_queue;

static const char* HTTP_METHOD_LOOKUP[] = {
    "GET",
    "POST",
    "PUT",
};

extern const char spotify_cert_pem_start[] asm("_binary_spotify_cert_pem_start");
extern const char spotify_cert_pem_end[] asm("_binary_spotify_cert_pem_end");

#define PREPARE_CLIENT(state, AUTH, TYPE)                            \
    esp_http_client_set_url(state.client, state.endpoint);           \
    esp_http_client_set_method(state.client, state.method);          \
    esp_http_client_set_header(state.client, "Authorization", AUTH); \
    esp_http_client_set_header(state.client, "Content-Type", TYPE)

/* -"204" on "GET /me/player" means the actual device is inactive
 * -"204" on "PUT /me/player" means playback sucessfuly transfered
 *   to an active device (although my Sangean returns 202) */
#define DEVICE_INACTIVE(state) (                \
    !strcmp(state.endpoint, PLAYERURL(PLAYING)) \
    && state.method == HTTP_METHOD_GET && state.status_code == 204)

#define PLAYBACK_TRANSFERED(state) (           \
    !strcmp(state.endpoint, PLAYERURL(PLAYER)) \
    && state.method == HTTP_METHOD_PUT         \
    && (state.status_code == 204 || state.status_code == 202))

#define SWAP_PTRS(pt1, pt2) \
    TrackInfo* temp = pt1;  \
    pt1 = pt2;              \
    pt2 = temp

/* Exported functions --------------------------------------------------------*/
esp_err_t
spotify_client_init(UBaseType_t priority, QueueHandle_t* playing_q_hlr)
{
    esp_http_client_config_t config = {
        .url = "https://api.spotify.com/v1",
        .event_handler = _http_event_handler,
        .cert_pem = spotify_cert_pem_start,
    };

    buffer = malloc(MAX_HTTP_BUFFER);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Error allocating memmory for HTTP buffer");
        return ESP_FAIL;
    }

    state.access_token = malloc(256);
    if (state.access_token == NULL)
        return ESP_FAIL;

    state.client = esp_http_client_init(&config);
    if (state.client == NULL)
        return ESP_FAIL;

    CALLOC_ESP_FAIL(tokens, sizeof(*tokens));
    CALLOC_ESP_FAIL(tokens->access_token, 1);
    CALLOC_ESP_FAIL(track, sizeof(*track));
    CALLOC_ESP_FAIL(track->name, 1);
    CALLOC_ESP_FAIL(track->artists, sizeof(*track->artists));
    CALLOC_ESP_FAIL(track->device, sizeof(*track->device));

    client_lock = xSemaphoreCreateMutex();
    if (client_lock == NULL)
        return ESP_FAIL;

    state.buffer_cb = default_fun;

    init_functions_cb();

    int res = xTaskCreate(now_playing_task, "now_playing_task", 4096, NULL, priority, &playing_task_hlr);
    if (res == pdPASS) {
        /* Create a queue for events from the playing task. Tasks can
           read from this queue to receive up to date playing track.*/
        playing_queue_hlr = xQueueCreate(1, sizeof(TrackInfo));
        *playing_q_hlr = playing_queue_hlr;
        return ESP_OK;
    }
    return ESP_FAIL;
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
        state.method = HTTP_METHOD_PUT;
        state.endpoint = track->isPlaying ? PLAYERURL(PAUSE) : PLAYERURL(PLAY);
        break;
    case cmdPrev:
        state.method = HTTP_METHOD_POST;
        state.endpoint = PLAYERURL(PREV);
        break;
    case cmdNext:
        state.method = HTTP_METHOD_POST;
        state.endpoint = PLAYERURL(NEXT);
        break;
    default:
        ESP_LOGE(TAG, "unknow command");
        return;
    }

    ACQUIRE_LOCK(client_lock);
    _validate_token();
    state.buffer_cb = default_fun;

    PREPARE_CLIENT(state, state.access_token, "application/json");
retry:
    ESP_LOGD(TAG, "Endpoint to send: %s", state.endpoint);
    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    int length = esp_http_client_get_content_length(state.client);

    if (state.err == ESP_OK) {
        retries = 0;
        ESP_LOGD(TAG, "HTTP Status Code = %d, content_length = %d", state.status_code, length);
        if (cmd == cmdToggle) {
            /* If for any reason, we dont have the actual state
             * of the player, then when sending play command when
             * paused, or viceversa, we receive error 403. */
            if (state.status_code == 403) {
                if (strcmp(state.endpoint, PLAYERURL(PLAY)) == 0) {
                    state.endpoint = PLAYERURL(PAUSE);
                } else {
                    state.endpoint = PLAYERURL(PLAY);
                }
                esp_http_client_set_url(state.client, state.endpoint);
                goto retry; // add max number of retries maybe
            } else { /* all ok?? */
                track->isPlaying = !track->isPlaying;
            }
        } else {
            /* The command was prev or next, so notify to the now_playing_task
             * to cut the default delay and fetch the new track info. TODO:
             * only notify if the status code of prev/next is 200 like */
            vTaskDelay(pdMS_TO_TICKS(1000)); /* wait for the server to update the current track */
            xTaskNotifyGive(playing_task_hlr);
        }
    } else {
        handle_err_connection();
        goto retry;
    }

    RELEASE_LOCK(client_lock);

    ESP_LOGW(TAG, "[PLAYER-TASK]: stack watermark: %d", uxTaskGetStackHighWaterMark(NULL));
}

void http_user_playlists()
{
    ACQUIRE_LOCK(client_lock);
    _validate_token();
    state.buffer_cb = get_playlists;
    state.method = HTTP_METHOD_GET;
    state.endpoint = PLAYERURL("/me/playlists?offset=0&limit=50");

    PREPARE_CLIENT(state, state.access_token, "application/json");
    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    RELEASE_LOCK(client_lock);
    /* if (state.err == ESP_OK) {
        ESP_LOGI(TAG, "Received:\n%s", buffer);
    } */
}

void http_play_context_uri(const char* uri)
{
    char* buf = malloc(19 + strlen(uri));

    sprintf(buf, "{\"context_uri\":\"%s\"}", uri);

    ACQUIRE_LOCK(client_lock);
    _validate_token();
    state.buffer_cb = default_fun;
    state.method = HTTP_METHOD_PUT;
    state.endpoint = PLAYERURL(PLAY);

    esp_http_client_set_post_field(state.client, buf, strlen(buf));
    PREPARE_CLIENT(state, state.access_token, "application/json");
    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    esp_http_client_set_post_field(state.client, NULL, 0);
    RELEASE_LOCK(client_lock);
    free(buf);
}

/* Private functions ---------------------------------------------------------*/

static esp_err_t _validate_token()
{
    /* client_lock lock already must be aquired */

    if ((tokens->expiresIn - 10) > time(0))
        return ESP_OK;
    free(tokens->access_token);
    ESP_LOGD(TAG, "Access Token expired or expiring soon. Fetching a new one.");
    state.buffer_cb = default_fun;
    state.method = HTTP_METHOD_POST;
    state.endpoint = TOKEN_URL;
    PREPARE_CLIENT(state, "Basic " AUTH_TOKEN, "application/x-www-form-urlencoded");

    const char* post_data = "grant_type=refresh_token&refresh_token=" REFRESH_TOKEN;
    esp_http_client_set_post_field(state.client, post_data, strlen(post_data));
    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    esp_http_client_set_post_field(state.client, NULL, 0); /* Clear post field */

    if (state.err != ESP_OK || state.status_code != 200) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s, status code: %d",
            esp_err_to_name(state.err), state.status_code);
        ESP_LOGE(TAG, "The answer was:\n%s", buffer);
        return ESP_FAIL;
    }
    if (tokenAllParsed != parseTokens(buffer, tokens)) {
        free(tokens->access_token); // unnecesary at the moment
        ESP_LOGE(TAG, "Error trying parse token from:\n%s", buffer);
        return ESP_FAIL;
    }
    strcpy(state.access_token, "Bearer ");
    strcat(state.access_token, tokens->access_token);
    ESP_LOGW(TAG, "Access Token obtained:\n%s", tokens->access_token);
    return ESP_OK;
}

static inline void get_active_devices(StrList* devices)
{
    /* client_lock lock already must be aquired */

    state.endpoint = PLAYERURL(PLAYER "/devices");
    state.method = HTTP_METHOD_GET;
    _validate_token();
    PREPARE_CLIENT(state, state.access_token, "application/json");

    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    esp_http_client_set_post_field(state.client, NULL, 0);

    ESP_LOGW(TAG, "Active devices:\n%s", buffer);

    available_devices(buffer, devices);
}

static inline void handle_new_track(TrackInfo** new_track)
{
    int val = parseTrackInfo(buffer, *new_track);

    if (trackAllParsed != val) {
        ESP_LOGE(TAG, "Error parsing track. Flags parsed: %x", val);
        ESP_LOGE(TAG, "\n%s", buffer);
        free_track(*new_track);
        return;
    }
    if (0 == strcmp(track->name, (*new_track)->name)) {
        /* same track */
        free_track(*new_track);
        return;
    }
    free_track(track);
    SWAP_PTRS(*new_track, track);
    ESP_LOGI(TAG, "New track");
    ESP_LOGI(TAG, "Title: %s", track->name);
    StrListItem* artist = track->artists->first;
    while (artist) {
        ESP_LOGI(TAG, "Artist: %s", artist->str);
        artist = artist->next;
    }
    ESP_LOGI(TAG, "Album: %s", track->album);

    xQueueOverwrite(playing_queue_hlr, track);
}

static inline void handle_err_connection()
{
    ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(state.err));
    if (retries > 0 && ++retries <= RETRIES_ERR_CONN) {
        ESP_LOGW(TAG, "Retrying %d/%d...", retries, RETRIES_ERR_CONN);
    } else {
        ESP_LOGW(TAG, "Restarting...");
        esp_restart();
    }
}

static esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    state.buffer_cb(buffer, evt);
    return ESP_OK;
}

static void now_playing_task(void* pvParameters)
{
    bool on_try_current_dev = false;

    char* buf = NULL;

    TrackInfo* new_track;
    CALLOC_LABEL(new_track, sizeof(*new_track), abort);
    CALLOC_LABEL(new_track->name, 1, abort);
    CALLOC_LABEL(new_track->device, sizeof(*new_track->device), abort);
    CALLOC_LABEL(new_track->artists, sizeof(*new_track->artists), abort);

    while (1) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000)); // TODO: config FETCH_FREQ_MS

        ACQUIRE_LOCK(client_lock);
        _validate_token();
        state.buffer_cb = default_fun;
        state.method = HTTP_METHOD_GET;
        state.endpoint = PLAYERURL(PLAYING);

    prepare:
        PREPARE_CLIENT(state, state.access_token, "application/json");

    retry:;
        state.err = esp_http_client_perform(state.client);
        state.status_code = esp_http_client_get_status_code(state.client);
        esp_http_client_set_post_field(state.client, NULL, 0); /* Clear post field */
        if (state.err == ESP_OK) {
            ESP_LOGD(TAG, "Received:\n%s", buffer);
            if (state.status_code == 200) {
                retries = 0;
                handle_new_track(&new_track);
                goto exit;
            }
            if (state.status_code == 401) { /* bad token or expired */
                ESP_LOGW(TAG, "Token expired, getting a new one");
                goto prepare;
            }
            if (DEVICE_INACTIVE(state)) { /* Playback not available or active */
                if (!on_try_current_dev && track->device->id) {
                    on_try_current_dev = true;

                    buf = malloc(33 + strlen(track->device->id)); // TODO: validate
                    sprintf(buf, "{\"device_ids\":[\"%s\"],\"play\":false}", track->device->id);
                    ESP_LOGW(TAG, "Device to transfer playback: %s", track->device->id);
                } else {
                    ESP_LOGW(TAG, "Failed connecting with last device");

                    StrList* devices;
                    CALLOC_LABEL(devices, sizeof(*devices), exit);
                    get_active_devices(devices);

                    // TODO: Here we should search for track->device->id
                    StrListItem* dev = devices->first; // pick the first device on the list

                    if (dev == NULL) {
                        ESP_LOGE(TAG, "No devices found :c");
                        free(devices);
                        goto exit;
                    }
                    buf = malloc(33 + strlen(dev->str)); // TODO: validate
                    sprintf(buf, "{\"device_ids\":[\"%s\"],\"play\":false}", dev->str);
                    free(track->device->id);
                    track->device->id = strdup(dev->str);
                    strListClear(devices);
                    free(devices);
                }

                _validate_token();
                state.buffer_cb = default_fun;
                state.method = HTTP_METHOD_PUT;
                state.endpoint = PLAYERURL(PLAYER);
                esp_http_client_set_post_field(state.client, buf, strlen(buf));
                goto prepare;
            }
            if (PLAYBACK_TRANSFERED(state)) {
                ESP_LOGI(TAG, "Playback transfered to: %s", track->device->id);
                on_try_current_dev = false;
                goto exit;
            }
            /* Unhandled status_code follows */
            ESP_LOGE(TAG, "ENDPOINT: %s, METHOD: %s, STATUS_CODE: %d", state.endpoint,
                HTTP_METHOD_LOOKUP[state.method], state.status_code);
            if (*buffer) {
                ESP_LOGE(TAG, buffer);
            }
            goto exit;
        } else {
            handle_err_connection();
            goto retry;
        }
    exit:
        free(buf);
        buf = NULL;
        RELEASE_LOCK(client_lock);
        /* uxTaskGetStackHighWaterMark() returns the minimum amount of remaining
         * stack space that was available to the task since the task started
         * executing - that is the amount of stack that remained unused when the
         * task stack was at its greatest (deepest) value. This is what is referred
         * to as the stack 'high water mark'.
         * */
        ESP_LOGD(TAG, "[CURRENTLY_PLAYING]: stack high water mark: %d", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGD(TAG, "[CURRENTLY_PLAYING]: minimum free heap size: %d", esp_get_minimum_free_heap_size());
        ESP_LOGD(TAG, "[CURRENTLY_PLAYING]: free heap size: %d", esp_get_free_heap_size());
    }
abort:
    free(buf);
    free_track(new_track);
    free(new_track);
    vTaskDelete(NULL);
}

static inline void free_track(TrackInfo* track)
{
    if (track == NULL)
        return;
    free(track->name);
    free(track->album);
    if (track->artists) {
        strListClear(track->artists);
    }
    if (track->device) {
        free(track->device->id);
        free(track->device->name);
        free(track->device->type);
        free(track->device->volume_percent);
    }
}