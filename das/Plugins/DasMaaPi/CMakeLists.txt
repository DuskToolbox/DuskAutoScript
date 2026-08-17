aux_source_directory(src DAS_MAAPI_SOURCES)

add_library(DasMaaPi SHARED ${DAS_MAAPI_SOURCES})
# 强制 lib 前缀，匹配 DasMaaPi 测试套件中 libDasMaaPi.dll 的预期
# （MaapiPluginScaffoldTest.ArtifactsUseOfficialPluginContract、
#  MaapiIntegrationFixture.SampleInterfaceAuthoringCompileRuntimeDryRun 等）。
# MSVC 默认无前缀；设 PREFIX "lib" 后所有平台统一为 libDasMaaPi.{dll|so}。
set_target_properties(DasMaaPi PROPERTIES PREFIX "lib")
target_include_directories(DasMaaPi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(DasMaaPi PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /MP /bigobj>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Werror>)
target_link_libraries(DasMaaPi PUBLIC DAS_EX_PRIVATE_LIBS)
target_link_libraries(DasMaaPi PRIVATE
    DasCore
    DasBundledMaaFramework)
# MaaToolkit is NOT hard-linked — it's runtime-loaded by MaaDynamicSymbols
# (boost::dll) from the plugin's co-located directory. Keeping it out of the
# link line is what makes this binary cross-platform-link-safe: the 10
# platform-specific symbols (MaaWin32ControllerCreate /
# MaaGamepadControllerCreate + 8 MaaToolkitDesktopWindow* family) never
# appear on the link line, so a non-Windows MaaFW build that doesn't export
# them links cleanly. The bundle DLL copy in the POST_BUILD loop below still
# copies MaaToolkit.dll into plugins/DasMaaPi/ for the runtime load.

if(WIN32)
    target_link_libraries(DasMaaPi PRIVATE bcrypt)
endif()

# PiHttpClient uses Beast TLS (boost::beast::ssl_stream) for https targets.
# Both toolchains resolve to an OpenSSL-API-compatible TLS backend, so the
# link line is identical:
#   - MinGW/clang64: the system OpenSSL import libs (ssl crypto).
#   - MSVC: the statically-linked AWS-LC targets (ssl crypto, BoringSSL fork),
#     fetched in 3rdParty/CMakeLists.txt as the boost::asio::ssl backend.
target_link_libraries(DasMaaPi PRIVATE ssl crypto)

if(TARGET DasBundledMaaFrameworkDll)
    get_target_property(_das_maapi_framework_implib DasBundledMaaFrameworkDll IMPORTED_IMPLIB)
    get_filename_component(_das_maapi_framework_lib_dir "${_das_maapi_framework_implib}" DIRECTORY)
    set(_das_maapi_agent_client_implib "${_das_maapi_framework_lib_dir}/MaaAgentClient.lib")
    if(NOT EXISTS "${_das_maapi_agent_client_implib}")
        message(FATAL_ERROR "MaaAgentClient.lib not found next to MaaFramework.lib")
    endif()
    target_link_libraries(DasMaaPi PRIVATE "${_das_maapi_agent_client_implib}")
endif()

# DasMaaPi 需要独立子目录布局（与一堆 MaaFramework 第三方 dll 隔离在
# plugins/DasMaaPi/ 下，由 CppHost 用 altered_search_path 从该目录加载依赖）。
# 必须用 per-config 属性覆盖：父目录 das/Plugins/CMakeLists.txt 设了目录级
# CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>=plugins，它会初始化 target 的
# per-config 属性，而 per-config 属性优先级高于不带后缀的
# RUNTIME_OUTPUT_DIRECTORY，因此只设不带后缀的属性会被压制、产物落回根目录。
foreach(_das_maapi_cfg "DEBUG;Debug" "RELEASE;Release" "RELWITHDEBINFO;RelWithDebInfo" "MINSIZEREL;MinSizeRel")
    list(GET _das_maapi_cfg 0 _das_maapi_suffix)
    list(GET _das_maapi_cfg 1 _das_maapi_dir)
    set_target_properties(DasMaaPi PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${_das_maapi_suffix} "${CMAKE_BINARY_DIR}/bin/${_das_maapi_dir}/plugins/DasMaaPi"
        LIBRARY_OUTPUT_DIRECTORY_${_das_maapi_suffix} "${CMAKE_BINARY_DIR}/bin/${_das_maapi_dir}/plugins/DasMaaPi"
        ARCHIVE_OUTPUT_DIRECTORY_${_das_maapi_suffix} "${CMAKE_BINARY_DIR}/lib/${_das_maapi_dir}/plugins/DasMaaPi")
endforeach()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/DasMaaPi.json
    ${CMAKE_BINARY_DIR}/das/tmp/DasMaaPi.json
    @ONLY)

add_custom_command(
    TARGET DasMaaPi
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_BINARY_DIR}/das/tmp/DasMaaPi.json
        $<TARGET_FILE_DIR:DasMaaPi>/DasMaaPi.json)

# i18n per-locale JSON 翻译表随插件部署到 plugins/DasMaaPi/i18n/，运行期
# 经 boost::dll::this_line_location().parent_path()/i18n 定位（cwd != 插件目录）。
add_custom_command(
    TARGET DasMaaPi
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/i18n
        $<TARGET_FILE_DIR:DasMaaPi>/i18n)

if(NOT DAS_MAAFRAMEWORK_RUNTIME_DLLS)
    message(FATAL_ERROR "DAS_MAAFRAMEWORK_RUNTIME_DLLS is empty; cannot assemble DasMaaPi package")
endif()

foreach(_das_maapi_runtime_dll IN LISTS DAS_MAAFRAMEWORK_RUNTIME_DLLS)
    add_custom_command(
        TARGET DasMaaPi
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_das_maapi_runtime_dll}"
            $<TARGET_FILE_DIR:DasMaaPi>)
endforeach()

if(DAS_BUILD_TEST)
    set(DAS_MAAPI_TEST_PLUGIN_SOURCES ${DAS_MAAPI_SOURCES})
    list(FILTER DAS_MAAPI_TEST_PLUGIN_SOURCES EXCLUDE REGEX "DllMain\.cpp$")

    aux_source_directory(test DAS_MAAPI_TEST_SOURCES)

    add_executable(DasMaaPiTest
        ${DAS_MAAPI_TEST_PLUGIN_SOURCES}
        ${DAS_MAAPI_TEST_SOURCES})
    # DasMaaPiTest 静态编译 AuthoringProjector.cpp，this_line_location 在测试
    # exe 目录解析；i18n 资源需同时部署到测试输出目录，否则投影 i18n 测试
    # 加载不到翻译表。
    add_custom_command(
        TARGET DasMaaPiTest
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/i18n
            $<TARGET_FILE_DIR:DasMaaPiTest>/i18n)
    target_link_libraries(DasMaaPiTest PRIVATE
        Das3rdParty
        GTest::gtest_main
        GTest::gtest
        DasCoreObjects)
    if(WIN32)
        target_link_libraries(DasMaaPiTest PRIVATE bcrypt)
    endif()
    target_link_libraries(DasMaaPiTest PRIVATE ssl crypto)
    target_compile_definitions(DasMaaPiTest PRIVATE
        DAS_MAAPI_DISABLE_REAL_MAA_BOUNDARY)
    # PiHttpClient pulls in boost::asio::ssl + openssl headers; MSVC COFF
    # 32k-section limit needs /bigobj (same as DasMaaPi).
    target_compile_options(DasMaaPiTest PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

    get_target_property(DAS_CORE_INCLUDE_DIRS DasCoreObjects INCLUDE_DIRECTORIES)
    target_include_directories(DasMaaPiTest PRIVATE
        ${DAS_CORE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/das/Core/ForeignInterfaceHost/include)

    # Per-config overrides REQUIRED — parent das/Plugins/CMakeLists.txt:1-15
    # sets directory-level CMAKE_RUNTIME_OUTPUT_DIRECTORY_<CONFIG>=bin/<CONFIG>/plugins
    # which initializes target per-config properties and beats the unsuffixed
    # RUNTIME_OUTPUT_DIRECTORY below. Mirror das_add_test (cmake/DasUtils.cmake:64-68,
    # 93-95) by overriding all 4 per-config properties to ${CMAKE_BINARY_DIR}/Test
    # so the exe lands next to every other test binary and matches the
    # gtest_discover_tests WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test contract.
    foreach(_das_maapi_test_cfg "DEBUG;Debug" "RELEASE;Release" "RELWITHDEBINFO;RelWithDebInfo" "MINSIZEREL;MinSizeRel")
        list(GET _das_maapi_test_cfg 0 _das_maapi_test_suffix)
        set_target_properties(DasMaaPiTest PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_das_maapi_test_suffix} ${CMAKE_BINARY_DIR}/Test)
    endforeach()

    set_target_properties(DasMaaPiTest PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test)

    add_dependencies(DasMaaPiTest
        DasMaaPi
        DasAutoCopyDll
        DasAutoCopyPluginMetadataFile)

    add_custom_command(
        TARGET DasMaaPiTest
        PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures
            ${CMAKE_BINARY_DIR}/Test/DasMaaPi/test/fixtures)

    das_get_test_runtime_environment(das_maapi_test_env)
    gtest_discover_tests(
        DasMaaPiTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        DISCOVERY_TIMEOUT 30
        PROPERTIES
            ENVIRONMENT "${das_maapi_test_env}")
endif()
