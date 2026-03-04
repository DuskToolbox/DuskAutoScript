#ifndef DAS_ASYNC_HANDSHAKE_OPERATION_H
#define DAS_ASYNC_HANDSHAKE_OPERATION_H

#include <das/IDasAsyncOperation.h>

DAS_DEFINE_GUID(
    DAS_IID_ASYNC_HANDSHAKE_OPERATION,
    IDasAsyncHandshakeOperation,
    0xD4E5F6A7,
    0xB8C9,
    0x4D0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E,
    0x7F,
    0x80)
DAS_SWIG_EXPORT_ATTRIBUTE(IDasAsyncHandshakeOperation)
DAS_INTERFACE IDasAsyncHandshakeOperation : public IDasAsyncOperation
{
    // 获取握手结果（仅当 status == COMPLETED 时有效）
    DAS_METHOD GetResults(uint16_t* p_out_session_id) = 0;
};

#endif // DAS_ASYNC_HANDSHAKE_OPERATION_H
