namespace Das.TestPlugin;

public sealed class LifecycleFixtures
{
    private readonly List<string> calls = [];

    public IReadOnlyList<string> Calls => calls;

    public void Record(string name)
    {
        calls.Add(name);
    }

    public void Clear()
    {
        calls.Clear();
    }
}
