#ifndef DAS_INSPECTABLE_H
#define DAS_INSPECTABLE_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>

// {C66B4652-BEA9-4985-B8AC-F168BF0442E8}
DAS_DEFINE_GUID(
    DAS_IID_TYPE_INFO,
    IDasTypeInfo,
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
SWIG_IGNORE(IDasTypeInfo)
DAS_INTERFACE IDasTypeInfo : public IDasBase
{
    /**
     * @brief return guid of implementation.
     * @param p_out_guid pass pointer to receive guid.
     * @return DAS_S_OK
     */
    DAS_METHOD GetGuid(DasGuid * p_out_guid) = 0;
    /**
     * Get derived class name.
     * @param p_runtime_string
     * @return DAS_S_OK
     */
    DAS_METHOD GetRuntimeClassName(IDasReadOnlyString * *pp_out_name) = 0;
};

// {B1090AA9-2AE8-4FBD-B486-CED42CE90915}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_TYPE_INFO,
    IDasSwigTypeInfo,
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
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigTypeInfo)
DAS_INTERFACE IDasSwigTypeInfo : public IDasSwigBase
{
    virtual DasRetGuid           GetGuid() = 0;
    virtual DasRetReadOnlyString GetRuntimeClassName() = 0;
};

DAS_DEFINE_RET_POINTER(DasRetTypeInfo, IDasSwigTypeInfo);

#endif // DAS_INSPECTABLE_H
