using Das.Generated;
using Das.Generated.Wrappers;

namespace Das.TestPlugin;

public sealed class Component : IDisposable
{
    private readonly LifecycleFixtures lifecycle = new();

    public DasResult Dispatch(string functionName)
    {
        if (functionName is null)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        lifecycle.Record(functionName);
        return DasResult.DAS_S_OK;
    }

    public DasResult Attach(IDasBase baseObject)
    {
        if (baseObject is null)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        return baseObject.Handle == IntPtr.Zero
            ? DasResult.DAS_E_INVALID_ARGUMENT
            : DasResult.DAS_S_OK;
    }

    public IReadOnlyList<string> Calls => lifecycle.Calls;

    public void Dispose()
    {
        lifecycle.Clear();
    }
}
