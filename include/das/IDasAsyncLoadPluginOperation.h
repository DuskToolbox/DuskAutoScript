#ifndef DAS_ASYNC_LOAD_PLUGIN_OPERATION_H
#define DAS_ASYNC_LOAD_PLUGIN_OPERATION_H

#include <cstdint>
#include <das/IDasAsyncOperation.h>

DAS_DEFINE_GUID(
    DAS_IID_ASYNC_LOAD_PLUGIN_OPERATION,
    IDasAsyncLoadPluginOperation,
    0xC3D4E5F6,
    0xA7B8,
    0x4C9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E,
    0x7F)
DAS_SWIG_EXPORT_ATTRIBUTE(IDasAsyncLoadPluginOperation)
DAS_INTERFACE IDasAsyncLoadPluginOperation : public IDasAsyncOperation
{
    // 获取加载结果。调用成功时 IDasBase* 已 AddRef，调用者负责 Release。
    // 仅当 status == COMPLETED 或 FAILED 时有效。
    DAS_METHOD GetResults(IDasBase * *pp_out_plugin) = 0;
};

#endif // DAS_ASYNC_LOAD_PLUGIN_OPERATION_H
