/* Includes ------------------------------------------------------------------*/
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/task.h"

#include "display.h"
#include "handler_callbacks.h"

/* Private macro -------------------------------------------------------------*/
#define MAX_HTTP_BUFFER 8192

#define MAX_TOKENS 100 /* We expect no more than 100 JSON tokens */

#define MATCH_KEY(data, str, left) \
    strncpy(buffer, data, left);   \
    buffer[left] = '\0';           \
    char* tmp = data;              \
    data = strstr(data, str);      \
    if (data == NULL)              \
        goto fail;                 \
    data += strlen(str);           \
    left -= (data - tmp);

#define MATCH_CHAR(data, ch, left)                                 \
    if (END_REACHED == skip_blanks(&data, &left) || *data != ch) { \
        goto fail;                                                 \
    }                                                              \
    data++, left--;

/* Private types -------------------------------------------------------------*/
typedef union {
    struct {
        uint32_t first_chunk : 1; /*!< First chunk of data */
        uint32_t abort       : 1; /*!< Found an error, skip all chunks */
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
static int         output_len; // Stores number of bytes read
static const char* HANDLER_CALLBACKS = "HANDLER_CALLBACKS";
static int         curly_braces;

/* External variables --------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/
match_result_t static skip_blanks(char** ptr, int* left);
esp_err_t static parse_playlist(char* buffer, int output_len);
esp_err_t static str_append(jsmntok_t* obj, const char* buff);
esp_err_t static uri_append(jsmntok_t* obj, const char* buf);

/* Exported functions --------------------------------------------------------*/
void default_event_handler(char* buffer, esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if ((output_len + evt->data_len) > MAX_HTTP_BUFFER) {
            ESP_LOGE(HANDLER_CALLBACKS, "Not enough space on buffer. Ignoring incoming data.");
            return;
        }
        memcpy(buffer + output_len, evt->data, evt->data_len);
        output_len += evt->data_len;
        break;
    case HTTP_EVENT_ON_FINISH:
        buffer[output_len] = 0;
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:;
        int       mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            buffer[output_len] = 0;
            output_len = 0;
            ESP_LOGI(HANDLER_CALLBACKS, "Last esp error code: 0x%x", err);
            ESP_LOGI(HANDLER_CALLBACKS, "Last mbedtls failure: 0x%x", mbedtls_err);
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
void playlists_handler(char* buffer, esp_http_client_event_t* evt)
{
    static json_state_t state = { { true, false, true, false } };

    char* data = (char*)evt->data;
    int   left = evt->data_len;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (state.abort || state.finished)
            return;

        if (state.first_chunk) {
            state.first_chunk = false;

            MATCH_KEY(data, "\"items\"", left);
            MATCH_CHAR(data, ':', left);
            MATCH_CHAR(data, '[', left);
        }
    get_new_obj:
        if (state.get_new_obj) {
            if (END_REACHED == skip_blanks(&data, &left)) {
                return;
            }
            if (*data != '{') {
                ESP_LOGE(HANDLER_CALLBACKS, "'{' expected. Got: '%c' instead", *data);
                goto fail;
            }
            output_len = curly_braces = 0;
            state.get_new_obj = false;
        }

        do {
            buffer[output_len++] = *data;
            if (*data == '{') {
                curly_braces++;
            } else if (*data == '}') {
                curly_braces--;
            }
            data++, left--;
        } while (left > 0 && curly_braces > 0);

        if (curly_braces == 0) {
            if (ESP_OK != parse_playlist(buffer, output_len)) {
                goto fail;
            }
            if (CHAR_DETECTED == skip_blanks(&data, &left)) {
                if (*data == ',') {
                    data++, left--;
                    state.get_new_obj = true;
                    goto get_new_obj;
                } else if (*data == ']') {
                    state.finished = true;
                } else {
                    ESP_LOGE(HANDLER_CALLBACKS, "Unexpected character '%c'. Abort\n", *data);
                    goto fail;
                }
            }
        }
        break;
    fail:
        state.abort = true;
        break;
    case HTTP_EVENT_ON_FINISH: // is it always called? even when an error or disconnect event occurs?
        if (state.abort || !state.finished) {
            free(PLAYLISTS->name_list);
            strListClear(PLAYLISTS->uris);
            free(PLAYLISTS->uris);
            PLAYLISTS->name_list = NULL;
            PLAYLISTS->uris = NULL;
        }
        output_len = 0;
        state.val = 5; // reset state (true, false, true, false)
        xTaskNotifyGive(MENU_TASK);
        break;
    case HTTP_EVENT_DISCONNECTED:
        if (ESP_OK != esp_tls_get_and_clear_last_error(evt->data, NULL, NULL)) {
            state.abort = true;
            ESP_LOGE(HANDLER_CALLBACKS, "HTTP_EVENT_DISCONNECTED, abort");
        }
        break;
    case HTTP_EVENT_ERROR:
        state.abort = true;
        ESP_LOGE(HANDLER_CALLBACKS, "HTTP_EVENT_ERROR, abort");
        break;
    default:
        break;
    }
}

/* Private functions ---------------------------------------------------------*/
match_result_t static skip_blanks(char** ptr, int* left)
{
    while (*left >= 0) {
        if (!isspace(**ptr))
            return CHAR_DETECTED;
        output_len++;
        (*left)--, (*ptr)++;
    }
    return END_REACHED;
}

esp_err_t static parse_playlist(char* buffer, int output_len)
{
    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * MAX_TOKENS);
    if (!tokens) {
        ESP_LOGE(HANDLER_CALLBACKS, "tokens not allocated");
        return ESP_ERR_NO_MEM;
    }

    jsmnerr_t n = jsmn_parse(&jsmn, buffer, output_len, tokens, MAX_TOKENS);
    output_len = 0;
    if (n < 0) {
        ESP_LOGE(HANDLER_CALLBACKS, "Parse error: %s\n", error_str(n));
        goto fail;
    }

    jsmntok_t* name = object_get_member(buffer, tokens, "name");
    if (!name) {
        ESP_LOGE(HANDLER_CALLBACKS, "key \"name\" missing");
        goto fail;
    }

    jsmntok_t* uri = object_get_member(buffer, tokens, "uri");
    if (!uri) {
        ESP_LOGE(HANDLER_CALLBACKS, "key \"uri\" missing");
        goto fail;
    }

    free(tokens);

    esp_err_t err = str_append(name, buffer);
    if (ESP_OK != err)
        return err;

    err = uri_append(uri, buffer);
    if (ESP_OK != err)
        return err;

    return ESP_OK;
fail:
    free(tokens);
    return ESP_FAIL;
}

/**
 * @brief u8g2 selection list menu uses a string with '\\n' as
 * item separator. For example: 'item1\\nitem2\\ngo to Menu\\nEtc...'.
 * This function build that string with each playlist name.
 *
 */
esp_err_t static str_append(jsmntok_t* obj, const char* buf)
{
    char** str = &PLAYLISTS->name_list;

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

    return ESP_OK;
}

esp_err_t static uri_append(jsmntok_t* obj, const char* buf)
{
    char* uri = jsmn_obj_dup(buf, obj);
    if (uri == NULL)
        return ESP_ERR_NO_MEM;

    return strListAppend(PLAYLISTS->uris, uri);
}