#ifndef DAS_CORE_FOREIGNINTERFACEHOST_TEMPORARYPLUGINOBJECTSTORAGE_H
#define DAS_CORE_FOREIGNINTERFACEHOST_TEMPORARYPLUGINOBJECTSTORAGE_H

#include <das/Core/ForeignInterfaceHost/Config.h>
#include <das/Core/ForeignInterfaceHost/IForeignLanguageRuntime.h>
#include <das/PluginInterface/IDasPlugin.h>
#include <mutex>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

class TemporaryPluginObjectStorage
{
    std::mutex             mutex_{};
    DasPtr<IDasSwigPlugin> p_plugin_{};

    friend DasResult(::DasRegisterPluginObject)(
        DasResult       error_code,
        IDasSwigPlugin* p_swig_plugin);

    void ObtainOwnership();
    void ReleaseOwnership();

public:
    class TemporaryPluginObjectStorageReader
    {
        TemporaryPluginObjectStorage& storage_;

    public:
        TemporaryPluginObjectStorageReader(
            TemporaryPluginObjectStorage& storage);
        ~TemporaryPluginObjectStorageReader();

        auto GetObject() -> DasPtr<IDasSwigPlugin>;
    };

    friend TemporaryPluginObjectStorageReader;

    auto GetOwnership() -> TemporaryPluginObjectStorageReader;
};

extern TemporaryPluginObjectStorage g_plugin_object;

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_CORE_FOREIGNINTERFACEHOST_TEMPORARYPLUGINOBJECTSTORAGE_H
