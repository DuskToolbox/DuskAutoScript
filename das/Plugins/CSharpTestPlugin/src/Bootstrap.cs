using System;

namespace Das.TestPlugin;

public static class Bootstrap
{
#if NETFRAMEWORK
    public static int Create(string bootstrapCookie)
    {
        return (int)Das.Generated.Runtime.DasCSharpBootstrap.Invoke(
            bootstrapCookie,
            CreatePackage);
    }
#else
    public static int Create(IntPtr args, int sizeBytes)
    {
        return (int)Das.Generated.Runtime.DasCSharpBootstrap.Invoke(
            args,
            sizeBytes,
            CreatePackage);
    }
#endif

    private static object CreatePackage()
    {
        return GeneratedPackageFactory.CreatePackage(new PluginPackage());
    }
}
