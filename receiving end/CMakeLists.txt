set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_SOURCE_DIR}/kernel/liteos/liteos_v208.5.0/Huawei_LiteOS/compat/linux/include"
    "${CMAKE_SOURCE_DIR}/kernel/liteos/liteos_v208.5.0/Huawei_LiteOS/compat/cmsis"
    "${CMAKE_SOURCE_DIR}/kernel/liteos/liteos_v208.5.0/Huawei_LiteOS/compat/nuttx/include"

    "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/sle"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/iis"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wifi"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/dlan"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/miniMP3"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/http_utils"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/spi_master"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/audio_play"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/wifi"

    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wlan/softap"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wlan/sta"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wlan/udp"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wlan/wifi_tool"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wlan/wifi_types"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/wlan/sta_softap_provisioner"

    PARENT_SCOPE
)

add_subdirectory(includes)
add_subdirectory(realize)

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    PARENT_SCOPE)
