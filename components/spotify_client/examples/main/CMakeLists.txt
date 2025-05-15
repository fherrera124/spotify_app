idf_component_register(SRCS "spotify_client_example.c"
    REQUIRES spotify_client protocol_examples_common esp_netif
    PRIV_REQUIRES nvs_flash esp_wifi)