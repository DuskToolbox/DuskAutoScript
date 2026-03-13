# ====== IPC 生成代码编译 ======
# 将 IDL 生成的 IPC 代码纳入编译

# IPC 输出目录（由 DasAddIdlExport.cmake 设置）
set(DASCORE_IPC_OUTPUT_DIR "${CMAKE_BINARY_DIR}/das/include/das/_autogen/idl/ipc")
set(DASCORE_IPC_GENERATED_CPP "${DASCORE_IPC_OUTPUT_DIR}/IpcGenerated.cpp")

# 确保 IDL 生成完成后再编译
if(EXISTS "${DASCORE_IPC_GENERATED_CPP}")
    # 添加到 DasCoreObjects 目标
    target_sources(DasCoreObjects PRIVATE "${DASCORE_IPC_GENERATED_CPP}")
    set_source_files_properties("${DASCORE_IPC_GENERATED_CPP}" PROPERTIES GENERATED TRUE)

    # 添加依赖确保 IDL 先生成
    add_dependencies(DasCoreObjects DasCoreIdlGenerated)
endif()
