#===============================================================================
# @brief    cmake file - sending end sources
#===============================================================================
set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/pcm2706"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/sle"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/denoise"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/audio_read_send"
    PARENT_SCOPE
)

add_subdirectory(includes)
add_subdirectory(realize)

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../other/adpcm/adpcm.cpp"
    PARENT_SCOPE)
