# 蓝牙 BR/EDR 头文件路径（全局 include，因为 main_app target 在当前 CMake 上下文中不可用）
include_directories(
    ${CMAKE_SOURCE_DIR}/include/middleware/services/bts/br
    ${CMAKE_SOURCE_DIR}/include/middleware/services/bts/common
)

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ble.cpp"
    PARENT_SCOPE)