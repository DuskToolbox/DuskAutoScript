---
name: das-yyjson-string-safety
description: >-
  Use when writing yyjson writer (mutable document) code in DuskAutoScript —
  assigning std::string values to yyjson writer objects, using yyjson::writer::value
  or array_ref/object_ref, calling yyjson mut_doc APIs, or serializing yyjson
  values with cpp_yyjson. Covers the heap-use-after-free pitfall when assigning
  lvalue std::string to yyjson writer values, and the correct copy_string API.
paths:
  - "das/**/*.cpp"
  - "das/**/*.h"
  - "das/**/*.hpp"
---

# yyjson Writer String Safety

## The Bug

cpp_yyjson's `set_value` for `std::string` has two paths:

```cpp
// cpp_yyjson.hpp:1465-1478 (simplified)
if constexpr (copy || (std::same_as<std::string, value_type> && std::is_rvalue_reference_v<T&&>))
{
    dst->uni.str = unsafe_yyjson_mut_strncpy(ptrs->self, t.data(), t.size()); // COPIES
}
else
{
    dst->uni.str = t.data(); // STORES RAW POINTER — NO COPY
}
```

When a `const std::string&` lvalue is assigned via `operator=`, template deduction yields `T = const std::string&`, making `std::is_rvalue_reference_v<T&&>` **false**. The `else` branch stores only a raw pointer (`t.data()`). When the source `std::string` is later destroyed, the yyjson value holds a dangling pointer — **heap-use-after-free**.

### Typical Crash Stack

```
ParsePluginPackageDescFromJson  →  fills PluginPackageDesc::name (std::string)
PluginPackageDescToJson(desc)   →  obj["name"] = desc.name  ← stores raw pointer
descs vector destroyed           →  desc.name::~string() frees the heap buffer
yyjson_mut_write_minify          →  reads freed memory → ASAN: heap-use-after-free
```

## Correct Patterns

### Recommended: `std::make_pair(std::string_view, yyjson::copy_string)`

Zero extra heap allocations. `string_view` is a pointer+length pair (no heap), and `copy_string` triggers `yyjson_mut_strncpy` to copy directly into the yyjson document pool.

```cpp
(*obj)[std::string_view("name")] =
    std::make_pair(std::string_view(desc.name), yyjson::copy_string);
```

This uses the `pair_like` + `copy_string_t` overload in `mutable_value_base::operator=`, which calls `set_value(val, std::get<0>(pair), copy_string)` → `create_primitive(std::string_view, copy_string_t)` → `yyjson_mut_strncpy`.

### Acceptable: `std::string(str)` temporary

Constructs a temporary rvalue `std::string`, which triggers the rvalue path (`std::is_rvalue_reference_v<T&&> = true`) → `yyjson_mut_strncpy`. One extra heap allocation for the temporary, but correct.

```cpp
(*obj)[std::string_view("name")] = std::string(desc.name);
```

### Explicit pair (verbose but clear)

```cpp
(*obj)[std::string_view("name")] =
    std::pair<std::string, yyjson::copy_string_t>{desc.name, yyjson::copy_string};
```

## Broken Pattern (DO NOT USE)

### Braced-init-list `{str, yyjson::copy_string}` — MSVC fails

MSVC cannot deduce the template parameter `T` from a braced-init-list for the `pair_like` concept overload. **This compiles on GCC/Clang but NOT on MSVC:**

```cpp
// ❌ MSVC: error C2679: no operator= accepts initializer list
(*obj)[std::string_view("name")] = {desc.name, yyjson::copy_string};
```

### Direct lvalue assignment — heap-use-after-free

```cpp
// ❌ Stores raw pointer — dangling after source string dies
(*obj)[std::string_view("name")] = desc.name;
```

## Safe Patterns (No Special Handling Needed)

These do NOT need `copy_string` because they are already safe:

| Pattern | Why Safe |
|---------|----------|
| `std::string_view("literal")` | String literal has static storage duration |
| Function-returned rvalue `std::string` (e.g., `DasGuidToStdString()`) | Rvalue triggers `yyjson_mut_strncpy` automatically |
| Numeric types (`int64_t`, `bool`, `double`) | No string pointer involved |

## Maintenance Obligation

If cpp_yyjson's string assignment semantics change (e.g., default behavior switches to always-copy, or `copy_string_t` API is removed), this skill **must** be updated. Check `set_value` and `create_primitive` in `cpp_yyjson.hpp` after any cpp_yyjson upgrade.