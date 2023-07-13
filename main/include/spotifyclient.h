/**
 * @file spotifyclient.h
 * @author Francisco Herrera (fherrera@lifia.info.unlp.edu.ar)
 * @brief
 * @version 0.1
 * @date 2022-11-06
 *
 * @copyright Copyright (c) 2022
 *
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "parseobjects.h"
#include "rotary_encoder.h"

/* Exported macro ------------------------------------------------------------*/
#define MAX_HTTP_BUFFER 8192

/* Exported types ------------------------------------------------------------*/
typedef enum {
    ENABLE_TASK = 1,
    DISABLE_TASK,
} nowPlayingAction;

typedef enum {
    cmdToggle,
    cmdPrev,
    cmdNext,
} Player_cmd_t;

typedef enum {
    NEW_TRACK = 1,
    SAME_TRACK,
    PLAYBACK_TRANSFERRED_OK,
    PLAYBACK_TRANSFERRED_FAIL,
    ACTIVE_DEVICES_FOUND,
    NO_ACTIVE_DEVICES,
    LAST_DEVICE_FAILED,
    PLAYLISTS_EMPTY,
    PLAYLISTS_OK,
    VOLUME_CHANGED
} spotify_client_event_t;

/* Exported variables declarations -------------------------------------------*/
extern TaskHandle_t PLAYER_TASK;
extern TrackInfo*   TRACK;

/* Exported macro ------------------------------------------------------------*/
#define ENABLE_PLAYER_TASK  xTaskNotify(PLAYER_TASK, ENABLE_TASK, eSetValueWithOverwrite)
#define DISABLE_PLAYER_TASK xTaskNotify(PLAYER_TASK, DISABLE_TASK, eSetValueWithOverwrite)
/* unblock task without updating its notify value */
#define UNBLOCK_PLAYER_TASK xTaskNotify(PLAYER_TASK, pdFALSE, eNoAction)
/* ms to wait to fetch current track */
#define MS_NOTIF_POLLING 10000

/* Exported functions prototypes ---------------------------------------------*/
void spotify_client_init(UBaseType_t priority);
void player_cmd(rotary_encoder_event_t* event);
void http_user_playlists();
void http_available_devices();
void http_play_context_uri(const char* uri);
void http_update_volume(int8_t volume_percent);
void http_set_device(const char* dev_id);

#ifdef __cplusplus
}
#endif