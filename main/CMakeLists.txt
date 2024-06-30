idf_component_register(SRCS "spotify_app.c"
    INCLUDE_DIRS "include"
    REQUIRES spotify_client protocol_examples_common
    PRIV_REQUIRES nvs_flash esp_wifi)