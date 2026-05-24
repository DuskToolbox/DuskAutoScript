---
name: das-foreign-runtime-plugin
description: Use when implementing DAS generated language-runtime plugin bridges or generated bindings that expose DAS COM-style interfaces through IPC. Apply for host/plugin E2E, runtime layout assembly, manifest entryPoint loading, director callbacks, wrapper type conversion, and IpcMultiProcessTest coverage.
paths:
  - "tools/das_idl/**/*"
  - "das/Core/ForeignInterfaceHost/**/*"
  - "das/Plugins/*TestPlugin/**/*"
  - "das/IpcMultiProcessTest/**/*"
  - "cmake/DasAddIdlExport.cmake"
  - "cmake/Find*.cmake"
---

# DAS Generated Runtime Plugin Bridge

## Maintenance

This skill records runtime-agnostic constraints for DAS generated language bridges. Keep runtime-specific examples in phase artifacts or dedicated skills. If generator shape, host bootstrap, plugin/runtime layout, manifest schema, or `IpcMultiProcessTest` conventions change, update this skill in the same change.

## When To Apply

Use this skill when a generated binding must expose DAS COM-style interfaces across IPC and prove the path with a real plugin E2E.

Before editing, read the relevant generator, host bootstrap, source-owned test plugin, CMake copy target, and `IpcMultiProcessTestIntegration` tests.

## Core Rules

### Rule 1: Real Plugin E2E Is The Proof

Do not use a standalone language-loader smoke test or minimal dummy package as the phase deliverable. The required proof is:

```text
LoadPluginAsync -> IDasPluginPackage -> EnumFeature -> CreateFeatureInterface
-> IDasComponentFactory -> IDasComponent -> Dispatch
```

Keep the focused E2E gate narrow and explicit. Full CTest should not become the only gate when known unrelated residuals exist.

Compatibility fallbacks may remain for older manifests, but they must not be the proof path. E2E acceptance must load the source-owned package entry and exercise real plugin code.

### Rule 2: Host Runners Must Not Block The Language Thread

Director callbacks often require the language runtime thread to stay available for callback dispatch, finalizers, event loops, or synchronization contexts. Running host IPC synchronously on that thread can deadlock real director E2E. Run IPC on an async/background runner and keep the language runtime thread available.

### Rule 3: Manifest Entry Is Package-Relative

Use `entryPoint`, not legacy temporary `entry`.

The entry must be package-root-relative and point to a package-local factory. Distribution details belong in the runtime package layout, not in ad hoc absolute paths inside tests.

The host process working directory and runtime root must match the manifest package root unless the runtime has an explicit equivalent. Otherwise bindings, runtime DLLs, package metadata, or relative entry files can resolve from stale build output.

### Rule 4: Source-Owned Test Package Plus Explicit Runtime Layout

Test packages must live in source, not be generated ad hoc inside tests. CMake must assemble a deterministic runtime layout that keeps these roles explicit:

- plugin package root: manifest, package-local entry file, source-owned package metadata, and package-owned assets
- binding/runtime root: generated public bindings, host/bootstrap files, native addons, runtime DLLs, and generated metadata

Do not assume generated binding/runtime artifacts live inside the plugin package root. If a runtime needs them there temporarily, record it as transitional technical debt in the phase artifacts and test the intended normalized layout separately.

The runtime copy target must be buildable independently and should assemble every file the host process needs at runtime. Tests should resolve the source-owned manifest through `IpcTestConfig`, then validate both the plugin package root and the binding/runtime root without constructing temporary plugin files.

### Rule 5: Public Interface Objects Are The API Boundary

Generated public interface objects are what plugin authors should return or pass. Internal native/runtime handles are implementation details.

Implicit interface conversion must support same-type and upcast through the IDL inheritance chain:

- normal interface parameters: borrow the same object, no `AddRef`
- director interface out returns: reuse the same object, static upcast, then `AddRef` for the out-param owner
- no implicit downcast or sidecast; no `QueryInterface` for implicit upcast

Explicit conversion APIs may use `QueryInterface` and return a new public interface object for the target interface.

All `language value -> DAS interface pointer` entry points must share this conversion layer. Do not implement separate exact-wrapper paths for method inputs, function inputs, director returns, and loader returns.

### Rule 6: Language Exceptions Must Not Kill The Host

Language exceptions and type-conversion failures must become DAS error results or explicit failed operations. They must not terminate the host process or leave the IPC caller waiting until timeout.

Focused E2E should include at least one error path and prove the host remains usable afterward.

### Rule 7: Retain Language Objects Behind Native Interfaces

When a host loader unwraps a language object to `IDasPluginPackage` or another DAS interface, retain the language object for at least the lifetime of the native interface. Otherwise GC can collect the callback object while IPC still holds the native pointer.

Each runtime must define its own object lifetime holder before E2E.

## Runtime Reuse Checklist

- Start from the verified vertical-slice shape, not from older binding assumptions.
- Define wrapper/director object model, implicit same/upcast rules, and explicit QI conversion before tests.
- Keep package files source-owned, and make binding/runtime artifact placement explicit in CMake.
- Add focused `IpcMultiProcessTestIntegration` coverage, including lifecycle/director callbacks, before broad routing.
- Freeze a focused baseline gate before replacing or broadening generator behavior.

## Focused Verification Pattern

Always build the generated binding target, test plugin copy target, and `IpcMultiProcessTest` target in the focused gate. The focused gtest filter must include host launch, plugin package QI, real plugin load/dispatch, lifecycle/director callback coverage, and one non-fatal error path.
