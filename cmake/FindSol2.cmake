# FindSol2.cmake
# Downloads sol2 header-only library source and applies required patches.

include(FetchContent)

set(SOL2_VERSION v3.3.0)

if(NOT TARGET sol2::sol2)
    FetchContent_Declare(
        sol2
        GIT_REPOSITORY ${GITHUB_MIRROR_URL}/ThePhD/sol2.git
        GIT_TAG ${SOL2_VERSION}
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(sol2)
    if(NOT sol2_POPULATED)
        FetchContent_Populate(sol2)
    endif()

    # Patch sol2 v3.3.0 bug: optional<T&>::emplace() calls this->construct()
    # which doesn't exist in the reference specialization.
    # See: https://github.com/ThePhD/sol2/issues/1524
    # Always check and apply patch (even if source was cached from a previous run)
    set(_SOL2_OPT_FILE "${sol2_SOURCE_DIR}/include/sol/optional_implementation.hpp")
    if(EXISTS "${_SOL2_OPT_FILE}")
        file(READ "${_SOL2_OPT_FILE}" _SOL2_OPT_CONTENT)
        string(FIND "${_SOL2_OPT_CONTENT}" "T ref(static_cast<T>(args)...)" _PATCH_ALREADY)
        if(_PATCH_ALREADY LESS 0)
            string(REPLACE
                "*this = nullopt;\n\t\t\tthis->construct(std::forward<Args>(args)...);\n\t\t}"
                "*this = nullopt;\n\t\t\t// For T& specialization: args must be a single lvalue reference.\n\t\t\tT ref(static_cast<T>(args)...);\n\t\t\tm_value = std::addressof(ref);\n\t\t\treturn *m_value;\n\t\t}"
                _SOL2_OPT_CONTENT "${_SOL2_OPT_CONTENT}")
            file(WRITE "${_SOL2_OPT_FILE}" "${_SOL2_OPT_CONTENT}")
            message(STATUS "sol2 optional<T&> emplace patch applied")
        else()
            message(STATUS "sol2 optional<T&> emplace patch already applied")
        endif()
    endif()

    add_library(sol2::sol2 INTERFACE IMPORTED)
    target_include_directories(sol2::sol2 INTERFACE ${sol2_SOURCE_DIR}/include)
    message(STATUS "sol2 ${SOL2_VERSION} configured from ${sol2_SOURCE_DIR}")
endif()
