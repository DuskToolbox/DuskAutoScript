#ifndef DAS_DASINPUTFACTORYVECTOR_H
#define DAS_DASINPUTFACTORYVECTOR_H

#include <das/PluginInterface/IDasInput.h>

// {43103B54-B9D6-404D-99D5-67328111A07C}
DAS_DEFINE_GUID(
    DAS_IID_INPUT_FACTORY_VECTOR,
    IDasInputFactoryVector,
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
SWIG_IGNORE(IDasInputFactoryVector)
DAS_INTERFACE IDasInputFactoryVector : public IDasBase
{
    DAS_METHOD Size(size_t * p_out_size) = 0;
    DAS_METHOD At(size_t index, IDasInputFactory * *pp_out_factory) = 0;
    DAS_METHOD Find(const DasGuid& iid, IDasInputFactory** pp_out_factory) = 0;
};

// {88E2E88E-80C0-4440-8979-987F2DE58009}
DAS_DEFINE_GUID(
    DAS_SWIG_IID_INPUT_FACTORY_VECTOR,
    IDasSwigInputFactoryVector,
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
DAS_SWIG_EXPORT_ATTRIBUTE(IDasSwigInputFactoryVector)
DAS_INTERFACE IDasSwigInputFactoryVector : public IDasSwigBase
{
    virtual DasRetUInt         Size() = 0;
    virtual DasRetInputFactory At(size_t index) = 0;
    virtual DasRetInputFactory Find(const DasGuid& iid) = 0;
};

DAS_DEFINE_RET_POINTER(DasRetInputFactoryVector, IDasSwigInputFactoryVector);

#endif // DAS_DASINPUTFACTORYVECTOR_H
