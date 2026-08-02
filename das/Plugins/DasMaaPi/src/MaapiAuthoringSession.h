#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H

#include "PiUpdateService.h"

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Plugins::DasMaaPi,
    MaapiAuthoringSession,
    0x69f20003,
    0x0000,
    0x4000,
    0x80,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01);

DAS_NS_BEGIN
namespace Plugins::DasMaaPi
{
    class MaapiAuthoringSession final
        : public PluginInterface::DasTaskAuthoringSessionImplBase<
              MaapiAuthoringSession>
    {
    public:
        MaapiAuthoringSession() = default;
        explicit MaapiAuthoringSession(
            AcceptedSettingsDto settings,
            int64_t             revision = 0);

        DAS_IMPL GetDocument(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_document_json) override;
        DAS_IMPL ApplyChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;
        DAS_IMPL Compile(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

        // D-89-04: checkUpdate 结果暂态（会话内存态，不入 acceptedProperties、
        // 不落盘——Phase 87 黑盒约束）。commands 投影的 applyUpdate disabled 由
        // 它驱动（后端状态驱动投影，Open Question 1 决策）。
        const std::optional<PiUpdateInfo>& PendingUpdate() const
        {
            return pending_update_;
        }
        void SetPendingUpdate(std::optional<PiUpdateInfo> pending)
        {
            pending_update_ = std::move(pending);
        }

        // 测试缝：注入网络 seam（生产 ApplyInvokeCommandChange 空指针时构造
        // BeastHttpClient）。DasMaaPiTest 直接编译本类 + PiUpdateService +
        // FakePiHttpClient，可无 IPC 验证 checkUpdate/applyUpdate 全链路。
        void SetHttpClientForTest(PiHttpClient* http) { test_http_ = http; }

    private:
        AcceptedSettingsDto         settings_{};
        int64_t                     revision_ = 0;
        std::optional<PiUpdateInfo> pending_update_;
        PiHttpClient*               test_http_ = nullptr;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
