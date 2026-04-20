#ifndef DAS_PLUGIN_MANAGER_SERVICE_H
#define DAS_PLUGIN_MANAGER_SERVICE_H

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/IDasBase.h>
#include <span>

DAS_DEFINE_GUID(
    DAS_IID_PLUGIN_MANAGER_SERVICE,
    IDasPluginManagerService,
    0xB2C3D4E5,
    0xF6A7,
    0x4B8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E)

DAS_SWIG_EXPORT_ATTRIBUTE(IDasPluginManagerService)
DAS_INTERFACE IDasPluginManagerService : public IDasBase
{
    DAS_METHOD_(Das::Core::ForeignInterfaceHost::ComponentFactoryManager&)
    GetComponentFactoryManager() = 0;
    DAS_METHOD_(std::span<Das::Core::ForeignInterfaceHost::FeatureInfo* const>)
    GetFeaturesByType(Das::PluginInterface::DasPluginFeature type) const = 0;
    DAS_METHOD_(Das::Core::ForeignInterfaceHost::PluginPackageDesc*)
    FindPluginPackageByGuid(const DasGuid& guid) = 0;
};

#endif // DAS_PLUGIN_MANAGER_SERVICE_H
