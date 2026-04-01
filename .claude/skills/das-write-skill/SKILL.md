---
name: das-write-skill
description: Use when creating or editing skills that must work in both OpenCode and Claude Code — covers path placement, frontmatter compatibility, and auto-injection strategies
paths:
  - ".claude/skills/**/*.md"
---

# 跨 OpenCode + Claude Code 的 Skill 编写指南

## 概述

本项目同时使用 OpenCode 和 Claude Code。编写 skill 时需要确保两个工具都能正确发现和加载。

## Skill 放置路径

**统一放在 `.claude/skills/<name>/SKILL.md`**。

两个工具都能发现此路径：

| 工具 | 发现路径 | 备注 |
|------|----------|------|
| Claude Code | `.claude/skills/*/SKILL.md` | 原生支持 |
| OpenCode | `.claude/skills/*/SKILL.md` | Claude 兼容路径 |
| OpenCode | `.agents/skills/*/SKILL.md` | OpenCode 兼容路径 |
| OpenCode | `.opencode/skills/*/SKILL.md` | OpenCode 专属路径 |

**不要用** `.agents/skills/` 或 `.opencode/skills/` — Claude Code 不发现这些路径。

## Frontmatter 兼容性

### 两个工具都识别的字段

| 字段 | OpenCode | Claude Code | 说明 |
|------|----------|-------------|------|
| `name` | ✅ 必需 | ✅ | 必须与目录名一致 |
| `description` | ✅ 必需 | ✅ 推荐 | OpenCode 据此匹配加载 |
| `license` | ✅ | ✅ | 可选 |
| `compatibility` | ✅ | ✅ | 可选 |
| `metadata` | ✅ | ✅ | string-to-string map |

### 仅 Claude Code 识别的字段

OpenCode 会**直接忽略**未知字段（不报错），所以 Claude Code 独有的字段可以放心写：

| 字段 | 说明 |
|------|------|
| `paths` | ⭐ **关键** — glob 模式，匹配时自动注入 |
| `disable-model-invocation` | 禁止 Claude 自动加载 |
| `user-invocable` | 对用户隐藏 `/name` 菜单 |
| `allowed-tools` | 限制可用工具 |
| `context` | `fork` 时在子代理中运行 |
| `agent` | 子代理类型 |
| `argument-hint` | 自动补全提示 |
| `model` / `effort` / `hooks` / `shell` | 其他高级配置 |

## 自动注入策略

目标是：**修改相关代码时 AI 自动获得约束，不依赖描述匹配的运气**。

### Claude Code：用 `paths` 字段（可靠）

```yaml
paths:
  - "das/Core/IPC/**/*.cpp"
  - "das/Core/IPC/**/*.h"
```

匹配文件进入上下文时自动加载 skill，无需 AI 判断。

### OpenCode：靠 `description` 关键词匹配（尽力而为）

OpenCode 不支持 `paths`，只能在 AI 判断 task 相关时通过 description 匹配加载。因此 `description` 中必须包含：

- 涉及的目录名（如 `IPC`、`Core/IPC`）
- 涉及的操作（如 `message header`、`session ID`）
- 涉及的类型名（如 `ValidatedIPCMessageHeader`、`CallKey`）

### 最佳实践：两者组合

```yaml
---
name: das-ipc-constraints
description: Use when modifying any IPC-related code in DuskAutoScript — constructing message headers, sending/receiving IPC messages, or handling session IDs in the IPC framework
paths:
  - "das/Core/IPC/**/*.cpp"
  - "das/Core/IPC/**/*.h"
---
```

- Claude Code：`paths` 保证自动注入
- OpenCode：`description` 关键词覆盖兜底

## Skill 同步维护

如果 skill 描述的代码事实发生变化（API 签名、文件位置、约定改变等），**必须同步更新 skill**，否则会传播过时知识。**必须**在 skill 中显式列出维护义务。

## 陷阱速查

- ❌ 放在 `.opencode/skills/` — Claude Code 不发现
- ❌ 放在 `.agents/skills/` — Claude Code 不发现
- ✅ 放在 `.claude/skills/` — 两个工具都发现
- ❌ `paths` 只写给 OpenCode — OpenCode 不识别此字段
- ✅ `paths` 写给 Claude Code + `description` 写给 OpenCode — 双重保障
- ❌ OpenCode 用 `disable-model-invocation` 等独有字段 — 被忽略
- ✅ OpenCode 独有字段可以写 — 静默忽略，不报错
