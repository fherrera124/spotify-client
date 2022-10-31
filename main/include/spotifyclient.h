#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "rotary_encoder.h"

typedef enum {
    cmdToggle,
    cmdPrev,
    cmdNext,
} Player_cmd_t;

typedef void (*buffer_cb_t)(char*, esp_http_client_event_t*);

typedef struct {
    char*                    access_token; /*!<*/
    const char*              endpoint;     /*!<*/
    int                      status_code;  /*!<*/
    esp_err_t                err;          /*!<*/
    esp_http_client_method_t method;       /*!<*/
    esp_http_client_handle_t client;       /*!<*/
    buffer_cb_t              buffer_cb;    /*!< Callback function for proccess the buffer on HTTP_EVENT_ON_DATA*/
} Client_state_t;

esp_err_t spotify_client_init(UBaseType_t priority, QueueHandle_t* playing_q_hlr);
void      player_cmd(rotary_encoder_event_t* event);
void      http_user_playlists();
void      http_play_context_uri(const char* uri);
