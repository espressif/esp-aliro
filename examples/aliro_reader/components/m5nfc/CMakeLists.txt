idf_component_register(SRCS "m5nfc.c" "st25r3916.c" "iso_dep.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private"
                       PRIV_REQUIRES esp_driver_i2c esp_timer freertos)
