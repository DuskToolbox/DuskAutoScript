#pragma once

#include <array>
#include <string_view>

namespace Das::Core::TaskScheduler::FlowControl
{

    struct ComponentSpec
    {
        std::string_view stable_name;
        std::string_view guid;
        std::string_view label;
        std::string_view kind;
    };

    inline constexpr std::array<ComponentSpec, 6> kOfficialComponents{
        ComponentSpec{
            "das.flow.branch",
            "68F10001-0000-4000-8000-000000000001",
            "Branch",
            "branch"},
        ComponentSpec{
            "das.flow.sequence",
            "68F10002-0000-4000-8000-000000000002",
            "Sequence",
            "sequence"},
        ComponentSpec{
            "das.flow.delay",
            "68F10003-0000-4000-8000-000000000003",
            "Delay",
            "delay"},
        ComponentSpec{
            "das.flow.for",
            "68F10004-0000-4000-8000-000000000004",
            "For",
            "for"},
        ComponentSpec{
            "das.flow.while",
            "68F10005-0000-4000-8000-000000000005",
            "While",
            "while"},
        ComponentSpec{
            "das.flow.goto",
            "68F10006-0000-4000-8000-000000000006",
            "Goto",
            "goto"},
    };

} // namespace Das::Core::TaskScheduler::FlowControl
