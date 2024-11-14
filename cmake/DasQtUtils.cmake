function(das_add_qrc_shared_lib SUB_DIR_NAME)
    file(GLOB SOURCES ${SUB_DIR_NAME}/src/*)
    add_library(${SUB_DIR_NAME} SHARED ${SOURCES})
    target_include_directories(${SUB_DIR_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_include_directories(${SUB_DIR_NAME} PUBLIC ${SUB_DIR_NAME}/include)
    set_target_properties(${SUB_DIR_NAME} PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden)
    target_link_libraries(${SUB_DIR_NAME} PRIVATE Qt5::Widgets)
    target_compile_definitions(${SUB_DIR_NAME} PRIVATE -DDAS_BUILD_SHARED)
endfunction()

function(das_add_qrc_shared_lib_from_file QRC_FILE_PATH OUTPUT_LIB_LIST)
    get_filename_component(QRC_FILE_NAME ${QRC_FILE_PATH} NAME_WE)
    add_library(${QRC_FILE_NAME} SHARED ${QRC_FILE_PATH})
    target_link_libraries(${QRC_FILE_NAME} PRIVATE Qt::Core)
    set_target_properties(${QRC_FILE_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

    list(APPEND ${QRC_FILE_NAME} ${OUTPUT_LIB_LIST})
    set(${OUTPUT_LIB_LIST} ${${OUTPUT_LIB_LIST}} PARENT_SCOPE)
endfunction()

