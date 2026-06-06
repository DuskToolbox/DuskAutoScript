using Das.Generated;
using Das.Generated.Results;
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

    public IDasComponentDispatchResult Dispatch(
        DasReadOnlyString functionName,
        IDasVariantVector arguments)
    {
        if (functionName is null)
        {
            return DispatchFailure(DasResult.DAS_E_INVALID_ARGUMENT);
        }

        if (arguments is null || !arguments.CanAssignTo("IDasVariantVector"))
        {
            return DispatchFailure(DasResult.DAS_E_INVALID_ARGUMENT);
        }

        string methodName;
        try
        {
            methodName = functionName.ToManagedString();
        }
        catch
        {
            return DispatchFailure(DasResult.DAS_E_INVALID_ARGUMENT);
        }

        if (methodName != "bridgeLifecycleTest")
        {
            return DispatchFailure(DasResult.DAS_E_NO_IMPLEMENTATION);
        }

        return DispatchBridgeLifecycleTest(arguments);
    }

    public DasResult Attach(IDasBase baseObject)
    {
        if (baseObject is null)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        return baseObject.CanAssignTo("IDasBase")
            ? DasResult.DAS_S_OK
            : DasResult.DAS_E_INVALID_ARGUMENT;
    }

    public IReadOnlyList<string> Calls => lifecycle.Calls;

    public int ActiveLifecycleDirectorCount => lifecycle.ActiveDirectorCount;

    public void Dispose()
    {
        lifecycle.Clear();
    }

    private static IDasComponentDispatchResult DispatchFailure(DasResult result)
    {
        return new IDasComponentDispatchResult(result, null!);
    }

    private IDasComponentDispatchResult DispatchBridgeLifecycleTest(
        IDasVariantVector arguments)
    {
        var callbackResult = arguments.GetComponent(0);
        if ((int)callbackResult.Result < 0 || callbackResult.Component is null)
        {
            return DispatchFailure(DasResult.DAS_E_INVALID_ARGUMENT);
        }

        var markerResult = arguments.GetString(1);
        if ((int)markerResult.Result < 0 || markerResult.String is null)
        {
            return DispatchFailure(DasResult.DAS_E_INVALID_ARGUMENT);
        }

        string marker;
        try
        {
            marker = markerResult.String.ToManagedString();
        }
        catch
        {
            return DispatchFailure(DasResult.DAS_E_INVALID_ARGUMENT);
        }

        var result = IDasVariantVector.Create();
        var pushStatusResult = result.PushBackString($"director_created:{marker}");
        if ((int)pushStatusResult < 0)
        {
            return DispatchFailure(pushStatusResult);
        }

        using var lifecycleComponent = GeneratedPackageFactory.CreateLifecycleComponent(
            callbackResult.Component,
            marker,
            lifecycle);
        var pushComponentResult = result.PushBackComponent(lifecycleComponent);
        if ((int)pushComponentResult < 0)
        {
            lifecycle.CancelLastTrackedDirector();
            return DispatchFailure(pushComponentResult);
        }

        return new IDasComponentDispatchResult(DasResult.DAS_S_OK, result);
    }
}
