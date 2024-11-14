#ifndef DAS_GUIDVECTOR_H
#define DAS_GUIDVECTOR_H

#include <das/DasPtr.hpp>
#include <das/IDasBase.h>

// {8AE436FE-590B-4B70-B24F-ED1327E9841C}
DAS_DEFINE_GUID(
    DAS_IID_READ_ONLY_GUID_VECTOR,
    IDasReadOnlyGuidVector,
    0x8ae436fe,
    0x590b,
    0x4b70,
    0xb2,
    0x4f,
    0xed,
    0x13,
    0x27,
    0xe9,
    0x84,
    0x1c);
SWIG_IGNORE(IDasReadOnlyGuidVector)
DAS_INTERFACE IDasReadOnlyGuidVector : public IDasBase
{
    DAS_METHOD Size(size_t * p_out_size) = 0;
    DAS_METHOD At(size_t index, DasGuid * p_out_iid) = 0;
    DAS_METHOD Find(const DasGuid& iid) = 0;
};

// {EBC40F58-F1A6-49FF-9241-18D155576F9E}
DAS_DEFINE_GUID(
    DAS_IID_GUID_VECTOR,
    IDasGuidVector,
    0xebc40f58,
    0xf1a6,
    0x49ff,
    0x92,
    0x41,
    0x18,
    0xd1,
    0x55,
    0x57,
    0x6f,
    0x9e)
SWIG_IGNORE(IDasGuidVector)
DAS_INTERFACE IDasGuidVector : public IDasBase
{
    DAS_METHOD Size(size_t * p_out_size) = 0;
    DAS_METHOD At(size_t index, DasGuid * p_out_iid) = 0;
    DAS_METHOD Find(const DasGuid& iid) = 0;
    DAS_METHOD PushBack(const DasGuid& iid) = 0;
    DAS_METHOD ToConst(IDasReadOnlyGuidVector * *pp_out_object) = 0;
};

SWIG_IGNORE(CreateIDasGuidVector)
DAS_C_API DasResult CreateIDasGuidVector(
    const DasGuid*   p_data,
    size_t           size,
    IDasGuidVector** pp_out_guid);

// {60A09918-04E3-44E8-936E-730EB72024F5}
DAS_DEFINE_GUID(
    DAS_SWIG_READ_ONLY_GUID_VECTOR,
    IDasSwigReadOnlyGuidVector,
    0x60a09918,
    0x4e3,
    0x44e8,
    0x93,
    0x6e,
    0x73,
    0xe,
    0xb7,
    0x20,
    0x24,
    0xf5);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigReadOnlyGuidVector)
DAS_INTERFACE IDasSwigReadOnlyGuidVector : public IDasSwigBase
{
    virtual DasRetUInt Size() = 0;
    virtual DasRetGuid At(size_t index) = 0;
    virtual DasResult  Find(const DasGuid& p_iid) = 0;
};

DAS_DEFINE_RET_POINTER(DasRetReadOnlyGuidVector, IDasSwigReadOnlyGuidVector);

// {E00E7F36-A7BC-4E35-8E98-5C9BB6B1D19B}
DAS_DEFINE_GUID(
    DAS_IID_GUID_SWIG_VECTOR,
    IDasSwigGuidVector,
    0xe00e7f36,
    0xa7bc,
    0x4e35,
    0x8e,
    0x98,
    0x5c,
    0x9b,
    0xb6,
    0xb1,
    0xd1,
    0x9b);
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigGuidVector)
DAS_INTERFACE IDasSwigGuidVector : public IDasSwigBase
{
    virtual DasRetUInt               Size() = 0;
    virtual DasRetGuid               At(size_t index) = 0;
    virtual DasResult                Find(const DasGuid& p_iid) = 0;
    virtual DasResult                PushBack(const DasGuid& p_iid) = 0;
    virtual DasRetReadOnlyGuidVector ToConst() = 0;
};

DAS_DEFINE_RET_POINTER(DasRetGuidVector, IDasSwigGuidVector);

DAS_API DasRetGuidVector CreateIDasSwigGuidVector();

#endif // DAS_GUIDVECTOR_H
