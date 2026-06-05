include(FindPackageHandleStandardArgs)

set(DotNet_EXECUTABLE "" CACHE FILEPATH "Path to the dotnet CLI executable")

set(_DOTNET_CANDIDATES "")

if(DotNet_EXECUTABLE)
    list(APPEND _DOTNET_CANDIDATES "${DotNet_EXECUTABLE}")
endif()

if(DEFINED ENV{DOTNET_EXECUTABLE})
    file(TO_CMAKE_PATH "$ENV{DOTNET_EXECUTABLE}" _DOTNET_ENV_EXECUTABLE)
    list(APPEND _DOTNET_CANDIDATES "${_DOTNET_ENV_EXECUTABLE}")
endif()

if(DEFINED ENV{DOTNET_ROOT})
    file(TO_CMAKE_PATH "$ENV{DOTNET_ROOT}" _DOTNET_ENV_ROOT)
    list(APPEND _DOTNET_CANDIDATES
        "${_DOTNET_ENV_ROOT}/dotnet"
        "${_DOTNET_ENV_ROOT}/dotnet.exe"
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
endif()

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

if(DotNet_FOUND)
    message(STATUS "[FindDotNet] dotnet: ${DotNet_EXECUTABLE}")
    message(STATUS "[FindDotNet] SDK version: ${DotNet_VERSION}")
    message(STATUS "[FindDotNet] net5+: ${DotNet_HAS_NET5}")
    message(STATUS "[FindDotNet] NativeAOT smoke default available: ${DotNet_NATIVEAOT_AVAILABLE}")
endif()
