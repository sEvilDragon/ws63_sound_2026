add_subdirectory_if_exist(led_test)
add_subdirectory_if_exist(iic)
add_subdirectory_if_exist(spi_slave)
set(SOURCES "${SOURCES}" PARENT_SCOPE)