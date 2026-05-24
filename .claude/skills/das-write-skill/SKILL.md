---
name: das-write-skill
description: Use when creating or editing DAS project skills that must work from the Claude-compatible source layout and be consumable by OpenCode, Claude Code, and Codex
paths:
  - ".claude/skills/**/*.md"
---

# DAS Project Skill 编写指南

## 概述

本项目的 skill 源文件统一使用 Claude 兼容布局。编写 skill 时，先保证 `.claude/skills` 中的源文件正确；其他 agent 的兼容发现由项目适配层处理，不要把 agent 专用路径或兼容逻辑写进单个业务 skill。

## Skill 放置路径

**统一放在 `.claude/skills/<name>/SKILL.md`**。

各工具的处理方式：

| 工具 | 发现路径 | 备注 |
|------|----------|------|
| Claude Code | `.claude/skills/*/SKILL.md` | 原生支持 |
| OpenCode | `.claude/skills/*/SKILL.md` | Claude 兼容路径 |
| OpenCode | `.agents/skills/*/SKILL.md` | OpenCode 兼容路径 |
| OpenCode | `.opencode/skills/*/SKILL.md` | OpenCode 专属路径 |
| Codex | `.codex/skills/*/SKILL.md` | 由 CMake 配置阶段从 `.claude/skills` 创建项目级 symlink |

**不要用** `.agents/skills/`、`.opencode/skills/` 或 `.codex/skills/` 作为项目源路径。项目源文件只放 `.claude/skills/`。

Codex 适配由 `cmake/DasAgentSkillSync.cmake` 维护。默认 `DAS_SYNC_CODEX_SKILLS=ON`，目标为项目内 `.codex/skills`，可通过 `DAS_CODEX_SKILLS_DIR` 覆盖。业务 skill 不应包含 Codex 专用说明，除非该说明本身就是业务约束。

## Frontmatter 兼容性

### 基础字段

| 字段 | OpenCode | Claude Code | Codex | 说明 |
|------|----------|-------------|-------|------|
| `name` | 必需 | 支持 | 支持 | 必须与目录名一致 |
| `description` | 必需 | 推荐 | 推荐 | OpenCode / Codex 据此匹配加载 |
| `license` | 可选 | 可选 | 可选 | 可选 |
| `compatibility` | 可选 | 可选 | 可选 | 可选 |
| `metadata` | 可选 | 可选 | 可选 | string-to-string map |

### 仅 Claude Code 识别的字段

OpenCode 和 Codex 会忽略不认识的字段，所以 Claude Code 独有字段可以保留在项目源 skill 中：

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

### OpenCode / Codex：靠 `description` 关键词匹配（尽力而为）

OpenCode 和 Codex 对 `paths` 的自动注入能力不应作为唯一前提，必须让 `description` 自身足够可匹配。因此 `description` 中必须包含：

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
- OpenCode / Codex：`description` 关键词覆盖兜底

## Skill 同步维护

如果 skill 描述的代码事实发生变化（API 签名、文件位置、约定改变等），**必须同步更新 skill**，否则会传播过时知识。**必须**在 skill 中显式列出维护义务。

## 陷阱速查

- ❌ 放在 `.opencode/skills/` — Claude Code 不发现
- ❌ 放在 `.agents/skills/` — Claude Code 不发现
- ❌ 放在 `.codex/skills/` — 不是项目源路径，会绕过代码审查和同步维护
- ✅ 放在 `.claude/skills/` — 项目唯一源路径
- ❌ `paths` 只写给 OpenCode — OpenCode 不识别此字段
- ✅ `paths` 写给 Claude Code + `description` 写给 OpenCode / Codex — 双重保障
- ❌ OpenCode 用 `disable-model-invocation` 等独有字段 — 被忽略
- ✅ OpenCode 独有字段可以写 — 静默忽略，不报错
- ✅ Codex 兼容通过 CMake 创建项目级 symlink 处理，不复制业务规则到每个 skill
