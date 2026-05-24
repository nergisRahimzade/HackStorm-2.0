##
# @file CMakeLists.txt
# @brief Standalone T5AI demo for SmartCane.
#
# This demo runs entirely on the Tuya T5AI board so it can be shown
# independently of the laptop-based web application.
##

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(APP_NAME ${APP_PATH} NAME)

aux_source_directory(${APP_PATH}/src APP_SRCS)

add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
)