#===============================================================================
# @brief    cmake file - control end sources
#===============================================================================
set(PUBLIC_HEADER "${PUBLIC_HEADER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/iic"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes/spi_slave"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/led_test"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/spi_task"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/ttp229"
    "${CMAKE_CURRENT_SOURCE_DIR}/realize/ui_task"
    PARENT_SCOPE
)

add_subdirectory(includes)
add_subdirectory(realize)

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    PARENT_SCOPE)
