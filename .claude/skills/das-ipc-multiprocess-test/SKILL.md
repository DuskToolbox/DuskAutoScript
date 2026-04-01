---
name: das-ipc-multiprocess-test
description: Use when running or debugging IpcMultiProcessTest in DuskAutoScript — setting up DAS_HOST_EXE_PATH/DAS_PLUGIN_DIR environment variables, executing multi-process integration tests via ctest or gtest, or configuring DasHost.exe for test execution
paths:
  - "das/IpcMultiProcessTest/**/*"
---

# DuskAutoScript IPC 多进程测试运行指南

## 概述

`IpcMultiProcessTest` 是 IPC 框架的多进程测试组件，分为两类测试：

| 测试套件 | 数量 | 是否需要环境变量 | 说明 |
|----------|------|------------------|------|
| `IpcMultiProcessTestBasic.*` | 15 | 不需要 | 单元测试，不启动真实进程 |
| `IpcMultiProcessTestIntegration.*` | 13 | 需要 | 集成测试，启动真实 DasHost.exe |

## 路径变量

以下为 CMake 变量，具体值视工程配置而定（可在 `.vscode/settings.json` 的 `cmake.buildDirectory` 中查看）：

| CMake 变量 | 含义 |
|------------|------|
| `${CMAKE_SOURCE_DIR}` 或 `${SOURCE_DIR}` | 源码根目录 |
| `${CMAKE_BINARY_DIR}` 或 `${BUILD_DIR}` | 构建输出目录 |
| `$<CONFIG>` | 当前构建配置（`Debug` / `Release` 等） |

本 skill 中 `${BUILD_DIR}` 和 `${SOURCE_DIR}` 均指代上述 CMake 变量。

## 测试二进制文件

```
${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe
```

CMake 通过 `RUNTIME_DIRECTORY` 属性将输出放到 `${BUILD_DIR}` 根目录，而非 `bin/$<CONFIG>/`。实际运行时 ctest 会自动定位。手动运行时请检查该路径。

## 环境变量（CMake 自动注入）

环境变量的值从 CMake 变量派生（见 `das/IpcMultiProcessTest/CMakeLists.txt:45-46`）：

```cmake
ENVIRONMENT "DAS_HOST_EXE_PATH=${CMAKE_BINARY_DIR}/bin/$<CONFIG>/DasHost.exe"
ENVIRONMENT "DAS_PLUGIN_DIR=${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins"
```

| 变量 | CMake 源 | 手动设置 | 说明 |
|------|----------|----------|------|
| `DAS_HOST_EXE_PATH` | `${CMAKE_BINARY_DIR}/bin/$<CONFIG>/DasHost.exe` | `${BUILD_DIR}/bin/$<CONFIG>/DasHost.exe` | DasHost.exe 路径，不存在则抛 `std::runtime_error` |
| `DAS_PLUGIN_DIR` | `${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins` | `${BUILD_DIR}/bin/$<CONFIG>/plugins` | 插件目录，包含 `IpcTestPlugin1.json` 等 |

ctest 通过 `gtest_discover_tests` 自动注入。手动运行时必须手动设置。

### 可选

| 变量 | 值 | 说明 |
|------|-----|------|
| `DAS_DEBUG` | `1` 或 `true` | 调试模式：所有超时变为无限等待，禁用心跳 |

`DAS_DEBUG` 的影响（见 `IpcTestConfig.h`）：

- `GetHostStartTimeoutMs()` 返回 0（无限等待），默认 10000ms
- `GetPluginLoadTimeout()` 返回 0（无限等待），默认 30000ms
- `ShouldDisableHeartbeat()` 返回 true

## 运行命令

### 通过 ctest（推荐，自动注入环境变量）

```bash
# 运行全部 IPC 多进程测试
ctest --test-dir ${BUILD_DIR} -R IpcMultiProcessTest -V

# 仅运行单元测试
ctest --test-dir ${BUILD_DIR} -R IpcMultiProcessTestBasic -V

# 仅运行集成测试
ctest --test-dir ${BUILD_DIR} -R IpcMultiProcessTestIntegration -V
```

### 手动运行（需要自行设置环境变量）

将 `${BUILD_DIR}` 替换为实际构建目录（如 `C:/vmbuild`），将 `$<CONFIG>` 替换为构建配置（如 `Debug` 或 `Release`）。

```powershell
# PowerShell
$env:DAS_HOST_EXE_PATH = "${BUILD_DIR}/bin/$<CONFIG>/DasHost.exe"
$env:DAS_PLUGIN_DIR   = "${BUILD_DIR}/bin/$<CONFIG>/plugins"

# 运行全部
${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe

# gtest filter 示例
${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe --gtest_filter=IpcMultiProcessTestBasic.*

# 调试模式（禁用超时）
$env:DAS_DEBUG = "1"
${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe --gtest_filter=IpcMultiProcessTestIntegration.CrossProcess_LoadPlugin
```

```cmd
REM cmd.exe
set DAS_HOST_EXE_PATH=${BUILD_DIR}\bin\$<CONFIG>\DasHost.exe
set DAS_PLUGIN_DIR=${BUILD_DIR}\bin\$<CONFIG>\plugins

${BUILD_DIR}\bin\$<CONFIG>\IpcMultiProcessTest.exe
```

```bash
# Linux bash
export DAS_HOST_EXE_PATH="${BUILD_DIR}/bin/$<CONFIG>/DasHost.exe"
export DAS_PLUGIN_DIR="${BUILD_DIR}/bin/$<CONFIG>/plugins"

# 运行全部
"${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe"

# gtest filter 示例
"${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe" --gtest_filter=IpcMultiProcessTestBasic.*

# 调试模式（禁用超时）
export DAS_DEBUG=1
"${BUILD_DIR}/bin/$<CONFIG>/IpcMultiProcessTest.exe" --gtest_filter=IpcMultiProcessTestIntegration.CrossProcess_LoadPlugin
```

## 常用 gtest filter 模式

| filter | 作用 |
|--------|------|
| `IpcMultiProcessTestBasic.*` | 全部单元测试 |
| `IpcMultiProcessTestBasic.ObjectId*` | ObjectId 编解码测试 |
| `IpcMultiProcessTestBasic.RemoteObjectRegistry*` | 远程对象注册表测试 |
| `IpcMultiProcessTestBasic.Handshake*` | 握手协议结构测试 |
| `IpcMultiProcessTestIntegration.*` | 全部集成测试 |
| `IpcMultiProcessTestIntegration.CrossProcess*` | 跨进程调用测试 |
| `IpcMultiProcessTestIntegration.HostLauncher*` | Host 启动/停止测试 |
| `IpcMultiProcessTestIntegration.ParentProcessExit*` | 父进程退出检测测试 |
| `IpcMultiProcessTestIntegration.RemoteProxy*` | 远程 Proxy 测试 |
| `IpcMultiProcessTestIntegration.QueryMainProcess*` | 主进程接口查询 E2E 测试 |

## 构建前置条件

CMake 配置时需要启用 `-DDAS_BUILD_TEST=ON`。构建目标依赖 `DasAutoCopyDll`（自动拷贝 DLL），见 `das/IpcMultiProcessTest/CMakeLists.txt`。

## 集成测试跳过条件

部分集成测试在以下情况会 `GTEST_SKIP()`（不是失败）：

- `DasHost.exe` 不存在于 `DAS_HOST_EXE_PATH`
- 插件 JSON 文件不存在于 `DAS_PLUGIN_DIR`（如 `JavaTestPlugin` 需要 JVM 环境）
- 测试插件 JAR 文件缺失

## 关键文件

| 文件 | 说明 |
|------|------|
| `das/IpcMultiProcessTest/CMakeLists.txt` | 测试构建配置，ctest 环境变量注入 |
| `das/IpcMultiProcessTest/test/IpcTestConfig.h` | 运行时环境变量读取与超时配置 |
| `das/IpcMultiProcessTest/test/IpcMultiProcessTestBasic.cpp` | 单元测试实现 |
| `das/IpcMultiProcessTest/test/IpcMultiProcessTestIntegration.cpp` | 集成测试实现 |
| `das/IpcMultiProcessTest/test/IpcMultiProcessTestIntegration.h` | 集成测试夹具（IIpcContext 初始化） |
| `das/Host/CMakeLists.txt` | DasHost.exe 构建配置 |
