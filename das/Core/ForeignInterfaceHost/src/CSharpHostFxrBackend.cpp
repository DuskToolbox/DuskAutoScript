#include "CSharpHostFxrBackend.h"

#include <das/DasPtr.hpp>

#include <cstdint>
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
               + manifest.plugin_binary_path.stem().string();
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
        return path.string();
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

    class DynamicHostFxr
    {
    public:
        explicit DynamicHostFxr(std::filesystem::path hostfxr_path)
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
            const std::filesystem::path& runtime_config_path,
            CSharpHostFxrHandle*         handle)
        {
            auto           runtime_config = ToHostString(runtime_config_path);
            hostfxr_handle host_context = nullptr;
            const auto     result = initialize_for_runtime_config_(
                runtime_config.c_str(),
                nullptr,
                &host_context);
            *handle = host_context;
            return result;
        }

        int GetRuntimeDelegate(
            CSharpHostFxrHandle       handle,
            CSharpHostFxrDelegateType type,
            void**                    runtime_delegate)
        {
            if (type
                != CSharpHostFxrDelegateType::LoadAssemblyAndGetFunctionPointer)
            {
                return -1;
            }

            return get_runtime_delegate_(
                static_cast<hostfxr_handle>(handle),
                hdt_load_assembly_and_get_function_pointer,
                runtime_delegate);
        }

        void Close(CSharpHostFxrHandle handle)
        {
            close_(static_cast<hostfxr_handle>(handle));
        }

        static int LoadAssemblyAndGetFunctionPointer(
            void*                        runtime_delegate,
            const std::filesystem::path& assembly_path,
            std::string_view             type_name,
            std::string_view             method_name,
            void**                       entrypoint)
        {
            auto load_assembly =
                reinterpret_cast<load_assembly_and_get_function_pointer_fn>(
                    runtime_delegate);
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
        DynamicLibrary library_;
        hostfxr_initialize_for_runtime_config_fn
            initialize_for_runtime_config_ = nullptr;
        hostfxr_get_runtime_delegate_fn get_runtime_delegate_ = nullptr;
        hostfxr_close_fn                close_ = nullptr;
    };

    CSharpHostFxrApi MakeDynamicHostFxrApi(
        const std::shared_ptr<DynamicHostFxr>& state)
    {
        CSharpHostFxrApi api{};
        api.user_data = state.get();
        api.initialize_for_runtime_config =
            [](const std::filesystem::path& runtime_config,
               CSharpHostFxrHandle*         handle,
               void*                        user_data) -> int
        {
            return static_cast<DynamicHostFxr*>(user_data)
                ->InitializeForRuntimeConfig(runtime_config, handle);
        };
        api.get_runtime_delegate = [](CSharpHostFxrHandle       handle,
                                      CSharpHostFxrDelegateType type,
                                      void** runtime_delegate,
                                      void*  user_data) -> int
        {
            return static_cast<DynamicHostFxr*>(user_data)->GetRuntimeDelegate(
                handle,
                type,
                runtime_delegate);
        };
        api.close = [](CSharpHostFxrHandle handle, void* user_data)
        { static_cast<DynamicHostFxr*>(user_data)->Close(handle); };
        api.load_assembly_and_get_function_pointer =
            [](void*                        runtime_delegate,
               const std::filesystem::path& assembly_path,
               std::string_view             type_name,
               std::string_view             method_name,
               void**                       entrypoint,
               void*) -> int
        {
            return DynamicHostFxr::LoadAssemblyAndGetFunctionPointer(
                runtime_delegate,
                assembly_path,
                type_name,
                method_name,
                entrypoint);
        };
        return api;
    }

    std::pair<CSharpHostFxrApi, std::shared_ptr<void>> LoadDefaultApi()
    {
        const auto hostfxr_path = ResolveHostFxrPath();
        if (hostfxr_path.empty())
        {
            return {};
        }

        auto state = std::make_shared<DynamicHostFxr>(hostfxr_path);
        if (!state->IsReady())
        {
            return {};
        }

        return {MakeDynamicHostFxrApi(state), state};
    }
#else
    std::pair<CSharpHostFxrApi, std::shared_ptr<void>> LoadDefaultApi()
    {
        return {};
    }
#endif
} // namespace

CSharpHostFxrBackend::CSharpHostFxrBackend()
{
    auto [api, state] = LoadDefaultApi();
    api_ = api;
    state_ = std::move(state);
}

CSharpHostFxrBackend::CSharpHostFxrBackend(CSharpHostFxrApi api) : api_{api} {}

CSharpHostFxrBackend::CSharpHostFxrBackend(
    CSharpHostFxrApi      api,
    std::shared_ptr<void> state)
    : api_{api}, state_{std::move(state)}
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

        if (api_.initialize_for_runtime_config == nullptr
            || api_.get_runtime_delegate == nullptr || api_.close == nullptr
            || api_.load_assembly_and_get_function_pointer == nullptr)
        {
            return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
        }

        *bootstrap_args.pp_package = nullptr;

        CSharpHostFxrHandle handle = nullptr;
        const auto          init_result = api_.initialize_for_runtime_config(
            *manifest.runtime_config_path,
            &handle,
            api_.user_data);
        if (init_result < 0 || handle == nullptr)
        {
            return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
        }

        struct HostContextGuard
        {
            CSharpHostFxrApi*   api = nullptr;
            CSharpHostFxrHandle handle = nullptr;

            ~HostContextGuard()
            {
                if (api != nullptr && api->close != nullptr
                    && handle != nullptr)
                {
                    api->close(handle, api->user_data);
                }
            }
        } guard{&api_, handle};

        void*      runtime_delegate = nullptr;
        const auto delegate_result = api_.get_runtime_delegate(
            handle,
            CSharpHostFxrDelegateType::LoadAssemblyAndGetFunctionPointer,
            &runtime_delegate,
            api_.user_data);
        if (delegate_result != 0 || runtime_delegate == nullptr)
        {
            return DAS_E_CSHARP_HOSTFXR_INIT_FAILED;
        }

        void*      entrypoint = nullptr;
        const auto type_name = BuildAssemblyQualifiedTypeName(manifest);
        const auto entrypoint_result =
            api_.load_assembly_and_get_function_pointer(
                runtime_delegate,
                manifest.plugin_binary_path,
                type_name,
                manifest.entry_point.method_name,
                &entrypoint,
                api_.user_data);
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
