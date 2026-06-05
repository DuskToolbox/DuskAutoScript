using Das.Generated;

namespace Das.TestPlugin;

public sealed class PluginPackage : IDisposable
{
    private readonly Component component = new();

    public string Name => "CSharpTestPlugin";

    public DasResult Initialize()
    {
        return DasResult.DAS_S_OK;
    }

    public Component CreateComponent()
    {
        return component;
    }

    public bool CanUnloadNow => component.ActiveLifecycleDirectorCount == 0;

    public void Dispose()
    {
        component.Dispose();
    }
}
