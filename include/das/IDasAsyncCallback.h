#ifndef DAS_ASYNC_CALLBACK_H
#define DAS_ASYNC_CALLBACK_H

#include <das/IDasBase.h>

DAS_DEFINE_GUID(
    DAS_IID_IDASASYNCCALLBACK,
    IDasAsyncCallback,
    0x12345678,
    0x1234,
    0x1234,
    0x12,
    0x34,
    0x56,
    0x78,
    0x90,
    0xAB,
    0xCD,
    0xEF)

DAS_INTERFACE IDasAsyncCallback : public IDasBase { DAS_METHOD Do() = 0; };

#endif // DAS_ASYNC_CALLBACK_H
