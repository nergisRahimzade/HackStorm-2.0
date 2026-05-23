##
# @file CMakeLists.txt
# @brief Build definition for the SmartCane speaker_test app.
#/

# APP_PATH = this folder
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME = folder name (speaker_test)
get_filename_component(APP_NAME ${APP_PATH} NAME)

# Collect all .c files under src/
aux_source_directory(${APP_PATH}/src APP_SRCS)

# Headers live next to the sources
set(APP_INC ${APP_PATH}/src)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )
