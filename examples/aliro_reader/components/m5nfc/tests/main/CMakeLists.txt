# Preserve Unity's constructor-registered test cases.
idf_component_register(SRC_DIRS "." PRIV_INCLUDE_DIRS "../../private" WHOLE_ARCHIVE)

# Protocol tests supply RF transcripts; reader tests use the real RF driver
# with an I2C chip model. ESP-IDF timers and FreeRTOS are not wrapped.
foreach(symbol st25r3916_transceive i2c_new_master_bus i2c_master_bus_add_device
               i2c_master_bus_rm_device i2c_del_master_bus i2c_master_transmit
               i2c_master_transmit_receive i2c_master_multi_buffer_transmit)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${symbol}")
endforeach()
