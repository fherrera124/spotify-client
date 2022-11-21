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
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "parseobjects.h"
#include "rotary_encoder.h"

/* Exported types ------------------------------------------------------------*/
typedef enum {
    TASK_ENABLED = 1,
    TASK_DISABLED,
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
} spotify_client_event_t;

/* Exported variables declarations -------------------------------------------*/
extern TaskHandle_t PLAYING_TASK;
extern TrackInfo*   TRACK;

/* Exported macro ------------------------------------------------------------*/
#define ENABLE_PLAYING_TASK  xTaskNotify(PLAYING_TASK, TASK_ENABLED, eSetValueWithOverwrite)
#define DISABLE_PLAYING_TASK xTaskNotify(PLAYING_TASK, TASK_DISABLED, eSetValueWithOverwrite)
#define GO_CHECK_DEVICE      xTaskNotify(PLAYING_TASK, CHECK_DEVICE, eSetValueWithOverwrite)
/* unblock task without updating its notify value */
#define UNBLOCK_PLAYING_TASK xTaskNotify(PLAYING_TASK, pdFALSE, eNoAction)
/* ms to wait to fetch current track */
#define MS_NOTIF_POLLING 10000

/* Exported functions prototypes ---------------------------------------------*/
void spotify_client_init(UBaseType_t priority);
void player_cmd(rotary_encoder_event_t* event);
void http_user_playlists();
void http_available_devices();
void http_play_context_uri(const char* uri);
void http_set_device(const char* dev_id);

#ifdef __cplusplus
}
#endif