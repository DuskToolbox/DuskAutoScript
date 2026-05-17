#include <das/Plugins/DasMaaPi/PiCatalog.h>

namespace Das::Plugins::DasMaaPi
{
    const PiOption* FindOption(const PiCatalog& catalog, std::string_view name)
    {
        for (const auto& option : catalog.options)
        {
            if (option.dto.name == name)
            {
                return &option;
            }
        }
        return nullptr;
    }
} // namespace Das::Plugins::DasMaaPi
