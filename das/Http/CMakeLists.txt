set(CMAKE_INCLUDE_CURRENT_DIR ON)

file(GLOB_RECURSE SOURCES src/*)

add_executable(DasHttp ${SOURCES})

target_include_directories(DasHttp PRIVATE src)
target_link_libraries(DasHttp PRIVATE
        DasCore
        DasGateway
        DAS_EX_PRIVATE_LIBS
        Boost::beast
        Boost::asio)

target_compile_definitions(DasHttp PRIVATE _WIN32_WINNT=0x0601)

set_target_properties(DasHttp PROPERTIES
        CXX_STANDARD 20
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
)