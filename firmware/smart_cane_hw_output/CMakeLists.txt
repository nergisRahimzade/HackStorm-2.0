##
# @file CMakeLists.txt
# @brief SmartCane Hardware Output — T5AI board display + speaker notifications.
#
# Polls the laptop smart_cane_app.py Flask server for AI scene alerts and
# renders them on the onboard LVGL display + audio codec speaker.
# Also runs onboard sensors (QMI8658 IMU + HC-SR04) and posts hardware
# events back to the laptop via POST /event.
#
# Build (from this folder, after activating the TuyaOpen toolchain):
#   tos.py build
#   tos.py flash   -p COM7 -b 921600
#   tos.py monitor -p COM8
##

if (CONFIG_ENABLE_WIFI STREQUAL "y")

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
)

else()
    message(FATAL_ERROR "smart_cane_hw_output requires WiFi — set CONFIG_ENABLE_WIFI=y")
endif()
