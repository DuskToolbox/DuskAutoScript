#ifndef ASR_INSPECTABLE_H
#define ASR_INSPECTABLE_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrBase.h>

// {C66B4652-BEA9-4985-B8AC-F168BF0442E8}
ASR_DEFINE_GUID(
    ASR_IID_TYPE_INFO,
    IAsrTypeInfo,
    0xc66b4652,
    0xbea9,
    0x4985,
    0xb8,
    0xac,
    0xf1,
    0x68,
    0xbf,
    0x4,
    0x42,
    0xe8)
SWIG_IGNORE(IAsrTypeInfo)
ASR_INTERFACE IAsrTypeInfo : public IAsrBase
{
    /**
     * @brief return guid of implementation.
     * @param p_out_guid pass pointer to receive guid.
     * @return ASR_S_OK
     */
    ASR_METHOD GetGuid(AsrGuid * p_out_guid) = 0;
    /**
     * Get derived class name.
     * @param p_runtime_string
     * @return ASR_S_OK
     */
    ASR_METHOD GetRuntimeClassName(IAsrReadOnlyString * *pp_out_name) = 0;
};

// {B1090AA9-2AE8-4FBD-B486-CED42CE90915}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_TYPE_INFO,
    IAsrSwigTypeInfo,
    0xb1090aa9,
    0x2ae8,
    0x4fbd,
    0xb4,
    0x86,
    0xce,
    0xd4,
    0x2c,
    0xe9,
    0x9,
    0x15)
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigTypeInfo)
ASR_INTERFACE IAsrSwigTypeInfo : public IAsrSwigBase
{
    virtual AsrRetGuid           GetGuid() = 0;
    virtual AsrRetReadOnlyString GetRuntimeClassName() = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetTypeInfo, IAsrSwigTypeInfo);

#endif // ASR_INSPECTABLE_H
