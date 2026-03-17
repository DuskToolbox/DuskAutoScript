# ====== IPC 生成代码编译 ======
# IpcGenerated.cpp 在主 CMakeLists.txt 中通过 target_sources 纳入 DasCoreObjects
# （与 OfficialIids.cpp 保持相同模式）

# ====== BusinessThread 源文件 ======
target_sources(DasCoreObjects PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/BusinessThread.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/IStubBase.cpp
)
