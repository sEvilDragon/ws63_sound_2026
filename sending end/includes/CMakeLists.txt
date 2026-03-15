#===============================================================================
# @brief    cmake file - includes pass-through
#===============================================================================
add_subdirectory(led_test)
add_subdirectory(pcm2706)
add_subdirectory(sle)
add_subdirectory(denoise)
set(SOURCES "${SOURCES}" PARENT_SCOPE)
