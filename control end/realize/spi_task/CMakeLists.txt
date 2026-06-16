#===============================================================================
# @brief    cmake file - led_test implementation
#===============================================================================
# [新增 .cpp 文件] 在此模块目录下新建 xxx.cpp 后，照下方格式追加一行即可
# "${CMAKE_CURRENT_SOURCE_DIR}/xxx.cpp"
set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/spi_task.cpp"
    PARENT_SCOPE)
