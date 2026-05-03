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

        # Patch sol2 v3.3.0 bug: optional<T&>::emplace() calls this->construct()
        # which doesn't exist in the reference specialization.
        # See: https://github.com/ThePhD/sol2/issues/1524
        # Replace the variadic emplace with a single-argument version that
        # directly stores the address of the lvalue reference.
        set(_SOL2_OPT_FILE "${sol2_SOURCE_DIR}/include/sol/optional_implementation.hpp")
        file(READ "${_SOL2_OPT_FILE}" _SOL2_OPT_CONTENT)
        string(REPLACE
            "*this = nullopt;\n\t\t\tthis->construct(std::forward<Args>(args)...);\n\t\t}"
            "*this = nullopt;\n\t\t\tm_value = std::addressof(u);\n\t\t\treturn *m_value;\n\t\t}"
            _SOL2_OPT_CONTENT "${_SOL2_OPT_CONTENT}")
        file(WRITE "${_SOL2_OPT_FILE}" "${_SOL2_OPT_CONTENT}")
        message(STATUS "sol2 optional<T&> emplace patch applied")
    endif()

    add_library(sol2::sol2 INTERFACE IMPORTED)
    target_include_directories(sol2::sol2 INTERFACE ${sol2_SOURCE_DIR}/include)
    message(STATUS "sol2 ${SOL2_VERSION} configured from ${sol2_SOURCE_DIR}")
endif()

# sol2 requires Lua headers — find Lua via CMake or pkg-config
find_package(Lua 5.4 QUIET)
if(NOT Lua_FOUND)
    find_package(Lua 5.3 QUIET)
endif()
if(NOT Lua_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LUA QUIET lua5.4)
        if(NOT LUA_FOUND)
            pkg_check_modules(LUA QUIET lua5.3)
        endif()
    endif()
endif()
if(Lua_FOUND)
    message(STATUS "Lua ${LUA_VERSION_STRING} found: include=${LUA_INCLUDE_DIR}, lib=${LUA_LIBRARIES}")
elseif(LUA_FOUND)
    message(STATUS "Lua found via pkg-config: include=${LUA_INCLUDE_DIRS}, lib=${LUA_LIBRARIES}")
else()
    message(WARNING "Lua not found. Lua export targets may fail to compile (lua.h missing).")
endif()
