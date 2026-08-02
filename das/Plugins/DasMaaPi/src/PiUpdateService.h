#pragma once

#include "PiHttpClient.h"

#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>
#include <das/_autogen/idl/abi/DasResult.h>

#include <cstdint>
#include <filesystem>
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

    // 共享更新编排层（MaaPI 逻辑层职责，D-89-05）。网络经 PiHttpClient seam
    // 注入（生产传 BeastHttpClient，测试传本地 mock——RESEARCH Pitfall 2），
    // 文件操作全部在 interface.json 所在目录（项目目录，D-89-01）内完成，
    // 不触碰 DAS 自身二进制。
    struct PiUpdateService
    {
        // 检查更新：读 interface.json 顶层 github / mirrorchyan_rid / version
        // （PiCatalog 只解析 version，此处顶层原始读），MirrorChyan 优先 +
        // GitHub 兜底 + PiSemver 比较（D-89-02 / D-89-05）。
        //
        // proxy_url 空串时读取 DasMaaPi.json 顶层
        // proxyUrl（ReadManifestProxyUrl， D-89-08）；测试可显式传入本地 mock
        // proxy。
        static DasResult CheckUpdate(
            const std::filesystem::path& pi_dir,
            PiHttpClient&                http,
            PiUpdateInfo&                out,
            std::string_view             proxy_url = {});

        // 应用更新：下载全量 zip（经 http seam，D-89-03）→ 安全解压（目录穿越
        // 防护）→ 备份旧项目目录 → 原子替换 interface.json 所在目录 →
        // resource.hash 校验（经 IMaaApiBoundary，D-89-05）→ 清理 .update
        // 临时目录（D-89-01 / D-89-03）。返回 -10008 时旧目录已回滚。
        // boundary 为空时用 DefaultMaaApiBoundary()（测试传
        // FakeMaaApiBoundary）。
        static DasResult ApplyUpdate(
            const std::filesystem::path& pi_dir,
            const PiUpdateInfo&          info,
            PiHttpClient&                http,
            std::string_view             proxy_url = {},
            IMaaApiBoundary*             boundary = nullptr);
    };
} // namespace Das::Plugins::DasMaaPi
