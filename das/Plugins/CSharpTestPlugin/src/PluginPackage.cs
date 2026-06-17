using Das.Generated;
using Das.Generated.Abi;
using Das.Generated.Wrappers;

namespace Das.TestPlugin;

public sealed class PluginPackage : IDasPluginPackage
{
    private readonly Component component = new();
    private readonly ComponentFactory componentFactory;

    public PluginPackage()
    {
        componentFactory = new ComponentFactory(this);
    }

    public string Name => "CSharpTestPlugin";

    public DasResult Initialize()
    {
        return DasResult.DAS_S_OK;
    }

    public Component CreateComponent()
    {
        return component;
    }

    public override (DasResult Result, DasPluginFeature Feature) EnumFeature(
        ulong index)
    {
        if (index == 0)
        {
            return (
                DasResult.DAS_S_OK,
                DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY);
        }

        return (DasResult.DAS_E_OUT_OF_RANGE, default);
    }

    public override (DasResult Result, IDasBase Interface) CreateFeatureInterface(
        ulong index)
    {
        if (index != 0)
        {
            return (DasResult.DAS_E_OUT_OF_RANGE, null!);
        }

        return (DasResult.DAS_S_OK, componentFactory);
    }

    public override void Dispose()
    {
        component.Dispose();
        componentFactory.Dispose();
        base.Dispose();
    }

    private sealed class ComponentFactory : IDasComponentFactory
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

        private readonly PluginPackage package;

        public ComponentFactory(PluginPackage package)
        {
            this.package = package;
        }

        public override (DasResult Result, DasGuid Guid) GetGuid()
        {
            return (DasResult.DAS_S_OK, ComponentIid);
        }

        public override DasResult IsSupported(DasGuid componentIid)
        {
            return GuidEquals(componentIid, ComponentIid)
                ? DasResult.DAS_S_OK
                : DasResult.DAS_E_NO_INTERFACE;
        }

        public override (DasResult Result, IDasComponent Component) CreateInstance(
            DasGuid componentIid)
        {
            if (!GuidEquals(componentIid, ComponentIid))
            {
                return (DasResult.DAS_E_NO_INTERFACE, null!);
            }

            return (DasResult.DAS_S_OK, package.CreateComponent());
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
    }
}
