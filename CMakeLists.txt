#===============================================================================
# @brief    cmake file - main application component
#===============================================================================
set(COMPONENT_NAME "main_app")

set(SOURCES
)

if(DEFINED CONFIG_MAIN_RECEIVING_END)
    set(PUBLIC_HEADER
        "${CMAKE_CURRENT_SOURCE_DIR}/receiving end/includes"
    )
    add_subdirectory("receiving end" receiving_end)
else()
    # 默认编译 sending end（CONFIG_MAIN_SENDING_END 或未配置时均走此分支）
    add_subdirectory("sending end" sending_end)
endif()

set(PRIVATE_HEADER
)

set(PRIVATE_DEFINES
)

set(PUBLIC_DEFINES
)

set(COMPONENT_PUBLIC_CCFLAGS
)

set(COMPONENT_CCFLAGS
    # 仅对 .cpp 文件生效：开启 C++17，并压制 C 专属的警告（全局 CCFLAGS 含 -Wstrict-prototypes/-Wpointer-arith，在 C++ 下会报错）
    "$<$<COMPILE_LANGUAGE:CXX>:-std=c++17>"
    # 1
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-unwind-tables>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-asynchronous-unwind-tables>"
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>"
    # 1
    "$<$<COMPILE_LANGUAGE:CXX>:-fpermissive>"      # 允许 C 风格的 NULL 指针赋值（降级为警告）
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-error>"        # 全局 -Werror 会把 C 专属选项的警告变为错误，此处关闭 C++ 的 error 提升
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-strict-prototypes>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-pointer-arith>"
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-jump-misses-init>"        # -Wjump-misses-init 仅对 C 有效
    "$<$<COMPILE_LANGUAGE:CXX>:-Wno-unused-but-set-variable>"  # 抑制未使用变量警告
)

set(WHOLE_LINK
    true
)

set(MAIN_COMPONENT
    true
)

build_component()
