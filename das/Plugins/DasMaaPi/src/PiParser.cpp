#include <das/Plugins/DasMaaPi/PiParser.h>
#include <das/Utils/DasJsonCore.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace Das::Plugins::DasMaaPi
{
    namespace
    {
        constexpr auto kJsoncFlags =
            yyjson::ReadFlag::AllowComments |
            yyjson::ReadFlag::AllowTrailingCommas;

        std::string ReadTextFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                return {};
            }
            return std::string(
                (std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
        }

        std::filesystem::path ResolvePath(
            const std::filesystem::path& base,
            std::string_view             path)
        {
            std::filesystem::path result(path);
            if (result.is_relative())
            {
                result = base / result;
            }
            return std::filesystem::absolute(result).lexically_normal();
        }

        template <typename TValue>
        std::string RawJson(const TValue& value)
        {
            try
            {
                const auto serialized = value.write(yyjson::WriteFlag::NoFlag);
                return std::string(serialized.data(), serialized.size());
            }
            catch (const yyjson::write_error&)
            {
                return {};
            }
        }

        template <typename TObject>
        std::vector<std::string> UnknownFields(
            const TObject&              obj,
            std::initializer_list<char const*> known)
        {
            std::set<std::string_view> known_set(known.begin(), known.end());
            std::vector<std::string>   result;
            for (auto it = obj.begin(); it != obj.end(); ++it)
            {
                const std::string_view key(it->first);
                if (!known_set.contains(key))
                {
                    result.emplace_back(key);
                }
            }
            return result;
        }

        template <typename TObject>
        std::optional<std::string> OptionalString(
            const TObject&    obj,
            std::string_view  key)
        {
            if (!obj.contains(key) || !obj[key].is_string())
            {
                return std::nullopt;
            }
            return std::string(obj[key].as_string().value_or(""));
        }

        template <typename TObject>
        std::string RequiredString(
            const TObject&   obj,
            std::string_view key)
        {
            return OptionalString(obj, key).value_or(std::string{});
        }

        template <typename TObject>
        bool OptionalBool(
            const TObject&   obj,
            std::string_view key,
            bool             fallback = false)
        {
            if (!obj.contains(key) || !obj[key].is_bool())
            {
                return fallback;
            }
            return obj[key].as_bool().value_or(fallback);
        }

        template <typename TValue>
        std::vector<std::string> ReadStringArrayValue(const TValue& value)
        {
            std::vector<std::string> result;
            if (value.is_string())
            {
                result.emplace_back(value.as_string().value_or(""));
                return result;
            }
            auto arr = value.as_array();
            if (!arr)
            {
                return result;
            }
            for (auto it = arr->begin(); it != arr->end(); ++it)
            {
                if (it->is_string())
                {
                    result.emplace_back(it->as_string().value_or(""));
                }
            }
            return result;
        }

        template <typename TObject>
        std::vector<std::string> ReadStringArray(
            const TObject&   obj,
            std::string_view key)
        {
            if (!obj.contains(key))
            {
                return {};
            }
            return ReadStringArrayValue(obj[key]);
        }

        template <typename TObject>
        void FillNamed(PiNamedDto& dto, const TObject& obj)
        {
            dto.name = RequiredString(obj, "name");
            dto.label = OptionalString(obj, "label");
            dto.description = OptionalString(obj, "description");
            dto.icon = OptionalString(obj, "icon");
        }

        template <typename TObject>
        PiRawMetadata MakeRaw(
            const TObject&              obj,
            std::initializer_list<char const*> known)
        {
            return PiRawMetadata{RawJson(obj), UnknownFields(obj, known)};
        }

        template <typename TObject>
        PiController ParseController(
            const TObject&                obj,
            const std::filesystem::path&  base)
        {
            PiController result;
            FillNamed(result.dto, obj);
            result.dto.type = RequiredString(obj, "type");
            result.dto.attach_resource_path =
                ReadStringArray(obj, "attach_resource_path");
            result.dto.option = ReadStringArray(obj, "option");
            for (const auto& path : result.dto.attach_resource_path)
            {
                result.resolved_attach_resource_paths.emplace_back(
                    ResolvePath(base, path));
            }
            result.raw = MakeRaw(
                obj,
                {"name",
                 "label",
                 "description",
                 "icon",
                 "type",
                 "display_short_side",
                 "display_long_side",
                 "display_raw",
                 "permission_required",
                 "attach_resource_path",
                 "option",
                 "adb",
                 "win32",
                 "macos",
                 "wlroots",
                 "playcover",
                 "gamepad"});
            return result;
        }

        template <typename TObject>
        PiResource ParseResource(
            const TObject&                obj,
            const std::filesystem::path&  base)
        {
            PiResource result;
            FillNamed(result.dto, obj);
            result.dto.path = ReadStringArray(obj, "path");
            result.dto.controller = ReadStringArray(obj, "controller");
            result.dto.option = ReadStringArray(obj, "option");
            result.dto.hash = OptionalString(obj, "hash");
            for (const auto& path : result.dto.path)
            {
                result.resolved_paths.emplace_back(ResolvePath(base, path));
            }
            result.raw = MakeRaw(
                obj,
                {"name",
                 "label",
                 "description",
                 "icon",
                 "path",
                 "controller",
                 "option",
                 "hash"});
            return result;
        }

        template <typename TObject>
        PiTask ParseTask(const TObject& obj)
        {
            PiTask result;
            FillNamed(result.dto, obj);
            result.dto.entry = RequiredString(obj, "entry");
            result.dto.default_check = OptionalBool(obj, "default_check");
            result.dto.controller = ReadStringArray(obj, "controller");
            result.dto.resource = ReadStringArray(obj, "resource");
            result.dto.group = ReadStringArray(obj, "group");
            result.dto.option = ReadStringArray(obj, "option");
            if (obj.contains(std::string_view("pipeline_override")))
            {
                result.raw_pipeline_override_json =
                    RawJson(obj[std::string_view("pipeline_override")]);
            }
            result.raw = MakeRaw(
                obj,
                {"name",
                 "label",
                 "description",
                 "icon",
                 "entry",
                 "default_check",
                 "controller",
                 "resource",
                 "group",
                 "option",
                 "pipeline_override"});
            return result;
        }

        template <typename TObject>
        PiGroup ParseGroup(const TObject& obj)
        {
            PiGroup result;
            FillNamed(result.dto, obj);
            result.dto.default_expand = OptionalBool(obj, "default_expand");
            result.raw = MakeRaw(
                obj,
                {"name", "label", "description", "icon", "default_expand"});
            return result;
        }

        template <typename TObject>
        PiOption ParseOption(std::string_view name, const TObject& obj)
        {
            PiOption result;
            result.dto.name = std::string(name);
            result.dto.type = RequiredString(obj, "type");
            if (result.dto.type.empty())
            {
                result.dto.type = "select";
            }
            result.dto.label = OptionalString(obj, "label");
            result.dto.description = OptionalString(obj, "description");
            result.dto.icon = OptionalString(obj, "icon");
            result.dto.controller = ReadStringArray(obj, "controller");
            result.dto.resource = ReadStringArray(obj, "resource");
            result.dto.default_cases = ReadStringArray(obj, "default_case");

            if (obj.contains(std::string_view("cases")))
            {
                if (auto cases = obj[std::string_view("cases")].as_array())
                {
                    for (auto it = cases->begin(); it != cases->end(); ++it)
                    {
                        if (auto case_obj = it->as_object())
                        {
                            PiCaseDto item;
                            FillNamed(item, *case_obj);
                            item.option = ReadStringArray(*case_obj, "option");
                            result.dto.cases.emplace_back(std::move(item));
                        }
                    }
                }
            }

            if (obj.contains(std::string_view("inputs")))
            {
                if (auto inputs = obj[std::string_view("inputs")].as_array())
                {
                    for (auto it = inputs->begin(); it != inputs->end(); ++it)
                    {
                        if (auto input_obj = it->as_object())
                        {
                            PiInputDto item;
                            FillNamed(item, *input_obj);
                            item.default_value =
                                OptionalString(*input_obj, "default");
                            item.pipeline_type =
                                OptionalString(*input_obj, "pipeline_type");
                            item.verify = OptionalString(*input_obj, "verify");
                            item.pattern_msg =
                                OptionalString(*input_obj, "pattern_msg");
                            result.dto.inputs.emplace_back(std::move(item));
                        }
                    }
                }
            }

            if (obj.contains(std::string_view("pipeline_override")))
            {
                result.raw_pipeline_override_json =
                    RawJson(obj[std::string_view("pipeline_override")]);
            }
            result.raw = MakeRaw(
                obj,
                {"type",
                 "label",
                 "description",
                 "icon",
                 "controller",
                 "resource",
                 "cases",
                 "inputs",
                 "default_case",
                 "pipeline_override"});
            return result;
        }

        template <typename TObject>
        PiPreset ParsePreset(const TObject& obj)
        {
            PiPreset result;
            FillNamed(result.dto, obj);
            if (obj.contains(std::string_view("task")))
            {
                if (auto tasks = obj[std::string_view("task")].as_array())
                {
                    for (auto it = tasks->begin(); it != tasks->end(); ++it)
                    {
                        if (auto task_obj = it->as_object())
                        {
                            PiPresetTaskDto task;
                            task.name = RequiredString(*task_obj, "name");
                            task.enabled =
                                OptionalBool(*task_obj, "enabled", true);
                            if (task_obj->contains(std::string_view("option")))
                            {
                                if (auto options =
                                        (*task_obj)[std::string_view("option")]
                                            .as_object())
                                {
                                    for (auto option_it = options->begin();
                                         option_it != options->end();
                                         ++option_it)
                                    {
                                        task.option_names.emplace_back(
                                            option_it->first);
                                    }
                                }
                            }
                            result.dto.task.emplace_back(std::move(task));
                        }
                    }
                }
            }
            result.raw = MakeRaw(
                obj,
                {"name", "label", "description", "icon", "task"});
            return result;
        }

        void AddDiagnostic(
            PiCatalog&                  catalog,
            std::string                 severity,
            std::string                 code,
            std::string                 message,
            const std::filesystem::path& source)
        {
            PiDiagnosticDto diagnostic;
            diagnostic.severity = std::move(severity);
            diagnostic.code = std::move(code);
            diagnostic.message = std::move(message);
            diagnostic.source = source.string();
            catalog.diagnostics.emplace_back(std::move(diagnostic));
        }

        template <typename TObject>
        void MergeObjectOptions(PiCatalog& catalog, const TObject& options)
        {
            for (auto it = options.begin(); it != options.end(); ++it)
            {
                if (!it->second.is_object())
                {
                    continue;
                }
                PiOption option = ParseOption(
                    std::string_view(it->first),
                    *it->second.as_object());
                auto existing = std::find_if(
                    catalog.options.begin(),
                    catalog.options.end(),
                    [&](const PiOption& item) {
                        return item.dto.name == option.dto.name;
                    });
                if (existing == catalog.options.end())
                {
                    catalog.options.emplace_back(std::move(option));
                }
                else
                {
                    *existing = std::move(option);
                }
            }
        }

        template <typename TArray, typename TParse>
        void AppendParsedArray(TArray&& array, TParse parse)
        {
            for (auto it = array.begin(); it != array.end(); ++it)
            {
                if (auto obj = it->as_object())
                {
                    parse(*obj);
                }
            }
        }

        void ParseTranslations(
            PiCatalog&                 catalog,
            const std::filesystem::path& source,
            std::string_view            language,
            const std::filesystem::path& path)
        {
            const auto content = ReadTextFile(path);
            if (content.empty())
            {
                AddDiagnostic(
                    catalog,
                    "warning",
                    "language-read-failed",
                    "Unable to read PI language file",
                    source);
                return;
            }
            auto parsed =
                Das::Utils::ParseYyjsonFromString(content, kJsoncFlags);
            if (!parsed || !parsed->is_object())
            {
                AddDiagnostic(
                    catalog,
                    "warning",
                    "language-parse-failed",
                    "Unable to parse PI language file",
                    source);
                return;
            }
            std::map<std::string, std::string> translations;
            auto obj = parsed->as_object();
            for (auto it = obj->begin(); it != obj->end(); ++it)
            {
                if (it->second.is_string())
                {
                    translations.emplace(
                        it->first,
                        it->second.as_string().value_or(""));
                }
            }
            catalog.translations.emplace(std::string(language), translations);
        }

        void ParseDocument(
            PiCatalog&                 catalog,
            yyjson::value&             document,
            const std::filesystem::path& source,
            bool                       is_import,
            bool                       load_languages,
            std::set<std::filesystem::path>& import_stack);

        void ParseImport(
            PiCatalog&                  catalog,
            const std::filesystem::path& import_path,
            bool                        load_languages,
            std::set<std::filesystem::path>& import_stack)
        {
            const auto normalized =
                std::filesystem::absolute(import_path).lexically_normal();
            if (import_stack.contains(normalized))
            {
                AddDiagnostic(
                    catalog,
                    "error",
                    "import-cycle",
                    "PI import cycle detected",
                    normalized);
                return;
            }

            const auto content = ReadTextFile(normalized);
            if (content.empty())
            {
                AddDiagnostic(
                    catalog,
                    "error",
                    "import-read-failed",
                    "Unable to read PI import file",
                    normalized);
                return;
            }
            auto parsed =
                Das::Utils::ParseYyjsonFromString(content, kJsoncFlags);
            if (!parsed || !parsed->is_object())
            {
                AddDiagnostic(
                    catalog,
                    "error",
                    "import-parse-failed",
                    "Unable to parse PI import file",
                    normalized);
                return;
            }
            import_stack.insert(normalized);
            ParseDocument(
                catalog,
                *parsed,
                normalized,
                true,
                load_languages,
                import_stack);
            import_stack.erase(normalized);
        }

        void ParseDocument(
            PiCatalog&                  catalog,
            yyjson::value&              document,
            const std::filesystem::path& source,
            bool                        is_import,
            bool                        load_languages,
            std::set<std::filesystem::path>& import_stack)
        {
            auto top = document.as_object();
            if (!top)
            {
                AddDiagnostic(
                    catalog,
                    "error",
                    "invalid-root",
                    "PI document root must be an object",
                    source);
                return;
            }

            const auto base = source.parent_path();
            if (!is_import)
            {
                catalog.name = RequiredString(*top, "name");
                catalog.version = OptionalString(*top, "version").value_or("");
                catalog.raw = MakeRaw(
                    *top,
                    {"interface_version",
                     "languages",
                     "name",
                     "label",
                     "title",
                     "icon",
                     "mirrorchyan_rid",
                     "mirrorchyan_multiplatform",
                     "github",
                     "version",
                     "contact",
                     "license",
                     "welcome",
                     "description",
                     "agent",
                     "controller",
                     "resource",
                     "group",
                     "task",
                     "option",
                     "global_option",
                     "import",
                     "preset"});

                if (!top->contains(std::string_view("interface_version"))
                    || (*top)[std::string_view("interface_version")]
                           .as_sint()
                           .value_or(0)
                           != 2)
                {
                    AddDiagnostic(
                        catalog,
                        "error",
                        "unsupported-interface-version",
                        "Only ProjectInterface V2 documents are supported",
                        source);
                }

                if (top->contains(std::string_view("agent")))
                {
                    catalog.raw_agent_json =
                        RawJson((*top)[std::string_view("agent")]);
                }

                if (top->contains(std::string_view("languages")))
                {
                    if (auto languages =
                            (*top)[std::string_view("languages")].as_object())
                    {
                        for (auto it = languages->begin();
                             it != languages->end();
                             ++it)
                        {
                            if (!it->second.is_string())
                            {
                                continue;
                            }
                            const auto resolved = ResolvePath(
                                base,
                                it->second.as_string().value_or(""));
                            catalog.language_paths.emplace(it->first, resolved);
                            if (load_languages)
                            {
                                ParseTranslations(
                                    catalog,
                                    source,
                                    it->first,
                                    resolved);
                            }
                        }
                    }
                }
            }

            if (top->contains(std::string_view("controller")))
            {
                if (auto arr =
                        (*top)[std::string_view("controller")].as_array())
                {
                    AppendParsedArray(*arr, [&](auto& obj) {
                        catalog.controllers.emplace_back(
                            ParseController(obj, base));
                    });
                }
            }

            if (top->contains(std::string_view("resource")))
            {
                if (auto arr =
                        (*top)[std::string_view("resource")].as_array())
                {
                    AppendParsedArray(*arr, [&](auto& obj) {
                        catalog.resources.emplace_back(
                            ParseResource(obj, base));
                    });
                }
            }

            if (top->contains(std::string_view("group")))
            {
                if (auto arr = (*top)[std::string_view("group")].as_array())
                {
                    AppendParsedArray(*arr, [&](auto& obj) {
                        auto group = ParseGroup(obj);
                        const auto existing = std::find_if(
                            catalog.groups.begin(),
                            catalog.groups.end(),
                            [&](const PiGroup& item) {
                                return item.dto.name == group.dto.name;
                            });
                        if (existing == catalog.groups.end())
                        {
                            catalog.groups.emplace_back(std::move(group));
                        }
                    });
                }
            }

            if (top->contains(std::string_view("task")))
            {
                if (auto arr = (*top)[std::string_view("task")].as_array())
                {
                    AppendParsedArray(*arr, [&](auto& obj) {
                        catalog.tasks.emplace_back(ParseTask(obj));
                    });
                }
            }

            if (top->contains(std::string_view("option")))
            {
                if (auto options =
                        (*top)[std::string_view("option")].as_object())
                {
                    MergeObjectOptions(catalog, *options);
                }
            }

            if (top->contains(std::string_view("preset")))
            {
                if (auto arr = (*top)[std::string_view("preset")].as_array())
                {
                    AppendParsedArray(*arr, [&](auto& obj) {
                        catalog.presets.emplace_back(ParsePreset(obj));
                    });
                }
            }

            if (!is_import && top->contains(std::string_view("global_option")))
            {
                for (const auto& name :
                     ReadStringArray(*top, "global_option"))
                {
                    if (const auto* option = FindOption(catalog, name))
                    {
                        catalog.global_options.emplace_back(*option);
                    }
                }
            }

            if (top->contains(std::string_view("import")))
            {
                for (const auto& item : ReadStringArray(*top, "import"))
                {
                    const auto resolved = ResolvePath(base, item);
                    catalog.imports.emplace_back(resolved);
                    ParseImport(
                        catalog,
                        resolved,
                        load_languages,
                        import_stack);
                }
            }

            if (!is_import)
            {
                catalog.global_options.clear();
                if (top->contains(std::string_view("global_option")))
                {
                    for (const auto& name :
                         ReadStringArray(*top, "global_option"))
                    {
                        if (const auto* option = FindOption(catalog, name))
                        {
                            catalog.global_options.emplace_back(*option);
                        }
                    }
                }
            }
        }
    } // namespace

    PiParseResult ParseProjectInterface(const PiParseRequest& request)
    {
        PiParseResult result;
        result.catalog.interface_path =
            std::filesystem::absolute(request.interface_path).lexically_normal();
        result.catalog.interface_directory =
            result.catalog.interface_path.parent_path();

        const auto content = ReadTextFile(result.catalog.interface_path);
        if (content.empty())
        {
            AddDiagnostic(
                result.catalog,
                "error",
                "interface-read-failed",
                "Unable to read ProjectInterface file",
                result.catalog.interface_path);
            return result;
        }

        auto parsed = Das::Utils::ParseYyjsonFromString(content, kJsoncFlags);
        if (!parsed || !parsed->is_object())
        {
            AddDiagnostic(
                result.catalog,
                "error",
                "interface-parse-failed",
                "Unable to parse ProjectInterface JSONC",
                result.catalog.interface_path);
            return result;
        }

        std::set<std::filesystem::path> import_stack{
            result.catalog.interface_path};
        ParseDocument(
            result.catalog,
            *parsed,
            result.catalog.interface_path,
            false,
            request.load_languages,
            import_stack);

        result.ok = std::none_of(
            result.catalog.diagnostics.begin(),
            result.catalog.diagnostics.end(),
            [](const PiDiagnosticDto& diagnostic) {
                return diagnostic.severity == "error";
            });
        return result;
    }
} // namespace Das::Plugins::DasMaaPi
