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

set(ASR_EXPORT_LANGUAGES_LIST "")
set(SWIG_COMPONENTS "")

asr_check_language_export(JAVA ASR_EXPORT_LANGUAGES_LIST)
asr_check_language_export(CSHARP ASR_EXPORT_LANGUAGES_LIST)
asr_check_language_export(PYTHON ASR_EXPORT_LANGUAGES_LIST)

if (NOT ASR_EXPORT_LANGUAGES_LIST)
    message(STATUS "No interface needs to be exported.")
    message(STATUS "Skipping search SWIG executable.")
else ()
    message(STATUS "The project will be exported as an interface to the following languages:\n${ASR_EXPORT_LANGUAGES_LIST}")

    # list(REMOVE_ITEM ASR_EXPORT_LANGUAGES_LIST lua)
    find_package(SWIG 4.1.1 COMPONENTS ${ASR_EXPORT_LANGUAGES_LIST} REQUIRED)

    if (SWIG_FOUND)
        message(STATUS "SWIG found: ${SWIG_EXECUTABLE}")
        set(SWIG_SOURCE_FILE_EXTENSIONS ".i" ".swg")
    endif ()
endif ()

add_custom_target(AsrAutoCopyDll ALL)

set(CMAKE_CXX_STANDARD 20)
add_library(Asr3rdParty INTERFACE)

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
    set(CMAKE_FOLDER "Asr3rd/zlib_ng")
    # 修改此处版本时，需一并修改../cmake/FindZLIB.cmake的第26行
    set(ASR_ZLIB_NG_VERSION "2.1.6")
    message(STATUS "Downloading and configuring zlib-ng (VERSION ${ASR_ZLIB_NG_VERSION})")
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(ZLIB_COMPAT ON)
    set(ZLIB_ENABLE_TESTS OFF)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(
            zlib-ng
            URL "${GITHUB_MIRROR_URL}/zlib-ng/zlib-ng/archive/refs/tags/${ASR_ZLIB_NG_VERSION}.tar.gz"
            URL_HASH SHA256=A5D504C0D52E2E2721E7E7D86988DEC2E290D723CED2307145DEDD06AEB6FEF2)
    FetchContent_MakeAvailable(zlib-ng)
endfunction()

_download_and_config_zlib_ng()

set(ZLIB_NG_INCLUDE_DIR ${CMAKE_BINARY_DIR}/_deps/zlib-ng-build)
set(ZLIB_NG_LIBRARY zlib)

function(_download_and_config_gtest)
    set(CMAKE_FOLDER "Asr3rd/gtest")
    # config GoogleTest
    # GoogleTest requires at least C++14
    message(STATUS "Downloading and configuring google test.")
    FetchContent_Declare(
        googletest
        URL ${GITHUB_MIRROR_URL}/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        URL_HASH SHA256=8AD598C73AD796E0D8280B082CEBD82A630D73E73CD3C70057938A6501BBA5D7
    )

    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)
endfunction()

if (${ASR_BUILD_TEST})
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

target_include_directories(Asr3rdParty INTERFACE ${tl_expected_SOURCE_DIR}/include)

# config magic_enum
message(STATUS "Downloading and collecting headers in magic enum.")
FetchContent_Declare(
        magic_enum
        URL "${GITHUB_MIRROR_URL}/Neargye/magic_enum/archive/refs/tags/v0.9.3.tar.gz"
        URL_HASH SHA256=3CADD6A05F1BFFC5141E5E731C46B2B73C2DBFF025E723C8ABAA659E0A24F072
)

if (NOT magic_enum_POPULATED)
    FetchContent_Populate(magic_enum)
endif ()

target_include_directories(Asr3rdParty INTERFACE ${magic_enum_SOURCE_DIR}/include)

if (ASR_USE_BUNDLED_FMT)
    set(ASR_FMT_VERSION "10.1.1")
    message(STATUS "Downloading and configuring fmt (VERSION = ${ASR_FMT_VERSION}).")
    FetchContent_Declare(
            Asrfmt
            URL "${GITHUB_MIRROR_URL}/fmtlib/fmt/releases/download/${ASR_FMT_VERSION}/fmt-${ASR_FMT_VERSION}.zip"
            URL_HASH SHA256=B84E58A310C9B50196CDA48D5678D5FA0849BCA19E5FDBA6B684F0EE93ED9D1B)
    FetchContent_MakeAvailable(Asrfmt)
    set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(Asr3rdParty INTERFACE fmt::fmt)
    target_compile_definitions(Asr3rdParty INTERFACE -DASR_USE_STD_FMT=0)
else ()
    target_compile_definitions(Asr3rdParty INTERFACE -DASR_USE_STD_FMT=1)
endif ()

function(_download_and_config_spdlog)
    set(CMAKE_FOLDER "Asr3rd/spdlog")
    # config spdlog
    message(STATUS "Downloading and configuring spdlog.")
    FetchContent_Declare(
        spdlog
        URL "${GITHUB_MIRROR_URL}/gabime/spdlog/archive/refs/tags/v1.12.0.zip"
        URL_HASH SHA256=6174BF8885287422A6C6A0312EB8A30E8D22BCFCEE7C48A6D02D1835D7769232)

    set(SPDLOG_BUILD_PIC ON)

    if (ASR_SPDLOG_USE_STD_FMT)
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
    target_link_libraries(Asr3rdParty INTERFACE spdlog::spdlog)
endfunction()

_download_and_config_spdlog()

# for OpenCV compatibility
if (POLICY CMP0148)
    cmake_policy(SET CMP0148 OLD)
endif ()

function(_download_and_config_opencv)
    message(STATUS "Downloading OpenCV source.")

    # NOTICE: OpenCV wil turn this option ON automatically, so I explicitly set it.
    set(BUILD_SHARED_LIBS ON)

    include(AsrResetLibPrefix)

    FetchContent_Declare(
            opencv
            URL ${GITHUB_MIRROR_URL}/opencv/opencv/archive/refs/tags/4.8.1.tar.gz
    )

    if (${ASR_USE_CCACHE})
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

    list(APPEND ASR_OPENCV_COMPONENTS opencv_core opencv_imgcodecs opencv_imgproc)
    foreach (ASR_OCV_LIB ${ASR_OPENCV_COMPONENTS})
        set(BUILD_${ASR_OCV_LIB} ON)
        asr_add_auto_copy_dll_path("$<TARGET_FILE:${ASR_OCV_LIB}>")
    endforeach ()

    FetchContent_MakeAvailable(opencv)

    set_target_properties(${ASR_OPENCV_COMPONENTS} PROPERTIES PREFIX ${ASR_LIB_PREFIX})
    target_link_libraries(Asr3rdParty INTERFACE ${ASR_OPENCV_COMPONENTS})
    file(COPY
            ${opencv_SOURCE_DIR}/include/opencv2/opencv.hpp
            ${CMAKE_BINARY_DIR}/opencv2/cvconfig.h
            ${CMAKE_BINARY_DIR}/opencv2/opencv_modules.hpp
            DESTINATION  ${CMAKE_BINARY_DIR}/AsrOpenCV/opencv2)
    file(COPY
            ${opencv_SOURCE_DIR}/modules/core/include/opencv2
            ${opencv_SOURCE_DIR}/modules/imgcodecs/include/opencv2
            ${opencv_SOURCE_DIR}/modules/imgproc/include/opencv2
            DESTINATION  ${CMAKE_BINARY_DIR}/AsrOpenCV)
    target_include_directories(Asr3rdParty INTERFACE ${CMAKE_BINARY_DIR}/AsrOpenCV)
endfunction()

if (ASR_USE_BUNDLED_OPENCV)
    if (ASR_USE_BUNDLED_OPENCV)
        if (${ASR_USE_BUNDLED_EIGEN})
            message(STATUS "Downloading Eigen3.4...")
            set(BUILD_TESTING OFF)
            set(EIGEN_BUILD_DOC OFF)
            FetchContent_Declare(AsrEigen3
                    URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.zip)
            FetchContent_MakeAvailable(AsrEigen3)
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
            target_link_libraries(Asr3rdParty INTERFACE ${OpenCV_LIBS})
        else ()
            message(FATAL_ERROR "OpenCV not found. Please install it manually or set ASR_USE_BUNDLED_OPENCV=ON.")
        endif ()
    endif ()

    _download_and_config_opencv()
else ()
    find_package(OpenCV 4.8)

    if (OpenCV_FOUND)
        target_link_libraries(Asr3rdParty INTERFACE ${OpenCV_LIBS})
    else ()
        message(FATAL_ERROR "OpenCV not found. Please install it manually or set ASR_USE_BUNDLED_OPENCV=ON.")
    endif ()
endif ()

function(_on_download_icu)
    message(STATUS "Downloading prebuilt icu binary...")
endfunction()

if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
    set(ASR_ICU_VERSION 73)

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
        list(APPEND ASR_BUNDLED_ICU_LIBRARIES
                ${icu_msvc_2019_binary_arm64_SOURCE_DIR}/binARM64/icudt${ASR_ICU_VERSION}.dll
                ${icu_msvc_2019_binary_arm64_SOURCE_DIR}/binARM64/icuin${ASR_ICU_VERSION}.dll
                ${icu_msvc_2019_binary_arm64_SOURCE_DIR}/binARM64/icuuc${ASR_ICU_VERSION}.dll)
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
            list(APPEND ASR_BUNDLED_ICU_LIBRARIES
                    ${icu_msvc_2019_binary_win64_SOURCE_DIR}/bin64/icudt${ASR_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win64_SOURCE_DIR}/bin64/icuin${ASR_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win64_SOURCE_DIR}/bin64/icuuc${ASR_ICU_VERSION}.dll)
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
            list(APPEND ASR_BUNDLED_ICU_LIBRARIES
                    ${icu_msvc_2019_binary_win32_SOURCE_DIR}/bin/icudt${ASR_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win32_SOURCE_DIR}/bin/icuin${ASR_ICU_VERSION}.dll
                    ${icu_msvc_2019_binary_win32_SOURCE_DIR}/bin/icuuc${ASR_ICU_VERSION}.dll)
        endif ()
    endif ()

    asr_add_auto_copy_dll_path("${ASR_BUNDLED_ICU_LIBRARIES}")
endif ()

find_package(ICU COMPONENTS data i18n uc REQUIRED)
target_link_libraries(Asr3rdParty INTERFACE ICU::data ICU::i18n ICU::uc)

function(_download_and_config_boost)
    message(STATUS "Downloading and configuring boost 1.84.")

    set(CMAKE_FOLDER "Asr3rd/boost")

    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(BUILD_TESTING OFF)

    FetchContent_Declare(
        AsrBoost
        URL ${GITHUB_MIRROR_URL}/boostorg/boost/releases/download/boost-1.84.0/boost-1.84.0.7z
        URL_HASH SHA256=CE132F85FC706AA8B5C7E6E52A8338DE33A688E92E7C8FADA3713194B109232E
        PATCH_COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/cmake/AsrBoostContext184Patch.cmake" <SOURCE_DIR>/libs/context/CMakeLists.txt)

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

    FetchContent_MakeAvailable(AsrBoost)
endfunction()

set(BOOST_FILESYSTEM_NO_DEPRECATED ON)

if (${ASR_USE_BUNDLED_BOOST})
    _download_and_config_boost()
    set(ASR_USE_BUNDLED_BOOST TRUE)
    list(APPEND ASR_BUNDLED_BOOST_LIBS
            Boost::regex_icu
            Boost::dll
            Boost::predef
            Boost::bimap
            Boost::container_hash
            Boost::pfr
            Boost::nowide
            Boost::circular_buffer
            Boost::signals2)
else ()
    find_package(Boost 1.82 COMPONENTS filesystem url)

    if (NOT Boost_FOUND)
        message(FATAL_ERROR "Boost not found. Please install it manually or set ASR_USE_BUNDLED_BOOST=ON.")
    else ()
        set(ASR_USE_BUNDLED_BOOST OFF)
    endif ()
endif ()

add_compile_definitions(AsrUtils PUBLIC BOOST_FILESYSTEM_NO_DEPRECATED)

# config Nlohmann Json
# Notice: This library assume string is encoded in UTF-8.
set(ASR_NLOHMANN_JSON_VERSION v3.11.3)
message(STATUS "Downloading and collecting headers in nlohmann json. (VERSION = ${ASR_NLOHMANN_JSON_VERSION})")
FetchContent_Declare(
        nlohmann_json
        URL ${GITHUB_MIRROR_URL}/nlohmann/json/releases/download/${ASR_NLOHMANN_JSON_VERSION}/include.zip
        URL_HASH SHA256=A22461D13119AC5C78F205D3DF1DB13403E58CE1BB1794EDC9313677313F4A9D)

if (NOT nlohmann_json_POPULATED)
    FetchContent_Populate(nlohmann_json)
endif ()

message(${nlohmann_json_SOURCE_DIR}/include)
target_include_directories(Asr3rdParty INTERFACE ${nlohmann_json_SOURCE_DIR}/include)

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

    add_library(AsrBundledOnnxRuntime INTERFACE IMPORTED)

    add_library(AsrBundledOnnxRuntimeDll SHARED IMPORTED)
    find_library(${TARGET_NAME}_DLL onnxruntime "${${TARGET_NAME}_SOURCE_DIR}/lib")
    set_target_properties(
            AsrBundledOnnxRuntimeDll
            PROPERTIES
            IMPORTED_CONFIGURATIONS RELEASE
            IMPORTED_LOCATION "${${TARGET_NAME}_DLL}"
            INTERFACE_INCLUDE_DIRECTORIES "${${TARGET_NAME}_SOURCE_DIR}/include"
            INTERFACE_COMPILE_OPTIONS "")
    set_property(
            TARGET AsrBundledOnnxRuntime
            APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)

    add_library(AsrBundledOnnxRuntimeProvidersSharedDll SHARED IMPORTED)
    find_library(${TARGET_NAME}_ProvidersSharedDLL onnxruntime_providers_shared "${${TARGET_NAME}_SOURCE_DIR}/lib")

    if (WIN32 OR (NOT ${${TARGET_NAME}_ProvidersSharedDLL} STREQUAL "${TARGET_NAME}_ProvidersSharedDLL-NOTFOUND"))
        set_target_properties(
                AsrBundledOnnxRuntimeProvidersSharedDll
                PROPERTIES
                IMPORTED_CONFIGURATIONS RELEASE
                IMPORTED_LOCATION "${${TARGET_NAME}_ProvidersSharedDLL}"
                INTERFACE_INCLUDE_DIRECTORIES "${${TARGET_NAME}_SOURCE_DIR}/include"
                INTERFACE_COMPILE_OPTIONS "")
        set_property(
                TARGET AsrBundledOnnxRuntime
                APPEND PROPERTY IMPORTED_CONFIGURATIONS RELWITHDEBINFO)

        if (WIN32)
            set_property(
                    TARGET AsrBundledOnnxRuntimeDll
                    PROPERTY IMPORTED_IMPLIB "${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime.lib")
            set_property(
                    TARGET AsrBundledOnnxRuntimeProvidersSharedDll
                    PROPERTY IMPORTED_IMPLIB "${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime_providers_shared.lib")
        endif ()

        set_property(
                TARGET AsrBundledOnnxRuntime
                PROPERTY
                INTERFACE_LINK_LIBRARIES
                AsrBundledOnnxRuntimeProvidersSharedDll)
    endif ()

    set_property(
            TARGET AsrBundledOnnxRuntime
            PROPERTY
            INTERFACE_LINK_LIBRARIES
            AsrBundledOnnxRuntimeDll)

    if (WIN32)
        set(ASR_ORT_DLL_PATH ${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime.dll)
        set(ASR_ORT_RUNTIME_PROVIDER_SHARED_DLL_PATH ${${TARGET_NAME}_SOURCE_DIR}/lib/onnxruntime_providers_shared.dll)
        list(APPEND ASR_ONNXRUNTIME_DLL_PATH "${ASR_ORT_DLL_PATH}" "${ASR_ORT_RUNTIME_PROVIDER_SHARED_DLL_PATH}")
    else ()
        if (NOT ${${TARGET_NAME}_ProvidersSharedDLL} STREQUAL "${TARGET_NAME}_ProvidersSharedDLL-NOTFOUND")
            list(APPEND ASR_ONNXRUNTIME_DLL_PATH "${${TARGET_NAME}_ProvidersSharedDLL}")
        endif ()

        list(APPEND ASR_ONNXRUNTIME_DLL_PATH "${${TARGET_NAME}_DLL}")
    endif ()

    asr_add_auto_copy_dll_path(${ASR_ONNXRUNTIME_DLL_PATH})
endfunction()

if (NOT onnxruntime_FOUND)
    set(ASR_ONNXRUNTIME_BINARY_VERSION "1.16.1")
    message(STATUS "Downloading prebuilt onnxruntime binary (VERSION = ${ASR_ONNXRUNTIME_BINARY_VERSION})...")

    if (WIN32)
        if (${PROCESSOR_ARCHITECTURE} MATCHES "ARM64")
            # arm64
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${ASR_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-arm64-${ASR_ONNXRUNTIME_BINARY_VERSION}.zip")
        elseif (${PROCESSOR_ARCHITECTURE} MATCHES "ARM")
            # arm
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${ASR_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-arm-${ASR_ONNXRUNTIME_BINARY_VERSION}.zip")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
            # amd64
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${ASR_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-x64-${ASR_ONNXRUNTIME_BINARY_VERSION}.zip")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
            # x86
            _download_and_config_onnxruntime(
                    "onnxruntime_msvc_x86"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${ASR_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-win-x86-${ASR_ONNXRUNTIME_BINARY_VERSION}.zip")
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
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${ASR_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-linux-aarch64-${ASR_ONNXRUNTIME_BINARY_VERSION}.tgz")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
            # amd64
            _download_and_config_onnxruntime(
                    "onnxruntime_linux_amd64"
                    "${GITHUB_MIRROR_URL}/microsoft/onnxruntime/releases/download/v${ASR_ONNXRUNTIME_BINARY_VERSION}/onnxruntime-linux-x64-${ASR_ONNXRUNTIME_BINARY_VERSION}.tgz")
        elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
            # x86
            message(FATAL_ERROR "No official prebuilt binary for this architecture.")
        else ()
            message(FATAL_ERROR "Unsupported architecture.")
        endif ()
    endif ()

    target_link_libraries(Asr3rdParty INTERFACE AsrBundledOnnxRuntime)
else ()
    target_link_libraries(Asr3rdParty INTERFACE onnxruntime::onnxruntime)
endif ()

message("================================================================")

message(STATUS "The third-party libraries listed later are only used for AsrHttp.")

message(STATUS "Downloading and configuring oatpp 1.3.0.")

function(_download_and_config_oatpp)
    set(CMAKE_FOLDER "Asr3rd/oatpp")

    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

    set(OATPP_BUILD_TESTS OFF)

    FetchContent_Declare(
        oatpp
        URL ${GITHUB_MIRROR_URL}/oatpp/oatpp/archive/refs/tags/1.3.0.tar.gz
    )

    FetchContent_MakeAvailable(oatpp)
endfunction()

_download_and_config_oatpp()

function(_download_and_config_drogon)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

    set(CMAKE_FOLDER "Asr3rd/jsoncpp")

    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_OBJECT_LIBS OFF)

    FetchContent_Declare(
        jsoncpp
        URL ${GITHUB_MIRROR_URL}/open-source-parsers/jsoncpp/archive/refs/tags/1.9.5.tar.gz
        OVERRIDE_FIND_PACKAGE
    )

    FetchContent_MakeAvailable(jsoncpp)

    include_directories(${CMAKE_BINARY_DIR}/_deps/jsoncpp-src/include)
    add_library(Jsoncpp_lib ALIAS jsoncpp_static)

    set(CMAKE_FOLDER "Asr3rd/trantor")

    set(USE_SPDLOG ON)
    set(BUILD_TESTING OFF)

    FetchContent_Declare(
        trantor
        URL ${GITHUB_MIRROR_URL}/an-tao/trantor/archive/refs/tags/v1.5.18.tar.gz
        OVERRIDE_FIND_PACKAGE
    )

    FetchContent_MakeAvailable(trantor)

    add_library(Trantor::Trantor ALIAS trantor)

    set(CMAKE_FOLDER "Asr3rd/drogon")

    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

    set(Trantor_DIR ${CMAKE_BINARY_DIR}/_deps/drogon-build)

    asr_force_set(USE_SUBMODULE OFF BOOL "Disallow drogon use trantor as submodule")
    set(BUILD_EXAMPLES OFF)

    FetchContent_Declare(
        drogon
        URL ${GITHUB_MIRROR_URL}/drogonframework/drogon/archive/refs/tags/v1.9.4.tar.gz
    )

    FetchContent_MakeAvailable(drogon)
endfunction()

_download_and_config_drogon()

message("================================================================")
