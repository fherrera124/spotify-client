/* Includes ------------------------------------------------------------------*/
#include <stdio.h>
#include <string.h>

#include "buffer_callbacks.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/task.h"

/* Private macro -------------------------------------------------------------*/
#define MAX_HTTP_BUFFER 8192

#define MAX_TOKENS 100 /* We expect no more than 100 JSON tokens */

#define MATCH_NEXT_CHAR(data, ch, left)                  \
    if (END == next_char(&data, &left) || *data != ch) { \
        goto fail;                                       \
    }                                                    \
    data++, left--;

#define MATCH_KEY(data, str, left) \
    strncpy(buffer, data, left);   \
    buffer[left] = '\0';           \
    char* tmp = data;              \
    data = strstr(data, str);      \
    if (data == NULL)              \
        goto fail;                 \
    data += strlen(str);           \
    left -= (data - tmp);

/* Private types -------------------------------------------------------------*/
typedef union {
    struct {
        uint32_t first_chunk : 1; /*!< First chunk of data */
        uint32_t abort       : 1; /*!< Found an error, skip all chunks */
        uint32_t get_new_obj : 1; /*!< Get New object */
        uint32_t finished    : 1; /*!< We're done */
    };
    uint32_t val; /*!< union fill */
} StateRequest_t;

typedef enum {
    END,
    MATCH,
    UNMATCH,
    BLANK,
    CHAR,
} match_result_t;

/* Private function prototypes -----------------------------------------------*/
match_result_t static next_char(char** ptr, int* left);
esp_err_t process_JSON_obj(char* buffer, int output_len);
esp_err_t str_append(jsmntok_t* obj, const char* buff);
esp_err_t uri_append(jsmntok_t* obj, const char* buf);

/* Private variables ---------------------------------------------------------*/
static int         output_len; // Stores number of bytes read
static const char* TAG = "HTTP-BUFFER";
static int         curly_count;

/* External variables --------------------------------------------------------*/
extern TaskHandle_t MENU_TASK_HLR;
extern Playlists_t* PLAYLISTS;

/* Exported functions --------------------------------------------------------*/
void default_fun(char* buffer, esp_http_client_event_t* evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if ((output_len + evt->data_len) > MAX_HTTP_BUFFER) {
            ESP_LOGE(TAG, "Not enough space on buffer. Ignoring incoming data.");
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
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief We can't afford to fetch the whole JSON (would be easy that way).
 * Instead we process each playlist JSON object of the array "items"
 *
 * @param buffer
 * @param evt
 */
void get_playlists(char* buffer, esp_http_client_event_t* evt)
{
    static StateRequest_t state = { { true, false, true, false } };

    char* data = (char*)evt->data;
    int   left = evt->data_len;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (state.abort || state.finished)
            return;

        if (state.first_chunk) {
            state.first_chunk = false;

            MATCH_KEY(data, "\"items\"", left);
            MATCH_NEXT_CHAR(data, ':', left);
            MATCH_NEXT_CHAR(data, '[', left);
        }
    get_new_obj:
        if (state.get_new_obj) {
            if (END == next_char(&data, &left)) {
                return;
            }
            if (*data == '{') {
                output_len = 0;
                state.get_new_obj = false;
                curly_count = 1;
                buffer[output_len++] = *data;
                data++, left--;
            } else {
                ESP_LOGE(TAG, "'{' not found. Instead: '%c'\n", *data);
                goto fail;
            }
        }

        do {
            buffer[output_len] = *data;
            output_len++;
            if (*data == '{') {
                curly_count++;
            } else if (*data == '}') {
                curly_count--;
            }
            data++, left--;
        } while (left > 0 && curly_count != 0);

        if (curly_count == 0) {
            if (ESP_OK != process_JSON_obj(buffer, output_len)) {
                goto fail;
            }
            if (left > 0 && END != next_char(&data, &left)) {
                if (*data == ',') {
                    data++, left--;
                    state.get_new_obj = true;
                    goto get_new_obj;
                } else if (*data == ']') {
                    state.finished = true;
                } else {
                    ESP_LOGE(TAG, "Unexpected character '%c'. Abort\n", *data);
                    goto fail;
                }
            }
        }
        break;
    fail:
        state.abort = true;
        break;
    case HTTP_EVENT_ON_FINISH: // doubt, always called? (even when error or disconnect event ocurr??)
        if (state.abort == true) {
            free(PLAYLISTS->name_list);
            PLAYLISTS->name_list = NULL;
        }
        output_len = 0;
        state.val = 5; // 0101
        ESP_LOGI(TAG, "Session finished");
        xTaskNotifyGive(MENU_TASK_HLR);
        break;
    case HTTP_EVENT_DISCONNECTED:
        if (ESP_OK != esp_tls_get_and_clear_last_error(evt->data, NULL, NULL)) {
            state.abort = true;
            ESP_LOGE(TAG, "Disconnected, abort");
        }
        break;
    case HTTP_EVENT_ERROR:
        state.abort = true;
        ESP_LOGE(TAG, "Error event, abort");
        break;
    default:
        break;
    }
}

/* Private functions ---------------------------------------------------------*/
match_result_t static next_char(char** ptr, int* left)
{
    while (*left > 0 && isspace(**ptr)) {
        output_len++;
        (*left)--;
        (*ptr)++;
    }
    if (!isspace(**ptr))
        return CHAR;

    ESP_LOGD(TAG, "END of chunk");
    return END;
}

esp_err_t process_JSON_obj(char* buffer, int output_len)
{
    jsmn_parser jsmn;
    jsmn_init(&jsmn);

    jsmntok_t* tokens = malloc(sizeof(jsmntok_t) * MAX_TOKENS);
    if (!tokens) {
        ESP_LOGE(TAG, "tokens not allocated");
        return ESP_ERR_NO_MEM;
    }

    jsmnerr_t n = jsmn_parse(&jsmn, buffer, output_len, tokens, MAX_TOKENS);
    output_len = 0;
    if (n < 0) {
        ESP_LOGE(TAG, "Parse error: %s\n", error_str(n));
        goto fail;
    }

    jsmntok_t* name = object_get_member(buffer, tokens, "name");
    if (!name) {
        ESP_LOGE(TAG, "key \"name\" missing");
        goto fail;
    }

    jsmntok_t* uri = object_get_member(buffer, tokens, "uri");
    if (!uri) {
        ESP_LOGE(TAG, "key \"uri\" missing");
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
esp_err_t str_append(jsmntok_t* obj, const char* buf)
{
    char** str = &PLAYLISTS->name_list; // more readable

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

esp_err_t uri_append(jsmntok_t* obj, const char* buf)
{
    char* uri = jsmn_obj_dup(buf, obj);
    if (uri == NULL)
        return ESP_ERR_NO_MEM;

    return strListAppend(PLAYLISTS->uris, uri);
}