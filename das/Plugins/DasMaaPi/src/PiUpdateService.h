#pragma once

#include "PiHttpClient.h"

#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/_autogen/idl/abi/DasResult.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    // 版本检查 / 下载结果（D-89-05）。download_url / sha256 仅在来源返回
    // 有效下载信息时填充；error_code 携带 MirrorChyan 业务错误码（code!=0
    // 但 data.version_name 存在时按 MXU 语义返回部分信息）。
    struct PiUpdateInfo
    {
        bool                       has_update = false;
        std::string                version_name;
        std::string                release_note;
        std::string                download_url;
        std::optional<std::string> sha256;
        std::optional<int64_t>     error_code;
    };

    // 更新流程阶段。
    enum class UpdateStage
    {
        Download, // 下载阶段：有 Content-Length 带字节对，无则阶段态
        Extract,  // 解压阶段：仅阶段事件
        Replace,  // 替换阶段：仅阶段事件
    };

    // 进度上报载荷。total_bytes 仅在下载来源返回 Content-Length 时填充；无则
    // 调用侧显示阶段态「正在下载…」。
    struct UpdateProgress
    {
        UpdateStage                  stage = UpdateStage::Download;
        std::uint64_t                bytes_received = 0;
        std::optional<std::uint64_t> total_bytes;
    };

    using ProgressReporter = std::function<void(const UpdateProgress&)>;

    // 共享更新编排层（MaaPI 逻辑层职责）。网络经 PiHttpClient seam
    // 注入（生产传 BeastHttpClient，测试传本地 mock），文件操作全部在
    // interface.json 所在目录（项目目录）内完成，不触碰 DAS 自身二进制。
    struct PiUpdateService
    {
        // 检查更新：读 interface.json 顶层 github / mirrorchyan_rid / version
        // （PiCatalog 只解析 version，此处顶层原始读），默认 MirrorChyan 优先 +
        // GitHub 兜底 + PiSemver 比较（D-89-02 / D-89-05）。
        //
        // proxy_url 来自 authoring session settings 的 adapter.proxyUrl
        // （经 invokeCommand apply 持久化，D-89-08 修订）；空串 = 直连。测试
        // 可显式传入本地 mock proxy。
        // mirrorchyan_cdk 来自 settings 的 adapter.mirrorchyanCdk，作为
        // MirrorChyan CDK query 参数（cdk，对齐 MXU 语义）；空串 = 不带。
        // update_channel 来自 settings 的 adapter.updateChannel（mirrorchyan /
        // github 二选一）；"github" → 仅 GitHub 单通道检查，空或
        // "mirrorchyan" → MirrorChyan 优先 + GitHub 兜底。
        static DasResult CheckUpdate(
            const std::filesystem::path& pi_dir,
            PiHttpClient&                http,
            PiUpdateInfo&                out,
            std::string_view             proxy_url = {},
            std::string_view             mirrorchyan_cdk = {},
            std::string_view             update_channel = {});

        // 应用更新：流式下载 zip（经 http seam，边下边写盘，不整体缓冲）→
        // 安全解压（目录穿越防护）→ 备份旧项目目录 → 原子替换 interface.json
        // 所在目录 → resource.hash 校验（经 IMaaApiBoundary）→ 清理 .update
        // 临时目录。返回 -10008 时旧目录已回滚。
        // boundary 为空时用 DefaultMaaApiBoundary()（测试传
        // FakeMaaApiBoundary）。mirrorchyan_cdk 非空且下载 URL 为 MirrorChyan
        // 来源时附带 cdk query（与检查请求一致）。
        // progress_reporter 为空时不产生进度回调；非空时按阶段（下载/解压/
        // 替换）上报进度，供 IPC 推送链路消费。
        static DasResult ApplyUpdate(
            const std::filesystem::path& pi_dir,
            const PiUpdateInfo&          info,
            PiHttpClient&                http,
            std::string_view             proxy_url = {},
            std::string_view             mirrorchyan_cdk = {},
            IMaaApiBoundary*             boundary = nullptr,
            ProgressReporter             progress_reporter = {});
    };
} // namespace Das::Plugins::DasMaaPi
