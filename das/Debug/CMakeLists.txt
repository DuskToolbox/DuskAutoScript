# DasDebug：进程级崩溃捕获静态库（自动 full minidump）。
# 跨平台主逻辑（CrashDumper）+ 平台隔离实现（Win32: CrashDumpWriterWin32）。
# 与 DasCore.dll 解耦，由 DasHttp / DasHostX 各自静态链。
option(DAS_AUTO_MINIDUMP
       "Enable automatic full minidump on process crash (SEH/terminate)" ON)

file(GLOB_RECURSE DAS_DEBUG_HEADERS include/*)

# 跨平台源始终编译；平台源按平台条件加入（未来 Linux 在此 elif 加
# CrashDumpWriterLinux.cpp，CrashDumper 主逻辑不动）。
add_library(DasDebug STATIC src/CrashDumper.cpp ${DAS_DEBUG_HEADERS})
if(WIN32)
    target_sources(DasDebug PRIVATE src/CrashDumpWriterWin32.cpp)
endif()

set_property(TARGET DasDebug PROPERTY POSITION_INDEPENDENT_CODE ON)

target_include_directories(DasDebug PUBLIC include)
# CrashDumper 的路径通道消费 das/Utils 的 header-only helper 家族
# （PathText.h/BoostPathBridge.h）；不链 DasUtils 库——那是带 yyjson/fmt
# 依赖的静态库，会破坏 DasDebug 与 DasCore.dll 解耦的轻量设计。
target_include_directories(
    DasDebug PRIVATE ${CMAKE_SOURCE_DIR}/das/Utils/include)
target_link_libraries(DasDebug PUBLIC DasPlatformDefs DasCommonHeaders)

# boost::dll::program_location（跨平台取 exe 路径）需要。
if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasDebug PUBLIC Boost::dll)
else()
    target_include_directories(DasDebug PUBLIC ${Boost_INCLUDE_DIRS})
endif()

# MiniDumpWriteDump（Win32 平台层）。PUBLIC 让静态链它的 exe 自动带上。
if(WIN32)
    target_link_libraries(DasDebug PUBLIC dbghelp)
endif()

target_compile_definitions(
    DasDebug PRIVATE $<$<BOOL:${DAS_AUTO_MINIDUMP}>:DAS_AUTO_MINIDUMP>)
