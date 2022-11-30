/* Includes ------------------------------------------------------------------*/
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "jsmn.h"
#include "parseobjects.h"

/* Private macro -------------------------------------------------------------*/
#define TRACK_CALLBACKS_SIZE  6
#define TOKENS_CALLBACKS_SIZE 2
#define MAX_TOKENS            500
#define PLAYLISTS_TOKENS      200
#define DEVICES_TOKENS        200

/* Private types -------------------------------------------------------------*/
typedef void (*PathCb)(const char*, jsmntok_t*, void*);

/* Private function prototypes -----------------------------------------------*/
static void       onDevicePlaying(const char* js, jsmntok_t* root, void* obj);
static void       onTrackName(const char* js, jsmntok_t* root, void* obj);
static void       onArtistsName(const char* js, jsmntok_t* root, void* obj);
static void       onAlbumName(const char* js, jsmntok_t* root, void* obj);
static void       onTrackIsPlaying(const char* js, jsmntok_t* root, void* obj);
static void       onTrackTime(const char* js, jsmntok_t* root, void* obj);
static void       onAccessToken(const char* js, jsmntok_t* root, void* obj);
static void       onExpiresIn(const char* js, jsmntok_t* root, void* obj);
static inline int natoi(const char* str, short len);
static void       parsejson(const char* js, PathCb* callbacks, size_t callbacksSize, void* obj);
esp_err_t static str_append(jsmntok_t* obj, const char* buff, char** str);
static inline esp_err_t uri_append(jsmntok_t* obj, const char* buf);

/* Locally scoped variables --------------------------------------------------*/
static const char* TAG = "PARSE_OBJECT";
PathCb             trackCallbacks[TRACK_CALLBACKS_SIZE];
PathCb             tokensCallbacks[TOKENS_CALLBACKS_SIZE];
static jsmntok_t   tokens[MAX_TOKENS];

/* Globally scoped variables definitions -------------------------------------*/
u8g2_items_list_t PLAYLISTS = { 0 };
u8g2_items_list_t DEVICES = { 0 };

/* Exported functions --------------------------------------------------------*/
void init_functions_cb()
{
    trackCallbacks[0] = onTrackName;
    trackCallbacks[1] = onArtistsName;
    trackCallbacks[2] = onAlbumName;
    trackCallbacks[3] = onTrackIsPlaying;
    trackCallbacks[4] = onTrackTime;
    trackCallbacks[5] = onDevicePlaying;

    tokensCallbacks[0] = onAccessToken;
    tokensCallbacks[1] = onExpiresIn;
}

void parseTrackInfo(const char* js, TrackInfo* track)
{
    parsejson(js, trackCallbacks, TRACK_CALLBACKS_SIZE, track);
}

void parseTokens(const char* js, Tokens* tokens)
{
    parsejson(js, tokensCallbacks, TOKENS_CALLBACKS_SIZE, tokens);
}

esp_err_t parse_available_devices(const char* js)
{
    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    esp_err_t err = ESP_FAIL;

    jsmnerr_t n = jsmn_parse(&jsmn, js, strlen(js), tokens, DEVICES_TOKENS);
    if (n < 0) {
        ESP_LOGE(TAG, "%s", error_str(n));
        abort();
    }

    jsmntok_t* devices = object_get_member(js, tokens, "devices");
    assert(devices && "key \"devices\" missing");

    if (devices->size == 0) {
        ESP_LOGE(TAG, "No active devices found");
        return ESP_FAIL;
    }

    for (uint16_t i = 0; i < devices->size; i++) {
        jsmntok_t* device = array_get_at(devices, i);
        assert(device && "array_get_at() failed. Maybe not an array");

        jsmntok_t* value = object_get_member(js, device, "name");
        assert(value && "key \"name\" missing");

        err = str_append(value, js, &DEVICES.items_string);
        assert((ESP_OK == err) && "str_append() failed. Error allocating memory");

        value = object_get_member(js, device, "id");
        assert(value && "key \"id\" missing");

        char* id = jsmn_obj_dup(js, value);
        assert(id && "jsmn_obj_dup() failed. Error allocating memory");

        err = strListAppend(&DEVICES.values, id);
        assert((ESP_OK == err) && "strListAppend() failed. Error allocating memory");
    }
    return ESP_OK;
}

void parse_playlist(const char* js, int output_len)
{
    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    jsmnerr_t n = jsmn_parse(&jsmn, js, output_len, tokens, PLAYLISTS_TOKENS);
    if (n < 0) {
        ESP_LOGE(TAG, "%s", error_str(n));
        abort();
    }

    jsmntok_t* name = object_get_member(js, tokens, "name");
    assert(name && "key \"name\" missing");

    jsmntok_t* uri = object_get_member(js, tokens, "uri");
    assert(uri && "key \"uri\" missing");

    esp_err_t err = str_append(name, js, &PLAYLISTS.items_string);
    assert((ESP_OK == err) && "str_append() failed. Error allocating memory");

    err = uri_append(uri, js);
    assert((ESP_OK == err) && "uri_append() failed. Error allocating memory");
}

/* Private functions ---------------------------------------------------------*/
static void onDevicePlaying(const char* js, jsmntok_t* root, void* obj)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* device = object_get_member(js, root, "device");
    assert(device && "key \"device\" missing");

    jsmntok_t* value = object_get_member(js, device, "id");
    assert(value && "key \"id\" missing");

    track->device.id = jsmn_obj_dup(js, value);
    assert(track->device.id && "Error allocating memory");

    value = object_get_member(js, device, "name");
    assert(value && "key \"name\" missing");

    track->device.name = jsmn_obj_dup(js, value);
    assert(track->device.name && "Error allocating memory");

    ESP_LOGD(TAG, "Device id: %s, name: %s", track->device.id, track->device.name);
}

static void onTrackName(const char* js, jsmntok_t* root, void* obj)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* value = object_get_member(js, root, "item");
    assert(value && "key \"item\" missing");

    value = object_get_member(js, value, "name");
    assert(value && "key \"name\" missing");

    track->name = jsmn_obj_dup(js, value);
    assert(track->name && "Error allocating memory");

    ESP_LOGD(TAG, "Track: %s", track->name);
}

static void onArtistsName(const char* js, jsmntok_t* root, void* obj)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* value = object_get_member(js, root, "item");
    assert(value && "key \"item\" missing");

    value = object_get_member(js, value, "artists");
    assert(value && "key \"artists\" missing");

    jsmntok_t* artists = value;
    for (uint16_t i = 0; i < (artists->size); i++) {
        value = array_get_at(artists, i);
        assert(value && "array_get_at() failed. Maybe not an array");

        value = object_get_member(js, value, "name");
        assert(value && "key \"name\" missing");

        char* artist = jsmn_obj_dup(js, value);
        assert(artist && "Error allocating memory");

        esp_err_t err = strListAppend(&track->artists, artist);
        assert((err == ESP_OK) && "Error allocating memory");
    }
}

static void onAlbumName(const char* js, jsmntok_t* root, void* obj)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* value = object_get_member(js, root, "item");
    assert(value && "key \"item\" missing");

    value = object_get_member(js, value, "album");
    assert(value && "key \"album\" missing");

    value = object_get_member(js, value, "name");
    assert(value && "key \"name\" missing");

    track->album = jsmn_obj_dup(js, value);
    assert(track->album && "Error allocating memory");

    ESP_LOGD(TAG, "Album: %s", track->album);
}

static void onTrackIsPlaying(const char* js, jsmntok_t* root, void* obj)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* value = object_get_member(js, root, "is_playing");
    assert(value && "key \"is_playing\" missing");

    char type = (js + (value->start))[0];
    track->isPlaying = type == 't' ? true : false;
}

static void onTrackTime(const char* js, jsmntok_t* root, void* obj)
{
    TrackInfo* track = (TrackInfo*)obj;

    jsmntok_t* value = object_get_member(js, root, "progress_ms");
    assert(value && "key \"progress_ms\" missing");

    track->progress_ms = natoi(js + value->start, value->end - value->start);

    value = object_get_member(js, root, "item");
    assert(value && "key \"item\" missing");

    value = object_get_member(js, value, "duration_ms");
    assert(value && "key \"duration_ms\" missing");

    track->duration_ms = natoi(js + value->start, value->end - value->start);
}

static void onAccessToken(const char* js, jsmntok_t* root, void* obj)
{
    Tokens* token = (Tokens*)obj;

    jsmntok_t* value = object_get_member(js, root, "access_token");
    assert(value && "key \"access_token\" missing");

    strncat(token->access_token, js + value->start, value->end - value->start);
}

static void onExpiresIn(const char* js, jsmntok_t* root, void* obj)
{
    Tokens* token = (Tokens*)obj;

    jsmntok_t* value = object_get_member(js, root, "expires_in");
    assert(value && "key \"expires_in\" missing");

    int seconds = natoi(js + value->start, value->end - value->start);
    token->expiresIn = time(0) + seconds;
}

static inline int natoi(const char* str, short len)
{
    int ret = 0;
    for (short i = 0; i < len; ++i) {
        ret = ret * 10 + (str[i] - '0');
    }
    return ret;
}

static void parsejson(const char* js, PathCb* callbacks, size_t callbacksSize, void* obj)
{
    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    jsmnerr_t n = jsmn_parse(&jsmn, js, strlen(js), tokens, MAX_TOKENS);
    if (n < 0) {
        ESP_LOGE(TAG, "%s", error_str(n));
        ESP_LOGE(TAG, "%s", js);
        abort();
    }

    for (size_t i = 0; i < callbacksSize; i++) {
        PathCb fn = callbacks[i];
        fn(js, tokens, obj);
    }
}

/**
 * @brief u8g2 selection list menu uses a string with '\\n' as
 * item separator. For example: 'item1\\nitem2\\ngo to Menu\\nEtc...'.
 * This function build that string with each playlist name.
 *
 */
esp_err_t static str_append(jsmntok_t* obj, const char* buf, char** str)
{
    if (*str == NULL) {
        *str = jsmn_obj_dup(buf, obj);
        return (*str == NULL) ? ESP_ERR_NO_MEM : ESP_OK;
    }

    uint16_t obj_len = obj->end - obj->start;
    uint16_t str_len = strlen(*str);

    char* r = realloc(*str, str_len + obj_len + 2);
    if (r == NULL)
        return ESP_ERR_NO_MEM;

    *str = r;

    (*str)[str_len++] = '\n';

    for (uint16_t i = 0; i < obj_len; i++) {
        (*str)[i + str_len] = *(buf + obj->start + i);
    }
    (*str)[str_len + obj_len] = '\0';

    ESP_LOGI(TAG, "str len: %d", strlen(*str));

    return ESP_OK;
}

static inline esp_err_t uri_append(jsmntok_t* obj, const char* buf)
{
    char* uri = jsmn_obj_dup(buf, obj);
    if (uri == NULL)
        return ESP_ERR_NO_MEM;

    return strListAppend(&PLAYLISTS.values, uri);
}
