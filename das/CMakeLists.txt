set(CMAKE_FOLDER "DAS")

add_compile_definitions($<$<CONFIG:Debug>:DEBUG>)

aux_source_directory(Utils/src DAS_UTILS_SOURCES)
file(GLOB_RECURSE Utils/include/* DAS_UTILS_HEADERS)
add_library(DasUtils STATIC ${DAS_UTILS_SOURCES} ${DAS_UTILS_HEADERS})
set_property(TARGET DasUtils PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(DasUtils PUBLIC Utils/include)
target_link_libraries(DasUtils PUBLIC Das3rdParty)

if(WIN32)
    target_compile_definitions(DasUtils PRIVATE DAS_WINDOWS)
endif()

if(DAS_USE_BUNDLED_BOOST)
    target_link_libraries(DasUtils PUBLIC Boost::dll)
else()
    target_include_directories(DasUtils PUBLIC ${Boost_INCLUDE_DIRS})
    target_link_libraries(DasUtils PRIVATE ${Boost_LIBRARIES})
endif()

# Prevent visual studio treat headers as sources.
#file(GLOB_RECURSE DAS_COMMON_HEADERS ${CMAKE_SOURCE_DIR}/include/*) ${DAS_COMMON_HEADERS}
add_library(DasCommonHeaders INTERFACE)
target_include_directories(DasCommonHeaders INTERFACE ${CMAKE_SOURCE_DIR}/das/_shims ${CMAKE_SOURCE_DIR}/include/)
target_link_libraries(DasUtils PUBLIC DasCommonHeaders)

add_library(DAS_EX_PRIVATE_LIBS ALIAS DasUtils)

if(${DAS_BUILD_TEST})
    include(GoogleTest)
    das_add_additional_test(AdditionalTest)
    target_include_directories(AdditionalTest PRIVATE Core/ForeignInterfaceHost/include)
    target_link_libraries(AdditionalTest PUBLIC DasUtils Boost::url)
endif()

# Build lock mechanism to prevent concurrent builds
set(DAS_BUILD_LOCK_FILE "${CMAKE_BINARY_DIR}/das_cmake_build_lock")

# Create lock file target - this will be executed first when any build starts
add_custom_target(DasBuildLockCreate
    COMMAND ${CMAKE_COMMAND} -E touch ${DAS_BUILD_LOCK_FILE}
    COMMENT "Creating build lock file: ${DAS_BUILD_LOCK_FILE}")
set_target_properties(DasBuildLockCreate PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Remove lock file target - this will be executed after all build targets complete
add_custom_target(DasBuildLockRemove
    COMMAND ${CMAKE_COMMAND} -E remove -f ${DAS_BUILD_LOCK_FILE}
    COMMENT "Removing build lock file: ${DAS_BUILD_LOCK_FILE}")
set_target_properties(DasBuildLockRemove PROPERTIES EXCLUDE_FROM_ALL TRUE)

# 未来Gateway将以独立进程的形式来转发请求到具体的DasCore实例上，但是目前，Gateway即为Core
add_subdirectory(Gateway)

add_subdirectory(Core)

add_subdirectory(Plugins)

add_subdirectory(Http)

# Add lock dependencies to all targets that were created after DasBuildLockCreate
# This ensures all build targets depend on creating the lock first
function(add_build_lock_dependencies)
    get_property(subdirs GLOBAL PROPERTY SUBDIRECTORIES)
    foreach(subdir ${subdirs})
        get_property(targets DIRECTORY ${subdir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach(target ${targets})
            if (NOT target MATCHES "^DasBuildLock")
                # Each build target must wait for lock creation
                add_dependencies(${target} DasBuildLockCreate)
                # Lock removal must wait for each build target
                add_dependencies(DasBuildLockRemove ${target})
            endif()
        endforeach()
    endforeach()
endfunction()

# Apply lock dependencies after all targets are defined
add_build_lock_dependencies()
