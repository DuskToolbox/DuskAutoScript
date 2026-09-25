#ifdef DAS_EXPORT_CSHARP

#include "CSharpNetFxBackend.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32) && defined(DAS_CSHARP_NETFX_BACKEND)
#include <metahost.h>
#include <mscoree.h>
#endif

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

namespace
{
    std::string BuildBootstrapCookie(DasCSharpBootstrapArgsV1& bootstrap_args)
    {
        return std::to_string(
            reinterpret_cast<std::uintptr_t>(&bootstrap_args));
    }

#if defined(_WIN32) && defined(DAS_CSHARP_NETFX_BACKEND)
    std::wstring ToWide(const std::filesystem::path& path)
    {
        // Win32 块内 native() 即 wstring 本体，原生串搬运零编码转换。
        return path.native();
    }

    std::wstring ToWide(std::string_view value)
    {
        return std::wstring{value.begin(), value.end()};
    }

    class DynamicNetFxRuntimeSession final : public ICSharpNetFxRuntimeHost
    {
    public:
        explicit DynamicNetFxRuntimeSession(ICLRRuntimeHost* runtime_host)
            : runtime_host_{runtime_host}
        {
        }

        auto ExecuteInDefaultAppDomain(
            const std::filesystem::path& assembly_path,
            std::string_view             type_name,
            std::string_view             method_name,
            std::string_view             bootstrap_cookie) const
            -> DAS::Utils::Expected<DasResult> override
        {
            if (!runtime_host_)
            {
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_COM_CLR_INIT_FAILED);
            }

            const auto wide_assembly_path = ToWide(assembly_path);
            const auto wide_type_name = ToWide(type_name);
            const auto wide_method_name = ToWide(method_name);
            const auto wide_bootstrap_cookie = ToWide(bootstrap_cookie);
            DWORD      managed_return = 0;
            const auto hr = runtime_host_->ExecuteInDefaultAppDomain(
                wide_assembly_path.c_str(),
                wide_type_name.c_str(),
                wide_method_name.c_str(),
                wide_bootstrap_cookie.c_str(),
                &managed_return);
            if (FAILED(hr))
            {
                DAS_CORE_LOG_ERROR(
                    "ICLRRuntimeHost::ExecuteInDefaultAppDomain failed: "
                    "hr={}",
                    static_cast<long>(hr));
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_ENTRYPOINT_MISSING);
            }

            return static_cast<DasResult>(managed_return);
        }

    private:
        // 拷贝构造语义自动 AddRef：会话自持引用，与 loader 生命周期解耦。
        Das::DasPtr<ICLRRuntimeHost> runtime_host_;
    };

    class DynamicNetFxRuntimeLoader final : public ICSharpNetFxRuntimeLoader
    {
    public:
        ~DynamicNetFxRuntimeLoader() override
        {
            if (runtime_host_ != nullptr)
            {
                runtime_host_->Release();
                runtime_host_ = nullptr;
            }
        }

        auto StartRuntime() const -> DAS::Utils::Expected<
            std::unique_ptr<ICSharpNetFxRuntimeHost>> override
        {
            if (runtime_host_ != nullptr)
            {
                return std::make_unique<DynamicNetFxRuntimeSession>(
                    runtime_host_);
            }

            // 非弃用宿主路径：CLRCreateInstance → GetRuntime → GetInterface，
            // 落点与旧 CorBindToRuntimeEx 相同的 ICLRRuntimeHost；工作站 GC
            // 为该路径默认，单域加载优化经 SetDefaultStartupFlags 显式保留。
            // 中间接口经 DasPtr 持有，早退路径自动 Release。
            Das::DasPtr<ICLRMetaHost> meta_host;
            auto                      hr = CLRCreateInstance(
                CLSID_CLRMetaHost,
                IID_ICLRMetaHost,
                meta_host.PutVoid());
            if (FAILED(hr) || !meta_host)
            {
                DAS_CORE_LOG_ERROR(
                    "CLRCreateInstance failed: hr={}",
                    static_cast<long>(hr));
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_COM_CLR_INIT_FAILED);
            }

            Das::DasPtr<ICLRRuntimeInfo> runtime_info;
            hr = meta_host->GetRuntime(
                L"v4.0.30319",
                IID_ICLRRuntimeInfo,
                runtime_info.PutVoid());
            if (FAILED(hr) || !runtime_info)
            {
                DAS_CORE_LOG_ERROR(
                    "ICLRMetaHost::GetRuntime failed: hr={}",
                    static_cast<long>(hr));
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_COM_CLR_INIT_FAILED);
            }

            hr = runtime_info->SetDefaultStartupFlags(
                STARTUP_LOADER_OPTIMIZATION_SINGLE_DOMAIN,
                nullptr);
            if (FAILED(hr))
            {
                DAS_CORE_LOG_ERROR(
                    "ICLRRuntimeInfo::SetDefaultStartupFlags failed: hr={}",
                    static_cast<long>(hr));
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_COM_CLR_INIT_FAILED);
            }

            // DasOutPtr 直接绑定成员槽位：GetInterface 写入、Start 失败
            // 自动 Release，成功后 Keep() 交还 loader 成员持有。
            Das::DasOutPtr<ICLRRuntimeHost> host_out{&runtime_host_};
            hr = runtime_info->GetInterface(
                CLSID_CLRRuntimeHost,
                IID_ICLRRuntimeHost,
                reinterpret_cast<LPVOID*>(host_out.Put()));
            if (FAILED(hr) || runtime_host_ == nullptr)
            {
                DAS_CORE_LOG_ERROR(
                    "ICLRRuntimeInfo::GetInterface failed: hr={}",
                    static_cast<long>(hr));
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_COM_CLR_INIT_FAILED);
            }

            hr = runtime_host_->Start();
            if (FAILED(hr))
            {
                DAS_CORE_LOG_ERROR(
                    "ICLRRuntimeHost::Start failed: hr={}",
                    static_cast<long>(hr));
                return DAS::Utils::MakeUnexpected(
                    DAS_E_CSHARP_COM_CLR_INIT_FAILED);
            }

            host_out.Keep();
            return std::make_unique<DynamicNetFxRuntimeSession>(runtime_host_);
        }

    private:
        // StartRuntime 为 const：惰性填充的运行时缓存属逻辑 const，可变。
        mutable ICLRRuntimeHost* runtime_host_ = nullptr;
    };

    std::shared_ptr<ICSharpNetFxRuntimeLoader> LoadDefaultRuntimeLoader()
    {
        return std::make_shared<DynamicNetFxRuntimeLoader>();
    }
#else
    std::shared_ptr<ICSharpNetFxRuntimeLoader> LoadDefaultRuntimeLoader()
    {
        return nullptr;
    }
#endif
} // namespace

CSharpNetFxBackend::CSharpNetFxBackend()
    : runtime_loader_{LoadDefaultRuntimeLoader()}
{
}

CSharpNetFxBackend::CSharpNetFxBackend(
    std::shared_ptr<ICSharpNetFxRuntimeLoader> runtime_loader)
    : runtime_loader_{std::move(runtime_loader)}
{
}

auto CSharpNetFxBackend::InvokeBootstrapEntrypoint(
    const CSharpManifest&     manifest,
    DasCSharpBootstrapArgsV1& bootstrap_args) -> DAS::Utils::Expected<DasResult>
{
    if (runtime_loader_ == nullptr)
    {
        return DAS::Utils::MakeUnexpected(
            CSharpHost::BackendUnavailableError(
                CSharpTargetFrameworkFamily::NetFx));
    }

    auto runtime_host = runtime_loader_->StartRuntime();
    if (!runtime_host)
    {
        return DAS::Utils::MakeUnexpected(runtime_host.error());
    }

    const auto bootstrap_cookie = BuildBootstrapCookie(bootstrap_args);
    return runtime_host.value()->ExecuteInDefaultAppDomain(
        manifest.plugin_binary_path,
        manifest.entry_point.type_name,
        manifest.entry_point.method_name,
        bootstrap_cookie);
}

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_EXPORT_CSHARP
