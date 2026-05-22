include(FetchContent)
include(FindPackageHandleStandardArgs)

set(NODE_API_INCLUDE_DIR "" CACHE PATH "Directory containing node_api.h")
set(NODE_API_LIBRARY "" CACHE FILEPATH "Path to node.lib when building Node addons on Windows")
set(NODE_ADDON_API_INCLUDE_DIR "" CACHE PATH "Directory containing napi.h from node-addon-api")

set(_NODE_ADDON_API_URL "https://github.com/nodejs/node-addon-api/archive/refs/tags/v8.7.0.tar.gz")
set(_NODE_ADDON_API_SHA256 "C0DACE4E1D4AE280ACCA09CEA8BBE7AF20683AC226844D834B5F34A0ABD78A89")
set(_NODE_HEADERS_URL "https://nodejs.org/download/release/v18.20.8/node-v18.20.8-headers.tar.gz")
set(_NODE_HEADERS_SHA256 "10EF9E563840B5BE0F291A114AFB726523178EE5F51D23BF532D28F22C23BCE9")
set(_NODE_WIN_X64_LIB_URL "https://nodejs.org/download/release/v18.20.8/win-x64/node.lib")
set(_NODE_WIN_X64_LIB_SHA256 "64D93225AAECE04E3CD45177D6DEA2B22DF49E127281FEFA3ADE43AC46A36CC6")

function(_node_api_require_file path cache_name guidance)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "[FindNodeApi] ${path} does not exist. "
            "Set ${cache_name} to ${guidance}.")
    endif()
endfunction()

if(NODE_API_INCLUDE_DIR)
    set(_NODE_API_INCLUDE_DIR "${NODE_API_INCLUDE_DIR}")
else()
    FetchContent_Declare(
        NodeApiHeaders
        URL "${_NODE_HEADERS_URL}"
        URL_HASH "SHA256=${_NODE_HEADERS_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(NodeApiHeaders)
    if(NOT nodeapiheaders_POPULATED)
        FetchContent_Populate(NodeApiHeaders)
    endif()
    set(_NODE_API_INCLUDE_DIR "${nodeapiheaders_SOURCE_DIR}/include/node")
endif()
_node_api_require_file(
    "${_NODE_API_INCLUDE_DIR}/node_api.h"
    "NODE_API_INCLUDE_DIR"
    "a directory containing node_api.h"
)

if(NODE_ADDON_API_INCLUDE_DIR)
    set(_NODE_ADDON_API_INCLUDE_DIR "${NODE_ADDON_API_INCLUDE_DIR}")
else()
    FetchContent_Declare(
        NodeAddonApi
        URL "${_NODE_ADDON_API_URL}"
        URL_HASH "SHA256=${_NODE_ADDON_API_SHA256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(NodeAddonApi)
    if(NOT nodeaddonapi_POPULATED)
        FetchContent_Populate(NodeAddonApi)
    endif()
    set(_NODE_ADDON_API_INCLUDE_DIR "${nodeaddonapi_SOURCE_DIR}")
endif()
_node_api_require_file(
    "${_NODE_ADDON_API_INCLUDE_DIR}/napi.h"
    "NODE_ADDON_API_INCLUDE_DIR"
    "a directory containing napi.h"
)

if(WIN32)
    if(NODE_API_LIBRARY)
        set(_NODE_API_LIBRARY "${NODE_API_LIBRARY}")
    else()
        if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR
                "[FindNodeApi] automatic Node import library download is only "
                "provided for Windows x64. Set NODE_API_LIBRARY explicitly.")
        endif()

        set(_NODE_API_LIBRARY "${CMAKE_BINARY_DIR}/_deps/nodeapi-node-lib/win-x64/node.lib")
        if(NOT EXISTS "${_NODE_API_LIBRARY}")
            file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/nodeapi-node-lib/win-x64")
            file(
                DOWNLOAD
                    "${_NODE_WIN_X64_LIB_URL}"
                    "${_NODE_API_LIBRARY}"
                EXPECTED_HASH "SHA256=${_NODE_WIN_X64_LIB_SHA256}"
                STATUS _NODE_API_LIBRARY_DOWNLOAD_STATUS
                TLS_VERIFY ON
            )
            list(GET _NODE_API_LIBRARY_DOWNLOAD_STATUS 0 _NODE_API_LIBRARY_DOWNLOAD_CODE)
            list(GET _NODE_API_LIBRARY_DOWNLOAD_STATUS 1 _NODE_API_LIBRARY_DOWNLOAD_MESSAGE)
            if(NOT _NODE_API_LIBRARY_DOWNLOAD_CODE EQUAL 0)
                message(FATAL_ERROR
                    "[FindNodeApi] failed to download Node Windows x64 import "
                    "library: ${_NODE_API_LIBRARY_DOWNLOAD_MESSAGE}. "
                    "Set NODE_API_LIBRARY explicitly.")
            endif()
        endif()
    endif()
    _node_api_require_file(
        "${_NODE_API_LIBRARY}"
        "NODE_API_LIBRARY"
        "the Node import library file"
    )
endif()

if(WIN32)
    find_package_handle_standard_args(
        NodeApi
        REQUIRED_VARS
            _NODE_API_INCLUDE_DIR
            _NODE_ADDON_API_INCLUDE_DIR
            _NODE_API_LIBRARY
    )
else()
    find_package_handle_standard_args(
        NodeApi
        REQUIRED_VARS
            _NODE_API_INCLUDE_DIR
            _NODE_ADDON_API_INCLUDE_DIR
    )
endif()

if(NodeApi_FOUND)
    if(NOT TARGET NodeApi::Headers)
        add_library(NodeApi::Headers INTERFACE IMPORTED)
        set_target_properties(NodeApi::Headers PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_NODE_API_INCLUDE_DIR}"
        )
    endif()

    if(NOT TARGET NodeApi::NodeAddonApi)
        add_library(NodeApi::NodeAddonApi INTERFACE IMPORTED)
        set_target_properties(NodeApi::NodeAddonApi PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_NODE_ADDON_API_INCLUDE_DIR}"
            INTERFACE_LINK_LIBRARIES NodeApi::Headers
        )
    endif()

    if(NOT TARGET NodeApi::NodeLib)
        if(WIN32)
            add_library(NodeApi::NodeLib UNKNOWN IMPORTED)
            set_target_properties(NodeApi::NodeLib PROPERTIES
                IMPORTED_LOCATION "${_NODE_API_LIBRARY}"
            )
        else()
            add_library(NodeApi::NodeLib INTERFACE IMPORTED)
        endif()
    endif()

    set(NodeApi_INCLUDE_DIRS "${_NODE_API_INCLUDE_DIR};${_NODE_ADDON_API_INCLUDE_DIR}")
    set(NodeApi_LIBRARIES NodeApi::Headers NodeApi::NodeAddonApi NodeApi::NodeLib)
endif()
