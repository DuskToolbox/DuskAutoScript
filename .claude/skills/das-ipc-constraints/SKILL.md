---
name: das-ipc-constraints
description: Use when modifying any IPC-related code in DuskAutoScript — constructing message headers, sending/receiving IPC messages, or handling session IDs in the IPC framework
paths:
  - "das/Core/IPC/**/*.cpp"
  - "das/Core/IPC/**/*.h"
  - "das/Core/IPC/include/**/*.h"
---

# DuskAutoScript IPC 约束规则

## ⚠️ Skill 同步维护义务

**本 Skill 描述的是 IPC 框架的当前事实。如果你在修改 IPC 代码时发现以下任何情况，必须同步更新本 Skill：**

- `ValidatedIPCMessageHeader` 的构造方式或友元关系发生变化
- `IPCMessageHeaderBuilder` 的新增/移除方法改变了使用方式
- RESPONSE 的 source/target session_id 交换约定被改变
- CallKey 的构造或匹配逻辑发生变化
- `PostSend`、`CompletePendingCall`、`NotifySendFailure` 等核心方法的签名或行为改变
- 新增/移除了本 Skill 关键文件索引中列出的文件

**不更新的后果**：Skill 将传播过时的知识，导致后续修改引入 bug。

## 概述

DuskAutoScript 的 IPC 框架有若干隐式约定和 API 约束，违反会导致编译失败或运行时 CallKey 匹配失败。在修改任何 IPC 相关代码前，必须遵守以下规则。

## 核心规则

### 规则 1: 构造 ValidatedIPCMessageHeader 必须使用 IPCMessageHeaderBuilder

`ValidatedIPCMessageHeader(IPCMessageHeader&)` 是 **private 构造函数**，只有 `IPCMessageHeaderBuilder`、`IpcTransport`、`MessageDeserializer` 是友元。其他类（包括 `IpcRunLoop`）不能直接构造。

```cpp
// ❌ 错误 — 编译失败：private constructor
ValidatedIPCMessageHeader fail_header{raw_header};

// ✅ 正确 — 使用 Builder
auto header = IPCMessageHeaderBuilder()
    .SetCallId(call_id)
    .SetSourceSessionId(source)
    .SetTargetSessionId(target)
    .Build();
```

**循环 include 约束**：`IpcMessageHeaderBuilder.h` 已 include `IpcRunLoop.h`。不得反向 include。Builder 只能在 `.cpp` 文件中使用。

### 规则 2: RESPONSE 的 source/target session_id 必须与 REQUEST 互换

REQUEST 和 RESPONSE 的 session_id 方向是相反的：

| | source_session_id | target_session_id |
|---|---|---|
| **REQUEST** | 发送方（本地） | 接收方（远程） |
| **RESPONSE** | 响应方（远程） | 请求方（本地） |

```cpp
// 构造 RESPONSE 时必须交换（参考 IStubBase.cpp:66-67）
.SetSourceSessionId(request_header.GetTargetSessionId())  // 远程端
.SetTargetSessionId(request_header.GetSourceSessionId())  // 本地端
```

**违反后果**：CallKey 匹配失败 → `CompletePendingCall: call_key not found` → 调用方永远收不到响应。

### 规则 3: CallKey 的 source_session_id 存的是远程 session_id

注册 pending call 和匹配 RESPONSE 时，CallKey 使用的是远程端的 session_id：

```cpp
// 注册时（IPCProxyBase.cpp）— 使用目标 session_id
CallKey call_key{object_id_.session_id, call_id};  // object_id_.session_id = 远程

// 匹配时（BusinessThread.cpp）— 从 RESPONSE 的 source_session_id 取
CallKey call_key{
    .source_session_id = header.GetSourceSessionId(),  // RESPONSE 的 source = 远程
    .call_id = header.GetCallId()};
```

### 规则 4: IPCMessageHeaderBuilder::SetErrorCode() 自动设置 message_type=RESPONSE

调用 `SetErrorCode(error_code)` 会自动将 `message_type` 设为 `RESPONSE`，无需手动 `SetMessageType(MessageType::RESPONSE)`。

## 关键文件索引

| 文件 | 内容 |
|------|------|
| `IpcMessageHeaderBuilder.h` | 构造 ValidatedIPCMessageHeader 的唯一公开方式 |
| `IpcRunLoop.cpp` — `PostSend` | 异步发送入口，失败时通过 NotifySendFailure 通知 |
| `IpcRunLoop.cpp` — `NotifySendFailure` | 构造失败 RESPONSE 推入 inbound_queue_ |
| `IpcRunLoop.cpp` — `CompletePendingCall` | 匹配 CallKey 并触发回调 |
| `BusinessThread.cpp` — `ProcessInboundMessage` | 分发 RESPONSE 时调 CompletePendingCall |
| `BusinessThread.cpp` — `PumpUntilResponse` | BusinessThread 路径从 inbound_queue_ 泵消息 |
| `IPCProxyBase.cpp` — `SendRequest` | 发送请求 + 注册 pending call |
| `IStubBase.cpp` | 远程端构造 RESPONSE（source/target 交换的参考实现） |
| `IpcResponseSender.cpp` | 响应传输层（接收已构造好的 header） |

## 异步失败通知设计

PostSend 失败时，通过推入 `inbound_queue_` 统一覆盖两条等待路径：

```
NotifySendFailure → inbound_queue_ → BusinessThread::ProcessInboundMessage
  ├─ BusinessThread 路径: PumpUntilResponse 从队列泵出
  └─ 外部线程路径: CompletePendingCall → on_complete 回调
```

**不要**在 NotifySendFailure 中直接调用 CompletePendingCall，否则 BusinessThread 路径的 PumpUntilResponse 永远收不到失败通知。

## 日志规范

- 日志内容必须使用英文
- `=` 前后要有空格：`"session_id = {}"` 而非 `"session_id={}"`
- 不要包含函数名（spdlog 自动添加）
