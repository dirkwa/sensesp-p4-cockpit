# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
file(GLOB_RECURSE jlp_sources ${CMAKE_CURRENT_LIST_DIR}/jlp/*.cpp)
idf_component_register(
    SRCS app_main.cpp ${jlp_sources}
    INCLUDE_DIRS "."
    PRIV_REQUIRES cockpit_hal cockpit_voice cockpit_n2k esp_app_format espos_config espos_log espos_httpd espos_wifi espos_sk espos_ota
                  lvgl__lvgl esp_http_server esp_http_client esp_timer nvs_flash espressif__mdns
                  bblanchon__arduinojson espressif__esp_new_jpeg esp_driver_jpeg mbedtls)

espos_config_add_descriptor(config/cockpit.json)
