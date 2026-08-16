#include "UpdateJobRegistry.h"

namespace Das::Plugins::DasMaaPi
{
    UpdateJobRegistry& UpdateJobRegistry::Instance()
    {
        static UpdateJobRegistry registry;
        return registry;
    }

    std::shared_ptr<UpdateJobSlot> UpdateJobRegistry::TryStart(
        const std::filesystem::path&  pi_dir,
        std::shared_ptr<PiHttpClient> http)
    {
        (void)pi_dir;
        (void)http;
        return nullptr;
    }

    bool UpdateJobRegistry::IsRunning(const std::filesystem::path& pi_dir) const
    {
        (void)pi_dir;
        return false;
    }

    void UpdateJobRegistry::Finish(const std::shared_ptr<UpdateJobSlot>& slot)
    {
        (void)slot;
    }

    bool UpdateJobRegistry::WaitForSlotIdleForTest(
        const std::filesystem::path& pi_dir,
        std::chrono::milliseconds    timeout)
    {
        (void)pi_dir;
        (void)timeout;
        return true;
    }

    UpdateJobRegistry::~UpdateJobRegistry() = default;
} // namespace Das::Plugins::DasMaaPi
