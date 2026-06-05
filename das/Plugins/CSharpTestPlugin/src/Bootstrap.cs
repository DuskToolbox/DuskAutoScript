namespace Das.TestPlugin;

public static class Bootstrap
{
    public static PluginPackage Create()
    {
        return new PluginPackage();
    }

    public static int Main(string[] args)
    {
        return args.Length >= 0 ? 0 : 1;
    }
}
