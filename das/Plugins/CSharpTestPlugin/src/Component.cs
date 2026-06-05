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

    public DasResult Dispatch(
        DasReadOnlyString functionName,
        IDasVariantVector arguments,
        out IntPtr resultHandle)
    {
        resultHandle = IntPtr.Zero;
        if (functionName is null || functionName.Handle == IntPtr.Zero)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        if (arguments is null || arguments.Handle == IntPtr.Zero)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        string methodName;
        try
        {
            methodName = functionName.ToManagedString();
        }
        catch
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        if (methodName != "bridgeLifecycleTest")
        {
            return DasResult.DAS_E_NO_IMPLEMENTATION;
        }

        return DispatchBridgeLifecycleTest(arguments, out resultHandle);
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

    public int ActiveLifecycleDirectorCount => lifecycle.ActiveDirectorCount;

    public void Dispose()
    {
        lifecycle.Clear();
    }

    private DasResult DispatchBridgeLifecycleTest(
        IDasVariantVector arguments,
        out IntPtr resultHandle)
    {
        resultHandle = IntPtr.Zero;

        var callbackResult = arguments.GetComponent(0);
        if ((int)callbackResult.Result < 0 || callbackResult.Component.Handle == IntPtr.Zero)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        var markerResult = arguments.GetString(1);
        if ((int)markerResult.Result < 0 || markerResult.String.Handle == IntPtr.Zero)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        string marker;
        try
        {
            marker = markerResult.String.ToManagedString();
        }
        catch
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        var result = IDasVariantVector.Create();
        var pushStatusResult = result.PushBackString($"director_created:{marker}");
        if ((int)pushStatusResult < 0)
        {
            return pushStatusResult;
        }

        var lifecycleComponent = GeneratedPackageFactory.CreateLifecycleComponent(
            callbackResult.Component,
            marker,
            lifecycle);
        var pushComponentResult = result.PushBackComponent(lifecycleComponent);
        if ((int)pushComponentResult < 0)
        {
            lifecycle.CancelLastTrackedDirector();
            return pushComponentResult;
        }

        resultHandle = result.Handle;
        return resultHandle == IntPtr.Zero
            ? DasResult.DAS_E_CSHARP_DIRECTOR_FACTORY_FAILED
            : DasResult.DAS_S_OK;
    }
}
