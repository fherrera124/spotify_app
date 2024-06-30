idf_component_register(SRCS "spotify_app.c"
    "src/display.c"
    "src/selection_list.c"
    "lib/strlib.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src/include" "lib/include"
    REQUIRES spotify_client protocol_examples_common u8g2_hal_esp_idf
    PRIV_REQUIRES nvs_flash esp_wifi)