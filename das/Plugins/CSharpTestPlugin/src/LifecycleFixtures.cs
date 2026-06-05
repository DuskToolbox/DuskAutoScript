namespace Das.TestPlugin;

public sealed class LifecycleFixtures
{
    private readonly List<string> calls = [];
    private readonly HashSet<object> activeDirectors = [];

    public IReadOnlyList<string> Calls => calls;

    public int ActiveDirectorCount => activeDirectors.Count;

    public void Record(string name)
    {
        calls.Add(name);
    }

    public void Track(object director)
    {
        activeDirectors.Add(director);
    }

    public void Release(object director)
    {
        activeDirectors.Remove(director);
    }

    public void CancelLastTrackedDirector()
    {
        if (activeDirectors.Count == 0)
        {
            return;
        }

        activeDirectors.Remove(activeDirectors.Last());
    }

    public void Clear()
    {
        calls.Clear();
        activeDirectors.Clear();
    }
}
