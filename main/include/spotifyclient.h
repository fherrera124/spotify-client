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
    TASK_DISABLED
} nowPlayingAction;

typedef enum {
    cmdToggle,
    cmdPrev,
    cmdNext,
} Player_cmd_t;

/* Exported variables declarations -------------------------------------------*/
extern TaskHandle_t  PLAYING_TASK;
extern TrackInfo*    TRACK;

/* Exported macro ------------------------------------------------------------*/
#define ENABLE_PLAYING_TASK  xTaskNotify(PLAYING_TASK, TASK_ENABLED, eSetValueWithOverwrite)
#define DISABLE_PLAYING_TASK xTaskNotify(PLAYING_TASK, TASK_DISABLED, eSetValueWithOverwrite)
/* unblock task without updating its notify value */
#define UNBLOCK_PLAYING_TASK xTaskNotify(PLAYING_TASK, pdFALSE, eNoAction)

/* Exported functions prototypes ---------------------------------------------*/
esp_err_t spotify_client_init(UBaseType_t priority);
void      player_cmd(rotary_encoder_event_t* event);
void      http_user_playlists();
void      http_play_context_uri(const char* uri);

#ifdef __cplusplus
}
#endif