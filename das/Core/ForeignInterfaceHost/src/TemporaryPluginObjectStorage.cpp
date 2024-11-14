#include "TemporaryPluginObjectStorage.h"

Das::Core::ForeignInterfaceHost::TemporaryPluginObjectStorage::
    TemporaryPluginObjectStorageReader::TemporaryPluginObjectStorageReader(
        TemporaryPluginObjectStorage& storage)
    : storage_{storage}
{
    storage_.ObtainOwnership();
}

Das::Core::ForeignInterfaceHost::TemporaryPluginObjectStorage::
    TemporaryPluginObjectStorageReader::~TemporaryPluginObjectStorageReader()
{
    storage_.ReleaseOwnership();
}

auto Das::Core::ForeignInterfaceHost::TemporaryPluginObjectStorage::
    TemporaryPluginObjectStorageReader::GetObject() -> DasPtr<IDasSwigPlugin>
{
    return std::exchange(storage_.p_plugin_, {});
}

auto Das::Core::ForeignInterfaceHost::TemporaryPluginObjectStorage::
    GetOwnership() -> Das::Core::ForeignInterfaceHost::
        TemporaryPluginObjectStorage::TemporaryPluginObjectStorageReader
{
    return {*this};
}

void Das::Core::ForeignInterfaceHost::TemporaryPluginObjectStorage::
    ObtainOwnership()
{
    mutex_.lock();
}

void Das::Core::ForeignInterfaceHost::TemporaryPluginObjectStorage::
    ReleaseOwnership()
{
    p_plugin_ = {};
    mutex_.unlock();
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

DAS_DEFINE_VARIABLE(g_plugin_object){};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END