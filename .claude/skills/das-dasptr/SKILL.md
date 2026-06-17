---
name: das-dasptr
description: Use when working with DAS COM-style smart pointer ownership in DuskAutoScript: DasPtr, AddRef/Release, QueryInterface, As, Attach, Put, interface out-parameters, plugin factories, proxies, or async callbacks.
paths:
  - "include/das/DasPtr.hpp"
  - "das/**/*.cpp"
  - "das/**/*.h"
  - "das/**/*.hpp"
---

# DasPtr Ownership Rules

## Purpose

`DasPtr<T>` owns a DAS COM-style interface reference and calls `Release()` in
its destructor. Its raw-pointer constructor and copy assignment call `AddRef()`.
This makes `DasPtr(raw)` correct only when you intentionally need an additional
reference.

## Caller-Side Out-Parameters

When calling a function that writes a newly owned interface pointer to `T**`,
receive it directly with `Put()`:

```cpp
DAS::DasPtr<IDasBase> factory_base;
DasResult hr = plugin_package->CreateFeatureInterface(0, factory_base.Put());
```

Do not receive into a raw pointer and then construct `DasPtr(raw)`:

```cpp
IDasBase* factory_base_raw = nullptr;
plugin_package->CreateFeatureInterface(0, &factory_base_raw);
DAS::DasPtr<IDasBase> factory_base(factory_base_raw); // Adds an extra ref.
```

The bug pattern above leaks one reference. In IPC plugin tests it can keep
remote objects alive and make lifecycle release checks fail.

## QueryInterface And As

Prefer the `Put()` shape when a local `DasPtr` receives a QI result:

```cpp
DAS::DasPtr<IDasComponentFactory> factory;
DasResult hr = factory_base.As(factory.Put());
```

`QueryInterface` returns a new owned reference on success. If using raw
`QueryInterface`, immediately receive through `PutVoid()` or through an out-param
owned by a RAII helper.

## Attach

`DasPtr<T>::Attach(raw)` adopts an already owned reference without `AddRef()`.
Use it only at a boundary where the ownership contract is explicit and there is
no better `Put()` call site.

Acceptable examples:

- wrapping a raw pointer returned by an async result contract that transfers
  ownership
- adopting `new Impl(...)` only when that implementation starts with an owned
  reference for the caller
- inside `DasPtr::As(DasPtr<Other>&)`, because QI already added the reference

Do not use `Attach()` for borrowed pointers. Keep borrowed pointers raw, or use
`DasPtr(raw)` only when taking an intentional extra reference.

## Members And Async Callbacks

Do not store an owning interface as a raw member unless the lifetime is proven by
another owner and documented locally. For callbacks posted across threads or IPC
turns, store a `DasPtr<T>` member and pass `.Get()` only for the immediate call:

```cpp
DAS::DasPtr<IDasAsyncCallback> completion_signal_;
ctx->PostCallback(completion_signal_.Get());
```

This prevents use-after-free when a callback arrives after a stack-local owner
has been destroyed.

## Returning Through T**

When implementing a function that receives `T** pp_out`, prefer `DasOutPtr<T>`
for error-path safety and explicit `Keep()` transfer. Use the `das-outptr` skill
for that implementation pattern.

## Verification

After changing ownership around factories, QI, proxies, plugin loading, or
lifecycle callbacks, run the focused IPC/plugin test that exercises release
behavior. For plugin packages, include a lifecycle-release check when possible.

## Maintenance

When `include/das/DasPtr.hpp`, interface out-param conventions, or IPC proxy
ownership contracts change, update this skill and `das-outptr` together.
