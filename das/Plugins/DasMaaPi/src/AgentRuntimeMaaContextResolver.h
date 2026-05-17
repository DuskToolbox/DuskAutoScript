#pragma once

#include "AgentRuntimeService.h"

#include <das/Plugins/DasMaaPi/AgentRuntimeDto.h>

#include <optional>
#include <string>

namespace Das::Plugins::DasMaaPi::AgentRuntime
{
    bool IsUsableMaaContext(const AgentRuntimeMaaContext& context);

    std::optional<AgentRuntimeMaaContext> ResolveMaaContext(
        const RuntimeRefDto& runtime_ref);

    class ScopedMaaContextRegistration final
    {
    public:
        // Registers borrowed Maa handles; the RAII owner must outlive this
        // guard.
        ScopedMaaContextRegistration(
            RuntimeRefDto          runtime_ref,
            AgentRuntimeMaaContext context);
        ~ScopedMaaContextRegistration();

        ScopedMaaContextRegistration(const ScopedMaaContextRegistration&) =
            delete;
        ScopedMaaContextRegistration& operator=(
            const ScopedMaaContextRegistration&) = delete;

        ScopedMaaContextRegistration(
            ScopedMaaContextRegistration&& other) noexcept;
        ScopedMaaContextRegistration& operator=(
            ScopedMaaContextRegistration&& other) noexcept;

    private:
        void Reset() noexcept;

        RuntimeRefDto runtime_ref_;
        bool          active_ = false;
    };
} // namespace Das::Plugins::DasMaaPi::AgentRuntime
