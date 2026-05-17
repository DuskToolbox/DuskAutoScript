#include "AgentRuntimeMaaContextResolver.h"

#include <map>
#include <mutex>
#include <string_view>
#include <utility>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    namespace
    {
        constexpr std::string_view kMaapiRuntimeSessionKind =
            "maapiRuntimeSession";

        struct RuntimeRefKey
        {
            std::string kind;
            std::string session_id;

            bool operator<(const RuntimeRefKey& other) const
            {
                if (kind != other.kind)
                {
                    return kind < other.kind;
                }
                return session_id < other.session_id;
            }
        };

        std::mutex& RegistryMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        std::map<RuntimeRefKey, AgentRuntimeMaaContext>& Registry()
        {
            static std::map<RuntimeRefKey, AgentRuntimeMaaContext> registry;
            return registry;
        }

        RuntimeRefKey KeyFrom(const RuntimeRefDto& runtime_ref)
        {
            return RuntimeRefKey{
                .kind = runtime_ref.kind,
                .session_id = runtime_ref.session_id};
        }

        bool IsSupportedRuntimeRef(const RuntimeRefDto& runtime_ref)
        {
            return runtime_ref.kind == kMaapiRuntimeSessionKind
                   && !runtime_ref.session_id.empty();
        }
    } // namespace

    bool IsUsableMaaContext(const AgentRuntimeMaaContext& context)
    {
        return context.resource != kInvalidMaaResourceHandle
               && context.controller != kInvalidMaaControllerHandle
               && context.tasker != kInvalidMaaTaskerHandle;
    }

    std::optional<AgentRuntimeMaaContext> ResolveMaaContext(
        const RuntimeRefDto& runtime_ref)
    {
        if (!IsSupportedRuntimeRef(runtime_ref))
        {
            return std::nullopt;
        }

        std::lock_guard lock(RegistryMutex());
        const auto      it = Registry().find(KeyFrom(runtime_ref));
        if (it == Registry().end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    ScopedMaaContextRegistration::ScopedMaaContextRegistration(
        RuntimeRefDto          runtime_ref,
        AgentRuntimeMaaContext context)
        : runtime_ref_(std::move(runtime_ref))
    {
        if (!IsSupportedRuntimeRef(runtime_ref_)
            || !IsUsableMaaContext(context))
        {
            return;
        }

        std::lock_guard lock(RegistryMutex());
        Registry()[KeyFrom(runtime_ref_)] = context;
        active_ = true;
    }

    ScopedMaaContextRegistration::~ScopedMaaContextRegistration() { Reset(); }

    ScopedMaaContextRegistration::ScopedMaaContextRegistration(
        ScopedMaaContextRegistration&& other) noexcept
        : runtime_ref_(std::move(other.runtime_ref_)),
          active_(std::exchange(other.active_, false))
    {
    }

    ScopedMaaContextRegistration& ScopedMaaContextRegistration::operator=(
        ScopedMaaContextRegistration&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            runtime_ref_ = std::move(other.runtime_ref_);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    void ScopedMaaContextRegistration::Reset() noexcept
    {
        if (!active_)
        {
            return;
        }

        std::lock_guard lock(RegistryMutex());
        Registry().erase(KeyFrom(runtime_ref_));
        active_ = false;
    }
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
