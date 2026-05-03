# FindSol2.cmake
# Downloads sol2 header-only library source and creates sol2::sol2 target

include(FetchContent)

set(SOL2_VERSION v3.3.0)

if(NOT TARGET sol2::sol2)
    FetchContent_Declare(
        sol2
        URL ${GITHUB_MIRROR_URL}/ThePhD/sol2/archive/refs/tags/${SOL2_VERSION}.tar.gz
    )

    FetchContent_GetProperties(sol2)
    if(NOT sol2_POPULATED)
        FetchContent_Populate(sol2)
    endif()

    add_library(sol2::sol2 INTERFACE IMPORTED)
    target_include_directories(sol2::sol2 INTERFACE ${sol2_SOURCE_DIR}/include)
    message(STATUS "sol2 ${SOL2_VERSION} configured from ${sol2_SOURCE_DIR}")
endif()
