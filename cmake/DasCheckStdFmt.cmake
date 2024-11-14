set(DAS_OLD_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS})

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(CMAKE_REQUIRED_FLAGS /std:c++20)
else()
    set(CMAKE_REQUIRED_FLAGS -std=c++20)
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <${CMAKE_SOURCE_DIR}/cmake/CppTestSource/StdFormatTest.cpp>"
    DAS_STD_FMT_SUPPORT)

set(CMAKE_REQUIRED_FLAGS ${DAS_OLD_REQUIRED_FLAGS})