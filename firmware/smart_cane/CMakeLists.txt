##
# @file CMakeLists.txt
# @brief SmartCane — WiFi + IMU + ultrasonic + TTS-over-HTTP firmware
#        for the Tuya T5-E1 Touch AMOLED board (BK7258).
#
# Build (from this folder, after activating D:\tuyaopen\export.ps1):
#   tos.py build
#   tos.py flash   -p COM3 -b 921600
#   tos.py monitor -p COM4
##

if (CONFIG_ENABLE_WIFI STREQUAL "y")

# APP_PATH / APP_NAME
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

# Collect all .c files under src/
aux_source_directory(${APP_PATH}/src APP_SRCS)

########################################
# Target configuration
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
)

else()
    message(FATAL_ERROR "smart_cane requires WiFi — enable CONFIG_ENABLE_WIFI=y")
endif()
