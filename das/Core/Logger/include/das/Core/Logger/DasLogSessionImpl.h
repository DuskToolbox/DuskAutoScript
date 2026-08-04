#ifndef DAS_CORE_LOGGER_DASLOGSESSIONIMPL_H
#define DAS_CORE_LOGGER_DASLOGSESSIONIMPL_H

#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasLogSession.Implements.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>

namespace Das::Core
{
    // IDasLogSession 实现：RAII 持有会话目录生命周期。
    // 工厂 CreateLogSession 在 ctor 中：
    //   1. 清理孤儿会话目录（pid 不存活的 {ts}-{pid} 目录）；
    //   2. 建立 {ts}-{pid}/img/ 子目录。
    // refcount 归零（最后一个 DasPtr 释）时析构函数递归删除整个会话目录。
    // GetSessionDir / GetUserJsonlPath / NextImageSeq 经接口暴露给 recorder 与
    // archive，替代 LogRecorderRuntime 的进程级静态状态。
    class DasLogSessionImpl final
        : public Das::ExportInterface::DasLogSessionImplBase<DasLogSessionImpl>
    {
    public:
        // debug_directory：会话目录的根目录；ctor 内部做 absolute +
        // lexically_normal。 img 子目录在 ctor 中创建；孤儿目录在 ctor 中清理。
        explicit DasLogSessionImpl(
            const std::filesystem::path& debug_directory);

        // 析构时递归删除会话目录（RAII）。Best-effort：删除失败仅记日志。
        ~DasLogSessionImpl() override;

        DasResult GetSessionDir(IDasReadOnlyString** pp_out_directory) override;
        DasResult GetUserJsonlPath(IDasReadOnlyString** pp_out_path) override;
        DasResult NextImageSeq(uint32_t* p_out_seq) override;

    private:
        std::filesystem::path session_dir_;
        std::filesystem::path user_jsonl_path_;
        std::atomic<uint32_t> image_seq_{0};
    };
} // namespace Das::Core

#endif // DAS_CORE_LOGGER_DASLOGSESSIONIMPL_H
