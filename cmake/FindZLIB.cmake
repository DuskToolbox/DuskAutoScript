# reference from CMake official implementation

file(STRINGS "${ZLIB_NG_INCLUDE_DIR}/zlib.h" ZLIB_H REGEX "^#define ZLIB_VERSION \"[^\"]*\"$")

string(REGEX REPLACE "^.*ZLIB_VERSION \"([0-9]+).*$" "\\1" ZLIB_VERSION_MAJOR "${ZLIB_H}")
string(REGEX REPLACE "^.*ZLIB_VERSION \"[0-9]+\\.([0-9]+).*$" "\\1" ZLIB_VERSION_MINOR "${ZLIB_H}")
string(REGEX REPLACE "^.*ZLIB_VERSION \"[0-9]+\\.[0-9]+\\.([0-9]+).*$" "\\1" ZLIB_VERSION_PATCH "${ZLIB_H}")
set(ZLIB_VERSION_STRING "${ZLIB_VERSION_MAJOR}.${ZLIB_VERSION_MINOR}.${ZLIB_VERSION_PATCH}")

# only append a TWEAK version if it exists:
set(ZLIB_VERSION_TWEAK "")

if("${ZLIB_H}" MATCHES "ZLIB_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\\.([0-9]+)")
    set(ZLIB_VERSION_TWEAK "${CMAKE_MATCH_1}")
    string(APPEND ZLIB_VERSION_STRING ".${ZLIB_VERSION_TWEAK}")
endif()

set(ZLIB_MAJOR_VERSION "${ZLIB_VERSION_MAJOR}")
set(ZLIB_MINOR_VERSION "${ZLIB_VERSION_MINOR}")
set(ZLIB_PATCH_VERSION "${ZLIB_VERSION_PATCH}")

set(ZLIB_VERSION ${ZLIB_VERSION_STRING})

message(STATUS "Find zlib-ng version: 2.2.2, which has zlib headers compatible with version ${ZLIB_VERSION} .")

if(NOT ZLIB_VERSION VERSION_EQUAL 1.3.1)
    message(FATAL_ERROR "Unexpected zlib-ng compatible version. Please check if bundled zlib-ng has already upgreade."
    "If so, please update zlib-ng version in this file and cmake/Modules/ZLIBConfig-version.cmake .")
endif()

set(ZLIB_VERSION_STATUS "")

set(ZLIB_INCLUDE_DIRS ${ZLIB_NG_INCLUDE_DIR})
set(ZLIB_INCLUDE_DIR ${ZLIB_NG_INCLUDE_DIR})

if(NOT ZLIB_LIBRARIES)
    set(ZLIB_LIBRARIES ${ZLIB_NG_LIBRARY})
endif()

if(NOT ZLIB_LIBRARY)
    set(ZLIB_LIBRARY ${ZLIB_NG_LIBRARY})
endif()

if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS zlib)
endif()

set(ZLIB_FOUND ON)