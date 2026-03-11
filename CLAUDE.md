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
