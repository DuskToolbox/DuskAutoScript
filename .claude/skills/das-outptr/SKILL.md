---
name: das-outptr
description: Use when implementing COM-style functions that return objects via T** out-parameters, or when a factory creates a COM object for local use that must be transferred to the caller on success and Released on error
paths:
  - "**/*.cpp"
  - "**/*.hpp"
---

# DasOutPtr — COM Out-Parameter Smart Pointer

## Overview

`DasOutPtr<T>` replaces raw-pointer manual `Release()` in functions that create COM objects and return them via `T** pp_out`. It binds the out-parameter at construction, auto-Releases on destruction, and requires an explicit `Keep()` on success paths.

## When to Use

- Function receives `T** pp_out` and calls a factory that writes to `T**`
- Need RAII `Release()` on error paths (the main value)
- Object must survive to the caller only on success

**Use DasPtr instead when:** you need shared ownership, copy semantics, or no out-parameter binding.

## Quick Reference

| Method | Purpose |
|--------|---------|
| `DasOutPtr(T** pp_out)` | Bind caller's out-param, write `nullptr` |
| `Put()` | Return `T**` for factory function |
| `Keep()` | Relinquish ownership — destructor won't Release |
| `Get()` / `->` / `*` / `bool` | Access the object |
| `~DasOutPtr()` | Release `*pp_out` if still bound |

## Core Pattern

```cpp
DasResult GetThings(IDasStringVector** pp_out)
{
    DAS_UTILS_CHECK_POINTER(pp_out)

    DAS::DasOutPtr<IDasStringVector> result(pp_out);

    auto cr = CreateIDasStringVector(result.Put());
    if (DAS::IsFailed(cr))
    {
        return cr;  // dtor: Release *pp_out, caller gets nullptr
    }

    for (auto& item : items)
    {
        result->PushBack(item.Get());
    }

    result.Keep();  // explicit: caller takes ownership
    return DAS_S_OK;
}
```

## Why Not DasPtr + Attach

| Approach | Error path | Forget to transfer |
|----------|-----------|-------------------|
| Raw pointer | Manual `Release` needed | *pp_out uninitialized (UB) |
| DasPtr + Attach | Auto-Release, but extra AddRef/Release | *pp_out uninitialized (UB) |
| **DasOutPtr** | **Auto-Release** | **Caller gets nullptr (safe)** |

DasOutPtr stores only `T**` — no separate `T*`. The object lives at `*pp_out` directly. This means:
- One less AddRef/Release cycle vs DasPtr
- Forgetting `Keep()` is a functional bug (null result) but **not** a resource leak or UB

## Common Mistakes

- **Calling `Put()` after `Keep()`**: `pp_out_` is null, `Put()` returns null — factory gets null T**. Always call `Keep()` last.
- **Using DasOutPtr without an out-parameter**: Use `DasPtr` for general smart pointer needs.
- **Expecting `Get()` after move**: Moved-from DasOutPtr has null `pp_out_`, `Get()` returns nullptr.

## Maintenance

When `DasOutPtr` in `include/das/DasPtr.hpp` changes, update this skill accordingly.
