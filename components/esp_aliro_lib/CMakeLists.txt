include($ENV{IDF_PATH}/tools/cmake/version.cmake)
set(_idf_version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH}")

idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES mbedtls nvs_flash)

if(IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR GREATER_EQUAL 2 AND IDF_VERSION_MINOR LESS_EQUAL 5)
    set(_aliro_variant "idf5${IDF_VERSION_MINOR}")
elseif(IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 0)
    set(_aliro_variant "idf60")
else()
    message(FATAL_ERROR "esp_aliro_lib: unsupported ESP-IDF version ${_idf_version}")
endif()

set(_aliro_lib "${CMAKE_CURRENT_LIST_DIR}/lib/${IDF_TARGET}/libesp_aliro_${_aliro_variant}.a")
if(NOT EXISTS "${_aliro_lib}")
    message(FATAL_ERROR
        "esp_aliro_lib: prebuilt library '${_aliro_variant}' missing for IDF_TARGET='${IDF_TARGET}'. "
        "Expected: ${_aliro_lib}.")
endif()

idf_component_get_property(_managed_requires ${COMPONENT_NAME} MANAGED_REQUIRES)
idf_component_get_property(_managed_priv_requires ${COMPONENT_NAME} MANAGED_PRIV_REQUIRES)

# Imported targets do not inherit manifest dependencies. Attach them directly
# so IDF orders dependency archives after the prebuilt Aliro archive.
add_prebuilt_library(esp_aliro_prebuilt "${_aliro_lib}"
                     REQUIRES ${_managed_requires}
                     PRIV_REQUIRES ${_managed_priv_requires} mbedtls nvs_flash)

target_link_libraries(${COMPONENT_LIB} INTERFACE esp_aliro_prebuilt)
