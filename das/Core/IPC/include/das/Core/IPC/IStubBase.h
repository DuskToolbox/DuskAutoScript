#ifndef DAS_CORE_IPC_ISTUB_BASE_H
#define DAS_CORE_IPC_ISTUB_BASE_H

#include <cstdint>
#include <das/Core/IPC/DistributedObjectManager.h>
#include <das/Core/IPC/IMessageHandler.h>
#include <das/Core/IPC/IpcMessageHeaderBuilder.h>
#include <das/Core/IPC/IpcResponseSender.h>
#include <das/Core/IPC/ObjectId.h>
#include <das/Core/IPC/ValidatedIPCMessageHeader.h>
#include <das/IDasBase.h>
#include <limits>
#include <vector>

#include <das/Core/IPC/Config.h>

DAS_CORE_IPC_NS_BEGIN

/**
 * @brief Stub 基类
 *
 * 实现 IMessageHandler 接口，用于处理业务消息。
 * 零成员变量：通过 HandleMessage 参数接收 DistributedObjectManager，
 * 通过 DispatchMethod 参数传递 impl 指针。
 */
class IStubBase : public IMessageHandler
{
public:
    ~IStubBase() override = default;

    /// 增加引用计数（Stub 是全局无状态单例，永不销毁）
    [[nodiscard]]
    uint32_t AddRef() override
    {
        return std::numeric_limits<uint32_t>::max();
    }

    /// 减少引用计数（Stub 是全局无状态单例，永不销毁）
    [[nodiscard]]
    uint32_t Release() override
    {
        return std::numeric_limits<uint32_t>::max();
    }

    /// 获取接口 ID（纯虚函数，每个生成的 stub 提供）
    [[nodiscard]]
    uint32_t GetInterfaceId() const noexcept override = 0;

    /// 处理 IPC 消息
    /// 解析 V3 Body Header，查找 impl，调用 DispatchMethod
    DasResult HandleMessage(
        const ValidatedIPCMessageHeader& header,
        const std::vector<uint8_t>&      body,
        IpcResponseSender&               sender,
        DistributedObjectManager&        object_manager) override;

    /**
     * @brief 分发方法调用（纯虚函数，每个生成的 stub 实现）
     * @param method_id 方法 ID
     * @param impl 实现指针（由 HandleMessage 通过 ObjectManager 查找）
     * @param params 参数数据（V3 Body Header 之后的数据）
     * @param params_size 参数数据大小
     * @param out_response [out] 响应体
     * @param object_manager
     * DistributedObjectManager（用于解析入参中的接口指针）
     * @return DasResult 处理结果
     */
    virtual DasResult DispatchMethod(
        uint16_t                  method_id,
        void*                     impl,
        const uint8_t*            params,
        size_t                    params_size,
        std::vector<uint8_t>&     out_response,
        DistributedObjectManager& object_manager) = 0;

private:
    /// 解析 V3 Body Header
    /// @return true 成功，false 失败（body 太短）
    static bool ParseV3BodyHeader(
        const std::vector<uint8_t>& body,
        uint32_t&                   out_interface_id,
        uint16_t&                   out_method_id,
        ObjectId&                   out_object_id);
};

DAS_CORE_IPC_NS_END

#endif // DAS_CORE_IPC_ISTUB_BASE_H
