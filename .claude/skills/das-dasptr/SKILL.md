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

## ImplBase Factory: Prefer Make()

Every IDL interface implementation generated with the `DasXxxImplBase<TImpl>`
CRTP base exposes two static factories:

- `TImpl::Make(args...)` → `DasPtr<IInterface>` — new + AddRef + Attach in one
  step, returns the interface smart pointer
- `TImpl::MakeRaw(args...)` → `TImpl*` — new + AddRef, returns the concrete
  raw pointer

Prefer `Make()` for C++ internal creation — it hands back a ready `DasPtr`
with no separate `Attach`:

```cpp
auto cv = CvCpuImpl::Make();                       // DasPtr<IDasCv>, no Attach
auto decorated = MaybeDecorateCvRaw(std::move(cv), "cv.cpu");
```

Use `MakeRaw()` + `Attach` only when a downstream consumer needs the
**concrete implementation type** — e.g. the class multiply-inherits a
non-interface mixin and a helper takes `TImpl*`:

```cpp
// DebugWriterImpl : DasDebugWriterImplBase<...>, IDebugSink, IDebugDrain
// MakeSinkRef(DebugWriterImpl*) requires the concrete type.
auto writer = Das::DasPtr<DebugWriterImpl>::Attach(
    DebugWriterImpl::MakeRaw(DebugRuntime::DebugDir()));
```

`Make()` returns the interface type and would drop the concrete type such
helpers need.

## Attach

`DasPtr<T>::Attach(raw)` adopts an already owned reference without `AddRef()`.
Use it only at a genuine boundary where ownership is explicit and there is no
better factory or `Put()` call site.

Acceptable boundaries:

- a raw pointer transferred by an async-result or IPC contract
- inside `DasPtr::As(DasPtr<Other>&)`, because QI already added the reference
- `MakeRaw()` + `Attach` for an ImplBase class whose consumer needs the
  concrete type (see above)

For ordinary ImplBase creation prefer `Make()`; for out-parameters prefer
`Put()`. Do not `Attach()` borrowed pointers — keep them raw, or use
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

## Do Not Return Raw Interface Pointers

A C++ function must not return a `IDasXxx*` raw pointer that forces the caller
to `Attach`. Return ownership explicitly instead:

- C++ internal helper → return `DasPtr<T>` (`std::move` / move-semantics
  transfer ownership to the caller)
- IDL / ABI / interface method → use a `[out] T**` parameter (caller receives
  via `Put()`), typically with a `DasResult` return code

```cpp
// C++ internal helper — return DasPtr
Das::DasPtr<Das::ExportInterface::IDasCv>
MaybeDecorateCvRaw(Das::DasPtr<Das::ExportInterface::IDasCv> cv, const char* name);

// IDL / ABI boundary — [out] parameter + DasResult
DasResult QueryInterface(const DasGuid& iid, void** pp_out);
```

This confines every `Attach` to genuine boundaries (transferred raw pointers,
`MakeRaw` for a concrete type, framework internals) and lets the rest of the
code flow `DasPtr` through `std::move` / `Put` with zero `Attach`.

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
