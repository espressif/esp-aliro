set(_aliro_lib_dir "${CMAKE_CURRENT_LIST_DIR}")

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES mbedtls nvs_flash)

set(_aliro_lib "${_aliro_lib_dir}/lib/${IDF_TARGET}/libesp_aliro.a")
if(NOT EXISTS "${_aliro_lib}")
    message(FATAL_ERROR
        "esp_aliro_lib: prebuilt library missing for IDF_TARGET='${IDF_TARGET}'."
        " Expected: ${_aliro_lib}.")
endif()

add_prebuilt_library(esp_aliro_prebuilt "${_aliro_lib}"
    REQUIRES mbedtls nvs_flash)

target_link_libraries(${COMPONENT_LIB} INTERFACE esp_aliro_prebuilt)
