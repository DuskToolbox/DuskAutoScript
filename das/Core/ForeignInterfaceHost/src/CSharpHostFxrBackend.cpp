#ifdef DAS_EXPORT_CSHARP

#include "CSharpHostFxrBackend.h"

#include <das/DasPtr.hpp>
#include <das/Utils/StringUtils.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#ifdef DAS_CSHARP_HOSTFXR_BACKEND
#include <coreclr_delegates.h>
#include <hostfxr.h>
#include <nethost.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
#ifdef _WIN32
    using CSharpComponentEntrypointFn =
        int(__stdcall*)(void* args, std::int32_t size_bytes);
#else
    using CSharpComponentEntrypointFn =
        int (*)(void* args, std::int32_t size_bytes);
#endif

    bool FileExists(const std::filesystem::path& path) noexcept
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec);
    }

    std::string BuildAssemblyQualifiedTypeName(const CSharpManifest& manifest)
    {
        return manifest.entry_point.type_name + ", "
               + std::string{DAS::Utils::U8AsString(
                   manifest.plugin_binary_path.stem().u8string())};
    }

    void ReleaseFailedPackage(DasCSharpBootstrapArgsV1& bootstrap_args)
    {
        if (bootstrap_args.pp_package == nullptr)
        {
            return;
        }

        if (*bootstrap_args.pp_package != nullptr)
        {
            (*bootstrap_args.pp_package)->Release();
            *bootstrap_args.pp_package = nullptr;
        }
    }

#ifdef DAS_CSHARP_HOSTFXR_BACKEND
    std::basic_string<char_t> ToHostString(const std::filesystem::path& path)
    {
#ifdef _WIN32
        return path.wstring();
#else
        return std::string{DAS::Utils::U8AsString(path.u8string())};
#endif
    }

    std::basic_string<char_t> ToHostString(std::string_view value)
    {
#ifdef _WIN32
        return std::wstring{value.begin(), value.end()};
#else
        return std::string{value};
#endif
    }

    class DynamicLibrary
    {
    public:
        DynamicLibrary() = default;
        explicit DynamicLibrary(const std::filesystem::path& path)
        {
#ifdef _WIN32
            handle_ = ::LoadLibraryW(path.wstring().c_str());
#else
            handle_ = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        }

        DynamicLibrary(const DynamicLibrary&) = delete;
        DynamicLibrary& operator=(const DynamicLibrary&) = delete;

        DynamicLibrary(DynamicLibrary&& other) noexcept
            : handle_{std::exchange(other.handle_, nullptr)}
        {
        }

        DynamicLibrary& operator=(DynamicLibrary&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                handle_ = std::exchange(other.handle_, nullptr);
            }
            return *this;
        }

        ~DynamicLibrary() { Reset(); }

        [[nodiscard]]
        bool IsLoaded() const noexcept
        {
            return handle_ != nullptr;
        }

        [[nodiscard]]
        void* Symbol(const char* name) const noexcept
        {
            if (handle_ == nullptr)
            {
                return nullptr;
            }

#ifdef _WIN32
            return reinterpret_cast<void*>(::GetProcAddress(handle_, name));
#else
            return ::dlsym(handle_, name);
#endif
        }

    private:
        void Reset() noexcept
        {
            if (handle_ == nullptr)
            {
                return;
            }

#ifdef _WIN32
            ::FreeLibrary(handle_);
#else
            ::dlclose(handle_);
#endif
            handle_ = nullptr;
        }

#ifdef _WIN32
        HMODULE handle_ = nullptr;
#else
        void* handle_ = nullptr;
#endif
    };

    std::filesystem::path ResolveHostFxrPath()
    {
        using GetHostFxrPathFn = int(NETHOST_CALLTYPE*)(
            char_t * buffer,
            size_t*                       buffer_size,
            const get_hostfxr_parameters* parameters);

#ifndef DAS_CSHARP_NETHOST_LIBRARY_PATH
        return {};
#else
        DynamicLibrary nethost_library{
            std::filesystem::path{DAS_CSHARP_NETHOST_LIBRARY_PATH}};
        if (!nethost_library.IsLoaded())
        {
            return {};
        }

        auto get_hostfxr_path_fn = reinterpret_cast<GetHostFxrPathFn>(
            nethost_library.Symbol("get_hostfxr_path"));
        if (get_hostfxr_path_fn == nullptr)
        {
            return {};
        }

        size_t size = 0;
        int    result = get_hostfxr_path_fn(nullptr, &size, nullptr);
        if (size == 0)
        {
            return {};
        }

        std::basic_string<char_t> buffer(size, char_t{});
        result = get_hostfxr_path_fn(buffer.data(), &size, nullptr);
        if (result != 0)
        {
            return {};
        }

        if (size > 0 && buffer[size - 1] == char_t{})
        {
            buffer.resize(size - 1);
        }

        return std::filesystem::path{buffer};
#endif
    }

    class DynamicHostFxrAssemblyResolver final
        : public ICSharpHostFxrAssemblyResolver
    {
    public:
        explicit DynamicHostFxrAssemblyResolver(void* runtime_delegate)
            : runtime_delegate_{runtime_delegate}
        {
        }

        int LoadAssemblyAndGetFunctionPointer(
            const std::filesystem::path& assembly_path,
            std::string_view             type_name,
            std::string_view             method_name,
            void**                       entrypoint) override
        {
            auto load_assembly =
                reinterpret_cast<load_assembly_and_get_function_pointer_fn>(
                    runtime_delegate_);
            if (load_assembly == nullptr)
            {
                return -1;
            }

            auto host_assembly_path = ToHostString(assembly_path);
            auto host_type_name = ToHostString(type_name);
            auto host_method_name = ToHostString(method_name);
            return load_assembly(
                host_assembly_path.c_str(),
                host_type_name.c_str(),
                host_method_name.c_str(),
                nullptr,
                nullptr,
                entrypoint);
        }

    private:
        void* runtime_delegate_ = nullptr;
    };

    class DynamicHostFxrRuntime final : public ICSharpHostFxrRuntime
    {
    public:
        DynamicHostFxrRuntime(
            hostfxr_handle                  handle,
            hostfxr_get_runtime_delegate_fn get_runtime_delegate,
            hostfxr_close_fn                close)
            : handle_{handle}, get_runtime_delegate_{get_runtime_delegate},
              close_{close}
        {
        }

        DynamicHostFxrRuntime(const DynamicHostFxrRuntime&) = delete;
        DynamicHostFxrRuntime& operator=(const DynamicHostFxrRuntime&) = delete;

        ~DynamicHostFxrRuntime() override
        {
            if (handle_ != nullptr && close_ != nullptr)
            {
                close_(handle_);
                handle_ = nullptr;
            }
        }

        int GetAssemblyResolver(
            std::unique_ptr<ICSharpHostFxrAssemblyResolver>* resolver) override
        {
            if (resolver == nullptr || get_runtime_delegate_ == nullptr
                || handle_ == nullptr)
            {
                return -1;
            }

            *resolver = nullptr;
            void*      runtime_delegate = nullptr;
            const auto result = get_runtime_delegate_(
                handle_,
                hdt_load_assembly_and_get_function_pointer,
                &runtime_delegate);
            if (result != 0 || runtime_delegate == nullptr)
            {
                return result;
            }

            *resolver = std::make_unique<DynamicHostFxrAssemblyResolver>(
                runtime_delegate);
            return result;
        }

    private:
        hostfxr_handle                  handle_ = nullptr;
        hostfxr_get_runtime_delegate_fn get_runtime_delegate_ = nullptr;
        hostfxr_close_fn                close_ = nullptr;
    };

    class DynamicHostFxrLoader final : public ICSharpHostFxrRuntimeLoader
    {
    public:
        explicit DynamicHostFxrLoader(std::filesystem::path hostfxr_path)
            : library_{hostfxr_path}
        {
            if (!library_.IsLoaded())
            {
                return;
            }

            initialize_for_runtime_config_ =
                reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
                    library_.Symbol("hostfxr_initialize_for_runtime_config"));
            get_runtime_delegate_ =
                reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
                    library_.Symbol("hostfxr_get_runtime_delegate"));
            close_ = reinterpret_cast<hostfxr_close_fn>(
                library_.Symbol("hostfxr_close"));

            if (initialize_for_runtime_config_ == nullptr
                || get_runtime_delegate_ == nullptr || close_ == nullptr)
            {
                initialize_for_runtime_config_ = nullptr;
                get_runtime_delegate_ = nullptr;
                close_ = nullptr;
            }
        }

        [[nodiscard]]
        bool IsReady() const noexcept
        {
            return initialize_for_runtime_config_ != nullptr
                   && get_runtime_delegate_ != nullptr && close_ != nullptr;
        }

        int InitializeForRuntimeConfig(
            const std::filesystem::path&            runtime_config_path,
            std::unique_ptr<ICSharpHostFxrRuntime>* runtime) override
        {
            if (runtime == nullptr || !IsReady())
            {
                return -1;
            }

            *runtime = nullptr;
            auto           runtime_config = ToHostString(runtime_config_path);
            hostfxr_handle host_context = nullptr;
            const auto     result = initialize_for_runtime_config_(
                runtime_config.c_str(),
                nullptr,
                &host_context);
            if (result < 0 || host_context == nullptr)
            {
                if (host_context != nullptr)
                {
                    close_(host_context);
                }
                return result;
            }

            *runtime = std::make_unique<DynamicHostFxrRuntime>(
                host_context,
                get_runtime_delegate_,
                close_);
            return result;
        }

    private:
        DynamicLibrary library_;
        hostfxr_initialize_for_runtime_config_fn
            initialize_for_runtime_config_ = nullptr;
        hostfxr_get_runtime_delegate_fn get_runtime_delegate_ = nullptr;
        hostfxr_close_fn                close_ = nullptr;
    };

    std::shared_ptr<ICSharpHostFxrRuntimeLoader> LoadDefaultRuntimeLoader()
    {
        const auto hostfxr_path = ResolveHostFxrPath();
        if (hostfxr_path.empty())
        {
            return nullptr;
        }

        auto loader = std::make_shared<DynamicHostFxrLoader>(hostfxr_path);
        if (!loader->IsReady())
        {
            return nullptr;
        }

        return loader;
    }
#else
    std::shared_ptr<ICSharpHostFxrRuntimeLoader> LoadDefaultRuntimeLoader()
    {
        return nullptr;
    }
#endif
} // namespace

CSharpHostFxrBackend::CSharpHostFxrBackend()
    : runtime_loader_{LoadDefaultRuntimeLoader()}
{
}

CSharpHostFxrBackend::CSharpHostFxrBackend(
    std::shared_ptr<ICSharpHostFxrRuntimeLoader> runtime_loader)
    : runtime_loader_{std::move(runtime_loader)}
{
}

DasResult CSharpHostFxrBackend::LoadPlugin(
    const CSharpManifest&     manifest,
    DasCSharpBootstrapArgsV1& bootstrap_args)
{
    try
    {
        if (!manifest.runtime_config_path
            || !FileExists(*manifest.runtime_config_path))
        {
            return DAS_E_CSHARP_MISSING_RUNTIMECONFIG;
        }

        if (manifest.entry_point.type_name.empty()
            || manifest.entry_point.method_name.empty())
        {
            return DAS_E_CSHARP_ENTRYPOINT_INVALID;
        }

        const auto bootstrap_result =
            ValidateDasCSharpBootstrapArgsV1(&bootstrap_args);
        if (DAS::IsFailed(bootstrap_result))
        {
            return bootstrap_result;
        }

        if (runtime_loader_ == nullptr)
        {
            return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
        }

        *bootstrap_args.pp_package = nullptr;

        std::unique_ptr<ICSharpHostFxrRuntime> runtime;
        const auto init_result = runtime_loader_->InitializeForRuntimeConfig(
            *manifest.runtime_config_path,
            &runtime);
        if (init_result < 0 || runtime == nullptr)
        {
            return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
        }

        std::unique_ptr<ICSharpHostFxrAssemblyResolver> assembly_resolver;
        const auto                                      delegate_result =
            runtime->GetAssemblyResolver(&assembly_resolver);
        if (delegate_result != 0 || assembly_resolver == nullptr)
        {
            return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
        }

        void*      entrypoint = nullptr;
        const auto type_name = BuildAssemblyQualifiedTypeName(manifest);
        const auto entrypoint_result =
            assembly_resolver->LoadAssemblyAndGetFunctionPointer(
                manifest.plugin_binary_path,
                type_name,
                manifest.entry_point.method_name,
                &entrypoint);
        if (entrypoint_result != 0 || entrypoint == nullptr)
        {
            return DAS_E_CSHARP_ENTRYPOINT_MISSING;
        }

        const auto component_entrypoint =
            reinterpret_cast<CSharpComponentEntrypointFn>(entrypoint);
        const auto plugin_result = static_cast<DasResult>(component_entrypoint(
            &bootstrap_args,
            static_cast<std::int32_t>(sizeof(DasCSharpBootstrapArgsV1))));
        if (DAS::IsFailed(plugin_result))
        {
            ReleaseFailedPackage(bootstrap_args);
            return DAS_E_CSHARP_PLUGIN_INIT_FAILED;
        }

        if (*bootstrap_args.pp_package == nullptr)
        {
            return DAS_E_CSHARP_PLUGIN_INIT_FAILED;
        }

        return DAS_S_OK;
    }
    catch (...)
    {
        ReleaseFailedPackage(bootstrap_args);
        return DAS_E_CSHARP_PLUGIN_INIT_FAILED;
    }
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP
