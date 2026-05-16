#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    using MaaResourceHandle = std::uintptr_t;
    using MaaControllerHandle = std::uintptr_t;
    using MaaTaskerHandle = std::uintptr_t;
    using MaaAsyncId = std::int64_t;

    inline constexpr MaaResourceHandle kInvalidMaaResourceHandle = 0;
    inline constexpr MaaControllerHandle kInvalidMaaControllerHandle = 0;
    inline constexpr MaaTaskerHandle kInvalidMaaTaskerHandle = 0;
    inline constexpr MaaAsyncId kInvalidMaaAsyncId = 0;

    enum class MaaTaskStatus : std::int32_t
    {
        Invalid = 0,
        Pending = 1000,
        Running = 2000,
        Succeeded = 3000,
        Failed = 4000,
    };

    struct ControllerSpec
    {
        std::string name;
        std::string type;
        std::string read_path;
        std::string address;
        std::string adb_path = "adb";
        std::string config_json = "{}";
        std::string agent_path;
    };

    struct MaaApiResult
    {
        bool         ok = true;
        std::int64_t provider_code = 0;
        std::string  message;
        MaaAsyncId   id = kInvalidMaaAsyncId;

        static MaaApiResult Ok(MaaAsyncId id = kInvalidMaaAsyncId)
        {
            MaaApiResult result;
            result.id = id;
            return result;
        }

        static MaaApiResult Failure(
            std::int64_t provider_code,
            std::string  message)
        {
            MaaApiResult result;
            result.ok = false;
            result.provider_code = provider_code;
            result.message = std::move(message);
            return result;
        }
    };

    class IMaaApiBoundary
    {
    public:
        virtual ~IMaaApiBoundary() = default;

        virtual MaaResourceHandle CreateResource() = 0;
        virtual void DestroyResource(MaaResourceHandle resource) noexcept = 0;
        virtual MaaApiResult LoadResource(
            MaaResourceHandle resource,
            std::string_view  path) = 0;
        virtual std::optional<std::string> GetResourceHash(
            MaaResourceHandle resource) = 0;

        virtual MaaControllerHandle CreateController(
            const ControllerSpec& spec) = 0;
        virtual void DestroyController(
            MaaControllerHandle controller) noexcept = 0;

        virtual MaaTaskerHandle CreateTasker() = 0;
        virtual void DestroyTasker(MaaTaskerHandle tasker) noexcept = 0;
        virtual MaaApiResult BindResource(
            MaaTaskerHandle   tasker,
            MaaResourceHandle resource) = 0;
        virtual MaaApiResult BindController(
            MaaTaskerHandle     tasker,
            MaaControllerHandle controller) = 0;

        virtual MaaApiResult PostTask(
            MaaTaskerHandle tasker,
            std::string_view entry,
            std::string_view pipeline_override) = 0;
        virtual MaaTaskStatus WaitTask(
            MaaTaskerHandle tasker,
            MaaAsyncId      task_id) = 0;
        virtual MaaApiResult PostStop(MaaTaskerHandle tasker) = 0;
    };

    IMaaApiBoundary& DefaultMaaApiBoundary();
} // namespace Das::Plugins::DasMaaPi
