set(CMAKE_INCLUDE_CURRENT_DIR ON)

file(GLOB_RECURSE SOURCES src/*)

add_executable(DasHttp ${SOURCES})

target_include_directories(DasHttp PRIVATE src)
target_link_libraries(DasHttp PRIVATE
        DasCore
        DAS_EX_PRIVATE_LIBS
        ${DAS_BUNDLED_BOOST_LIBS})

if(WIN32)
    target_link_libraries(DasHttp PRIVATE mswsock)
endif()

target_compile_definitions(DasHttp PRIVATE _WIN32_WINNT=0x0601
        CPPYYJSON_DEFAULT_TRANSFORM=::yyjson::snake_to_camel_transform)

if(MSVC)
    target_compile_options(DasHttp PRIVATE /bigobj)
endif()

set_target_properties(DasHttp PROPERTIES
        CXX_STANDARD 20
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
)

# 静态图片路径防护单元测试（DasImageController::SanitizeImagePath）
file(GLOB HTTP_TEST_SOURCES test/*.cpp)
if(HTTP_TEST_SOURCES)
    add_executable(HttpTest ${HTTP_TEST_SOURCES})
    # SSE 测试链接 SseHub 实现（SseSession 为 header-only，经 src 直达）
    target_sources(HttpTest PRIVATE src/SseHub.cpp)
    target_include_directories(HttpTest PRIVATE src)
    target_link_libraries(
        HttpTest
        PRIVATE Das3rdParty
                GTest::gtest_main
                GTest::gtest
                DasCoreObjects
                ${DAS_BUNDLED_BOOST_LIBS})
    if(WIN32)
        target_link_libraries(HttpTest PRIVATE mswsock)
    endif()
    target_compile_definitions(HttpTest PRIVATE _WIN32_WINNT=0x0601
            CPPYYJSON_DEFAULT_TRANSFORM=::yyjson::snake_to_camel_transform)
    set_target_properties(
        HttpTest
        PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/Test
            CXX_STANDARD 20
            CXX_EXTENSIONS OFF
            CXX_STANDARD_REQUIRED ON)
    add_dependencies(HttpTest DasAutoCopyDll)
    das_get_test_runtime_environment(das_test_env)
    gtest_discover_tests(
        HttpTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/Test
        PROPERTIES
            ENVIRONMENT "${das_test_env}")
endif()