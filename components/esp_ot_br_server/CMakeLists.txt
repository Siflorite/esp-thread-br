set(requires mdns fatfs spiffs esp_eth nvs_flash freertos openthread esp_http_server protocol_examples_common esp_wifi esp_app_format esp_timer)

# For IDF < 6.0, use IDF's built-in json component; for IDF >= 6.0 use managed cJSON component through idf_component.yml
if("${IDF_VERSION_MAJOR}" VERSION_LESS "6")
    list(APPEND requires json)
endif()

set(excluded_sources "")
if(NOT CONFIG_ESP_BR_WEB_CLI)
    list(APPEND excluded_sources "src/esp_br_web_cli.c" "src/esp_br_web_cli_ring.c")
endif()

idf_component_register(
    SRC_DIRS src
    EXCLUDE_SRCS ${excluded_sources}
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS private_include
    PRIV_REQUIRES ${requires}
    EMBED_FILES "favicon.ico"
    EMBED_TXTFILES "frontend/wifi_configuration.html"
)

spiffs_create_partition_image(web_storage ${CMAKE_CURRENT_SOURCE_DIR}/frontend FLASH_IN_PROJECT)

# Tee the existing CLI callback without reinitializing the interpreter or
# replacing the serial console's completion notification callback.
if(CONFIG_ESP_BR_WEB_CLI AND CONFIG_OPENTHREAD_CLI)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=otCliInit" "-Wl,--wrap=otCliInputLine")
endif()
