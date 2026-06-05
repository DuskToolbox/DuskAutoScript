include(FindPackageHandleStandardArgs)

set(DotNet_EXECUTABLE "" CACHE FILEPATH "Path to the dotnet CLI executable")
set(DotNet_ROOT "" CACHE PATH "Path to the dotnet installation root")
set(DotNet_NATIVE_HOSTING_DIR "" CACHE PATH "Directory containing nethost.h, hostfxr.h, coreclr_delegates.h, and nethost library")

if(DEFINED DotNet_NATIVE_HOSTING_INCLUDE_DIR AND DotNet_NATIVE_HOSTING_INCLUDE_DIR STREQUAL "")
    unset(DotNet_NATIVE_HOSTING_INCLUDE_DIR CACHE)
endif()

if(DEFINED DotNet_NETHOST_LIBRARY AND DotNet_NETHOST_LIBRARY STREQUAL "")
    unset(DotNet_NETHOST_LIBRARY CACHE)
endif()

if(WIN32 AND DEFINED DotNet_NETHOST_LIBRARY AND DotNet_NETHOST_LIBRARY MATCHES "\\.lib$")
    unset(DotNet_NETHOST_LIBRARY CACHE)
endif()

if(NOT DEFINED DotNet_NATIVE_HOST_RID)
    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_DOTNET_DEFAULT_NATIVE_HOST_RID "win-x64")
        else()
            set(_DOTNET_DEFAULT_NATIVE_HOST_RID "win-x86")
        endif()
    elseif(APPLE)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            set(_DOTNET_DEFAULT_NATIVE_HOST_RID "osx-arm64")
        else()
            set(_DOTNET_DEFAULT_NATIVE_HOST_RID "osx-x64")
        endif()
    else()
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
            set(_DOTNET_DEFAULT_NATIVE_HOST_RID "linux-arm64")
        else()
            set(_DOTNET_DEFAULT_NATIVE_HOST_RID "linux-x64")
        endif()
    endif()
    set(DotNet_NATIVE_HOST_RID "${_DOTNET_DEFAULT_NATIVE_HOST_RID}" CACHE STRING "Runtime identifier for .NET native hosting pack discovery")
endif()

set(_DOTNET_CANDIDATES "")
set(_DOTNET_ROOT_CANDIDATES "")

if(DotNet_EXECUTABLE)
    list(APPEND _DOTNET_CANDIDATES "${DotNet_EXECUTABLE}")
endif()

if(DEFINED ENV{DOTNET_EXECUTABLE})
    file(TO_CMAKE_PATH "$ENV{DOTNET_EXECUTABLE}" _DOTNET_ENV_EXECUTABLE)
    list(APPEND _DOTNET_CANDIDATES "${_DOTNET_ENV_EXECUTABLE}")
endif()

if(DEFINED ENV{DOTNET_ROOT})
    file(TO_CMAKE_PATH "$ENV{DOTNET_ROOT}" _DOTNET_ENV_ROOT)
    list(APPEND _DOTNET_ROOT_CANDIDATES "${_DOTNET_ENV_ROOT}")
    list(APPEND _DOTNET_CANDIDATES
        "${_DOTNET_ENV_ROOT}/dotnet"
        "${_DOTNET_ENV_ROOT}/dotnet.exe"
    )
endif()

if(DotNet_ROOT)
    list(APPEND _DOTNET_ROOT_CANDIDATES "${DotNet_ROOT}")
    list(APPEND _DOTNET_CANDIDATES
        "${DotNet_ROOT}/dotnet"
        "${DotNet_ROOT}/dotnet.exe"
    )
endif()

find_program(_DOTNET_FOUND_EXECUTABLE
    NAMES dotnet dotnet.exe
    HINTS ${_DOTNET_CANDIDATES}
    NO_CACHE
)

if(_DOTNET_FOUND_EXECUTABLE)
    set(DotNet_EXECUTABLE "${_DOTNET_FOUND_EXECUTABLE}" CACHE FILEPATH "Path to the dotnet CLI executable" FORCE)
endif()

set(DotNet_INFO "")
set(DotNet_SDKS "")
set(DotNet_VERSION "")
set(DotNet_HAS_NET5 FALSE)
set(DotNet_HAS_NET8 FALSE)
set(DotNet_NATIVEAOT_AVAILABLE FALSE)

if(DotNet_EXECUTABLE)
    get_filename_component(_DOTNET_EXECUTABLE_DIR "${DotNet_EXECUTABLE}" DIRECTORY)
    list(APPEND _DOTNET_ROOT_CANDIDATES "${_DOTNET_EXECUTABLE_DIR}")

    execute_process(
        COMMAND "${DotNet_EXECUTABLE}" --info
        OUTPUT_VARIABLE DotNet_INFO
        ERROR_VARIABLE _DOTNET_INFO_ERROR
        RESULT_VARIABLE _DOTNET_INFO_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    execute_process(
        COMMAND "${DotNet_EXECUTABLE}" --list-sdks
        OUTPUT_VARIABLE DotNet_SDKS
        ERROR_VARIABLE _DOTNET_SDKS_ERROR
        RESULT_VARIABLE _DOTNET_SDKS_RESULT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(_DOTNET_SDKS_RESULT EQUAL 0 AND DotNet_SDKS)
        string(REGEX MATCHALL "[0-9]+\\.[0-9]+\\.[0-9]+" _DOTNET_SDK_VERSIONS "${DotNet_SDKS}")
        if(_DOTNET_SDK_VERSIONS)
            list(GET _DOTNET_SDK_VERSIONS -1 DotNet_VERSION)
        endif()

        foreach(_DOTNET_SDK_VERSION IN LISTS _DOTNET_SDK_VERSIONS)
            if(_DOTNET_SDK_VERSION VERSION_GREATER_EQUAL "5.0.0")
                set(DotNet_HAS_NET5 TRUE)
            endif()
            if(_DOTNET_SDK_VERSION VERSION_GREATER_EQUAL "8.0.0")
                set(DotNet_HAS_NET8 TRUE)
                set(DotNet_NATIVEAOT_AVAILABLE TRUE)
            endif()
        endforeach()
    endif()

    if(_DOTNET_INFO_RESULT EQUAL 0 AND DotNet_INFO)
        string(REGEX MATCH "Base Path:[ \t]*([^\r\n]+)" _DOTNET_BASE_PATH_MATCH "${DotNet_INFO}")
        if(CMAKE_MATCH_1)
            string(STRIP "${CMAKE_MATCH_1}" _DOTNET_BASE_PATH)
            file(TO_CMAKE_PATH "${_DOTNET_BASE_PATH}" _DOTNET_BASE_PATH)
            get_filename_component(_DOTNET_SDK_ROOT "${_DOTNET_BASE_PATH}" DIRECTORY)
            get_filename_component(_DOTNET_TOOL_ROOT "${_DOTNET_SDK_ROOT}" DIRECTORY)
            list(APPEND _DOTNET_ROOT_CANDIDATES "${_DOTNET_TOOL_ROOT}")
        endif()
    endif()
endif()

list(REMOVE_DUPLICATES _DOTNET_ROOT_CANDIDATES)

set(_DOTNET_NATIVE_HOSTING_HINTS "")
if(DotNet_NATIVE_HOSTING_DIR)
    list(APPEND _DOTNET_NATIVE_HOSTING_HINTS "${DotNet_NATIVE_HOSTING_DIR}")
endif()

if(DEFINED ENV{DOTNET_NATIVE_HOSTING_DIR})
    file(TO_CMAKE_PATH "$ENV{DOTNET_NATIVE_HOSTING_DIR}" _DOTNET_ENV_NATIVE_HOSTING_DIR)
    list(APPEND _DOTNET_NATIVE_HOSTING_HINTS "${_DOTNET_ENV_NATIVE_HOSTING_DIR}")
endif()

foreach(_DOTNET_ROOT IN LISTS _DOTNET_ROOT_CANDIDATES)
    if(NOT _DOTNET_ROOT)
        continue()
    endif()

    set(_DOTNET_HOST_PACK_ROOT
        "${_DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.${DotNet_NATIVE_HOST_RID}")
    if(EXISTS "${_DOTNET_HOST_PACK_ROOT}")
        file(GLOB _DOTNET_HOST_PACK_VERSIONS
            LIST_DIRECTORIES TRUE
            "${_DOTNET_HOST_PACK_ROOT}/*")
        list(SORT _DOTNET_HOST_PACK_VERSIONS COMPARE NATURAL ORDER DESCENDING)
        foreach(_DOTNET_HOST_PACK_VERSION IN LISTS _DOTNET_HOST_PACK_VERSIONS)
            list(APPEND _DOTNET_NATIVE_HOSTING_HINTS
                "${_DOTNET_HOST_PACK_VERSION}/runtimes/${DotNet_NATIVE_HOST_RID}/native")
        endforeach()
    endif()
endforeach()

list(REMOVE_DUPLICATES _DOTNET_NATIVE_HOSTING_HINTS)

find_path(DotNet_NATIVE_HOSTING_INCLUDE_DIR
    NAMES nethost.h hostfxr.h coreclr_delegates.h
    HINTS ${_DOTNET_NATIVE_HOSTING_HINTS}
)

find_library(_DotNet_NETHOST_LINK_LIBRARY
    NAMES nethost libnethost
    HINTS ${_DOTNET_NATIVE_HOSTING_HINTS}
)

find_file(DotNet_NETHOST_LIBRARY
    NAMES nethost.dll nethost.lib libnethost.so libnethost.dylib libnethost.a
    HINTS ${_DOTNET_NATIVE_HOSTING_HINTS}
)

find_package_handle_standard_args(DotNetNativeHosting
    REQUIRED_VARS DotNet_NATIVE_HOSTING_INCLUDE_DIR DotNet_NETHOST_LIBRARY
    NAME_MISMATCHED
)

set(DotNet_NATIVE_HOSTING_FOUND ${DotNetNativeHosting_FOUND})

find_package_handle_standard_args(DotNet
    REQUIRED_VARS DotNet_EXECUTABLE DotNet_VERSION
    VERSION_VAR DotNet_VERSION
)

if(DotNet_FOUND AND NOT TARGET DotNet::dotnet)
    add_executable(DotNet::dotnet IMPORTED)
    set_target_properties(DotNet::dotnet PROPERTIES
        IMPORTED_LOCATION "${DotNet_EXECUTABLE}"
    )
endif()

if(DotNet_NATIVE_HOSTING_FOUND AND NOT TARGET DotNet::nethost)
    add_library(DotNet::nethost INTERFACE IMPORTED)
    set_target_properties(DotNet::nethost PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${DotNet_NATIVE_HOSTING_INCLUDE_DIR}"
        INTERFACE_COMPILE_DEFINITIONS "DAS_CSHARP_NETHOST_LIBRARY_PATH=\"${DotNet_NETHOST_LIBRARY}\""
    )
endif()

if(DotNet_FOUND)
    message(STATUS "[FindDotNet] dotnet: ${DotNet_EXECUTABLE}")
    message(STATUS "[FindDotNet] SDK version: ${DotNet_VERSION}")
    message(STATUS "[FindDotNet] net5+: ${DotNet_HAS_NET5}")
    message(STATUS "[FindDotNet] NativeAOT smoke default available: ${DotNet_NATIVEAOT_AVAILABLE}")
    message(STATUS "[FindDotNet] native hosting: ${DotNet_NATIVE_HOSTING_FOUND}")
endif()
