using Das.Generated;
using Das.Generated.Abi;
using Das.Generated.Directors;
using Das.Generated.Results;
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
        if (package is null)
        {
            throw new ArgumentNullException(nameof(package));
        }

        return IDasPluginPackageDirector.Create(new PackageCallbacks(package));
    }

    public static IDasComponentFactory CreateComponentFactory(PluginPackage package)
    {
        if (package is null)
        {
            throw new ArgumentNullException(nameof(package));
        }

        return IDasComponentFactoryDirector.Create(new ComponentFactoryCallbacks(package));
    }

    public static IDasComponent CreateComponent(Component component)
    {
        if (component is null)
        {
            throw new ArgumentNullException(nameof(component));
        }

        return IDasComponentDirector.Create(new ComponentCallbacks(component));
    }

    public static IDasComponent CreateLifecycleComponent(
        IDasComponent callback,
        string marker,
        LifecycleFixtures lifecycle)
    {
        if (callback is null)
        {
            throw new ArgumentNullException(nameof(callback));
        }

        if (marker is null)
        {
            throw new ArgumentNullException(nameof(marker));
        }

        if (lifecycle is null)
        {
            throw new ArgumentNullException(nameof(lifecycle));
        }

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

        public IDasPluginPackageEnumFeatureResult EnumFeature(ulong index)
        {
            if (index == 0)
            {
                return new IDasPluginPackageEnumFeatureResult(
                    DasResult.DAS_S_OK,
                    DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
            }

            return new IDasPluginPackageEnumFeatureResult(
                DasResult.DAS_E_OUT_OF_RANGE,
                default);
        }

        public IDasPluginPackageCreateFeatureInterfaceResult
        CreateFeatureInterface(ulong index)
        {
            if (index != 0)
            {
                return new IDasPluginPackageCreateFeatureInterfaceResult(
                    DasResult.DAS_E_OUT_OF_RANGE,
                    null!);
            }

            try
            {
                var factory = CreateComponentFactory(package);
                return new IDasPluginPackageCreateFeatureInterfaceResult(
                    DasResult.DAS_S_OK,
                    factory);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"CSharpTestPlugin CreateFeatureInterface failed: {ex}");
                return new IDasPluginPackageCreateFeatureInterfaceResult(
                    DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED,
                    null!);
            }
        }

        public IDasPluginPackageCanUnloadNowResult CanUnloadNow()
        {
            return new IDasPluginPackageCanUnloadNowResult(
                DasResult.DAS_S_OK,
                package.CanUnloadNow);
        }
    }

    private sealed class ComponentFactoryCallbacks : IDasComponentFactoryDirectorCallbacks
    {
        private readonly PluginPackage package;

        public ComponentFactoryCallbacks(PluginPackage package)
        {
            this.package = package;
        }

        public IDasComponentFactoryGetGuidResult GetGuid()
        {
            return new IDasComponentFactoryGetGuidResult(
                DasResult.DAS_S_OK,
                ComponentIid);
        }

        public IDasComponentFactoryGetRuntimeClassNameResult GetRuntimeClassName()
        {
            return new IDasComponentFactoryGetRuntimeClassNameResult(
                DasResult.DAS_E_NO_IMPLEMENTATION,
                null!);
        }

        public DasResult IsSupported(DasGuid componentIid)
        {
            return GuidEquals(componentIid, ComponentIid)
                ? DasResult.DAS_S_OK
                : DasResult.DAS_E_NO_INTERFACE;
        }

        public IDasComponentFactoryCreateInstanceResult CreateInstance(
            DasGuid componentIid)
        {
            if (!GuidEquals(componentIid, ComponentIid))
            {
                return new IDasComponentFactoryCreateInstanceResult(
                    DasResult.DAS_E_NO_INTERFACE,
                    null!);
            }

            try
            {
                var component = CreateComponent(package.CreateComponent());
                return new IDasComponentFactoryCreateInstanceResult(
                    DasResult.DAS_S_OK,
                    component);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(
                    $"CSharpTestPlugin CreateInstance failed: {ex}");
                return new IDasComponentFactoryCreateInstanceResult(
                    DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED,
                    null!);
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

        public IDasComponentGetGuidResult GetGuid()
        {
            return new IDasComponentGetGuidResult(
                DasResult.DAS_S_OK,
                ComponentIid);
        }

        public IDasComponentGetRuntimeClassNameResult GetRuntimeClassName()
        {
            return new IDasComponentGetRuntimeClassNameResult(
                DasResult.DAS_E_NO_IMPLEMENTATION,
                null!);
        }

        public IDasComponentDispatchResult Dispatch(
            DasReadOnlyString functionName,
            IDasVariantVector arguments)
        {
            return component.Dispatch(functionName, arguments);
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

        public IDasComponentGetGuidResult GetGuid()
        {
            return new IDasComponentGetGuidResult(
                DasResult.DAS_S_OK,
                ComponentIid);
        }

        public IDasComponentGetRuntimeClassNameResult GetRuntimeClassName()
        {
            return new IDasComponentGetRuntimeClassNameResult(
                DasResult.DAS_E_NO_IMPLEMENTATION,
                null!);
        }

        public IDasComponentDispatchResult Dispatch(
            DasReadOnlyString functionName,
            IDasVariantVector arguments)
        {
            return new IDasComponentDispatchResult(
                DasResult.DAS_E_NO_IMPLEMENTATION,
                null!);
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
