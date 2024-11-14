set(CMAKE_INCLUDE_CURRENT_DIR ON)

file(GLOB_RECURSE SOURCES src *.cpp)

add_executable(DasHttp ${SOURCES})

target_include_directories(DasHttp PRIVATE src)
target_link_libraries(DasHttp PRIVATE
        DasCore
        DAS_EX_PRIVATE_LIBS
        oatpp)

set_target_properties(DasHttp PROPERTIES
        CXX_STANDARD 20
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
)

