include(FindPackageHandleStandardArgs)

set(DOTNET_FRAMEWORK_REFERENCE_ASSEMBLIES_DIR "" CACHE PATH "Directory containing net48 reference assemblies")
set(NET48_REFERENCE_ASSEMBLIES_DIR "" CACHE PATH "Directory containing net48 reference assemblies")
set(DotNetFramework_CSC_EXECUTABLE "" CACHE FILEPATH "Path to the C# compiler executable for net48 builds")

set(_NET48_REF_CANDIDATES "")

if(DOTNET_FRAMEWORK_REFERENCE_ASSEMBLIES_DIR)
    list(APPEND _NET48_REF_CANDIDATES "${DOTNET_FRAMEWORK_REFERENCE_ASSEMBLIES_DIR}")
endif()

if(NET48_REFERENCE_ASSEMBLIES_DIR)
    list(APPEND _NET48_REF_CANDIDATES "${NET48_REFERENCE_ASSEMBLIES_DIR}")
endif()

if(DEFINED ENV{DOTNET_FRAMEWORK_REFERENCE_ASSEMBLIES_DIR})
    file(TO_CMAKE_PATH "$ENV{DOTNET_FRAMEWORK_REFERENCE_ASSEMBLIES_DIR}" _NET48_ENV_REF_DIR)
    list(APPEND _NET48_REF_CANDIDATES "${_NET48_ENV_REF_DIR}")
endif()

if(DEFINED ENV{NET48_REFERENCE_ASSEMBLIES_DIR})
    file(TO_CMAKE_PATH "$ENV{NET48_REFERENCE_ASSEMBLIES_DIR}" _NET48_ENV_REF_DIR)
    list(APPEND _NET48_REF_CANDIDATES "${_NET48_ENV_REF_DIR}")
endif()

set(DotNetFramework_NET48_REFERENCE_DIR "")
set(DotNetFramework_NET48_MSCORLIB "")

foreach(_NET48_REF_DIR IN LISTS _NET48_REF_CANDIDATES)
    if(EXISTS "${_NET48_REF_DIR}/mscorlib.dll")
        set(DotNetFramework_NET48_REFERENCE_DIR "${_NET48_REF_DIR}")
        set(DotNetFramework_NET48_MSCORLIB "${_NET48_REF_DIR}/mscorlib.dll")
        break()
    endif()
endforeach()

if(DotNetFramework_CSC_EXECUTABLE)
    set(_DOTNET_FRAMEWORK_CSC_CANDIDATES "${DotNetFramework_CSC_EXECUTABLE}")
else()
    set(_DOTNET_FRAMEWORK_CSC_CANDIDATES "")
endif()

if(DEFINED ENV{DotNetFramework_CSC_EXECUTABLE})
    file(TO_CMAKE_PATH "$ENV{DotNetFramework_CSC_EXECUTABLE}" _DOTNET_FRAMEWORK_ENV_CSC)
    list(APPEND _DOTNET_FRAMEWORK_CSC_CANDIDATES "${_DOTNET_FRAMEWORK_ENV_CSC}")
endif()

find_program(_DOTNET_FRAMEWORK_FOUND_CSC
    NAMES csc csc.exe
    HINTS ${_DOTNET_FRAMEWORK_CSC_CANDIDATES}
    NO_CACHE
)

if(_DOTNET_FRAMEWORK_FOUND_CSC)
    set(DotNetFramework_CSC_EXECUTABLE "${_DOTNET_FRAMEWORK_FOUND_CSC}" CACHE FILEPATH "Path to the C# compiler executable for net48 builds" FORCE)
endif()

find_package_handle_standard_args(DotNetFramework
    REQUIRED_VARS
        DotNetFramework_NET48_REFERENCE_DIR
        DotNetFramework_NET48_MSCORLIB
)

if(DotNetFramework_FOUND)
    message(STATUS "[FindDotNetFramework] net48 refs: ${DotNetFramework_NET48_REFERENCE_DIR}")
    if(DotNetFramework_CSC_EXECUTABLE)
        message(STATUS "[FindDotNetFramework] csc: ${DotNetFramework_CSC_EXECUTABLE}")
    endif()
endif()
