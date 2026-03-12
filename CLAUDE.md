# Project Instructions

## DAS_LIFETIMEBOUND 使用规则

### 正确语法（Clang + MSVC）
```cpp
// 放在变量名之后，函数返回非 void
IpcRunLoop* Create(
    void* obj,
    IpcRunLoop* run_loop DAS_LIFETIMEBOUND = nullptr);  // ✓

// 构造函数参数
explicit IpcRunLoop(IpcRunLoop& other DAS_LIFETIMEBOUND);  // ✓

// 返回引用
IpcRunLoop& GetRunLoop(IpcRunLoop& obj DAS_LIFETIMEBOUND);  // ✓
```

### 错误语法
```cpp
// ✗ 函数返回 void 时不能使用
void SetRunLoop(IpcRunLoop& run_loop DAS_LIFETIMEBOUND);

// ✗ 放在变量名之前
void SetRunLoop(IpcRunLoop& DAS_LIFETIMEBOUND run_loop);
```

### 规则
1. 只能用于**返回非 void 类型**的函数参数
2. 必须放在**变量名之后**
3. 构造函数参数可以使用
4. 返回指针/引用的 getter 函数参数可以使用

## 代码格式化规则

每次编辑 C++ 文件（`.cpp`、`.h`、`.hpp`）后，**必须**调用 clang-format 格式化文件：

```bash
clang-format -i <文件路径>
```

### 禁止单行 if 语句

**严禁**使用单行 if 语句，必须使用花括号：

```cpp
// ✗ 禁止
if (x) return;
if (x) doSomething();

// ✓ 正确
if (x) {
    return;
}

if (x) {
    doSomething();
}
```

## 日志记录规则

### 禁止使用十六进制格式记录错误码

**严禁**在日志中使用 `0x{:08X}` 等十六进制格式记录 DasResult 错误码：

```cpp
// ✗ 禁止
DAS_CORE_LOG_ERROR("Operation failed: result=0x{:08X}", result);

// ✓ 正确 - 直接记录数值
DAS_CORE_LOG_ERROR("Operation failed: result={}", result);
```

原因：DasResult 错误码使用十进制数值定义，使用十六进制格式会导致日志难以阅读和调试。
```
