#pragma once

#include <cassert>
#include <cpp_yyjson.hpp>
#include <string>

namespace Das::Core::TaskScheduler
{

    struct AuthoringValidationResult
    {
        bool        valid = false;
        std::string error_kind;
        std::string message;
    };

    AuthoringValidationResult ValidateAuthoringDocument(
        const yyjson::value& document);

    AuthoringValidationResult ValidateAuthoringChange(
        const yyjson::value& change);

    yyjson::value MakeAuthoringError(
        std::string_view error_kind,
        std::string_view message);

} // namespace Das::Core::TaskScheduler
