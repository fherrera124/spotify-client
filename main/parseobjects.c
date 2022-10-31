#include "parseobjects.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "jsmn.h"

typedef void (*PathCb)(const char *, jsmntok_t *, void *);

#define TRACK_CALLBACKS_SIZE 5
PathCb trackCallbacks[TRACK_CALLBACKS_SIZE];

#define TOKENS_CALLBACKS_SIZE 2
PathCb tokensCallbacks[TOKENS_CALLBACKS_SIZE];

#define MAX_TOKENS 600

static const char *TAG = "PARSE_OBJECT";

void onDevicePlaying(const char *js, jsmntok_t *root, void *object) {
    TrackInfo *track = (TrackInfo *)object;

    jsmntok_t *device = object_get_member(js, root, "device");
    if (!device) return;

    jsmntok_t *value = object_get_member(js, device, "id");
    if (!value) return;

    track->device->id = jsmn_obj_dup(js, value);
    if (track->device->id == NULL) return;

    value = object_get_member(js, device, "name");
    if (!value) return;

    track->device->name = jsmn_obj_dup(js, value);
    if (track->device->name == NULL) return;

    track->parsed |= deviceParsed;
    ESP_LOGD(TAG, "Device id: %s, name: %s", track->device->id, track->device->name);
}

void onTrackName(const char *js, jsmntok_t *root, void *object) {
    TrackInfo *track = (TrackInfo *)object;

    jsmntok_t *value = object_get_member(js, root, "item");
    if (!value) return;

    value = object_get_member(js, value, "name");
    if (!value) return;

    track->name = jsmn_obj_dup(js, value);
    if (track->name == NULL) return;

    track->parsed |= nameParsed;
    ESP_LOGD(TAG, "Track: %s", track->name);
}

void onArtistsName(const char *js, jsmntok_t *root, void *object) {
    TrackInfo *track = (TrackInfo *)object;

    jsmntok_t *value = object_get_member(js, root, "item");
    if (!value) return;

    value = object_get_member(js, value, "artists");
    if (!value) return;

    jsmntok_t *artists = value;
    for (uint16_t i = 0; i < (artists->size); i++) {
        value = array_get_at(artists, i);
        if (!value) return;

        value = object_get_member(js, value, "name");
        if (!value) return;

        char *artist = jsmn_obj_dup(js, value);
        if (artist == NULL) return;

        if (ESP_OK != strListAppend(track->artists, artist)) {
            free(artist);
            strListClear(track->artists);
            return;
        }
    }
    track->parsed |= artistParsed;
}

void onAlbumName(const char *js, jsmntok_t *root, void *object) {
    TrackInfo *track = (TrackInfo *)object;

    jsmntok_t *value = object_get_member(js, root, "item");
    if (!value) return;

    value = object_get_member(js, value, "album");
    if (!value) return;

    value = object_get_member(js, value, "name");
    if (!value) return;

    track->album = jsmn_obj_dup(js, value);
    if (track->album == NULL) return;

    track->parsed |= albumParsed;
    ESP_LOGD(TAG, "Album: %s", track->album);
}

void onTrackIsPlaying(const char *js, jsmntok_t *root, void *object) {
    TrackInfo *track = (TrackInfo *)object;

    jsmntok_t *value = object_get_member(js, root, "is_playing");
    if (!value) return;

    char type        = (js + (value->start))[0];
    track->isPlaying = type == 't' ? true : false;
    track->parsed |= isPlayingParsed;
}

void onAccessToken(const char *js, jsmntok_t *root, void *object) {
    Tokens *token = (Tokens *)object;

    jsmntok_t *value = object_get_member(js, root, "access_token");
    if (!value) return;
    token->access_token = jsmn_obj_dup(js, value);
    if (token->access_token == NULL) return;
    token->parsed |= accessTokenParsed;
}

static int str2int(const char *str, short len) {
    int ret = 0;
    for (short i = 0; i < len; ++i) {
        ret = ret * 10 + (str[i] - '0');
    }
    return ret;
}

void onExpiresIn(const char *js, jsmntok_t *root, void *object) {
    Tokens *token = (Tokens *)object;

    jsmntok_t *value = object_get_member(js, root, "expires_in");
    if (!value) return;

    int seconds      = str2int(js + value->start, value->end - value->start);
    token->expiresIn = time(0) + seconds;
    token->parsed |= expiresInParsed;
}

void parsejson(const char *js, PathCb *callbacks, size_t callbacksSize, void *object) {
    jsmntok_t *tokens = malloc(sizeof(*tokens) * MAX_TOKENS);

    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    jsmnerr_t n = jsmn_parse(&jsmn, js, strlen(js), tokens, MAX_TOKENS);

    if (n < 0) {
        ESP_LOGE(TAG, "Parse error: %s\n", error_str(n));
    } else {
        jsmntok_t *root = &tokens[0];
        for (size_t i = 0; i < callbacksSize; i++) {
            PathCb fn = callbacks[i];
            fn(js, root, object);
        }
    }
    free(tokens);
}

void init_functions_cb() {
    trackCallbacks[0] = onTrackName;
    trackCallbacks[1] = onArtistsName;
    trackCallbacks[2] = onAlbumName;
    trackCallbacks[3] = onTrackIsPlaying;
    trackCallbacks[4] = onDevicePlaying;

    tokensCallbacks[0] = onAccessToken;
    tokensCallbacks[1] = onExpiresIn;
}

TrackParsed parseTrackInfo(const char *js, TrackInfo *track) {
    parsejson(js, trackCallbacks, TRACK_CALLBACKS_SIZE, track);
    return track->parsed;
}

TokensParsed parseTokens(const char *js, Tokens *tokens) {
    parsejson(js, tokensCallbacks, TOKENS_CALLBACKS_SIZE, tokens);
    return tokens->parsed;
}

void available_devices(const char *js, StrList *dev_list) {
    jsmntok_t *tokens = malloc(sizeof(jsmntok_t) * 40); /* We expect no more than 40 JSON tokens */

    jsmntok_t *root = &tokens[0];

    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    jsmnerr_t n = jsmn_parse(&jsmn, js, strlen(js), tokens, MAX_TOKENS);
    if (n < 0) {
        ESP_LOGE(TAG, "Parse error: %s\n", error_str(n));
        goto exit;
    }

    jsmntok_t *value = object_get_member(js, root, "devices");
    if (!value) goto exit;

    jsmntok_t *devices = value;

    for (uint16_t i = 0; i < (devices->size); i++) {
        value = array_get_at(devices, i);
        if (!value) goto exit;

        value = object_get_member(js, value, "id");
        if (!value) goto exit;

        char *id = jsmn_obj_dup(js, value);
        if (!id) goto exit;

        if (ESP_OK != strListAppend(dev_list, id)) {
            free(id);
            strListClear(dev_list);
            goto exit;
        }
    }
exit:
    free(tokens);
}