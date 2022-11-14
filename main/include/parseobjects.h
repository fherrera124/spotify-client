#pragma once

#include <stdbool.h>
#include <time.h>

#include "strlib.h"

typedef enum {
    nameParsed = 0x01,
    artistParsed = 0x02,
    albumParsed = 0x04,
    durationParsed = 0x08,
    progressParsed = 0x10,
    isPlayingParsed = 0x20,
    deviceParsed = 0x40,
    trackAllParsed = nameParsed | artistParsed | albumParsed
        | isPlayingParsed | deviceParsed
        | durationParsed | progressParsed

} TrackParsed;

typedef enum {
    accessTokenParsed = 0x01,
    // refreshTokenParsed = 0x02,
    expiresInParsed = 0x04,
    tokenAllParsed = accessTokenParsed | expiresInParsed // | refreshTokenParsed

} TokensParsed;

typedef struct
{
    char* id;
    bool  is_active;
    char* name;
    char* type;
    char* volume_percent;
} Device;

typedef struct
{
    char*       name;
    StrList*    artists;
    char*       album;
    time_t      duration_ms;
    time_t      progress_ms;
    bool        isPlaying : 1;
    Device*     device;
    TrackParsed parsed;
} TrackInfo;

typedef struct
{
    // char     *refreshToken;
    // char     *authToken;
    char*        access_token;
    time_t       expiresIn;
    TokensParsed parsed;
} Tokens;

void init_functions_cb(void);

TrackParsed  parseTrackInfo(const char* js, TrackInfo* track);
TokensParsed parseTokens(const char* js, Tokens* tokens);
void         available_devices(const char* js, StrList* device_list);
