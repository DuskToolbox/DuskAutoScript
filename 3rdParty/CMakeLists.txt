include(FetchContent)
include(ExternalProject)


if (POLICY CMP0135)
    # 减少不必要的第三方库下载
    cmake_policy(SET CMP0135 OLD)
endif ()

# config SWIG
include(UseSWIG)

if (POLICY CMP0122)
    cmake_policy(SET CMP0122 NEW)
endif ()

set(DAS_EXPORT_LANGUAGES_LIST "")
set(SWIG_COMPONENTS "")

das_check_language_export(JAVA DAS_EXPORT_LANGUAGES_LIST)
das_check_language_export(CSHARP DAS_EXPORT_LANGUAGES_LIST)
das_check_language_export(PYTHON DAS_EXPORT_LANGUAGES_LIST)

if (NOT DAS_EXPORT_LANGUAGES_LIST)
    message(STATUS "No interface needs to be exported.")
    message(STATUS "Skipping search SWIG executable.")
else ()
    message(STATUS "The project will be exported as an interface to the following languages:\n${DAS_EXPORT_LANGUAGES_LIST}")

    # list(REMOVE_ITEM DAS_EXPORT_LANGUAGES_LIST lua)
    find_package(SWIG 4.1.1 COMPONENTS ${DAS_EXPORT_LANGUAGES_LIST} REQUIRED)

    if (SWIG_FOUND)
        message(STATUS "SWIG found: ${SWIG_EXECUTABLE}")
        set(SWIG_SOURCE_FILE_EXTENSIONS ".i" ".swg")
    endif ()
endif ()

add_custom_target(DasAutoCopyDll ALL)

set(CMAKE_CXX_STANDARD 20)
add_library(Das3rdParty INTERFACE)

message("================================================================")

message(STATUS "The third-party libraries listed later are only used for plugins.")

message(STATUS "Downloading and collecting headers in gzip-hpp")

FetchContent_Declare(
        gzip-hpp
        URL "${GITHUB_MIRROR_URL}/mapbox/gzip-hpp/archive/refs/tags/v0.1.0.tar.gz"
        URL_HASH SHA256=7ce3908cd13f186987820be97083fc5e62a7c6df0877af44b334a92e868eff06)

if (NOT gzip-hpp_POPULATED)
    FetchContent_Populate(gzip-hpp)
endif ()

message("================================================================")

message("================================================================")

message(STATUS "The third-party libraries listed later are globally shared in the project.")

function(_download_and_config_zlib_ng)
    set(CMAKE_FOLDER "Das3rd/zlib_ng")
    # 修改此处版本时，需一并修改../cmake/FindZLIB.cmake的第26行
    set(DAS_ZLIB_NG_VERSION "2.3.2")
    message(STATUS "Downloading and configuring zlib-ng (VERSION ${DAS_ZLIB_NG_VERSION})")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(ZLIB_COMPAT ON)
    set(ZLIB_ENABLE_TESTS OFF)
    set(BUILD_SHARED_LIBS OFF)
    if(MSVC)
        if(NOT CMAKE_C_FLAGS MATCHES "[\\/\\-]utf-8")
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /utf-8")
        endif()
        if(NOT CMAKE_CXX_FLAGS MATCHES "[\\/\\-]utf-8")
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
        endif()
    endif()
    FetchContent_Declare(
            zlib-ng
            URL "${GITHUB_MIRROR_URL}/zlib-ng/zlib-ng/archive/refs/tags/${DAS_ZLIB_NG_VERSION}.tar.gz"
            URL_HASH SHA256=6A0561B50B8F5F6434A6A9E667A67026F2B2064A1FFA959C6B2DAE320161C2A8)
    FetchContent_MakeAvailable(zlib-ng)
    if(MSVC)
        target_compile_options(zlib-ng PUBLIC /wd4142)
    endif()
endfunction()

_download_and_config_zlib_ng()

set(ZLIB_NG_INCLUDE_DIR ${CMAKE_BINARY_DIR}/_deps/zlib-ng-build)
set(ZLIB_NG_LIBRARY zlib)
target_link_libraries(Das3rdParty INTERFACE zlib)

function(_download_and_config_gtest)
    set(DAS_GTESTS_VERSION "1.15.2")
    set(CMAKE_FOLDER "Das3rd/gtest")
    # config GoogleTest
    # GoogleTest requires at least C++14
    message(STATUS "Downloading and configuring google test. VERSION = ${DAS_GTESTS_VERSION}")
    FetchContent_Declare(
        googletest
        URL ${GITHUB_MIRROR_URL}/google/googletest/releases/download/v${DAS_GTESTS_VERSION}/googletest-${DAS_GTESTS_VERSION}.tar.gz
        URL_HASH SHA256=7B42B4D6ED48810C5362C265A17FAEBE90DC2373C885E5216439D37927F02926
    )

    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)
endfunction()

if (${DAS_BUILD_TEST})
    _download_and_config_gtest()
endif ()

message(STATUS "Downloading and collecting headers in tl::expected")
FetchContent_Declare(
        tl_expected
        URL "${GITHUB_MIRROR_URL}/TartanLlama/expected/archive/refs/tags/v1.1.0.zip"
        URL_HASH SHA256=4B2A347CF5450E99F7624247F7D78F86F3ADB5E6ACD33CE307094E9507615B78
)

if (NOT tl_expected_POPULATED)
    FetchContent_Populate(tl_expected)
endif ()

target_include_directories(Das3rdParty INTERFACE ${tl_expected_SOURCE_DIR}/include)

# config magic_enum
message(STATUS "Downloading and collecting headers in magic enum.")
FetchContent_Declare(
        magic_enum
        URL "${GITHUB_MIRROR_URL}/Neargye/magic_enum/archive/refs/tags/v0.9.6.tar.gz"
        URL_HASH SHA256=FCDA8295256A2084F1F98A63B3D2C66B3D7140EEA008E1EF94EA015B2F6D3034
)

if (NOT magic_enum_POPULATED)
    FetchContent_Populate(magic_enum)
endif ()

target_include_directories(Das3rdParty INTERFACE ${magic_enum_SOURCE_DIR}/include/magic_enum)

if (DAS_USE_BUNDLED_FMT)
    set(DAS_FMT_VERSION "12.1.0")
    message(STATUS "Downloading and configuring fmt (VERSION = ${DAS_FMT_VERSION}).")
    FetchContent_Declare(
            Dasfmt
            URL "${GITHUB_MIRROR_URL}/fmtlib/fmt/releases/download/${DAS_FMT_VERSION}/fmt-${DAS_FMT_VERSION}.zip"
            URL_HASH SHA256=EA7DE4299689E12B6DDDD392F9896F08FB0777AC7168897A244A6D6085043FEA)
    FetchContent_MakeAvailable(Dasfmt)
    set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(Das3rdParty INTERFACE fmt::fmt)
    target_compile_definitions(Das3rdParty INTERFACE -DDAS_USE_STD_FMT=0)
else ()
    target_compile_definitions(Das3rdParty INTERFACE -DDAS_USE_STD_FMT=1)
endif ()

function(_download_and_config_spdlog)
    set(CMAKE_FOLDER "Das3rd/spdlog")
    # config spdlog
    set(DAS_SPDLOG_VERSION "1.17.0")
    message(STATUS "Downloading and configuring spdlog. VERSION = ${DAS_SPDLOG_VERSION}")
    FetchContent_Declare(
        spdlog
        URL "${GITHUB_MIRROR_URL}/gabime/spdlog/archive/refs/tags/v${DAS_SPDLOG_VERSION}.tar.gz"
        URL_HASH SHA256=D8862955C6D74E5846B3F580B1605D2428B11D97A410D86E2FB13E857CD3A744)

    set(SPDLOG_BUILD_PIC ON)

    if (DAS_SPDLOG_USE_STD_FMT)
        message(STATUS "spdlog will use std format library.")
        set(SPDLOG_USE_STD_FORMAT ON)
    else ()
        # NOTE: The fmt version should be compatible with the spdlog version.
        # https://github.com/gabime/spdlog/pull/2694
        set(SPDLOG_FMT_EXTERNAL ON)
    endif ()

    if (WIN32)
        set(SPDLOG_WCHAR_SUPPORT ON)
        set(SPDLOG_WCHAR_FILENAMES OFF)
    endif ()

    FetchContent_MakeAvailable(spdlog)
    target_link_libraries(Das3rdParty INTERFACE spdlog::spdlog)
endfunction()

_download_and_config_spdlog()

# for OpenCV compatibility
if (POLICY CMP0148)
    cmake_policy(SET CMP0148 OLD)
endif ()

function(_download_and_config_opencv)
    set(DAS_OPENCV_VERSION "4.10.0")
    message(STATUS "Downloading OpenCV source. VERSION = ${DAS_OPENCV_VERSION}")

    # NOTICE: OpenCV wil turn this option ON automatically, so I explicitly set it.
    set(BUILD_SHARED_LIBS ON)

    include(DasResetLibPrefix)

    FetchContent_Declare(
            opencv
            URL ${GITHUB_MIRROR_URL}/opencv/opencv/archive/refs/tags/${DAS_OPENCV_VERSION}.tar.gz
    )

    if (${DAS_USE_CCACHE})
        set(ENABLE_CCACHE ON)
    endif ()

    # config OpenCV
    set(BUILD_opencv_apps OFF)
    set(BUILD_opencv_calib3d OFF)
    set(BUILD_opencv_dnn OFF)
    set(BUILD_opencv_features2d OFF)
    set(BUILD_opencv_highgui OFF)
    set(BUILD_opencv_gapi OFF)
    set(BUILD_opencv_flann OFF)
    set(BUILD_opencv_ml OFF)
    set(BUILD_opencv_photo OFF)
    set(BUILD_opencv_stitching OFF)
    set(BUILD_opencv_video OFF)
    set(BUILD_opencv_videoio OFF)
    set(BUILD_opencv_world OFF)
    set(WITH_OPENCL ON)
    set(BUILD_opencv_python_bindings_generator OFF)
    set(BUILD_opencv_python_test OFF)
    set(BUILD_opencv_java_bindings_generator OFF)
    set(BUILD_opencv_java_bindings_generator OFF)
    set(BUILD_opencv_js_bindings_generator OFF)
    set(BUILD_opencv_js OFF)
    set(BUILD_opencv_ts OFF)
    set(BUILD_PERF_TESTS OFF)
    set(BUILD_TESTS OFF)
    set(BUILD_ZLIB OFF)

    list(APPEND DAS_OPENCV_COMPONENTS opencv_core opencv_imgcodecs opencv_imgproc)
    foreach (DAS_OCV_LIB ${DAS_OPENCV_COMPONENTS})
        set(BUILD_${DAS_OCV_LIB} ON)
        das_add_auto_copy_dll_path("$<TARGET_FILE:${DAS_OCV_LIB}>")
    endforeach ()

    FetchContent_MakeAvailable(opencv)

    set_target_properties(${DAS_OPENCV_COMPONENTS} PROPERTIES PREFIX ${DAS_LIB_PREFIX})
    target_link_libraries(Das3rdParty INTERFACE ${DAS_OPENCV_COMPONENTS})
    file(COPY
            ${opencv_SOURCE_DIR}/include/opencv2/opencv.hpp
            ${CMAKE_BINARY_DIR}/opencv2/cvconfig.h
            ${CMAKE_BINARY_DIR}/opencv2/opencv_modules.hpp
            DESTINATION  ${CMAKE_BINARY_DIR}/DasOpenCV/opencv2)
    file(COPY
            ${opencv_SOURCE_DIR}/modules/core/include/opencv2
            ${opencv_SOURCE_DIR}/modules/imgcodecs/include/opencv2
            ${opencv_SOURCE_DIR}/modules/imgproc/include/opencv2
            DESTINATION  ${CMAKE_BINARY_DIR}/DasOpenCV)
    target_include_directories(Das3rdParty INTERFACE ${CMAKE_BINARY_DIR}/DasOpenCV)
endfunction()

if (DAS_USE_BUNDLED_OPENCV)
    if (DAS_USE_BUNDLED_OPENCV)
        if (${DAS_USE_BUNDLED_EIGEN})
            message(STATUS "Downloading Eigen3.4...")
            set(BUILD_TESTING OFF)
            set(EIGEN_BUILD_DOC OFF)
            FetchContent_Declare(DasEigen3
                    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip)
            FetchContent_MakeAvailable(DasEigen3)
            set(WITH_EIGEN ON)
        else ()
            find_package(Eigen3 3.4 NO_MODULE)

            if (NOT Eigen3_FOUND)
                set(WITH_EIGEN ON)
            else ()
                set(WITH_EIGEN OFF)
            endif ()
        endif ()

        _download_and_config_opencv()
    else ()
        find_package(OpenCV 4.8)

        if (NOT OpenCV_FOUND)
            target_link_libraries(Das3rdParty INTERFACE ${OpenCV_LIBS})
        else ()
            message(FATAL_ERROR "OpenCV not found. Please install it manually or set DAS_USE_BUNDLED_OPENCV=ON.")
        endif ()
    endif ()

    _download_and_config_opencv()
else ()
    find_package(OpenCV 4.8)

    if (OpenCV_FOUND)
        target_link_libraries(Das3rdParty INTERFACE ${OpenCV_LIBS})
    else ()
        message(FATAL_ERROR "OpenCV not found. Please install it manually or set DAS_USE_BUNDLED_OPENCV=ON.")
    endif ()
endif ()

function(_download_and_config_nv_stdexec)
    set(CMAKE_FOLDER "Das3rd/stdexec")

    # config stdexec
    set(DAS_STDEXEC_VERSION "nvhpc-25.09")
    message(STATUS "Downloading and configuring stdexec (VERSION = ${DAS_STDEXEC_VERSION}).")
    FetchContent_Declare(
        stdexec
        URL "${GITHUB_MIRROR_URL}/NVIDIA/stdexec/archive/refs/tags/${DAS_STDEXEC_VERSION}.tar.gz"
        URL_HASH SHA256=46651430312B1F66A9120DDE3DAA578E9C9EBA709E135D5E508AD5F8F4F18DAB)

    set(STDEXEC_BUILD_EXAMPLES OFF)
    set(STDEXEC_ENABLE_IO_URING_TESTS OFF)
    set(STDEXEC_BUILD_DOCS OFF)
    set(BUILD_TESTING OFF)
    set(STDEXEC_BUILD_TESTS OFF)

    if (POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()

    FetchContent_MakeAvailable(stdexec)
    target_link_libraries(Das3rdParty INTERFACE STDEXEC::stdexec)
endfunction()

_download_and_config_nv_stdexec()

function(_on_download_icu)
    message(STATUS "Downloading prebuilt icu binary...")
endfunction()

if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    set(DAS_ICU_VERSION 73)

    if (${PROCESSOR_ARCHITECTURE} MATCHES "ARM64")
        # arm64
        _on_download_icu()
        FetchContent_Declare(
                icu_msvc_2019_binary_arm64
                URL ${GITHUB_MIRROR_URL}/unicode-org/icu/releases/download/release-73-1/icu4c-73_1-WinARM64-MSVC2019.zip
                CONFIGURE_COMMAND ""
                BUILD_COMMAND ""
                INSTALL_COMMAND "")
        FetchContent_MakeAvailable(icu_msvc_2019_binary_arm64)
        set(ICU_ROOT ${icu_msvc_2019_binary_arm64_SOURCE_DIR})
        set(ICU_DIR ${icu_msvc_2019_binary_arm64_SOURCE_DIR})
        list(APPEND DAS_BUNDLED_ICU_LIBRARIES
                ${icu_msvc_2019_binary_arm64_SOURCE_DIR}/binARM64/icudt${DAS_ICU_VERSION}.dll
                ${icu_msvc_2019_binary_arm64_SOURCE_DIR}/binARM64/icuin${DAS_ICU_VERSION}.dll
                ${icu_msvc_2019_binary_arm64_SOURCE_DIR}/binARM64/icuuc${DAS_ICU_VERSION}.dll)
    else ()
        if (CMAKE_SIZEOF_VOID_P EQUAL 8)
            # 64 bits
            _on_download_icu()
            FetchContent_Declare(
                    icu_msvc_2019_binary_win64
                    URL ${GITHUB_MIRROR_URL}/unicode-org/icu/releases/download/release-73-1/icu4c-73_1-Win64-MSVC2019.zip
                    CONFIGURE_COMMAND ""
                    BUILD_COMMAND ""
                    INSTALL_COMMAND "")
            FetchContent_MakeAvailable(icu_msvc_2019_binary_win64)
            set(ICU_ROOT ${icu_msvc_2019_binary_win64_SOURCE_DIR})
            set(ICU_DIR ${icu_msvc_2019_binary_win64_SOURCE_DIR})
            list(APPEND DAS_BUNDLED_ICU_LIBRARIES
                    ${icu_msvc_2019_binary_win64_SOURCE_DIR}/bin64/icudt${DAS_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win64_SOURCE_DIR}/bin64/icuin${DAS_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win64_SOURCE_DIR}/bin64/icuuc${DAS_ICU_VERSION}.dll)
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
            # 32 bits
            _on_download_icu()
            FetchContent_Declare(
                    icu_msvc_2019_binary_win32
                    URL ${GITHUB_MIRROR_URL}/unicode-org/icu/releases/download/release-73-1/icu4c-73_1-Win32-MSVC2019.zip
                    CONFIGURE_COMMAND ""
                    BUILD_COMMAND ""
                    INSTALL_COMMAND "")
            FetchContent_MakeAvailable(icu_msvc_2019_binary_win32)
            set(ICU_ROOT ${icu_msvc_2019_binary_win32_SOURCE_DIR})
            set(ICU_DIR ${icu_msvc_2019_binary_win32_SOURCE_DIR})
            list(APPEND DAS_BUNDLED_ICU_LIBRARIES
                    ${icu_msvc_2019_binary_win32_SOURCE_DIR}/bin/icudt${DAS_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win32_SOURCE_DIR}/bin/icuin${DAS_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win32_SOURCE_DIR}/bin/icuuc${DAS_ICU_VERSION}.dll)
        endif ()
    endif ()

    das_add_auto_copy_dll_path("${DAS_BUNDLED_ICU_LIBRARIES}")
endif ()

find_package(ICU COMPONENTS data i18n uc REQUIRED)
target_link_libraries(Das3rdParty INTERFACE ICU::data ICU::i18n ICU::uc)

function(_download_and_config_boost)
    set(DAS_BOOST_VERSION 1.90.0)
    message(STATUS "Downloading and configuring boost ${DAS_BOOST_VERSION}.")

    set(CMAKE_FOLDER "Das3rd/boost")

    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_TESTING OFF)

    FetchContent_Declare(
        DasBoost
        URL ${GITHUB_MIRROR_URL}/boostorg/boost/releases/download/boost-${DAS_BOOST_VERSION}/boost-${DAS_BOOST_VERSION}-cmake.7z
        URL_HASH SHA256=218E74C4AA362A994B7B7A23B2920F455A00205C656405FCF262CF60B8871921)

    # See [Symbol redefinition on latest mingw gcc](https://github.com/boostorg/stacktrace/issues/133)
    # You should use boost from msys package manager to avoid this error.
    # set(BOOST_STACKTRACE_ENABLE_WINDBG_CACHED OFF)
    # set(BOOST_STACKTRACE_ENABLE_WINDBG OFF)
    set(BOOST_IOSTREAMS_ENABLE_ZLIB ON)
    set(BOOST_IOSTREAMS_ENABLE_LZMA OFF)
    set(BOOST_IOSTREAMS_ENABLE_ZSTD OFF)

    # Fix error: .../libs/asio/include/boost/asio/detail/impl/winsock_init.ipp:39: undefined reference to `__imp_WSAStartup'
    if (WIN32)
        link_libraries(ws2_32)
    endif ()

    # 指定需要编译的boost组件，包括beast和asio
    set(BOOST_INCLUDE_LIBRARIES
        filesystem
        url
        beast
        asio
        regex_icu
        dll
        predef
        bimap
        container_hash
        pfr
        nowide
        circular_buffer
        signals2
        process
        interprocess
    )

    FetchContent_MakeAvailable(DasBoost)
endfunction()

set(BOOST_FILESYSTEM_NO_DEPRECATED ON)

if (${DAS_USE_BUNDLED_BOOST})
    _download_and_config_boost()
    set(DAS_USE_BUNDLED_BOOST TRUE)
    list(APPEND DAS_BUNDLED_BOOST_LIBS
            Boost::filesystem
            Boost::url
            Boost::beast
            Boost::asio
            Boost::regex_icu
            Boost::dll
            Boost::predef
            Boost::bimap
            Boost::container_hash
            Boost::pfr
            Boost::nowide
            Boost::circular_buffer
            Boost::signals2
            Boost::process
            Boost::interprocess)
else ()
    # 这里先不改，只是自用boost版本拉上去先
    find_package(Boost CONFIG 1.82 COMPONENTS filesystem url beast asio process)

    if (NOT Boost_FOUND)
        message(FATAL_ERROR "Boost not found. Please install it manually or set DAS_USE_BUNDLED_BOOST=ON.")
    else ()
        set(DAS_USE_BUNDLED_BOOST OFF)
    endif ()
endif ()

add_compile_definitions(DasUtils PUBLIC BOOST_FILESYSTEM_NO_DEPRECATED)

# config Nlohmann Json
# Notice: This library assume string is encoded in UTF-8.
set(DAS_NLOHMANN_JSON_VERSION v3.12.0)
message(STATUS "Downloading and collecting headers in nlohmann json. (VERSION = ${DAS_NLOHMANN_JSON_VERSION})")
FetchContent_Declare(
        nlohmann_json
        URL ${GITHUB_MIRROR_URL}/nlohmann/json/releases/download/${DAS_NLOHMANN_JSON_VERSION}/include.zip
        URL_HASH SHA256=B8CB0EF2DD7F57F18933997C9934BB1FA962594F701CD5A8D3C2C80541559372)

if (NOT nlohmann_json_POPULATED)
    FetchContent_Populate(nlohmann_json)
endif ()

message(${nlohmann_json_SOURCE_DIR}/include)
target_include_directories(Das3rdParty INTERFACE ${nlohmann_json_SOURCE_DIR}/include)

# 创建 nlohmann_json::nlohmann_json 目标供其他库使用
add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${nlohmann_json_SOURCE_DIR}/include"
)

# See [added support for cmake "find_package"](https://github.com/microsoft/onnxruntime/pull/8919)
# prevent warning when package not found.
find_package(onnxruntime QUIET)

function(_download_and_config_onnxruntime TARGET_NAME URL)
    FetchContent_Declare(${TARGET_NAME}
            URL ${URL}
            CONFIGURE_COMMAND ""
            BUILD_COMMAND ""
            INSTALL_COMMAND "")
    FetchContent_MakeAvailable(${TARGET_NAME})
    FetchContent_GetProperties(${TARGET_NAME})

    add_library(DasBundledOnnxRuntime INTERFACE IMPORTED)

    add_library(DasBundledOnnxRuntimeDll SHARED IMPORTED)
    find_library(${TARGET_NAME}_DLL onnxruntime "${${TARGET_NAME}_SOURCE_DIR}/lib")
    set_target_properties(
            DasBundledOnnxRuntimeDll
            PROPERTIES
            IMPORTED_CONFIGURATIONS RELEASE
            IMPORTED_LOCATION "${${TARGET_NAME}_DLL}"
            INTERFACE_INCLUDE_DIRECTORIES "${${TARGET_NAME}_SOURCE_DIR}/include"
            INTERFACE_COMPILE_OPTIONS "")
    set_property(
            TARGET DasBundledOnnxRuntime
            APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)

    add_library(DasBundledOnnxRuntimeProvidersSharedDll SHARED IMPORTED)
    find_library(${TARGET_NAME}_ProvidersSharedDLL onnxruntime_providers_shared "${${TARGET_NAME}_SOURCE_DIR}/lib")

    if (WIN32 OR (NOT ${${TARGET_NAME}_ProvidersSharedDLL} STREQUAL "${TARGET_NAME}_ProvidersSharedDLL-NOTFOUND"))
        set_target_properties(
                DasBundledOnnxRuntimeProvidersSharedDll
                PROPERTIES
                IMPORTED_CONFIGURATIONS RELEASE
                IMPORTED_LOCATION "${${TARGET_NAME}_ProvidersSharedDLL}"
                INTERFACE_INCLUDE_DIRECTORIES "${${TARGET_NAME}_SOURCE_DIR}/include"
                INTERFACE_COMPILE_OPTIONS "")
        set_property(
                TARGET DasBundledOnnxRuntime
                APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)

        if (WIN32)
            set_property(
                    TARGET DasBundledOnnxRuntimeDll
                    PROPERTY IMPORTED_IMPLIB "${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime.lib")
            set_property(
                    TARGET DasBundledOnnxRuntimeProvidersSharedDll
                    PROPERTY IMPORTED_IMPLIB "${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime_providers_shared.lib")
        endif ()

        set_property(
                TARGET DasBundledOnnxRuntime
                PROPERTY
                INTERFACE_LINK_LIBRARIES
                DasBundledOnnxRuntimeProvidersSharedDll)
    endif ()

    set_property(
            TARGET DasBundledOnnxRuntime
            PROPERTY
            INTERFACE_LINK_LIBRARIES
            DasBundledOnnxRuntimeDll)

    if (WIN32)
        set(DAS_ORT_DLL_PATH ${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime.dll)
        set(DAS_ORT_RUNTIME_PROVIDER_SHARED_DLL_PATH ${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime_providers_shared.dll)
        list(APPEND DAS_ONNXRUNTIME_DLL_PATH "${DAS_ORT_DLL_PATH}" "${DAS_ORT_RUNTIME_PROVIDER_SHARED_DLL_PATH}")
    else ()
        if (NOT ${${TARGET_NAME}_ProvidersSharedDLL} STREQUAL "${TARGET_NAME}_ProvidersSharedDLL-NOTFOUND")
            list(APPEND DAS_ONNXRUNTIME_DLL_PATH "${${TARGET_NAME}_ProvidersSharedDLL}")
        endif ()

        list(APPEND DAS_ONNXRUNTIME_DLL_PATH "${${TARGET_NAME}_DLL}")
    endif ()

    das_add_auto_copy_dll_path(${DAS_ONNXRUNTIME_DLL_PATH})
endfunction()

if (NOT onnxruntime_FOUND)
    set(DAS_ONNXRUNTIME_BINARY_VERSION "1.23.2")
    message(STATUS "Downloading prebuilt onnxruntime binary (VERSION = ${DAS_ONNXRUNTIME_BINARY_VERSION})...")

    if (WIN32)
        if (${PROCESSOR_ARCHITECTURE} MATCHES "ARM64")
            # arm64
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${DAS_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-arm64-${DAS_ONNXRUNTIME_BINARY_VERSION}.zip")
        elseif (${PROCESSOR_ARCHITECTURE} MATCHES "ARM")
            # arm
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${DAS_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-arm-${DAS_ONNXRUNTIME_BINARY_VERSION}.zip")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
            # amd64
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${DAS_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-x64-${DAS_ONNXRUNTIME_BINARY_VERSION}.zip")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
            # x86
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_x86"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${DAS_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-x86-${DAS_ONNXRUNTIME_BINARY_VERSION}.zip")
        else ()
            message(FATAL_ERROR "Unsupported architecture.")
        endif ()
    elseif (APPLE)
        # TODO
        message(FATAL_ERROR "Not yet supported. PR welcome.")
    elseif (UNIX)
        if (${PROCESSOR_ARCHITECTURE} MATCHES "ARM")
            # arm64
            _download_and_config_onnxruntime(
                    "onnxruntime_linux_arm64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${DAS_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-linux-aarch64-${DAS_ONNXRUNTIME_BINARY_VERSION}.tgz")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
            # amd64
            _download_and_config_onnxruntime(
                    "onnxruntime_linux_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${DAS_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-linux-x64-${DAS_ONNXRUNTIME_BINARY_VERSION}.tgz")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
            # x86
            message(FATAL_ERROR "No official prebuilt binary for this architecture.")
        else ()
            message(FATAL_ERROR "Unsupported architecture.")
        endif ()
    endif ()

    target_link_libraries(Das3rdParty INTERFACE DasBundledOnnxRuntime)
else ()
    target_link_libraries(Das3rdParty INTERFACE onnxruntime::onnxruntime)
endif ()

message("================================================================")

# message(STATUS "The third-party libraries listed later are only used for Plugin.")

# message("================================================================")

# 因为cmake限制，只能在这里写需要copy到测试目录的二进制，后续命令多了就改成文件include一下
das_add_auto_copy_dll_path($<TARGET_FILE:DasGateway>)
