---
name: das-cmake-presets
description: Use when running cmake configure, cmake build, cmake --preset, or modifying CMakePresets.json, CMakeUserPresets.json, CMakeLists.txt, .vscode/settings.json in DuskAutoScript
paths:
  - "CMakePresets.json"
  - "CMakeUserPresets.json*"
  - "CMakeLists.txt"
  - ".vscode/settings.json"
---

# DAS CMake Preset 体系

项目使用 `CMakePresets.json`（git 版本控制）+ `CMakeUserPresets.json`（gitignore，用户本地）管理构建配置。

**涉及 CMake 相关操作前，必须先阅读 `CMakePresets.json` 和 `CMakeUserPresets.json.example` 了解当前预设命名和继承关系。**

## 预设架构

```
CMakePresets.json（公共）
├── common-base (hidden)          ← 共享默认值
├── msvc-base → msvc-prebuilt-base → msvc-prebuilt-debug/release
│          → msvc-bundled-base  → msvc-bundled-debug/release
├── mingw-base (Ninja) → mingw-debug/release
└── linux-base → linux-debug/release

CMakeUserPresets.json（用户本地）
└── 个人调试预设（如 ember）
```

## 新成员配置流程

1. `cp CMakeUserPresets.json.example CMakeUserPresets.json`
2. 修改 `inherits` 指向合适的平台预设
3. `cacheVariables` 设置个人偏好：`EXPORT_PYTHON/JAVA/CSHARP`、`DAS_USE_ASAN/LTO`、`DAS_BUILD_TEST`、`SWIG_EXECUTABLE`、`ICU_ROOT`
4. `cmake --preset <name>` 一键配置

## 注意事项

- 公共预设变更需团队沟通；用户预设仅影响本地
- 新增公共预设时设置 `condition` 做平台过滤
- 修改 hidden 模板预设（如 `common-base`、`msvc-base`）的 `cacheVariables` 时，所有 `inherits` 该模板的子预设都会继承变更，需逐一确认子预设行为是否仍然正确
