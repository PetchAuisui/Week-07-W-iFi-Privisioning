idf_component_register(SRCS "main.c"
                        PRIV_REQUIRES esp_wifi nvs_flash network_provisioning driver
                        INCLUDE_DIRS ".")
