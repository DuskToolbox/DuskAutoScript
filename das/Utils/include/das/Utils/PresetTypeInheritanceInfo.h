// This file is automatically generated by generator.py
// !!! DO NOT EDIT !!!

#ifndef DAS_UTILS_PRESETTYPEINHERITANCEINFO_H
#define DAS_UTILS_PRESETTYPEINHERITANCEINFO_H

#include <das/IDasBase.h>
#include <das/Utils/InternalTypeList.hpp>

DAS_UTILS_NS_BEGIN

template <class T>
struct PresetTypeInheritanceInfo;

#define DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(EndType, ...)             \
DAS_UTILS_NS_BEGIN                                                         \
using EndType##InheritanceInfo =                                           \
    DAS::Utils::internal_type_holder<__VA_ARGS__, EndType>;                \
template <>                                                                \
struct PresetTypeInheritanceInfo<EndType>                                  \
{                                                                          \
    using TypeInfo = EndType##InheritanceInfo;                             \
};                                                                         \
DAS_UTILS_NS_END

// IDasBase.h
using IDasBaseInheritanceInfo = internal_type_holder<IDasBase>;
template <>
struct PresetTypeInheritanceInfo<::IDasBase>
{
    using TypeInfo = IDasBaseInheritanceInfo;
};

using IDasSwigBaseInheritanceInfo = internal_type_holder<IDasSwigBase>;
template <>
struct PresetTypeInheritanceInfo<::IDasSwigBase>
{
    using TypeInfo = IDasSwigBaseInheritanceInfo;
};

DAS_UTILS_NS_END

DAS_INTERFACE IDasBase;
DAS_INTERFACE IDasBasicErrorLens;
DAS_INTERFACE IDasCapture;
DAS_INTERFACE IDasCaptureFactory;
DAS_INTERFACE IDasCaptureManager;
DAS_INTERFACE IDasComponent;
DAS_INTERFACE IDasComponentFactory;
DAS_INTERFACE IDasErrorLens;
DAS_INTERFACE IDasGuidVector;
DAS_INTERFACE IDasImage;
DAS_INTERFACE IDasInitializeIDasPluginManagerCallback;
DAS_INTERFACE IDasInitializeIDasPluginManagerWaiter;
DAS_INTERFACE IDasInput;
DAS_INTERFACE IDasInputFactory;
DAS_INTERFACE IDasInputFactoryVector;
DAS_INTERFACE IDasInputManager;
DAS_INTERFACE IDasJson;
DAS_INTERFACE IDasLogReader;
DAS_INTERFACE IDasLogRequester;
DAS_INTERFACE IDasMemory;
DAS_INTERFACE IDasPlugin;
DAS_INTERFACE IDasPluginInfo;
DAS_INTERFACE IDasPluginInfoVector;
DAS_INTERFACE IDasPluginManager;
DAS_INTERFACE IDasPluginManagerForUi;
DAS_INTERFACE IDasReadOnlyGuidVector;
DAS_INTERFACE IDasReadOnlyString;
DAS_INTERFACE IDasSettingsForUi;
DAS_INTERFACE IDasStopToken;
DAS_INTERFACE IDasString;
DAS_INTERFACE IDasSwigBase;
DAS_INTERFACE IDasSwigBasicErrorLens;
DAS_INTERFACE IDasSwigCapture;
DAS_INTERFACE IDasSwigCaptureManager;
DAS_INTERFACE IDasSwigComponent;
DAS_INTERFACE IDasSwigComponentFactory;
DAS_INTERFACE IDasSwigErrorLens;
DAS_INTERFACE IDasSwigGuidVector;
DAS_INTERFACE IDasSwigInput;
DAS_INTERFACE IDasSwigInputFactory;
DAS_INTERFACE IDasSwigInputFactoryVector;
DAS_INTERFACE IDasSwigPlugin;
DAS_INTERFACE IDasSwigPluginInfo;
DAS_INTERFACE IDasSwigPluginInfoVector;
DAS_INTERFACE IDasSwigPluginManager;
DAS_INTERFACE IDasSwigReadOnlyGuidVector;
DAS_INTERFACE IDasSwigStopToken;
DAS_INTERFACE IDasSwigTask;
DAS_INTERFACE IDasSwigTouch;
DAS_INTERFACE IDasSwigTypeInfo;
DAS_INTERFACE IDasSwigVariantVector;
DAS_INTERFACE IDasTask;
DAS_INTERFACE IDasTaskInfo;
DAS_INTERFACE IDasTaskInfoVector;
DAS_INTERFACE IDasTaskManager;
DAS_INTERFACE IDasTaskScheduler;
DAS_INTERFACE IDasTouch;
DAS_INTERFACE IDasTypeInfo;
DAS_INTERFACE IDasVariantVector;
DAS_INTERFACE IDasWeakReference;
DAS_INTERFACE IDasWeakReferenceSource;

DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasBasicErrorLens, IDasBase, IDasErrorLens);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasCapture, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasCaptureFactory, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasCaptureManager, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasComponent, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasComponentFactory, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasErrorLens, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasGuidVector, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasImage, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasInitializeIDasPluginManagerCallback, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasInitializeIDasPluginManagerWaiter, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasInput, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasInputFactory, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasInputFactoryVector, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasJson, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasLogReader, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasLogRequester, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasMemory, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasPlugin, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasPluginInfo, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasPluginInfoVector, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasPluginManager, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasPluginManagerForUi, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasReadOnlyGuidVector, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasReadOnlyString, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSettingsForUi, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasStopToken, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasString, IDasBase, IDasReadOnlyString);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigBasicErrorLens, IDasSwigBase, IDasSwigErrorLens);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigCapture, IDasSwigBase, IDasSwigTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigCaptureManager, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigComponent, IDasSwigBase, IDasSwigTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigComponentFactory, IDasSwigBase, IDasSwigTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigErrorLens, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigGuidVector, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigInput, IDasSwigBase, IDasSwigTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigInputFactory, IDasSwigBase, IDasSwigTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigInputFactoryVector, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigPlugin, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigPluginInfo, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigPluginInfoVector, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigPluginManager, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigReadOnlyGuidVector, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigStopToken, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigTask, IDasSwigBase, IDasSwigTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigTouch, IDasSwigBase, IDasSwigTypeInfo, IDasSwigInput);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigTypeInfo, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasSwigVariantVector, IDasSwigBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTask, IDasBase, IDasTypeInfo);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTaskInfo, IDasBase, IDasWeakReferenceSource);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTaskInfoVector, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTaskManager, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTaskScheduler, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTouch, IDasBase, IDasTypeInfo, IDasInput);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasTypeInfo, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasVariantVector, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasWeakReference, IDasBase);
DAS_UTILS_DEFINE_PRESET_INHERITANCE_INFO(IDasWeakReferenceSource, IDasBase);

#endif // DAS_UTILS_PRESETTYPEINHERITANCEINFO_H