#ifndef ASR_ICOMPONENT_H
#define ASR_ICOMPONENT_H

#include <AutoStarRail/IAsrTypeInfo.h>

ASR_INTERFACE IAsrVariantVector;
ASR_INTERFACE IAsrSwigVariantVector;

ASR_INTERFACE IAsrSwigComponent;

ASR_DEFINE_RET_POINTER(AsrRetComponent, IAsrSwigComponent);

#include <AutoStarRail/ExportInterface/IAsrVariantVector.h>

// {15FF0855-E031-4602-829D-040230515C55}
ASR_DEFINE_GUID(
    ASR_IID_COMPONENT,
    IAsrComponent,
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
SWIG_IGNORE(IAsrComponent)
ASR_INTERFACE IAsrComponent : public IAsrTypeInfo
{
    ASR_METHOD Dispatch(
        IAsrReadOnlyString * p_function_name,
        IAsrVariantVector * p_arguments,
        IAsrVariantVector * *pp_out_result) = 0;
};

// {CF5730A3-D5F6-4422-A3D6-EF6145AC4DFF}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_COMPONENT,
    IAsrSwigComponent,
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
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigComponent)
ASR_INTERFACE IAsrSwigComponent : public IAsrSwigTypeInfo
{
    virtual AsrRetVariantVector Dispatch(
        AsrReadOnlyString function_name,
        IAsrSwigVariantVector * p_arguments) = 0;
};

// {104C288C-5970-40B9-8E3F-B0B7E4ED509A}
ASR_DEFINE_GUID(
    ASR_IID_COMPONENT_FACTORY,
    IAsrComponentFactory,
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
SWIG_IGNORE(IAsrComponentFactory)
ASR_INTERFACE IAsrComponentFactory : public IAsrTypeInfo
{
    ASR_METHOD IsSupported(const AsrGuid& component_iid) = 0;
    ASR_METHOD CreateInstance(
        const AsrGuid&  component_iid,
        IAsrComponent** pp_out_component) = 0;
};

// {9A933F2B-A2BB-4A0C-A0E5-83AA7E08ECA2}
ASR_DEFINE_GUID(
    ASR_IID_SWIG_COMPONENT_FACTORY,
    IAsrSwigComponentFactory,
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
ASR_SWIG_DIRECTOR_ATTRIBUTE(IAsrSwigComponentFactory)
ASR_INTERFACE IAsrSwigComponentFactory : public IAsrSwigTypeInfo
{
    virtual AsrResult       IsSupported(const AsrGuid& component_iid) = 0;
    virtual AsrRetComponent CreateInstance(const AsrGuid& component_iid) = 0;
};
#endif // ASR_ICOMPONENT_H
