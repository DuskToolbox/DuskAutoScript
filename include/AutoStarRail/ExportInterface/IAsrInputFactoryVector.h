#ifndef ASR_ASRINPUTFACTORYVECTOR_H
#define ASR_ASRINPUTFACTORYVECTOR_H

#include <AutoStarRail/PluginInterface/IAsrInput.h>

// {43103B54-B9D6-404D-99D5-67328111A07C}
ASR_DEFINE_GUID(
    ASR_IID_INPUT_FACTORY_VECTOR,
    IAsrInputFactoryVector,
    0x43103b54,
    0xb9d6,
    0x404d,
    0x99,
    0xd5,
    0x67,
    0x32,
    0x81,
    0x11,
    0xa0,
    0x7c);
SWIG_IGNORE(IAsrInputFactoryVector)
ASR_INTERFACE IAsrInputFactoryVector : public IAsrBase
{
    ASR_METHOD Size(size_t * p_out_size) = 0;
    ASR_METHOD At(size_t index, IAsrInputFactory * *pp_out_factory) = 0;
    ASR_METHOD Find(const AsrGuid& iid, IAsrInputFactory** pp_out_factory) = 0;
};

// {88E2E88E-80C0-4440-8979-987F2DE58009}
ASR_DEFINE_GUID(
    ASR_SWIG_IID_INPUT_FACTORY_VECTOR,
    IAsrSwigInputFactoryVector,
    0x88e2e88e,
    0x80c0,
    0x4440,
    0x89,
    0x79,
    0x98,
    0x7f,
    0x2d,
    0xe5,
    0x80,
    0x9);
ASR_SWIG_EXPORT_ATTRIBUTE(IAsrSwigInputFactoryVector)
ASR_INTERFACE IAsrSwigInputFactoryVector : public IAsrSwigBase
{
    virtual AsrRetUInt         Size() = 0;
    virtual AsrRetInputFactory At(size_t index) = 0;
    virtual AsrRetInputFactory Find(const AsrGuid& iid) = 0;
};

ASR_DEFINE_RET_POINTER(AsrRetInputFactoryVector, IAsrSwigInputFactoryVector);

#endif // ASR_ASRINPUTFACTORYVECTOR_H
