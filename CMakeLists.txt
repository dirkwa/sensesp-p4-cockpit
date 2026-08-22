# SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
#
# espos-p4-cockpit: a pure ESP-IDF 6 project. WiFi, config, the
# web UI, SignalK discovery/token/stream and OTA come from the espOS
# submodule (espos/); this project adds the display HAL, LVGL and the JSON
# Layout Player. Build:  . espos/.idf-version's IDF; idf.py set-target esp32p4; scripts/build.sh
cmake_minimum_required(VERSION 3.22)

# The IDF version pin, espos/components on EXTRA_COMPONENT_DIRS, the signing
# key and the re-sign-on-key-change workaround all live in espOS now. They
# used to be ~55 lines copied into this file, ble-gateway's and espOS's own,
# which had already started to drift — and the signing half of it is exactly
# the code that must not, since getting it wrong yields a panel that flashes
# fine over USB and then rejects every OTA.
include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
espos_project_prologue(NAME "cockpit")

# The 4B (720x720 ST7703) HAL is parked until a board is on hand again; its
# vendored driver predates the ESP-IDF 6 esp_lcd API. Drop it from the build
# rather than carry a dead #ifdef.
set(EXCLUDE_COMPONENTS esp_lcd_st7703)

# LVGL configuration lives in the project, not in Kconfig.
idf_build_set_property(COMPILE_DEFINITIONS "-DLV_CONF_PATH=\"${CMAKE_CURRENT_LIST_DIR}/components/cockpit_hal/lv_conf.h\"" APPEND)

project(cockpit)

# espOS web UI → LittleFS "storage" image (built in espos/ui by `npm run build`).
espos_project_ui_partition(NAME "cockpit")
