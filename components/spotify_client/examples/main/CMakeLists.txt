idf_component_register(SRCS "spotify_client_example.c"
    INCLUDE_DIRS "."
    REQUIRES spotify_client protocol_examples_common
    PRIV_REQUIRES nvs_flash esp_wifi)