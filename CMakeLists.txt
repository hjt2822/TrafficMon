# 流量台 TrafficMon — MSVC / MinGW-w64 共用构建。
#
# Visual Studio:
#   cmake -S . -B build/vs -G "Visual Studio 17 2022" -A x64
#   cmake --build build/vs --config Release
# MinGW-w64（与 build_ui.sh 等价，用于本机验证 CMake 本身）:
#   cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/mingw
#
# UI 源码为 C11，核心与桥接为 C++17，两者由 ui/bridge.h 的 C ABI 连接。

cmake_minimum_required(VERSION 3.21)
project(trafficmon VERSION 0.1.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "流量台只针对 Windows 构建（Win32/GDI + ETW + iphlpapi）。")
endif()

option(TRAFFICMON_BUILD_TOOLS "构建控制台版 trafficmon_m1" ON)
option(TRAFFICMON_BUILD_TESTS "构建核心测试 connection_tests / storage_tests" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 单配置生成器（Ninja / Makefiles）默认 Release，与 build_ui.sh 的 -O2 -DNDEBUG 一致。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# 静态链接 CRT，让 exe 可以单文件拷走运行（对应 MinGW 的 -static）。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# --- 编译器差异集中在这里 -----------------------------------------------------
# 源码本身不含 GCC 扩展（无 __attribute__ / VLA / 语句表达式），所以两套工具链
# 只在命令行选项上分叉。
add_library(tm_flags INTERFACE)

if(MSVC)
    # /utf-8: 源码是无 BOM 的 UTF-8，含中文宽字符串字面量，不加会按本地代码页解读。
    # _CRT_SECURE_NO_WARNINGS: wcsncpy/wcscat/swprintf 的 C4996，调用点均已显式限长。
    # 用 COMPILE_LANGUAGE 限定，避免这些 cl 选项被传给 rc.exe（它不认 /utf-8 等）。
    target_compile_options(tm_flags INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/W4;/Zc:preprocessor>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive-;/Zc:__cplusplus>
        $<$<COMPILE_LANGUAGE:C,CXX>:/wd4100>   # 未引用的形参（Win32 窗口过程惯例）
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release>>:/O2;/GL;/Gy;/Gw>
    )
    target_compile_definitions(tm_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
    target_link_options(tm_flags INTERFACE
        $<$<CONFIG:Release>:/LTCG;/OPT:REF;/OPT:ICF>
    )
else()
    target_compile_options(tm_flags INTERFACE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wall;-Wextra>
        $<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release>>:-O2;-ffunction-sections;-fdata-sections>
    )
    target_link_options(tm_flags INTERFACE
        -static
        $<$<CONFIG:Release>:-s;-Wl,--gc-sections>
    )
endif()

# NOMINMAX 是必需的，不是保险：MinGW 的 windef.h 用 __cplusplus 挡住了 min/max 宏，
# MSVC 没有。ui/bridge.cpp 在 include <windows.h> 之后调用 std::min，不加会被宏吃掉。
#
# 测试目标依赖 assert()，不要通过共享选项为它们额外定义 NDEBUG；
# CMake Release 自带的 NDEBUG 在测试目标上显式取消（见下）。
target_compile_definitions(tm_flags INTERFACE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
)

# 发布目标用的 NDEBUG，逐个目标添加，不经过 tm_flags。
set(TM_RELEASE_DEFS $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# --- SQLite amalgamation ------------------------------------------------------
add_library(sqlite3 STATIC vendor/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC vendor/sqlite)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION      # 不需要动态扩展，去掉可减小体积与攻击面
    SQLITE_DQS=0                    # 禁用双引号当字符串的兼容行为
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /utf-8 /W0 $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(sqlite3 PRIVATE -w $<$<CONFIG:Release>:-O2>)
endif()

# --- 采集与存储核心（C++17）--------------------------------------------------
add_library(tmcore STATIC
    src/nic_collector.cpp
    src/proc_collector.cpp
    src/connection_collector.cpp
    src/traffic_aggregator.cpp
    src/storage.cpp
    src/local_time.cpp
)
target_include_directories(tmcore PUBLIC include)
target_compile_definitions(tmcore PRIVATE ${TM_RELEASE_DEFS})
target_link_libraries(tmcore PUBLIC tm_flags sqlite3 iphlpapi advapi32 ws2_32)

# --- 界面版（纯 C UI + C ABI 桥接）------------------------------------------
add_executable(trafficmon_ui WIN32
    ui/main.c
    ui/theme.c
    ui/format.c
    ui/draw.c
    ui/tray.c
    ui/bridge.cpp
    ui/app.rc
)
# app.rc 里 "ui/app.ico" / "ui/app.manifest" 是相对项目根的路径，所以根目录必须在
# 资源编译器的搜索路径上（windres 与 rc.exe 都用 include 路径解析这两个文件）。
target_include_directories(trafficmon_ui PRIVATE ui "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(trafficmon_ui PRIVATE ${TM_RELEASE_DEFS})
target_link_libraries(trafficmon_ui PRIVATE tm_flags tmcore
    gdi32 user32 shell32 comctl32 ole32 uuid
)

# app.rc 以 ID 1 / 类型 24 嵌入 ui/app.manifest（PerMonitorV2 DPI + Common Controls v6），
# 必须阻止链接器再生成一份默认清单，否则两份冲突。
if(MSVC)
    target_link_options(trafficmon_ui PRIVATE /MANIFEST:NO)
else()
    target_link_options(trafficmon_ui PRIVATE
        "-specs=${CMAKE_CURRENT_SOURCE_DIR}/ui/no-default-manifest.specs")
endif()

# --- 控制台版 ---------------------------------------------------------------
if(TRAFFICMON_BUILD_TOOLS)
    add_executable(trafficmon_m1 src/main.cpp)
    target_compile_definitions(trafficmon_m1 PRIVATE ${TM_RELEASE_DEFS})
    target_link_libraries(trafficmon_m1 PRIVATE tm_flags tmcore)
endif()

# --- 核心测试 ---------------------------------------------------------------
if(TRAFFICMON_BUILD_TESTS)
    enable_testing()

    # CMake 的 Release 配置自带 -DNDEBUG/-D NDEBUG，会把测试里的 assert() 全部编译掉，
    # 结果是测试"通过"但一条都没执行。这里显式取消定义，保证断言在 Release 下仍然生效。
    if(MSVC)
        set(TM_KEEP_ASSERTS /UNDEBUG)
    else()
        set(TM_KEEP_ASSERTS -UNDEBUG)
    endif()

    add_executable(connection_tests tests/connection_tests.cpp)
    target_compile_options(connection_tests PRIVATE ${TM_KEEP_ASSERTS})
    target_link_libraries(connection_tests PRIVATE tm_flags tmcore)
    add_test(NAME connection_tests COMMAND connection_tests)

    add_executable(storage_tests tests/storage_tests.cpp)
    target_compile_options(storage_tests PRIVATE ${TM_KEEP_ASSERTS})
    target_link_libraries(storage_tests PRIVATE tm_flags tmcore)
    add_test(NAME storage_tests COMMAND storage_tests)
    # 兼容存储测试的 build/storage-test.sqlite3 相对路径，并隔离真实数据库。
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-work/build")
    set_tests_properties(storage_tests PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-work")
endif()
