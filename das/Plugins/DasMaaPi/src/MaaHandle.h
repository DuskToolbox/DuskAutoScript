#pragma once

#include <das/Plugins/DasMaaPi/MaaApiBoundary.h>

#include <utility>

namespace Das::Plugins::DasMaaPi
{
    template <
        typename Handle,
        Handle Invalid,
        void (IMaaApiBoundary::*Destroy)(Handle) noexcept>
    class ScopedMaaHandle final
    {
    public:
        ScopedMaaHandle() noexcept = default;

        ScopedMaaHandle(
            IMaaApiBoundary& boundary,
            Handle           handle) noexcept
            : boundary_(&boundary), handle_(handle)
        {
        }

        ScopedMaaHandle(const ScopedMaaHandle&) = delete;
        ScopedMaaHandle& operator=(const ScopedMaaHandle&) = delete;

        ScopedMaaHandle(ScopedMaaHandle&& other) noexcept
            : boundary_(std::exchange(other.boundary_, nullptr)),
              handle_(std::exchange(other.handle_, Invalid))
        {
        }

        ScopedMaaHandle& operator=(ScopedMaaHandle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                boundary_ = std::exchange(other.boundary_, nullptr);
                handle_ = std::exchange(other.handle_, Invalid);
            }
            return *this;
        }

        ~ScopedMaaHandle() { reset(); }

        Handle get() const noexcept { return handle_; }

        explicit operator bool() const noexcept
        {
            return handle_ != Invalid;
        }

        Handle release() noexcept
        {
            boundary_ = nullptr;
            return std::exchange(handle_, Invalid);
        }

        void reset() noexcept
        {
            if (!boundary_ || handle_ == Invalid)
            {
                return;
            }

            IMaaApiBoundary* boundary = std::exchange(boundary_, nullptr);
            const Handle handle = std::exchange(handle_, Invalid);
            ((*boundary).*Destroy)(handle);
        }

        void reset(
            IMaaApiBoundary& boundary,
            Handle           handle) noexcept
        {
            reset();
            boundary_ = &boundary;
            handle_ = handle;
        }

    private:
        IMaaApiBoundary* boundary_ = nullptr;
        Handle           handle_ = Invalid;
    };

    using ScopedResource = ScopedMaaHandle<
        MaaResourceHandle,
        kInvalidMaaResourceHandle,
        &IMaaApiBoundary::DestroyResource>;
    using ScopedController = ScopedMaaHandle<
        MaaControllerHandle,
        kInvalidMaaControllerHandle,
        &IMaaApiBoundary::DestroyController>;
    using ScopedTasker = ScopedMaaHandle<
        MaaTaskerHandle,
        kInvalidMaaTaskerHandle,
        &IMaaApiBoundary::DestroyTasker>;

    class ScopedAgentClient final
    {
    public:
        ScopedAgentClient() noexcept = default;

        ScopedAgentClient(
            IMaaApiBoundary&     boundary,
            MaaAgentClientHandle handle) noexcept
            : boundary_(&boundary), handle_(handle)
        {
        }

        ScopedAgentClient(const ScopedAgentClient&) = delete;
        ScopedAgentClient& operator=(const ScopedAgentClient&) = delete;

        ScopedAgentClient(ScopedAgentClient&& other) noexcept
            : boundary_(std::exchange(other.boundary_, nullptr)),
              handle_(
                  std::exchange(other.handle_, kInvalidMaaAgentClientHandle))
        {
        }

        ScopedAgentClient& operator=(ScopedAgentClient&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                boundary_ = std::exchange(other.boundary_, nullptr);
                handle_ =
                    std::exchange(other.handle_, kInvalidMaaAgentClientHandle);
            }
            return *this;
        }

        ~ScopedAgentClient() { reset(); }

        MaaAgentClientHandle get() const noexcept { return handle_; }

        explicit operator bool() const noexcept
        {
            return handle_ != kInvalidMaaAgentClientHandle;
        }

        MaaAgentClientHandle release() noexcept
        {
            boundary_ = nullptr;
            return std::exchange(handle_, kInvalidMaaAgentClientHandle);
        }

        void reset() noexcept
        {
            if (!boundary_ || handle_ == kInvalidMaaAgentClientHandle)
            {
                return;
            }

            IMaaApiBoundary* boundary = std::exchange(boundary_, nullptr);
            const auto handle =
                std::exchange(handle_, kInvalidMaaAgentClientHandle);

            // AgentClient cleanup policy is fixed: disconnect before destroy.
            boundary->DisconnectAgentClient(handle);
            boundary->DestroyAgentClient(handle);
        }

        void reset(
            IMaaApiBoundary&     boundary,
            MaaAgentClientHandle handle) noexcept
        {
            reset();
            boundary_ = &boundary;
            handle_ = handle;
        }

    private:
        IMaaApiBoundary*     boundary_ = nullptr;
        MaaAgentClientHandle handle_ = kInvalidMaaAgentClientHandle;
    };
} // namespace Das::Plugins::DasMaaPi
