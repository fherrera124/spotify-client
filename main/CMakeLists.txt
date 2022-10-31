# Embed the server root certificate into the final binary
#
# (If this was a component, we would set COMPONENT_EMBED_TXTFILES here.)
idf_component_register(SRCS "main.c" "parseobjects.c" "strlib.c" "spotifyclient.c" "wifi.c" "display.c" "selection_list.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES spotify_cert.pem)

register_component()