# Embed the server root certificate into the final binary
#
# (If this was a component, we would set COMPONENT_EMBED_TXTFILES here.)
set(PROJECT_NAME "spotify_client")
idf_component_register(SRCS "spiffs_wifi.c" "handler_callbacks.c" "main.c" "parseobjects.c" "strlib.c" "spotifyclient.c" "wifi.c" "display.c" "selection_list.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES spotify_cert.pem)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")