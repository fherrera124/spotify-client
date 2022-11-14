/**
 * @file display.h
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
#include "freertos/task.h"

#include "handler_callbacks.h"

/* Exported types ------------------------------------------------------------*/
typedef enum {
    NEW_TRACK = 1,
    SAME_TRACK = 2,
    PAUSED = 4,
} Menu_event_t;

/* Globally scoped variables declarations ------------------------------------*/
extern TaskHandle_t MENU_TASK;
extern Playlists_t* PLAYLISTS; // maybe should be defined in handler_calbacks.c, instead of display.c

/* Exported macro ------------------------------------------------------------*/
#define NOTIFY_DISPLAY(event) xTaskNotify(MENU_TASK, event, eSetBits)

/* Exported functions prototypes ---------------------------------------------*/
esp_err_t display_init(UBaseType_t priority, QueueHandle_t encoder_q_hlr);

#ifdef __cplusplus
}
#endif