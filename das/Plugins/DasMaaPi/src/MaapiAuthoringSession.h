#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H

#include "AdbDeviceFinder.h"
#include "PiUpdateService.h"

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/PiDto.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

namespace Das::ExportInterface
{
    struct IDasProgressService;
}

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
    // Forward decl for friend declaration below.
    struct InvokeCommandResult;
    InvokeCommandResult ApplyInvokeCommandChange(
        MaapiAuthoringSession&                     session,
        AcceptedSettingsDto&                       settings,
        const yyjson::value&                       payload,
        std::vector<PiDiagnosticDto>&              diagnostics,
        PiHttpClient*                              http_seam,
        Das::ExportInterface::IDasProgressService* progress_service_seam);

    // discoverAdbDevices 命令 handler — 经 session& 引用直写 device_cache_
    // （零 T* 出参指针，与 ApplyInvokeCommandChange 同范式）。
    bool ApplyDiscoverAdbDevicesCommand(
        MaapiAuthoringSession&        session,
        std::vector<PiDiagnosticDto>& diagnostics);

    class MaapiAuthoringSession final
        : public PluginInterface::DasTaskAuthoringSessionImplBase<
              MaapiAuthoringSession>
    {
        // ApplyInvokeCommandChange 是 invokeCommand dispatch 中枢，直接
        // 读写 pending_update_ / device_cache_（零 T* 出参指针 — 通过
        // session& 引用而非成员地址指针承载状态）。
        friend InvokeCommandResult ApplyInvokeCommandChange(
            MaapiAuthoringSession&                     session,
            AcceptedSettingsDto&                       settings,
            const yyjson::value&                       payload,
            std::vector<PiDiagnosticDto>&              diagnostics,
            PiHttpClient*                              http_seam,
            Das::ExportInterface::IDasProgressService* progress_service_seam);

        // ApplyDiscoverAdbDevicesCommand 直写 device_cache_（扫描结果填充）。
        friend bool ApplyDiscoverAdbDevicesCommand(
            MaapiAuthoringSession&        session,
            std::vector<PiDiagnosticDto>& diagnostics);

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

        // ADB 扫描结果暂态（会话内存态，不入 acceptedProperties、不落
        // 盘——同 pending_update_ 范式）。discoverAdbDevices 命令调用
        // AdbDeviceFinder::FindAllAdbDevices 填充；adbDevice 字段投影的
        // candidates 从此读取；PiCompiler 编译期按
        // settings.pi.controller_adb_device 在此查六字段。空 vector =
        // 未扫描或扫描到 0 设备（前端 dropdown 空候补）。
        const std::vector<AdbDeviceInfo>& DeviceCache() const
        {
            return device_cache_;
        }

        // 测试缝：注入网络 seam（生产 ApplyInvokeCommandChange 空指针时构造
        // BeastHttpClient）。DasMaaPiTest 直接编译本类 + PiUpdateService +
        // FakePiHttpClient，可无 IPC 验证 checkUpdate/applyUpdate 全链路。
        void SetHttpClientForTest(PiHttpClient* http) { test_http_ = http; }

        // 测试缝：注入进度 service（生产为空指针时 applyUpdate 路径经
        // GetCurrentIpcContext Resolve 主进程 service）。测试传本地 fake
        // 即可无 IPC 驱动 applyUpdate 期间的进度上报链路。
        void SetProgressServiceForTest(
            Das::ExportInterface::IDasProgressService* svc)
        {
            progress_service_seam_ = svc;
        }

    private:
        AcceptedSettingsDto         settings_{};
        int64_t                     revision_ = 0;
        std::optional<PiUpdateInfo> pending_update_;
        // session 内存态，不进 AcceptedSettingsDto 不序列化
        // （设备状态会变 — adb 拔出/换机，持久化 cache 会陈旧）。重启时
        // controllerAdbDevice 持久化字段仍在但 cache 空 → 用户重扫描即可。
        std::vector<AdbDeviceInfo>                 device_cache_;
        PiHttpClient*                              test_http_ = nullptr;
        Das::ExportInterface::IDasProgressService* progress_service_seam_ =
            nullptr;
    };
} // namespace Plugins::DasMaaPi
DAS_NS_END

#endif // DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
