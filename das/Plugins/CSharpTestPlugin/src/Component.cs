using Das.Generated;
using Das.Generated.Abi;
using Das.Generated.Wrappers;

namespace Das.TestPlugin;

public sealed class Component : IDasComponent
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

    private readonly LifecycleFixtures lifecycle = new();

    public DasResult RecordDispatch(string functionName)
    {
        if (functionName is null)
        {
            return DasResult.DAS_E_INVALID_ARGUMENT;
        }

        lifecycle.Record(functionName);
        return DasResult.DAS_S_OK;
    }

    public override (DasResult Result, DasGuid Guid) GetGuid()
    {
        return (DasResult.DAS_S_OK, ComponentIid);
    }

    public override (DasResult Result, IDasVariantVector ResultValue) Dispatch(
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

    public override void Dispose()
    {
        lifecycle.Clear();
        base.Dispose();
    }

    private static (DasResult Result, IDasVariantVector ResultValue) DispatchFailure(
        DasResult result)
    {
        return (result, null!);
    }

    private (DasResult Result, IDasVariantVector ResultValue)
    DispatchBridgeLifecycleTest(
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

        using var lifecycleComponent = new LifecycleComponent(
            callbackResult.Component,
            marker,
            lifecycle);
        var pushComponentResult = result.PushBackComponent(lifecycleComponent);
        if ((int)pushComponentResult < 0)
        {
            lifecycle.CancelLastTrackedDirector();
            return DispatchFailure(pushComponentResult);
        }

        return (DasResult.DAS_S_OK, result);
    }

    private sealed class LifecycleComponent : IDasComponent
    {
        private readonly IDasComponent callback;
        private readonly string marker;
        private readonly LifecycleFixtures lifecycle;

        public LifecycleComponent(
            IDasComponent callback,
            string marker,
            LifecycleFixtures lifecycle)
        {
            this.callback = callback;
            this.marker = marker;
            this.lifecycle = lifecycle;
            lifecycle.Track(this);
        }

        public override (DasResult Result, DasGuid Guid) GetGuid()
        {
            return (DasResult.DAS_S_OK, ComponentIid);
        }

        public override (DasResult Result, IDasVariantVector ResultValue) Dispatch(
            DasReadOnlyString functionName,
            IDasVariantVector arguments)
        {
            return DispatchFailure(DasResult.DAS_E_NO_IMPLEMENTATION);
        }

        protected internal override void OnFinalRelease()
        {
            lifecycle.Release(this);
            DispatchReleaseCallback();
        }

        private void DispatchReleaseCallback()
        {
            var arguments = IDasVariantVector.Create();
            var pushResult = arguments.PushBackString(
                $"bridge_released:CSharp:{marker}");
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
