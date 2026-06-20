#ifndef DAS_TEST_TESTABLE_PLUGIN_MANAGER_H
#define DAS_TEST_TESTABLE_PLUGIN_MANAGER_H

// =====================================================================
// TestablePluginManager：测试专用 PluginManager 子类。
//
// 用途：把 PluginManager 业务头中已删除的测试辅助方法
// (RegisterTestFeature / SetRemotePluginHostFactoryForTest) 集中到本测试
// 专用头中，保持业务头零测试入侵（没有 ForTest 方法、没有 friend、没有
// 测试专用宏）。
//
// 实现机制：
// - PluginManager 业务头里测试子类需要访问的字段 (loaded_plugins_ /
//   path_to_guid_ / feature_type_index_ / remote_plugin_host_factory_ /
//   mutex_) 和方法 (OnHostProcessExit / OnHeartbeatTimeout) 都已经以
//   protected 暴露，作为合法的"子类扩展点"（不是测试 hack）。
// - TestablePluginManager 继承 PluginManager，用 `using` 声明把 protected
//   成员重新暴露为 public，让测试代码可以直接访问。
// - 重新实现原业务头的 RegisterTestFeature / SetRemotePluginHostFactoryForTest
//   两个测试辅助方法（业务头已删除，测试侧承接）。
//
// 注意：业务头里的 GetIidForFeature 仍是 private，测试子类无法调用。
// Mock 类自己实现等价的 ComputeIidForFeature（switch + DasIidOf<T>()）。
//
// 不依赖 #define private protected hack：那个方案在跨 TU 场景下会失效
// （业务头传递性 include PluginManager.h 会先 set include guard）。
// =====================================================================

#include <das/Core/ForeignInterfaceHost/PluginManager.h>
#include <das/Core/ForeignInterfaceHost/RemotePluginHost.h>
#include <das/_autogen/idl/abi/IDasCapture.h>
#include <das/_autogen/idl/abi/IDasComponent.h>
#include <das/_autogen/idl/abi/IDasErrorLens.h>
#include <das/_autogen/idl/abi/IDasInput.h>
#include <das/_autogen/idl/abi/IDasTask.h>
#include <das/_autogen/idl/abi/IDasTaskAuthoring.h>
#include <das/_autogen/idl/abi/IDasTaskComponent.h>
#include <functional>
#include <memory>
#include <vector>

DAS_CORE_FOREIGNINTERFACEHOST_NS_BEGIN

// =====================================================================
// 测试专用 PluginManager 子类：暴露原 friend 用户与原 *ForTest 方法
// 所需的 private 成员访问能力。
//
// 生产构建里 PluginManager 仍是真正的 private；本类只在测试目标里编译，
// 不会进入 libDasCoreObjects.so / .dll。
// =====================================================================
class TestablePluginManager : public PluginManager
{
public:
    // 继承所有父类构造函数 (C++11)，构造方式和 PluginManager 完全一致
    using PluginManager::PluginManager;

    // ----- 暴露 protected 字段为 public (测试代码直接读写) -----
    // 4 个 PluginManagerGuidTest_*_Test 测试用例直接读写以下索引：
    //   - OnHostProcessExit_DirectCall_CleansUpIndex
    //   - OnHeartbeatTimeout_DirectCall_CleansUpIndex
    //   - HeartbeatTimeout_RealThread_CleansUpIndex
    //   - Shutdown_DoesNotHoldMutexDuringStop
    using PluginManager::loaded_plugins_;
    using PluginManager::path_to_guid_;

    // ----- 暴露 protected 方法为 public (测试代码直接调用) -----
    // OnHostProcessExit / OnHeartbeatTimeout 在业务头里是真正的 protected
    // (回调入口子类触发是合理的)，测试侧用 using 暴露后可直接触发以
    // 验证索引清理逻辑。
    using PluginManager::OnHeartbeatTimeout;
    using PluginManager::OnHostProcessExit;

    // ----- 重新实现原 RegisterTestFeature (业务头已删除) -----
    // 3 参数签名与原业务头完全一致，所有调用点无需改动。
    // 注意：业务头里的 GetIidForFeature 是 private，测试侧调用会因为
    // MSVC mangling (private=A, protected=I) 与业务库不匹配导致链接失败。
    // 这里改用等价的本地实现 ComputeIidForFeature (不调用父类 private 方法)。
    void RegisterTestFeature(
        Das::PluginInterface::DasPluginFeature type,
        const DasGuid&                         plugin_guid,
        IDasBase*                              interface_ptr)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 用静态容器持有 FeatureInfo 生命周期到进程结束 (保留原实现语义)
        static std::vector<std::unique_ptr<FeatureInfo>> test_features;
        auto fi = std::make_unique<FeatureInfo>();
        fi->feature_index = 0;
        fi->feature_type = type;
        fi->iid = ComputeIidForFeature(type);
        fi->interface_ptr = interface_ptr;
        fi->plugin_guid = plugin_guid;
        fi->plugin_name = "test_plugin";
        fi->session_id = 0;
        // object_id 保持默认构造 (与原业务实现一致)

        auto* raw = fi.get();
        test_features.push_back(std::move(fi));

        feature_type_index_[type].push_back(raw);
    }

    // ----- 重新实现原 SetRemotePluginHostFactoryForTest (业务头已删除) -----
    // 1 参数签名与原业务头完全一致，所有调用点无需改动。
    void SetRemotePluginHostFactoryForTest(
        std::function<std::unique_ptr<IRemotePluginHost>()> factory)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remote_plugin_host_factory_ = std::move(factory);
    }

private:
    // 等价于 PluginManager::GetIidForFeature 的本地实现。
    // 业务头里 GetIidForFeature 是 private，测试侧调用会因为 MSVC
    // mangling 不匹配导致链接失败，所以这里复制等价逻辑 (switch + 模板)。
    // 模板调用 DasIidOf<T>() 是编译期解析，不涉及 mangled name 冲突。
    static DasGuid ComputeIidForFeature(
        Das::PluginInterface::DasPluginFeature feature)
    {
        using namespace Das::PluginInterface;

        switch (feature)
        {
        case DAS_PLUGIN_FEATURE_CAPTURE_FACTORY:
            return DasIidOf<IDasCaptureFactory>();
        case DAS_PLUGIN_FEATURE_ERROR_LENS:
            return DasIidOf<IDasErrorLens>();
        case DAS_PLUGIN_FEATURE_TASK:
            return DasIidOf<IDasTask>();
        case DAS_PLUGIN_FEATURE_INPUT_FACTORY:
            return DasIidOf<IDasInputFactory>();
        case DAS_PLUGIN_FEATURE_COMPONENT_FACTORY:
            return DasIidOf<IDasComponentFactory>();
        case DAS_PLUGIN_FEATURE_TASK_AUTHORING_FACTORY:
            return DasIidOf<IDasTaskAuthoringSessionFactory>();
        case DAS_PLUGIN_FEATURE_TASK_COMPONENT_FACTORY:
            return DasIidOf<IDasTaskComponentFactory>();
        default:
            return DasGuid{}; // 空 GUID
        }
    }
};

DAS_CORE_FOREIGNINTERFACEHOST_NS_END

#endif // DAS_TEST_TESTABLE_PLUGIN_MANAGER_H
