#===============================================================================
# @brief    cmake file - realize pass-through
#===============================================================================
add_subdirectory(led_test)
add_subdirectory(audio_read_send)
# [新增功能模块] 在 realize/ 下新建 xxx_test/ 目录后：
#   1. 在该目录创建 CMakeLists.txt（照 led_test/CMakeLists.txt 改文件名）
#   2. 在此处追加一行：add_subdirectory(xxx_test)
set(SOURCES "${SOURCES}" PARENT_SCOPE)
