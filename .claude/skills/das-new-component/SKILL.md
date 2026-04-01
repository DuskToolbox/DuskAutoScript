---
name: das-new-component
description: Use when adding a new component or interface implementation to DuskAutoScript that involves creating an Impl class, C API factory function, SWIG wrapper, or integrating with the IDL autogen pipeline
---

# DAS 新增组件指南

## 概述

DAS 组件的创建遵循 IDL → autogen → Impl → C API → SWIG API 的固定流程。本 Skill 归纳了完整的代码编写方法和检查清单。

## 完整流程

```
1. 定义 IDL 接口          → idl/IDasXxx.idl
2. CMake 自动生成         → ${BUILD_DIR}/das/include/das/_autogen/idl/
3. 编写 Impl 实现类       → das/Core/<Component>/include/.../XxxImpl.h
                           das/Core/<Component>/src/XxxImpl.cpp
4. 注册 C API 工厂函数    → include/das/DasApi.h + XxxImpl.cpp
5. 注册 SWIG 包装函数     → include/das/DasSwigApi.h + XxxImpl.cpp
6. 更新 SWIG 导出         → SWIG/ExportAll.i
```

**CMake 集成**：现有组件目录下新增文件**无需修改 CMakeLists.txt**。`das_add_core_component()` 宏通过 `GLOB_RECURSE` 自动发现 `src/` 和 `include/` 下的所有文件。仅在创建全新组件目录时才需要在 `das/Core/CMakeLists.txt` 添加 `das_add_core_component(<ComponentName>)`。

## 步骤 1：定义 IDL 接口

在 `idl/` 目录创建接口定义文件：

```cpp
// idl/IDasXxx.idl
[uuid("XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX")]
interface IDasXxx : IDasBase {
    DasResult MethodA(int64_t in_param, [out] int64_t* p_out_result);
    DasResult MethodB();
};
```

CMake 会自动触发生成，**无需手动 reconfigure**。

## 步骤 2：确认 autogen 产物

构建后检查 `${BUILD_DIR}/das/include/das/_autogen/idl/` 下的生成文件：

| 路径 | 内容 |
|------|------|
| `abi/IDasXxx.h` | C ABI 接口定义 + GUID |
| `wrapper/Das.ExportInterface.IDasXxx.Implements.hpp` | **ImplBase 模板** |
| `wrapper/Das.ExportInterface.IDasXxx.hpp` | RAII 包装类 |
| `swig/IDasXxx.i` | SWIG 绑定 |
| `ipc/proxy/DasXxxProxy.h` | IPC 代理 |

## 步骤 3：编写 Impl 头文件

**文件位置**: `das/Core/<Component>/include/das/Core/<Component>/XxxImpl.h`

```cpp
#ifndef DAS_CORE_<COMPONENT>_XXXIMPL_H
#define DAS_CORE_<COMPONENT>_XXXIMPL_H

#include <das/Core/<Component>/Config.h>
#include <das/DasBase.hpp>
#include <das/_autogen/idl/abi/IDasXxx.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasXxx.Implements.hpp>
// 如需依赖其他接口的包装类：
// #include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasOther.hpp>

DAS_CORE_<COMPONENT>_NS_BEGIN

class XxxImpl final
    : public Das::ExportInterface::XxxImplBase<XxxImpl>
{
public:
    XxxImpl() = default;

    DAS_IMPL MethodA(int64_t in_param, int64_t* p_out_result) override;
    DAS_IMPL MethodB() override;

private:
    // 成员变量
};

DAS_CORE_<COMPONENT>_NS_END

#endif
```

### 关键约定

- **继承**: `Das::ExportInterface::XxxImplBase<XxxImpl>`（CRTP 模式）
- **方法前缀**: `DAS_IMPL`（展开为 `DasResult DAS_STD_CALL`）
- **命名空间**: 使用组件宏 `DAS_CORE_<COMPONENT>_NS_BEGIN/END`
- **include 顺序**: Config.h → DasBase.hpp → autogen abi → autogen Implements.hpp

## 步骤 4：编写 Impl 源文件

**文件位置**: `das/Core/<Component>/src/XxxImpl.cpp`

```cpp
#include "das/DasConfig.h"
#include "das/DasPtr.hpp"
#include <das/Core/Logger/Logger.h>
#include <das/Core/<Component>/XxxImpl.h>
#include <das/DasApi.h>
#include <das/DasSwigApi.h>
#include <das/Utils/CommonUtils.hpp>

DAS_CORE_<COMPONENT>_NS_BEGIN

DasResult XxxImpl::MethodA(int64_t in_param, int64_t* p_out_result)
{
    DAS_UTILS_CHECK_POINTER(p_out_result);

    // ... 实现逻辑
    return DAS_S_OK;
}

DasResult XxxImpl::MethodB()
{
    // ... 实现逻辑
    return DAS_S_OK;
}

DAS_CORE_<COMPONENT>_NS_END
```

### 错误处理模式

```cpp
// 1. 指针检查
DAS_UTILS_CHECK_POINTER(p_out);

// 2. 越界检查
if (index >= size) { return DAS_E_OUT_OF_RANGE; }

// 3. 内存分配（容器操作）
try { vec.emplace_back(value); }
catch (const std::bad_alloc&) { return DAS_E_OUT_OF_MEMORY; }

// 4. 类型错误
DAS_CORE_LOG_ERROR("Type error. Expected = {}, actual = {}.", expected, actual);
return DAS_E_TYPE_ERROR;

// 5. COM 接口获取（AddRef 转移所有权）
auto* p_raw = wrapper.Get();
p_raw->AddRef();
*pp_out = p_raw;
```

## 步骤 5：C API 工厂函数

### 5.1 DasApi.h — 前置声明 + 函数声明

```cpp
// 在 Das::ExportInterface 命名空间块中添加前置声明
namespace Das::ExportInterface
{
    struct IDasXxx;
} // namespace Das::ExportInterface

// 在 #ifndef SWIG ... #endif 块内添加函数声明
DAS_C_API DasResult CreateIDasXxx(
    Das::ExportInterface::IDasXxx** pp_out_xxx);
```

### 5.2 XxxImpl.cpp — 工厂函数实现（命名空间外）

```cpp
// 放在文件末尾，DAS_CORE_<COMPONENT>_NS_END 之后

DasResult CreateIDasXxx(
    Das::ExportInterface::IDasXxx** pp_out_xxx)
{
    DAS_UTILS_CHECK_POINTER(pp_out_xxx);

    try
    {
        auto* result = new DAS::Core::<Component>::XxxImpl{};
        result->AddRef();
        *pp_out_xxx = result;
        return DAS_S_OK;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
}
```

## 步骤 6：SWIG 包装函数

### 6.1 DasSwigApi.h — 返回类型 + 函数声明

```cpp
#include <das/DasConfig.h>
#include <das/DasString.hpp>
#include "IDasXxx.h"

DAS_DEFINE_RET_POINTER(
    DasRetXxx,
    Das::ExportInterface::IDasXxx);

DAS_SWIG_NS_BEGIN

DAS_API DasRetXxx CreateDasRetXxx();

DAS_SWIG_NS_END
```

### 6.2 XxxImpl.cpp — SWIG 函数实现

```cpp
// 放在 C API 工厂函数之后

DAS_SWIG_NS_BEGIN

DasRetXxx CreateDasRetXxx()
{
    DAS::DasPtr<Das::ExportInterface::IDasXxx> p_xxx;
    const auto result = CreateIDasXxx(p_xxx.Put());
    if (DAS::IsFailed(result))
    {
        return {.error_code = result, .value = nullptr};
    }
    return {.error_code = DAS_S_OK, .value = std::move(p_xxx)};
}

DAS_SWIG_NS_END
```

## 步骤 7：更新 ExportAll.i

在 `SWIG/ExportAll.i` 中添加：

```cpp
// 头部 include 区（与其它 include 排列在一起）
#include <das/DasSwigApi.h>

// 文件末尾 SWIG 导出区
%include <das/DasSwigApi.h>
```

## 关键宏速查

| 宏 | 展开 | 用途 |
|----|------|------|
| `DAS_IMPL` | `DasResult DAS_STD_CALL` | 接口方法返回类型 |
| `DAS_C_API` | `extern "C" __declspec(dllexport)` | C API 导出函数 |
| `DAS_DEFINE_RET_POINTER(Name, Ptr)` | 生成含 `DasPtr<Ptr>` 的返回结构体 | SWIG 安全的智能指针返回 |
| `DAS_DEFINE_RET_TYPE(Name, T)` | 生成含值类型 `T` 的返回结构体 | SWIG 安全的值类型返回 |
| `DAS_SWIG_NS_BEGIN` | `namespace Das { namespace Swig {` | SWIG 命名空间开始 |
| `DAS_SWIG_NS_END` | `}}` | SWIG 命名空间结束 |
| `DAS_UTILS_CHECK_POINTER(p)` | 空指针检查 → 返回错误码 | 输入验证 |

## ImplBase 继承模式对照

| 接口类型 | 基类 | 示例 |
|----------|------|------|
| ExportInterface | `Das::ExportInterface::XxxImplBase<T>` | `DasVariantVectorImplBase<DasVariantVectorImpl>` |
| PluginInterface | `Das::PluginInterface::XxxImplBase<T>` | `DasStopTokenImplBase<DasStopTokenImplOnStack>` |
| 多接口继承 | 直接继承多个接口 | `IDasGuidVectorImpl : IDasGuidVector, IDasReadOnlyGuidVector` |

**推荐使用 ImplBase 模式**：自动提供引用计数、QueryInterface、工厂方法。

## DasApi.h 与 DasSwigApi.h 的分工

| 文件 | 内容 | 可见性 |
|------|------|--------|
| `DasApi.h` | 前置声明、`DAS_C_API` 函数声明 | C/C++ 可见，`#ifndef SWIG` 内对 SWIG 隐藏 |
| `DasSwigApi.h` | `DAS_DEFINE_RET_*` 返回类型、`DAS_SWIG_NS_*` 函数 | SWIG 可见，供脚本语言调用 |

**不能被 SWIG 直接处理的类型**（如 `DasReadOnlyString` 直接传值）必须从 `DasApi.h` 迁移到 `DasSwigApi.h`，并改为指针参数形式。

## 检查清单

新增组件时，按以下顺序逐项确认：

- [ ] `idl/IDasXxx.idl` — 接口定义（含 uuid）
- [ ] 确认 autogen 产物已生成（`abi/`、`wrapper/Implements.hpp`）
- [ ] `das/Core/<Component>/include/.../XxxImpl.h` — Impl 类声明
- [ ] `das/Core/<Component>/src/XxxImpl.cpp` — Impl 类实现
- [ ] `include/das/DasApi.h` — 前置声明 + `DAS_C_API` 函数声明
- [ ] `include/das/DasSwigApi.h` — 返回类型定义 + SWIG 函数声明
- [ ] `SWIG/ExportAll.i` — `#include` + `%include` DasSwigApi.h
- [ ] 构建通过（`cmake --build ${BUILD_DIR}` — 全量构建）
- [ ] `clang-format` 格式化
