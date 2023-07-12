/* Includes ------------------------------------------------------------------*/
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "handler_callbacks.h"
#include "parseobjects.h"
#include "spotifyclient.h"

/* Private macro -------------------------------------------------------------*/
#define MATCH_KEY(data, str, left)    \
    strncpy(http_buffer, data, left); \
    http_buffer[left] = '\0';         \
    char* tmp = data;                 \
    data = strstr(data, str);         \
    assert(data && "key missing");    \
    data += strlen(str);              \
    left -= (data - tmp);

#define MATCH_CHAR(data, ch, left)                                 \
    if (END_REACHED == skip_blanks(&data, &left) || *data != ch) { \
        assert(false && "Char expected not found");                \
    }                                                              \
    data++, left--;

/* Private types -------------------------------------------------------------*/
typedef union {
    struct {
        uint32_t first_chunk : 1; /*!< First chunk of data */
        uint32_t empty       : 1; /*!< Empty playlist array */
        uint32_t get_new_obj : 1; /*!< Go get a new object */
        uint32_t finished    : 1; /*!< We're done */
    };
    uint32_t val; /*!< union fill */
} json_state_t;

typedef enum {
    END_REACHED,
    CHAR_DETECTED,
} match_result_t;

/* Private variables ---------------------------------------------------------*/
static int         s_output_len; // Stores number of bytes read
static int         s_curly_braces;
static const char* TAG = "HANDLER_CALLBACKS";

/* External variables --------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
match_result_t static skip_blanks(char** ptr, int* left);

/* Exported functions --------------------------------------------------------*/
void default_http_event_handler(char* http_buffer, esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if ((s_output_len + evt->data_len) > MAX_HTTP_BUFFER) {
            ESP_LOGE(TAG, "Not enough space on http_buffer. Ignoring incoming data.");
            return;
        }
        memcpy(http_buffer + s_output_len, evt->data, evt->data_len);
        s_output_len += evt->data_len;
        break;
    case HTTP_EVENT_ON_FINISH:
        http_buffer[s_output_len] = 0;
        s_output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:;
        int       mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            http_buffer[s_output_len] = 0;
            s_output_len = 0;
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief We don't have enough memory to store the whole JSON. So the
 * approach is to process the "items" array one playlist at a time.
 *
 */
void playlists_handler(char* http_buffer, esp_http_client_event_t* evt)
{
    static json_state_t s_state = { { true, false, true, false } };

    char* data = (char*)evt->data;
    int   left = evt->data_len;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_state.empty || s_state.finished)
            return;

        if (s_state.first_chunk) {
            s_state.first_chunk = false;

            MATCH_KEY(data, "\"items\"", left);
            MATCH_CHAR(data, ':', left);
            MATCH_CHAR(data, '[', left);
            // TODO: revise
            if (left && *(data + 1) == ']') {
                s_state.empty = true;
                ESP_LOGW(TAG, "User doesn't have playlists");
                break;
            }
        }
    get_new_obj:
        if (s_state.get_new_obj) {
            if (END_REACHED == skip_blanks(&data, &left)) {
                return;
            }
            if (*data != '{') {
                ESP_LOGE(TAG, "'{' expected. Got: '%c' instead", *data);
                assert(false);
            }
            s_output_len = s_curly_braces = 0;
            s_state.get_new_obj = false;
        }

        do {
            http_buffer[s_output_len++] = *data;
            if (*data == '{') {
                s_curly_braces++;
            } else if (*data == '}') {
                s_curly_braces--;
            }
            data++, left--;
        } while (left > 0 && s_curly_braces > 0);

        if (s_curly_braces == 0) {
            parse_playlist(http_buffer, s_output_len);
            if (CHAR_DETECTED == skip_blanks(&data, &left)) {
                if (*data == ',') {
                    data++, left--;
                    s_state.get_new_obj = true;
                    goto get_new_obj;
                } else if (*data == ']') {
                    s_state.finished = true;
                } else {
                    ESP_LOGE(TAG, "Unexpected character '%c'. Abort", *data);
                    abort();
                }
            }
        }
        break;
    case HTTP_EVENT_ON_FINISH: // is it always called? even when an error or disconnect event occurs?
        assert(s_state.finished && "Error, incomplete json. More character/s expected");
        s_output_len = 0;
        s_state.val = 5; // reset state (true, false, true, false)
        s_state.empty ? NOTIFY_DISPLAY(PLAYLISTS_EMPTY) : NOTIFY_DISPLAY(PLAYLISTS_OK);
        break;
    default:
        break;
    }
}

/* Private functions ---------------------------------------------------------*/
match_result_t static skip_blanks(char** ptr, int* left)
{
    while (*left >= 0) {
        if (!isspace((unsigned char)**ptr)) // isspace expects an integer, the value of which can fit in an unsigned char
            return CHAR_DETECTED;
        s_output_len++;
        (*left)--, (*ptr)++;
    }
    return END_REACHED;
}
