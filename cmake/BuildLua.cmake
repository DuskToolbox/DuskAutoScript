# BuildLua.cmake
# Builds Lua from source as a static library linked into DasCore.

include(FetchContent)

if(NOT TARGET Das::Lua)
    set(DAS_LUA_VERSION "v5.4.7")
    message(STATUS "Downloading and building Lua ${DAS_LUA_VERSION} from source")

    FetchContent_Declare(
        das_lua
        GIT_REPOSITORY ${GITHUB_MIRROR_URL}/lua/lua.git
        GIT_TAG ${DAS_LUA_VERSION}
        GIT_SHALLOW TRUE
        PATCH_COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_LIST_DIR}/CompileLua.cmake"
            "<SOURCE_DIR>/CMakeLists.txt"
    )

    FetchContent_MakeAvailable(das_lua)

    # Ensure PIC so the static Lua lib can be linked into DasCore.dll
    set_target_properties(DasLua546 PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CMAKE_FOLDER "Das3rd/lua"
    )

    message(STATUS "Lua ${DAS_LUA_VERSION} built as static library (target: Das::Lua)")
endif()
