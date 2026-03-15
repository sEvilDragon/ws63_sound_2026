set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/sle"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/pcm5102"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/audio_play"
    PARENT_SCOPE
)

add_subdirectory(includes)
add_subdirectory(realize)

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    PARENT_SCOPE)
