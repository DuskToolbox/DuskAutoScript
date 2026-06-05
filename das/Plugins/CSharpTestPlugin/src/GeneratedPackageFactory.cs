using Das.Generated;
using Das.Generated.Abi;
using Das.Generated.Directors;
using Das.Generated.Wrappers;

namespace Das.TestPlugin;

public static class GeneratedPackageFactory
{
    private static readonly DasGuid ComponentIid = new(
        0x15FF0855,
        0xE031,
        0x4602,
        0x82,
        0x9D,
        0x04,
        0x02,
        0x30,
        0x51,
        0x5C,
        0x55);

    public static IDasPluginPackage CreatePackage(PluginPackage package)
    {
        ArgumentNullException.ThrowIfNull(package);
        return IDasPluginPackageDirector.Create(new PackageCallbacks(package));
    }

    public static IDasComponentFactory CreateComponentFactory(PluginPackage package)
    {
        ArgumentNullException.ThrowIfNull(package);
        return IDasComponentFactoryDirector.Create(new ComponentFactoryCallbacks(package));
    }

    public static IDasComponent CreateComponent(Component component)
    {
        ArgumentNullException.ThrowIfNull(component);
        return IDasComponentDirector.Create(new ComponentCallbacks(component));
    }

    public static IDasComponent CreateLifecycleComponent(
        IDasComponent callback,
        string marker,
        LifecycleFixtures lifecycle)
    {
        ArgumentNullException.ThrowIfNull(callback);
        ArgumentNullException.ThrowIfNull(marker);
        ArgumentNullException.ThrowIfNull(lifecycle);

        var callbacks = new LifecycleComponentCallbacks(callback, marker, lifecycle);
        lifecycle.Track(callbacks);
        return IDasComponentDirector.Create(callbacks);
    }

    private static bool GuidEquals(DasGuid left, DasGuid right)
    {
        return left.Data1 == right.Data1
            && left.Data2 == right.Data2
            && left.Data3 == right.Data3
            && left.Data4_0 == right.Data4_0
            && left.Data4_1 == right.Data4_1
            && left.Data4_2 == right.Data4_2
            && left.Data4_3 == right.Data4_3
            && left.Data4_4 == right.Data4_4
            && left.Data4_5 == right.Data4_5
            && left.Data4_6 == right.Data4_6
            && left.Data4_7 == right.Data4_7;
    }

    private sealed class PackageCallbacks : IDasPluginPackageDirectorCallbacks
    {
        private readonly PluginPackage package;

        public PackageCallbacks(PluginPackage package)
        {
            this.package = package;
        }

        public DasResult EnumFeature(ulong index, out DasPluginFeature feature)
        {
            if (index == 0)
            {
                feature = DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY;
                return DasResult.DAS_S_OK;
            }

            feature = default;
            return DasResult.DAS_E_OUT_OF_RANGE;
        }

        public DasResult CreateFeatureInterface(ulong index, out IntPtr interfaceHandle)
        {
            if (index != 0)
            {
                interfaceHandle = IntPtr.Zero;
                return DasResult.DAS_E_OUT_OF_RANGE;
            }

            try
            {
                var factory = CreateComponentFactory(package);
                interfaceHandle = factory.Handle;
                return interfaceHandle == IntPtr.Zero
                    ? DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED
                    : DasResult.DAS_S_OK;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"CSharpTestPlugin CreateFeatureInterface failed: {ex}");
                interfaceHandle = IntPtr.Zero;
                return DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;
            }
        }

        public DasResult CanUnloadNow(out bool canUnloadNow)
        {
            canUnloadNow = package.CanUnloadNow;
            return DasResult.DAS_S_OK;
        }
    }

    private sealed class ComponentFactoryCallbacks : IDasComponentFactoryDirectorCallbacks
    {
        private readonly PluginPackage package;

        public ComponentFactoryCallbacks(PluginPackage package)
        {
            this.package = package;
        }

        public DasResult GetGuid(out DasGuid guid)
        {
            guid = ComponentIid;
            return DasResult.DAS_S_OK;
        }

        public DasResult GetRuntimeClassName(out IntPtr nameHandle)
        {
            nameHandle = IntPtr.Zero;
            return DasResult.DAS_E_NO_IMPLEMENTATION;
        }

        public DasResult IsSupported(DasGuid componentIid)
        {
            return GuidEquals(componentIid, ComponentIid)
                ? DasResult.DAS_S_OK
                : DasResult.DAS_E_NO_INTERFACE;
        }

        public DasResult CreateInstance(DasGuid componentIid, out IntPtr componentHandle)
        {
            if (!GuidEquals(componentIid, ComponentIid))
            {
                componentHandle = IntPtr.Zero;
                return DasResult.DAS_E_NO_INTERFACE;
            }

            try
            {
                var component = CreateComponent(package.CreateComponent());
                componentHandle = component.Handle;
                return componentHandle == IntPtr.Zero
                    ? DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED
                    : DasResult.DAS_S_OK;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"CSharpTestPlugin CreateInstance failed: {ex}");
                componentHandle = IntPtr.Zero;
                return DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED;
            }
        }
    }

    private sealed class ComponentCallbacks : IDasComponentDirectorCallbacks
    {
        private readonly Component component;

        public ComponentCallbacks(Component component)
        {
            this.component = component;
        }

        public DasResult GetGuid(out DasGuid guid)
        {
            guid = ComponentIid;
            return DasResult.DAS_S_OK;
        }

        public DasResult GetRuntimeClassName(out IntPtr nameHandle)
        {
            nameHandle = IntPtr.Zero;
            return DasResult.DAS_E_NO_IMPLEMENTATION;
        }

        public DasResult Dispatch(
            DasReadOnlyString functionName,
            IDasVariantVector arguments,
            out IntPtr resultHandle)
        {
            return component.Dispatch(functionName, arguments, out resultHandle);
        }
    }

    private sealed class LifecycleComponentCallbacks :
        IDasComponentDirectorCallbacks,
        IDasComponentDirectorFinalReleaseCallbacks
    {
        private readonly IDasComponent callback;
        private readonly string marker;
        private readonly LifecycleFixtures lifecycle;

        public LifecycleComponentCallbacks(
            IDasComponent callback,
            string marker,
            LifecycleFixtures lifecycle)
        {
            this.callback = callback;
            this.marker = marker;
            this.lifecycle = lifecycle;
        }

        public DasResult GetGuid(out DasGuid guid)
        {
            guid = ComponentIid;
            return DasResult.DAS_S_OK;
        }

        public DasResult GetRuntimeClassName(out IntPtr nameHandle)
        {
            nameHandle = IntPtr.Zero;
            return DasResult.DAS_E_NO_IMPLEMENTATION;
        }

        public DasResult Dispatch(
            DasReadOnlyString functionName,
            IDasVariantVector arguments,
            out IntPtr resultHandle)
        {
            resultHandle = IntPtr.Zero;
            return DasResult.DAS_E_NO_IMPLEMENTATION;
        }

        public void OnFinalRelease()
        {
            lifecycle.Release(this);
            DispatchReleaseCallback();
        }

        private void DispatchReleaseCallback()
        {
            var arguments = IDasVariantVector.Create();
            var pushResult = arguments.PushBackString($"bridge_released:CSharp:{marker}");
            if ((int)pushResult < 0)
            {
                lifecycle.Record($"bridge_release_failed:CSharp:{marker}");
                return;
            }

            var dispatchResult = callback.Dispatch("lifecycle_callback", arguments);
            if ((int)dispatchResult.Result < 0)
            {
                lifecycle.Record($"bridge_release_failed:CSharp:{marker}");
            }
        }
    }
}
