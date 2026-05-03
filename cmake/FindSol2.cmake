# FindSol2.cmake
# Downloads sol2 header-only library and creates sol2::sol2 target

set(SOL2_VERSION v3.3.0)
set(SOL2_URL ${GITHUB_MIRROR_URL}/ThePhD/sol2/releases/download/${SOL2_VERSION})
set(SOL2_DIR ${CMAKE_BINARY_DIR}/_deps/sol2/include/sol)

file(DOWNLOAD ${SOL2_URL}/sol.hpp ${SOL2_DIR}/sol.hpp
     STATUS SOL2_DOWNLOAD_STATUS
     SHOW_PROGRESS)

list(GET SOL2_DOWNLOAD_STATUS 0 SOL2_DOWNLOAD_STATUS_CODE)
if(NOT SOL2_DOWNLOAD_STATUS_CODE EQUAL 0)
    message(FATAL_ERROR "Failed to download sol2: ${SOL2_DOWNLOAD_STATUS}")
endif()

if(NOT TARGET sol2::sol2)
    add_library(sol2::sol2 INTERFACE IMPORTED)
    target_include_directories(sol2::sol2 INTERFACE ${SOL2_DIR}/..)
    message(STATUS "sol2 ${SOL2_VERSION} downloaded to ${SOL2_DIR}")
endif()
