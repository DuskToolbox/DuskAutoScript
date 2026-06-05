using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace Das.TestPlugin;

public static class Bootstrap
{
#if !NETFRAMEWORK
    private static bool resolverInstalled;

    private static void EnsureNativeResolver()
    {
        if (resolverInstalled)
        {
            return;
        }

        NativeLibrary.SetDllImportResolver(
            typeof(Das.Generated.Wrappers.IDasBase).Assembly,
            ResolveNativeLibrary);
        resolverInstalled = true;
    }

    private static IntPtr ResolveNativeLibrary(
        string libraryName,
        Assembly assembly,
        DllImportSearchPath? searchPath)
    {
        _ = assembly;
        _ = searchPath;

        var pluginDir = Path.GetDirectoryName(typeof(Bootstrap).Assembly.Location)
                        ?? AppContext.BaseDirectory;
        var appDir = AppContext.BaseDirectory;

        if (libraryName == "DasCSharpNativeSupport")
        {
            return LoadFirstExisting(
                Path.Combine(pluginDir, "DasCSharpNativeSupport.dll"));
        }

        if (libraryName == "das")
        {
            return LoadFirstExisting(
                Path.Combine(appDir, "libDasCore.dll"),
                Path.Combine(pluginDir, "libDasCore.dll"),
                Path.Combine(appDir, "das.dll"),
                Path.Combine(pluginDir, "das.dll"));
        }

        return IntPtr.Zero;
    }

    private static IntPtr LoadFirstExisting(params string[] paths)
    {
        foreach (var path in paths)
        {
            if (!File.Exists(path))
            {
                continue;
            }

            if (NativeLibrary.TryLoad(path, out var handle))
            {
                return handle;
            }
        }

        return IntPtr.Zero;
    }
#endif

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
        EnsureNativeResolver();
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
