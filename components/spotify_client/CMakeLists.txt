idf_component_register(SRC_DIRS .
    INCLUDE_DIRS "./include"
    PRIV_INCLUDE_DIRS "./priv_include"
    REQUIRES esp_http_client
    PRIV_REQUIRES esp_websocket_client json_parser
    EMBED_TXTFILES certs.pem)
