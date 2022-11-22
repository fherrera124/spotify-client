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
#define MAX_HTTP_BUFFER     8192
#define RETRIES_ERR_CONN    3

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

#define PREPARE_CLIENT(state, AUTH, TYPE)                            \
    esp_http_client_set_url(state.client, state.endpoint);           \
    esp_http_client_set_method(state.client, state.method);          \
    esp_http_client_set_header(state.client, "Authorization", AUTH); \
    esp_http_client_set_header(state.client, "Content-Type", TYPE)

#define SWAP_PTRS(pt1, pt2) \
    TrackInfo* temp = pt1;  \
    pt1 = pt2;              \
    pt2 = temp

/* DRY macros */
#define CALLOC(var, size)  \
    var = calloc(1, size); \
    assert((var) && "Error allocating memory")

#define MALLOC(var, size) \
    var = malloc(size);   \
    assert((var) && "Error allocating memory")

#define MAX_DEV_RETRIES 3

/* Private types -------------------------------------------------------------*/
typedef void (*handler_cb_t)(char*, esp_http_client_event_t*);

typedef struct {
    char*                    access_token; /*!<*/
    const char*              endpoint; /*!<*/
    int                      status_code; /*!<*/
    esp_err_t                err; /*!<*/
    esp_http_client_method_t method; /*!<*/
    esp_http_client_handle_t client; /*!<*/
    handler_cb_t             handler_cb; /*!< Callback function to handle http events */
} Client_state_t;

/* Locally scoped variables --------------------------------------------------*/
static const char*       TAG = "SPOTIFY_CLIENT";
char*                    buffer;
static SemaphoreHandle_t client_lock = NULL; /* Mutex to manage access to the http client handle */
short                    retries = 0; /* number of retries on error connections */
static Tokens*           tokens;
static Client_state_t    state = { 0 };
QueueHandle_t*           encoder_queue;
static const char*       HTTP_METHOD_LOOKUP[] = { "GET", "POST", "PUT" };

/* Globally scoped variables definitions -------------------------------------*/
TaskHandle_t PLAYING_TASK = NULL;
TrackInfo*   TRACK = NULL;

/* External variables --------------------------------------------------------*/
extern const char spotify_cert_pem_start[] asm("_binary_spotify_cert_pem_start");
extern const char spotify_cert_pem_end[] asm("_binary_spotify_cert_pem_end");

/* Private function prototypes -----------------------------------------------*/
static esp_err_t   validate_token();
static inline void free_track(TrackInfo* track);
static inline void handle_track_fetched(TrackInfo** new_track);
static inline void handle_err_connection();
static esp_err_t   _http_event_handler(esp_http_client_event_t* evt);
static void        now_playing_task(void* pvParameters);

/* Exported functions --------------------------------------------------------*/
void spotify_client_init(UBaseType_t priority)
{
    esp_http_client_config_t config = {
        .url = "https://api.spotify.com/v1",
        .event_handler = _http_event_handler,
        .cert_pem = spotify_cert_pem_start,
    };

    MALLOC(buffer, MAX_HTTP_BUFFER);
    MALLOC(state.access_token, 256);

    state.client = esp_http_client_init(&config);
    assert(state.client && "Error on esp_http_client_init()");

    CALLOC(tokens, sizeof(*tokens));
    CALLOC(tokens->access_token, 1);
    CALLOC(TRACK, sizeof(*TRACK));
    CALLOC(TRACK->name, 1);
    CALLOC(TRACK->artists, sizeof(*TRACK->artists));
    CALLOC(TRACK->device, sizeof(*TRACK->device));

    client_lock = xSemaphoreCreateMutex();
    assert(client_lock && "Error on xSemaphoreCreateMutex()");

    state.handler_cb = default_event_handler;

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
        state.method = HTTP_METHOD_PUT;
        state.endpoint = TRACK->isPlaying ? PLAYERURL(PAUSE) : PLAYERURL(PLAY);
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
    validate_token();
    state.handler_cb = default_event_handler;

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
    state.handler_cb = playlists_handler;
    state.method = HTTP_METHOD_GET;
    state.endpoint = PLAYERURL("/me/playlists?offset=0&limit=50");

    PREPARE_CLIENT(state, state.access_token, "application/json");
retry:
    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    if (state.err == ESP_OK) {
        retries = 0;
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
    state.handler_cb = default_event_handler;
    state.endpoint = PLAYERURL(PLAYER "/devices");
    state.method = HTTP_METHOD_GET;
    PREPARE_CLIENT(state, state.access_token, "application/json");

    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    esp_http_client_set_post_field(state.client, NULL, 0);

    ESP_LOGW(TAG, "Active devices:\n%s", buffer);

    esp_err_t err = parse_available_devices(buffer);

    RELEASE_LOCK(client_lock);
    if (ESP_OK == err) {
        NOTIFY_DISPLAY(ACTIVE_DEVICES_FOUND);
    } else {
        free(DEVICES->items_string);
        strListClear(DEVICES->values);
        free(DEVICES->values);
        DEVICES->items_string = NULL;
        DEVICES->values = NULL;
        NOTIFY_DISPLAY(NO_ACTIVE_DEVICES);
    }
}

void http_set_device(const char* dev_id)
{
    char* buf = NULL;
    MALLOC(buf, 33 + strlen(dev_id));
    sprintf(buf, "{\"device_ids\":[\"%s\"],\"play\":true}", dev_id); // TODO: true if now playing, else false

    ACQUIRE_LOCK(client_lock);
    validate_token();
    state.handler_cb = default_event_handler;
    state.method = HTTP_METHOD_PUT;
    state.endpoint = PLAYERURL(PLAYER);
    esp_http_client_set_post_field(state.client, buf, strlen(buf));

    PREPARE_CLIENT(state, state.access_token, "application/json");
retry:
    state.err = esp_http_client_perform(state.client);
    state.status_code = esp_http_client_get_status_code(state.client);
    esp_http_client_set_post_field(state.client, NULL, 0); /* Clear post field */
    free(buf);
    if (state.err == ESP_OK) {
        retries = 0;
        if (PLAYBACK_TRANSFERED(state)) {
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
    char* buf = malloc(19 + strlen(uri));

    sprintf(buf, "{\"context_uri\":\"%s\"}", uri);

    ACQUIRE_LOCK(client_lock);
    validate_token();
    state.handler_cb = default_event_handler;
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

static esp_err_t validate_token()
{
    /* client_lock lock already must be aquired */

    if ((tokens->expiresIn - 10) > time(0))
        return ESP_OK;
    free(tokens->access_token);
    ESP_LOGD(TAG, "Access Token expired or expiring soon. Fetching a new one.");
    state.handler_cb = default_event_handler;
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
    parseTokens(buffer, tokens);
    strcpy(state.access_token, "Bearer ");
    strcat(state.access_token, tokens->access_token);
    ESP_LOGW(TAG, "Access Token obtained:\n%s", tokens->access_token);
    return ESP_OK;
}

static inline void handle_track_fetched(TrackInfo** new_track)
{
    parseTrackInfo(buffer, *new_track);

    if (0 == strcmp(TRACK->name, (*new_track)->name)) {
        /* same track */
        TRACK->progress_ms = (*new_track)->progress_ms;
        TRACK->isPlaying = (*new_track)->isPlaying;
        free_track(*new_track);
        NOTIFY_DISPLAY(SAME_TRACK);
    } else {
        free_track(TRACK);
        SWAP_PTRS(*new_track, TRACK);
        ESP_LOGI(TAG, "New track");
        ESP_LOGI(TAG, "Title: %s", TRACK->name);
        StrListItem* artist = TRACK->artists->first;
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
        HTTP_METHOD_LOOKUP[state.method],
        esp_err_to_name(state.err));
    assert((++retries <= RETRIES_ERR_CONN) && "Restarting...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(TAG, "Retrying %d/%d...", retries, RETRIES_ERR_CONN);
}

static esp_err_t _http_event_handler(esp_http_client_event_t* evt)
{
    state.handler_cb(buffer, evt);
    return ESP_OK;
}

static void now_playing_task(void* pvParameters)
{
    bool first_try = true;

    char* buf = NULL;

    TrackInfo* new_track;
    CALLOC(new_track, sizeof(*new_track));
    CALLOC(new_track->name, 1);
    CALLOC(new_track->device, sizeof(*new_track->device));
    CALLOC(new_track->artists, sizeof(*new_track->artists));

    uint32_t notif;
    bool     task_enabled = false;

    while (1) {
        xTaskNotifyWait(
            pdFALSE, /* bits to clear on entry (if there are no pending notifications) */
            ULONG_MAX, /* bits to clear on exit (if there are pending notifications) */
            &notif, /* Stores the notified value */
            pdMS_TO_TICKS(MS_NOTIF_POLLING) /* xTicksToWait */
        );

        if (notif == ENABLE_TASK)
            task_enabled = true;
        else if (notif == DISABLE_TASK)
            task_enabled = false;

        if (!task_enabled)
            continue;

        ACQUIRE_LOCK(client_lock);
        validate_token();
        state.handler_cb = default_event_handler;
        state.method = HTTP_METHOD_GET;
        state.endpoint = PLAYERURL(PLAYING);

    prepare:
        PREPARE_CLIENT(state, state.access_token, "application/json");

    retry:
        state.err = esp_http_client_perform(state.client);
        state.status_code = esp_http_client_get_status_code(state.client);
        esp_http_client_set_post_field(state.client, NULL, 0); /* Clear post field */
        if (state.err == ESP_OK) {
            retries = 0;
            ESP_LOGD(TAG, "Received:\n%s", buffer);
            if (state.status_code == 200) {
                handle_track_fetched(&new_track);
                goto exit;
            }
            if (state.status_code == 401) { /* bad token or expired */
                ESP_LOGW(TAG, "Token expired, getting a new one");
                goto prepare;
            }
            if (DEVICE_INACTIVE(state)) { /* Playback not available or active */
                ESP_LOGW(TAG, "Device inactive");
                if (first_try && TRACK->device->id) {
                    first_try = false;
                    MALLOC(buf, 33 + strlen(TRACK->device->id));
                    sprintf(buf, "{\"device_ids\":[\"%s\"],\"play\":false}", TRACK->device->id);
                    ESP_LOGW(TAG, "Device to transfer playback: %s", TRACK->device->id);
                } else {
                    ESP_LOGW(TAG, "Failed connecting with last device");
                    first_try = true;
                    NOTIFY_DISPLAY(LAST_DEVICE_FAILED);
                    goto exit;
                }

                validate_token();
                state.handler_cb = default_event_handler;
                state.method = HTTP_METHOD_PUT;
                state.endpoint = PLAYERURL(PLAYER);
                esp_http_client_set_post_field(state.client, buf, strlen(buf));
                goto prepare;
            }
            if (PLAYBACK_TRANSFERED(state)) {
                ESP_LOGI(TAG, "Playback transfered to: %s", TRACK->device->id);
                first_try = true;
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
        ESP_LOGD(TAG, "[NOW_PLAYING]: stack high water mark: %d", uxTaskGetStackHighWaterMark(NULL));
        ESP_LOGD(TAG, "[NOW_PLAYING]: minimum free heap size: %d", esp_get_minimum_free_heap_size());
        ESP_LOGD(TAG, "[NOW_PLAYING]: free heap size: %d", esp_get_free_heap_size());
    }
    assert(false && "Unexpected exit of infinite task loop");
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