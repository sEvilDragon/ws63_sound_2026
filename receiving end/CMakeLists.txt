set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/sle"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/pcm5102"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wifi"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/audio_play"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/wifi"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/dlan"
    "${CMAKE_SOURCE_DIR}/kernel/liteos/liteos_v208.5.0/Huawei_LiteOS/compat/linux/include"
    "${CMAKE_SOURCE_DIR}/kernel/liteos/liteos_v208.5.0/Huawei_LiteOS/compat/cmsis"
    "${CMAKE_SOURCE_DIR}/kernel/liteos/liteos_v208.5.0/Huawei_LiteOS/compat/nuttx/include"
    PARENT_SCOPE
)

add_subdirectory(includes)
add_subdirectory(realize)

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    PARENT_SCOPE)
