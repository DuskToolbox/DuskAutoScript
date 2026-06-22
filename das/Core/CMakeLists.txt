#根据导出语言查找依赖包
if (EXPORT_JAVA)
find_package(JNI REQUIRED)
endif()

if(EXPORT_PYTHON)
    find_package(Python3 COMPONENTS Development Development.SABIModule REQUIRED)
    # 创建 DasPython::SABI 包装目标（带 /NODEFAULTLIB 修复 MSVC pyconfig.h 冲突）
    include(DasPythonFallback)
    das_python_debug_fallback()
endif()

if(EXPORT_LUA)
    include(BuildLua)
    include(FindSol2)
endif()

add_library(DasCoreObjects OBJECT)
add_custom_target(DasCoreObjectsPreBuild ALL)
add_dependencies(
    DasCoreObjects
    DasCoreObjectsPreBuild)
add_dependencies(DasGateway DasCoreObjectsPreBuild)

#== == = das_idl模块工具集（未来独立模块） == == =
#使用GLOB自动收集das_idl目录下的所有Python脚本
file(GLOB DAS_IDL_MODULE_TOOLS "${CMAKE_SOURCE_DIR}/tools/das_idl/*.py")

message(STATUS "[DAS IDL] Module tools (${CMAKE_COUNT_VAR} files):")
foreach(tool ${DAS_IDL_MODULE_TOOLS})
    message(STATUS "  - ${tool}")
endforeach()

#引入 DasIdlGenerator.cmake 模块
if (NOT EXISTS "${CMAKE_SOURCE_DIR}/cmake/DasIdlGenerator.cmake")
    message(FATAL_ERROR "DasIdlGenerator.cmake not found at ${CMAKE_SOURCE_DIR}/cmake/DasIdlGenerator.cmake")
endif ()
include("${CMAKE_SOURCE_DIR}/cmake/DasIdlGenerator.cmake")
message(STATUS "[DAS IDL] DasIdlGenerator.cmake module loaded successfully")
include("${CMAKE_SOURCE_DIR}/cmake/DasAddIdlExport.cmake")

#确保Python虚拟环境已创建（在CMake配置阶段运行）
message(STATUS "[DAS IDL] Setting up Python virtual environment...")
das_idl_setup_venv()
if (NOT EXISTS "${DAS_IDL_VENV_PYTHON}")
    message(FATAL_ERROR "[DAS IDL] Python virtual environment setup failed. ${DAS_IDL_VENV_PYTHON} does not exist.")
endif ()
message(STATUS "[DAS IDL] Python virtual environment is ready at ${DAS_IDL_VENV_PYTHON}")

#收集所有IDL文件
file(GLOB DAS_IDL_ROOT "${CMAKE_SOURCE_DIR}/idl/*.idl")

set(DAS_IDL_FILES ${DAS_IDL_ROOT})

#使用 das_add_idl_export 函数替换现有的 IDL 生成和 SWIG 导出代码
#动态构建 LANGUAGES 参数
set(_LANGUAGES "")

if(EXPORT_PYTHON)
    list(APPEND _LANGUAGES "Python")
endif()

if(EXPORT_JAVA)
    list(APPEND _LANGUAGES "Java")
endif()

if(EXPORT_CSHARP)
    list(APPEND _LANGUAGES "CSharp")
endif()

if(EXPORT_LUA)
    list(APPEND _LANGUAGES "Lua")
endif()

if(EXPORT_NODE)
    list(APPEND _LANGUAGES "Node")
endif()

set(DAS_CORE_LUA_OPEN_MODULE_NAME DasCoreApi)
set(DAS_CORE_LUA_OPEN_FUNCTION_NAME "luaopen_${DAS_CORE_LUA_OPEN_MODULE_NAME}")
set(DAS_CORE_CSHARP_NAMESPACE_ROOT Das.Generated)
set(DAS_CORE_CSHARP_DAS_NATIVE_MODULE_NAME das)
set(DAS_CORE_CSHARP_NATIVE_SUPPORT_MODULE_NAME DasCSharpNativeSupport)

#获取 idl / 目录下所有.idl 文件名
file(GLOB _IDL_FILES "${CMAKE_SOURCE_DIR}/idl/*.idl")
set(_IDL_FILE_NAMES "")

foreach(_FILE ${_IDL_FILES})
    get_filename_component(_NAME ${_FILE} NAME)
    list(APPEND _IDL_FILE_NAMES ${_NAME})
endforeach()

# 指定用户自定义的 SWIG 文件（SWIG / ExportAll.i 通过 %include 包含自动生成的内容）
set(_USER_SWIG_FILES "${CMAKE_SOURCE_DIR}/SWIG/ExportAll.i")

#使用新函数替换现有的 IDL 生成和 SWIG 导出代码
das_add_idl_export(
    NAME DasCore
    IDL_DIR "${CMAKE_SOURCE_DIR}/idl"
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das"
    IDL_FILES ${_IDL_FILE_NAMES}
    LANGUAGES ${_LANGUAGES}
    USER_SWIG_FILES ${_USER_SWIG_FILES}
    SWIG_MODULE_NAME DuskAutoScript
    SWIG_OPTIONS_Python -threads
    SWIG_OPTIONS_Java -package org.das
    EXPORT_MACRO DAS_API
    EXPORT_C_MACRO DAS_C_API
    LUA_OPEN_MODULE_NAME ${DAS_CORE_LUA_OPEN_MODULE_NAME}
    NODE_PACKAGE_NAME ${NODE_PACKAGE_NAME}
    NODE_ADDON_NAME ${NODE_ADDON_NAME}
    CSHARP_NAMESPACE_ROOT ${DAS_CORE_CSHARP_NAMESPACE_ROOT}
    CSHARP_DAS_NATIVE_MODULE_NAME ${DAS_CORE_CSHARP_DAS_NATIVE_MODULE_NAME}
    CSHARP_NATIVE_SUPPORT_MODULE_NAME ${DAS_CORE_CSHARP_NATIVE_SUPPORT_MODULE_NAME}
    SWIG_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/include/"
    "${CMAKE_BINARY_DIR}/das/include"
    DEPENDS_ON DasCore
)

#将 Java JAR 路径变量导出到父作用域（供 Plugins 使用）
if(EXPORT_JAVA AND DEFINED DAS_CORE_JAVA_JAR)
    set(DAS_CORE_JAVA_JAR "${DAS_CORE_JAVA_JAR}" PARENT_SCOPE)
    set(DAS_JAVA_JAR_DIR "${DAS_JAVA_JAR_DIR}" PARENT_SCOPE)
    message(STATUS "[DasCore] Exporting DAS_CORE_JAVA_JAR: ${DAS_CORE_JAVA_JAR}")
endif()

#让预构建目标依赖IDL生成目标
#注意：DasCoreSwigAllGenerated 目标只在启用 SWIG 导出时才存在
add_dependencies(DasCoreObjectsPreBuild DasCoreIdlUpdateList DasCoreIdlGenerated)

if(_LANGUAGES)
    if(TARGET DasCoreSwigAllGenerated)
        add_dependencies(DasCoreObjectsPreBuild DasCoreSwigAllGenerated)
    endif()
endif()

#== == == 官方IID列表生成 == == ==
#设置OfficialIids输出目录
set(DAS_OFFICIAL_IIDS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/iids")
set(DAS_OFFICIAL_IIDS_HEADER "${DAS_OFFICIAL_IIDS_OUTPUT_DIR}/OfficialIids.h")
set(DAS_OFFICIAL_IIDS_SOURCE "${DAS_OFFICIAL_IIDS_OUTPUT_DIR}/OfficialIids.cpp")
set(DAS_OFFICIAL_IIDS_DEPS_FILE "${CMAKE_BINARY_DIR}/das/official_iids_deps.txt")

#生成官方IID列表的自定义命令
add_custom_command(
    OUTPUT ${DAS_OFFICIAL_IIDS_HEADER} ${DAS_OFFICIAL_IIDS_SOURCE}
    COMMAND ${CMAKE_COMMAND} -E remove -f ${DAS_OFFICIAL_IIDS_DEPS_FILE}
    COMMAND ${DAS_IDL_VENV_PYTHON} ${CMAKE_SOURCE_DIR}/tools/generate_official_iids.py
    -i ${DAS_IDL_FILES}
    -o ${DAS_OFFICIAL_IIDS_OUTPUT_DIR}
    --check-deps ${DAS_OFFICIAL_IIDS_DEPS_FILE}
    -v
    DEPENDS ${DAS_IDL_FILES}
    ${CMAKE_SOURCE_DIR}/tools/generate_official_iids.py
    ${CMAKE_SOURCE_DIR}/tools/das_idl/das_idl_parser.py
    COMMENT "Generate official IIDs from IDL files"
    VERBATIM
)

#创建官方IID列表生成目标
add_custom_target(DasCoreOfficialIidsGenerated DEPENDS ${DAS_OFFICIAL_IIDS_HEADER} ${DAS_OFFICIAL_IIDS_SOURCE})
add_dependencies(DasCoreObjectsPreBuild DasCoreOfficialIidsGenerated)

#== == == 官方IID列表生成结束 == == ==
target_compile_options(DasCoreObjects PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>
)
set_property(TARGET DasCoreObjects PROPERTY POSITION_INDEPENDENT_CODE ON)

#初始化外部语言库列表
set(DAS_FOREIGN_LANGUAGE_LIB_LIST "")

#设置导出宏列表
set(DAS_EXPORT_MACRO_LIST "")

if(EXPORT_PYTHON)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_PYTHON)
    list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST DasPython::SABI)
endif()

if(EXPORT_JAVA)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_JAVA)
    list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST JNI::JNI)
endif()

if(EXPORT_CSHARP)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_CSHARP)
    if(DAS_CSHARP_HOSTFXR_BACKEND)
        list(APPEND DAS_EXPORT_MACRO_LIST DAS_CSHARP_HOSTFXR_BACKEND)
        list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST DotNet::nethost)
    endif()
    if(DAS_CSHARP_NETFX_BACKEND)
        list(APPEND DAS_EXPORT_MACRO_LIST DAS_CSHARP_NETFX_BACKEND)
        list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST DotNetFramework::clr_hosting)
    endif()
    set(DAS_CORE_CSHARP_OUTPUT_DIR "${DasCore_CSHARP_OUTPUT_DIR}" PARENT_SCOPE)
    set(DAS_CORE_CSHARP_GENERATED_FILES "${DasCore_CSHARP_GENERATED_FILES}" PARENT_SCOPE)
    set(DAS_CORE_CSHARP_EXPORT_TARGET "${DasCore_CSHARP_EXPORT_TARGET}" PARENT_SCOPE)
    set(DAS_CORE_CSHARP_DAS_NATIVE_MODULE_NAME "${DasCore_CSHARP_DAS_NATIVE_MODULE_NAME}" PARENT_SCOPE)
    set(DAS_CORE_CSHARP_NATIVE_SUPPORT_MODULE_NAME "${DasCore_CSHARP_NATIVE_SUPPORT_MODULE_NAME}" PARENT_SCOPE)
endif()

if(EXPORT_LUA)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_LUA)
    list(APPEND DAS_FOREIGN_LANGUAGE_LIB_LIST sol2::sol2 Das::Lua)
endif()

if(EXPORT_NODE)
    list(APPEND DAS_EXPORT_MACRO_LIST DAS_EXPORT_NODE)
endif()

#应用导出宏定义
if(DAS_EXPORT_MACRO_LIST)
    target_compile_definitions(DasCoreObjects PUBLIC ${DAS_EXPORT_MACRO_LIST})
endif()

if(EXPORT_LUA)
    target_compile_definitions(DasCoreObjects PRIVATE
        DAS_LUA_OPEN_FUNCTION_NAME="${DAS_CORE_LUA_OPEN_FUNCTION_NAME}")
endif()

#Python Stable ABI 配置
if(EXPORT_PYTHON)
    target_compile_definitions(DasCoreObjects PRIVATE Py_LIMITED_API=0x030A0000)
endif()

target_include_directories(DasCoreObjects PUBLIC ${CMAKE_BINARY_DIR}/das/include/)
target_include_directories(DasCoreObjects PUBLIC ${DasCore_ABI_OUTPUT_DIR})
target_include_directories(DasCoreObjects PUBLIC ${DasCore_WRAPPER_OUTPUT_DIR})
target_include_directories(DasCoreObjects PUBLIC ${DasCore_HEADER_OUTPUT_DIR})
target_compile_definitions(DasCoreObjects PRIVATE -DDAS_BUILD_SHARED)

# DasUtils needs generated header include path via DasCommonHeaders
target_include_directories(DasCommonHeaders INTERFACE ${DasCore_HEADER_OUTPUT_DIR})
add_dependencies(DasUtils DasCoreIdlGenerated)

target_link_libraries(DasCoreObjects PUBLIC
    Boost::filesystem
    ICU::data
    ICU::i18n
    ICU::uc
    Das3rdParty)

if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasCoreObjects PUBLIC ${DAS_BUNDLED_BOOST_LIBS})
else()
# 对于使用msys包管理器安装boost的用户，使用msys的clangd或使用这个语句强制将Boost_INCLUDE_DIRS添加到compile_commands.json
#target_compile_options(DasCoreObjects PRIVATE "-I${Boost_INCLUDE_DIRS}")
    target_include_directories(DasCoreObjects PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasCoreObjects PRIVATE ${Boost_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(DasCoreObjects PRIVATE ws2_32 Mswsock)
endif()

das_add_core_component(Utils)
das_add_core_component(Debug)
das_add_core_component(OcvWrapper)
das_add_core_component(OrtWrapper)
das_add_core_component(ForeignInterfaceHost)

#将OfficialIids生成的文件添加到DasCoreObjects
set(DAS_OFFICIAL_IIDS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/iids")

#注意：无条件添加这些文件，因为 add_custom_command 会确保文件在编译前生成
target_sources(DasCoreObjects PRIVATE
    ${DAS_OFFICIAL_IIDS_SOURCE}
    ${DAS_OFFICIAL_IIDS_HEADER}
)
target_include_directories(DasCoreObjects PUBLIC ${DAS_OFFICIAL_IIDS_OUTPUT_DIR})
source_group("Autogen\\OfficialIids" FILES "${DAS_OFFICIAL_IIDS_SOURCE}" "${DAS_OFFICIAL_IIDS_HEADER}")
das_add_core_component(SettingsManager)
das_add_core_component(Logger)
das_add_core_component(Exceptions)
das_add_core_component(i18n)
das_add_core_component(TaskScheduler)
das_add_core_component(GraphRuntime)
das_add_core_component(CoreServices)
das_add_core_component(IPC)

#将IpcGenerated.cpp添加到DasCoreObjects（与OfficialIids相同的模式）
target_sources(DasCoreObjects PRIVATE ${DasCore_IPC_GENERATED_CPP})
set_source_files_properties(${DasCore_IPC_GENERATED_CPP} PROPERTIES GENERATED TRUE)
target_include_directories(DasCoreObjects PUBLIC "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/ipc")

add_library(DasCore SHARED)
add_dependencies(DasCore DasCoreObjects)

add_compile_definitions(DAS_BUILD_SHARED)

#FetchContent_Declare(
#Sol2
#URL ${GITHUB_MIRROR_URL } / ThePhD / sol2 / archive / refs / tags / v3.3.0.zip
#)
#FetchContent_MakeAvailable(Sol2)
#target_link_libraries(DasCoreObjects PUBLIC sol2::sol2)
#find_package(Lua 5.4)

#if (NOT ${LUA_FOUND})
#message(STATUS "Downloading and configuring Lua 5.4.6.")
#FetchContent_Declare(
#DasLua
#URL ${GITHUB_MIRROR_URL } / lua / lua / archive / refs / tags / v5.4.6.zip
#PATCH_COMMAND ${CMAKE_COMMAND } - E copy
#"${CMAKE_SOURCE_DIR}/cmake/CompileLua.cmake"<SOURCE_DIR> / CMakeLists.txt
#)
#FetchContent_MakeAvailable(DasLua)
#target_link_libraries(DasCoreObjects PUBLIC Das::Lua)
#else()
#target_include_directories(DasCoreObjects PRIVATE ${LUA_INCLUDE_DIR})
#target_link_libraries(DasCoreObjects PUBLIC ${LUA_LIBRARIES})
#endif()

target_link_libraries(DasCoreObjects
    PUBLIC DasUtils DasPlatformDefs ${DAS_FOREIGN_LANGUAGE_LIB_LIST})

#Windows 平台需要链接 ntdll（Boost.Process 使用了 NT 内部 API）
if(WIN32)
    target_link_libraries(DasCoreObjects PRIVATE ntdll)
endif()

target_compile_definitions(DasCoreObjects PUBLIC ${DAS_EXPORT_MACRO_LIST})
target_compile_definitions(DasCoreObjects PRIVATE DAS_CORE_NAME="$<TARGET_FILE_NAME:DasCore>")

target_link_libraries(DasCore PUBLIC DasCoreObjects)
target_link_libraries(DasCore PRIVATE ${DAS_FOREIGN_LANGUAGE_LIB_LIST})

if(${DAS_BUILD_TEST})
    das_add_core_test(ForeignInterfaceHost)

    # 将 Node addon/package 名称传递给生产和测试目标，
    # 替代各文件中硬编码的 das_core_napi / das-core-node 字符串
    if(EXPORT_NODE AND NODE_ADDON_NAME)
        target_compile_definitions(DasCoreObjects PRIVATE
            DAS_NODE_ADDON_NAME="${NODE_ADDON_NAME}"
            DAS_NODE_PACKAGE_NAME="${NODE_PACKAGE_NAME}")
        target_compile_definitions(ForeignInterfaceHostTest PRIVATE
            DAS_NODE_ADDON_NAME="${NODE_ADDON_NAME}"
            DAS_NODE_PACKAGE_NAME="${NODE_PACKAGE_NAME}")
    endif()

    das_add_core_test(i18n)
    das_add_core_test(Utils)
    das_add_core_test(SettingsManager)
    das_add_core_test(Exceptions)
    das_add_core_test(TaskScheduler)

#TaskSchedulerTest needs Http headers for controller validation tests
    target_include_directories(TaskSchedulerTest PRIVATE ${CMAKE_SOURCE_DIR}/das/Http/src)
    das_add_core_test(IPC)
    das_add_core_test(Debug)
    das_add_core_test(OcvWrapper)
    das_add_core_test(OrtWrapper)
    das_add_core_test(GraphRuntime)

# ForeignInterfaceHost/test 暴露 TestablePluginManager.h 测试专用头，
# 跨模块测试（TaskSchedulerTest / DebugTest 调用 RegisterTestFeature）
# 通过 #include "TestablePluginManager.h" 引用。这里只在确实需要访问
# 测试专用头的目标上加 include path，不污染 das_add_core_test 通用函数。
    target_include_directories(ForeignInterfaceHostTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ForeignInterfaceHost/test)
    target_include_directories(TaskSchedulerTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ForeignInterfaceHost/test)
    target_include_directories(DebugTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/ForeignInterfaceHost/test)

endif()

# Python 绑定文件拷贝：DuskAutoScript.py 和 _DasCorePythonExport.pyd 必须与 DasCore.dll 在同一目录
# 使用 DasCorePythonExport 自身的 POST_BUILD 事件，避免阻塞 DasAutoCopyDll 中的其他 DLL 拷贝
if(EXPORT_PYTHON)
    add_custom_command(
        TARGET DasCorePythonExport
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/include/das/Python/DuskAutoScript.py" $<TARGET_FILE_DIR:DasCore>)
    add_custom_command(
        TARGET DasCorePythonExport
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:DasCorePythonExport> $<TARGET_FILE_DIR:DasCore>)

    if(WIN32)
        add_custom_command(
            TARGET DasCorePythonExport
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:Python3::SABIModule>
                $<TARGET_FILE_DIR:DasCore>
            COMMENT "[DasCore] Copying Python stable ABI runtime DLL")

        if(TARGET Python3::Python)
            add_custom_command(
                TARGET DasCorePythonExport
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:Python3::Python>
                    $<TARGET_FILE_DIR:DasCore>
                COMMENT "[DasCore] Copying Python runtime DLL")
        endif()
    endif()

    if(DAS_BUILD_TEST)
#确保 Test 目录存在
        if(NOT EXISTS ${CMAKE_BINARY_DIR}/Test)
            file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Test)
        endif()

        add_custom_command(
            TARGET DasCorePythonExport
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/include/das/Python/DuskAutoScript.py" ${CMAKE_BINARY_DIR}/Test)
        add_custom_command(
            TARGET DasCorePythonExport
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:DasCorePythonExport> ${CMAKE_BINARY_DIR}/Test)
    endif()
endif()
