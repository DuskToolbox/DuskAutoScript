#ifndef DAS_ICOMPONENT_H
#define DAS_ICOMPONENT_H

#include <das/IDasTypeInfo.h>

DAS_INTERFACE IDasVariantVector;
DAS_INTERFACE IDasSwigVariantVector;

DAS_INTERFACE IDasSwigComponent;

DAS_DEFINE_RET_POINTER(DasRetComponent, IDasSwigComponent);

#include <das/ExportInterface/IDasVariantVector.h>

// {15FF0855-E031-4602-829D-040230515C55}
DAS_DEFINE_GUID(
    DAS_IID_COMPONENT,
    IDasComponent,
    0x15ff0855,
    0xe031,
    0x4602,
    0x82,
    0x9d,
    0x4,
    0x2,
    0x30,
    0x51,
    0x5c,
    0x55);
SWIG_IGNORE(IDasComponent)
DAS_INTERFACE IDasComponent : public IDasTypeInfo
{
    DAS_METHOD Dispatch(
        IDasReadOnlyString * p_function_name,
        IDasVariantVector * p_arguments,
        IDasVariantVector * *pp_out_result) = 0;
};

// {CF5730A3-D5F6-4422-A3D6-EF6145AC4DFF}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_COMPONENT,
    IDasSwigComponent,
    0xcf5730a3,
    0xd5f6,
    0x4422,
    0xa3,
    0xd6,
    0xef,
    0x61,
    0x45,
    0xac,
    0x4d,
    0xff);
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigComponent)
DAS_INTERFACE IDasSwigComponent : public IDasSwigTypeInfo
{
    virtual DasRetVariantVector Dispatch(
        DasReadOnlyString function_name,
        IDasSwigVariantVector * p_arguments) = 0;
};

// {104C288C-5970-40B9-8E3F-B0B7E4ED509A}
DAS_DEFINE_GUID(
    DAS_IID_COMPONENT_FACTORY,
    IDasComponentFactory,
    0x104c288c,
    0x5970,
    0x40b9,
    0x8e,
    0x3f,
    0xb0,
    0xb7,
    0xe4,
    0xed,
    0x50,
    0x9a);
SWIG_IGNORE(IDasComponentFactory)
DAS_INTERFACE IDasComponentFactory : public IDasTypeInfo
{
    DAS_METHOD IsSupported(const DasGuid& component_iid) = 0;
    DAS_METHOD CreateInstance(
        const DasGuid&  component_iid,
        IDasComponent** pp_out_component) = 0;
};

// {9A933F2B-A2BB-4A0C-A0E5-83AA7E08ECA2}
DAS_DEFINE_GUID(
    DAS_IID_SWIG_COMPONENT_FACTORY,
    IDasSwigComponentFactory,
    0x9a933f2b,
    0xa2bb,
    0x4a0c,
    0xa0,
    0xe5,
    0x83,
    0xaa,
    0x7e,
    0x8,
    0xec,
    0xa2);
DAS_SWIG_DIRECTOR_ATTRIBUTE(IDasSwigComponentFactory)
DAS_INTERFACE IDasSwigComponentFactory : public IDasSwigTypeInfo
{
    virtual DasResult       IsSupported(const DasGuid& component_iid) = 0;
    virtual DasRetComponent CreateInstance(const DasGuid& component_iid) = 0;
};
#endif // DAS_ICOMPONENT_H
