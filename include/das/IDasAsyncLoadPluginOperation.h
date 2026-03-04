#ifndef DAS_ASYNC_LOAD_PLUGIN_OPERATION_H
#define DAS_ASYNC_LOAD_PLUGIN_OPERATION_H

#include <das/Core/IPC/ObjectId.h>
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
    // 获取加载结果（仅当 status == COMPLETED 时有效）
    DAS_METHOD GetResults(ObjectId * p_out_object_id) = 0;
};

#endif // DAS_ASYNC_LOAD_PLUGIN_OPERATION_H
