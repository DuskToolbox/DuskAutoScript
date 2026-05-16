#pragma once

#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace Das::Plugins::DasMaaPi::Test
{
    class FakeMaaApiBoundary final : public IMaaApiBoundary
    {
    public:
        std::vector<std::string> calls;
        std::string              resource_hash = "hash-expected";
        std::map<std::string, MaaTaskStatus> wait_status_by_entry;
        std::map<std::string, MaaApiResult>  post_result_by_entry;

        MaaResourceHandle CreateResource() override
        {
            calls.emplace_back("CreateResource");
            return NextHandle();
        }

        void DestroyResource(MaaResourceHandle resource) noexcept override
        {
            calls.emplace_back("DestroyResource:" + std::to_string(resource));
        }

        MaaApiResult LoadResource(
            MaaResourceHandle,
            std::string_view path) override
        {
            calls.emplace_back("LoadResource:" + std::string(path));
            return MaaApiResult::Ok(NextId());
        }

        std::optional<std::string> GetResourceHash(
            MaaResourceHandle) override
        {
            calls.emplace_back("GetResourceHash");
            return resource_hash;
        }

        MaaControllerHandle CreateController(
            const ControllerSpec& spec) override
        {
            calls.emplace_back("CreateController:" + spec.name + ":" + spec.type);
            return NextHandle();
        }

        void DestroyController(MaaControllerHandle controller) noexcept override
        {
            calls.emplace_back("DestroyController:" + std::to_string(controller));
        }

        MaaTaskerHandle CreateTasker() override
        {
            calls.emplace_back("CreateTasker");
            return NextHandle();
        }

        void DestroyTasker(MaaTaskerHandle tasker) noexcept override
        {
            calls.emplace_back("DestroyTasker:" + std::to_string(tasker));
        }

        MaaApiResult BindResource(
            MaaTaskerHandle,
            MaaResourceHandle) override
        {
            calls.emplace_back("BindResource");
            return MaaApiResult::Ok();
        }

        MaaApiResult BindController(
            MaaTaskerHandle,
            MaaControllerHandle) override
        {
            calls.emplace_back("BindController");
            return MaaApiResult::Ok();
        }

        MaaApiResult PostTask(
            MaaTaskerHandle,
            std::string_view entry,
            std::string_view pipeline_override) override
        {
            last_entry_ = std::string(entry);
            calls.emplace_back(
                "PostTask:" + std::string(entry) + ":"
                + std::string(pipeline_override));
            if (auto it = post_result_by_entry.find(last_entry_);
                it != post_result_by_entry.end())
            {
                return it->second;
            }
            return MaaApiResult::Ok(NextId());
        }

        MaaTaskStatus WaitTask(
            MaaTaskerHandle,
            MaaAsyncId task_id) override
        {
            calls.emplace_back("WaitTask:" + std::to_string(task_id));
            if (auto it = wait_status_by_entry.find(last_entry_);
                it != wait_status_by_entry.end())
            {
                return it->second;
            }
            return MaaTaskStatus::Succeeded;
        }

        MaaApiResult PostStop(MaaTaskerHandle) override
        {
            calls.emplace_back("PostStop");
            return MaaApiResult::Ok(NextId());
        }

        bool Contains(std::string_view expected) const
        {
            return std::find(calls.begin(), calls.end(), expected)
                   != calls.end();
        }

    private:
        MaaResourceHandle NextHandle() { return next_handle_++; }
        MaaAsyncId        NextId() { return next_id_++; }

        MaaResourceHandle next_handle_ = 1;
        MaaAsyncId        next_id_ = 100;
        std::string       last_entry_;
    };
} // namespace Das::Plugins::DasMaaPi::Test
