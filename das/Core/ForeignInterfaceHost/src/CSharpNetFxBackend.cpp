#include "CSharpNetFxBackend.h"

#include <cstdint>
#include <string>
#include <utility>

#if defined(_WIN32) && defined(DAS_CSHARP_NETFX_BACKEND)
#include <mscoree.h>
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
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

    std::string BuildBootstrapCookie(DasCSharpBootstrapArgsV1& bootstrap_args)
    {
        return std::to_string(
            reinterpret_cast<std::uintptr_t>(&bootstrap_args));
    }

#if defined(_WIN32) && defined(DAS_CSHARP_NETFX_BACKEND)
    std::wstring ToWide(const std::filesystem::path& path)
    {
        return path.wstring();
    }

    std::wstring ToWide(std::string_view value)
    {
        return std::wstring{value.begin(), value.end()};
    }

    class RuntimeHostPtr
    {
    public:
        RuntimeHostPtr() = default;
        explicit RuntimeHostPtr(ICLRRuntimeHost* host) : host_{host} {}

        RuntimeHostPtr(const RuntimeHostPtr&) = delete;
        RuntimeHostPtr& operator=(const RuntimeHostPtr&) = delete;

        RuntimeHostPtr(RuntimeHostPtr&& other) noexcept
            : host_{std::exchange(other.host_, nullptr)}
        {
        }

        RuntimeHostPtr& operator=(RuntimeHostPtr&& other) noexcept
        {
            if (this != &other)
            {
                Reset();
                host_ = std::exchange(other.host_, nullptr);
            }
            return *this;
        }

        ~RuntimeHostPtr() { Reset(); }

        [[nodiscard]]
        ICLRRuntimeHost* Get() const noexcept
        {
            return host_;
        }

        ICLRRuntimeHost* Detach() noexcept
        {
            return std::exchange(host_, nullptr);
        }

        void Reset() noexcept
        {
            if (host_ == nullptr)
            {
                return;
            }

            host_->Release();
            host_ = nullptr;
        }

    private:
        ICLRRuntimeHost* host_ = nullptr;
    };

    class DynamicNetFxClr
    {
    public:
        ~DynamicNetFxClr()
        {
            if (runtime_host_ != nullptr)
            {
                runtime_host_->Release();
                runtime_host_ = nullptr;
            }
        }

        int Start(CSharpNetFxHostHandle* handle)
        {
            if (handle == nullptr)
            {
                return -1;
            }

            if (runtime_host_)
            {
                *handle = runtime_host_;
                return 0;
            }

            static const GUID RUNTIME_HOST_CLASS_ID{
                0x90F1A06E,
                0x7712,
                0x4762,
                {0x86, 0xB5, 0x7A, 0x5E, 0xBA, 0x6B, 0xDB, 0x02}};

            ICLRRuntimeHost* raw_host = nullptr;
            auto             hr = CorBindToRuntimeEx(
                L"v4.0.30319",
                L"wks",
                STARTUP_LOADER_OPTIMIZATION_SINGLE_DOMAIN,
                RUNTIME_HOST_CLASS_ID,
                IID_ICLRRuntimeHost,
                reinterpret_cast<void**>(&raw_host));
            if (FAILED(hr) || raw_host == nullptr)
            {
                return static_cast<int>(hr);
            }

            RuntimeHostPtr host{raw_host};
            hr = host.Get()->Start();
            if (FAILED(hr))
            {
                return static_cast<int>(hr);
            }

            runtime_host_ = host.Detach();
            *handle = runtime_host_;
            return 0;
        }

        int ExecuteInDefaultAppDomain(
            CSharpNetFxHostHandle        handle,
            const std::filesystem::path& assembly_path,
            std::string_view             type_name,
            std::string_view             method_name,
            std::string_view             bootstrap_cookie,
            unsigned long*               return_value)
        {
            auto* runtime_host = static_cast<ICLRRuntimeHost*>(handle);
            if (runtime_host == nullptr || return_value == nullptr)
            {
                return -1;
            }

            const auto wide_assembly_path = ToWide(assembly_path);
            const auto wide_type_name = ToWide(type_name);
            const auto wide_method_name = ToWide(method_name);
            const auto wide_bootstrap_cookie = ToWide(bootstrap_cookie);
            DWORD      managed_return = 0;
            const auto hr = runtime_host->ExecuteInDefaultAppDomain(
                wide_assembly_path.c_str(),
                wide_type_name.c_str(),
                wide_method_name.c_str(),
                wide_bootstrap_cookie.c_str(),
                &managed_return);
            *return_value = managed_return;
            return static_cast<int>(hr);
        }

        void Release(CSharpNetFxHostHandle) noexcept {}

    private:
        ICLRRuntimeHost* runtime_host_ = nullptr;
    };

    CSharpNetFxApi MakeDynamicNetFxApi(
        const std::shared_ptr<DynamicNetFxClr>& state)
    {
        CSharpNetFxApi api{};
        api.user_data = state.get();
        api.start_clr = [](CSharpNetFxHostHandle* handle,
                           void*                  user_data) -> int
        { return static_cast<DynamicNetFxClr*>(user_data)->Start(handle); };
        api.execute_in_default_app_domain =
            [](CSharpNetFxHostHandle        handle,
               const std::filesystem::path& assembly_path,
               std::string_view             type_name,
               std::string_view             method_name,
               std::string_view             bootstrap_cookie,
               unsigned long*               return_value,
               void*                        user_data) -> int
        {
            return static_cast<DynamicNetFxClr*>(user_data)
                ->ExecuteInDefaultAppDomain(
                    handle,
                    assembly_path,
                    type_name,
                    method_name,
                    bootstrap_cookie,
                    return_value);
        };
        api.release = [](CSharpNetFxHostHandle handle, void* user_data)
        { static_cast<DynamicNetFxClr*>(user_data)->Release(handle); };
        return api;
    }

    std::pair<CSharpNetFxApi, std::shared_ptr<void>> LoadDefaultApi()
    {
        auto state = std::make_shared<DynamicNetFxClr>();
        return {MakeDynamicNetFxApi(state), state};
    }
#else
    std::pair<CSharpNetFxApi, std::shared_ptr<void>> LoadDefaultApi()
    {
        return {};
    }
#endif
} // namespace

CSharpNetFxBackend::CSharpNetFxBackend()
{
    auto [api, state] = LoadDefaultApi();
    api_ = api;
    state_ = std::move(state);
}

CSharpNetFxBackend::CSharpNetFxBackend(CSharpNetFxApi api) : api_{api} {}

CSharpNetFxBackend::CSharpNetFxBackend(
    CSharpNetFxApi        api,
    std::shared_ptr<void> state)
    : api_{api}, state_{std::move(state)}
{
}

DasResult CSharpNetFxBackend::LoadPlugin(
    const CSharpManifest&     manifest,
    DasCSharpBootstrapArgsV1& bootstrap_args)
{
#ifndef _WIN32
    (void)manifest;
    (void)bootstrap_args;
    return DAS_E_CSHARP_NETFX_UNSUPPORTED_PLATFORM;
#else
    try
    {
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

        if (api_.start_clr == nullptr
            || api_.execute_in_default_app_domain == nullptr
            || api_.release == nullptr)
        {
            return DAS_E_CSHARP_COM_CLR_INIT_FAILED;
        }

        *bootstrap_args.pp_package = nullptr;

        CSharpNetFxHostHandle handle = nullptr;
        const auto start_result = api_.start_clr(&handle, api_.user_data);
        if (start_result < 0 || handle == nullptr)
        {
            return DAS_E_CSHARP_COM_CLR_INIT_FAILED;
        }

        struct HostHandleGuard
        {
            CSharpNetFxApi*       api = nullptr;
            CSharpNetFxHostHandle handle = nullptr;

            ~HostHandleGuard()
            {
                if (api != nullptr && api->release != nullptr
                    && handle != nullptr)
                {
                    api->release(handle, api->user_data);
                }
            }
        } guard{&api_, handle};

        unsigned long managed_return = 0;
        const auto    bootstrap_cookie = BuildBootstrapCookie(bootstrap_args);
        const auto    entrypoint_result = api_.execute_in_default_app_domain(
            handle,
            manifest.plugin_binary_path,
            manifest.entry_point.type_name,
            manifest.entry_point.method_name,
            bootstrap_cookie,
            &managed_return,
            api_.user_data);
        if (entrypoint_result < 0)
        {
            return DAS_E_CSHARP_ENTRYPOINT_MISSING;
        }

        if (DAS::IsFailed(static_cast<DasResult>(managed_return)))
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
#endif
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END
