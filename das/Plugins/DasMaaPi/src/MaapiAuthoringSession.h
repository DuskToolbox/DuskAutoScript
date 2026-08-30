#ifndef DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H
#define DAS_PLUGINS_DASMAAPI_MAAPIAUTHORINGSESSION_H

#include "PiUpdateService.h"
#include <das/Plugins/DasMaaPi/AdbDeviceFinder.h>

#include <das/Plugins/DasMaaPi/AcceptedSettings.h>
#include <das/Plugins/DasMaaPi/PiDto.h>
#include <das/_autogen/idl/wrapper/Das.PluginInterface.IDasTaskAuthoringSession.Implements.hpp>

#include <string>
#include <vector>

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
    // 组件注册表目录条目 {actionId,label}：Scheduler 注入 authoring context
    // 的注册表快照（与 graph 任务的 actionCatalog 同源同形状，公共 wire 两
    // 键投影）。PI 接口目录缺席（新建条目尚未配置 interfacePath）时作为
    // GetDocument/ApplyChange 文档 actionCatalog.entries 的兜底内容。
    struct RegistryActionCatalogEntry
    {
        std::string action_id;
        std::string label;
    };

    // Forward decl for friend declaration below.
    struct InvokeCommandResult;
    InvokeCommandResult ApplyInvokeCommandChange(
        MaapiAuthoringSession&                     session,
        AcceptedSettingsDto&                       settings,
        const yyjson::value&                       payload,
        std::vector<PiDiagnosticDto>&              diagnostics,
        PiHttpClient*                              http_seam,
        Das::ExportInterface::IDasProgressService* progress_service_seam,
        AdbDeviceEnumerator                        adb_enumerator);

    // discoverAdbDevices 命令 handler — 经 session& 引用直写 device_cache_
    // （零 T* 出参指针，与 ApplyInvokeCommandChange 同范式）。
    bool ApplyDiscoverAdbDevicesCommand(
        MaapiAuthoringSession&        session,
        std::vector<PiDiagnosticDto>& diagnostics,
        const AdbDeviceEnumerator&    adb_enumerator);

    class MaapiAuthoringSession final
        : public PluginInterface::DasTaskAuthoringSessionImplBase<
              MaapiAuthoringSession>
    {
        // ApplyInvokeCommandChange 是 invokeCommand dispatch 中枢，直接
        // 读写 device_cache_（零 T* 出参指针 — 通过 session& 引用而非成员
        // 地址指针承载状态）。checkUpdate/applyUpdate 的检查结果经进程级
        // UpdateCheckStore 传递，不经 session。
        friend InvokeCommandResult ApplyInvokeCommandChange(
            MaapiAuthoringSession&                     session,
            AcceptedSettingsDto&                       settings,
            const yyjson::value&                       payload,
            std::vector<PiDiagnosticDto>&              diagnostics,
            PiHttpClient*                              http_seam,
            Das::ExportInterface::IDasProgressService* progress_service_seam,
            AdbDeviceEnumerator                        adb_enumerator);

        // ApplyDiscoverAdbDevicesCommand 直写 device_cache_（扫描结果填充）。
        friend bool ApplyDiscoverAdbDevicesCommand(
            MaapiAuthoringSession&        session,
            std::vector<PiDiagnosticDto>& diagnostics,
            const AdbDeviceEnumerator&    adb_enumerator);

    public:
        MaapiAuthoringSession() = default;
        explicit MaapiAuthoringSession(
            AcceptedSettingsDto                       settings,
            int64_t                                   revision = 0,
            std::vector<RegistryActionCatalogEntry>&& registry_action_catalog =
                {});

        DAS_IMPL GetDocument(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_document_json) override;
        DAS_IMPL ApplyChange(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;
        DAS_IMPL Compile(
            ExportInterface::IDasJson*  p_request_json,
            ExportInterface::IDasJson** pp_out_result_json) override;

        // ADB 扫描结果暂态（会话内存态，不入 acceptedProperties、不落
        // 盘）。discoverAdbDevices 命令调用
        // AdbDeviceFinder::FindAllAdbDevices 填充；adbDevice 字段投影的
        // candidates 从此读取；PiCompiler 编译期按
        // settings.pi.controller_adb_device 在此查六字段。空 vector =
        // 未扫描或扫描到 0 设备（前端 dropdown 空候补）。
        // checkUpdate 结果不再走 session 暂态：进程级 UpdateCheckStore 按
        // pi_dir 存活（后台 job 线程不许碰 session——session 会被驱逐重建）。
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
        // PI 接口目录缺席时（新建条目未配置 interfacePath），把注入的注册表
        // 目录快照填入文档 actionCatalog.entries（条目键恰 {actionId,label}，
        // Core 校验器严格形状）。PI 目录已在时不动——可添加的 PI 任务与
        // 组件 guid 禁止混排。
        void FillEmptyActionCatalogFromRegistry(
            yyjson::writer::object_ref document_object) const;

        AcceptedSettingsDto                     settings_{};
        int64_t                                 revision_ = 0;
        std::vector<RegistryActionCatalogEntry> registry_action_catalog_{};
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
