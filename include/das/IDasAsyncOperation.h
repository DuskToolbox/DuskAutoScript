#ifndef DAS_ASYNC_OPERATION_H
#define DAS_ASYNC_OPERATION_H

#include <das/IDasBase.h>

// 异步操作状态
enum DasAsyncStatus : int32_t
{
    DAS_ASYNC_STARTED = 0,   // 已启动，等待完成
    DAS_ASYNC_COMPLETED = 1, // 已成功完成
    DAS_ASYNC_CANCELED = 2,  // 已取消
    DAS_ASYNC_FAILED = 3,    // 已失败
};

// 完成回调接口
DAS_DEFINE_GUID(
    DAS_IID_ASYNC_COMPLETED_HANDLER,
    IDasAsyncCompletedHandler,
    0xA1B2C3D4,
    0xE5F6,
    0x4A7B,
    0x8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D)
DAS_SWIG_EXPORT_ATTRIBUTE(IDasAsyncCompletedHandler)
DAS_INTERFACE IDasAsyncCompletedHandler : public IDasBase
{
    DAS_METHOD OnCompleted(IDasBase * p_sender, int32_t status) = 0;
};

// 异步操作基类
DAS_DEFINE_GUID(
    DAS_IID_ASYNC_OPERATION,
    IDasAsyncOperation,
    0xB2C3D4E5,
    0xF6A7,
    0x4B8C,
    0x9D,
    0x0E,
    0x1F,
    0x2A,
    0x3B,
    0x4C,
    0x5D,
    0x6E)
DAS_SWIG_EXPORT_ATTRIBUTE(IDasAsyncOperation)
DAS_INTERFACE IDasAsyncOperation : public IDasBase
{
    // 获取当前状态
    DAS_METHOD_(int32_t) GetStatus() = 0;

    // 设置完成回调（如果已完成，立即调用）
    DAS_METHOD SetCompleted(IDasAsyncCompletedHandler * p_handler) = 0;

    // 取消操作
    DAS_METHOD Cancel() = 0;
};

#endif // DAS_ASYNC_OPERATION_H
