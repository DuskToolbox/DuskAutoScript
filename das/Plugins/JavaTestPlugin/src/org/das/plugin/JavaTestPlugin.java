package org.das.plugin;

import org.das.*;
import java.math.BigInteger;

/**
 * Java IPC 测试插件
 *
 * 对照 C++ IpcTestPlugin2 实现，用于 IPC 集成测试。
 * 继承 ISwigDasPluginPackage 以支持 SWIG Director 模式。
 *
 * 包含三个组件：
 * - JavaTestPlugin: 插件包入口，提供 DasComponentFactory 特性
 * - DasComponentFactoryImpl: 组件工厂，创建 DasComponentImpl 实例
 * - DasComponentImpl: 组件实现，支持 echo/compute/getSessionInfo 方法
 */
public class JavaTestPlugin extends ISwigDasPluginPackage {

    private static final DasPluginFeature[] FEATURES = {
        DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY
    };

    private int sessionId_ = 0;

    /**
     * 插件入口方法 - 由 entryPoint 配置指定
     * 对应 C++ 的 DasCoCreatePlugin 函数
     */
    public static DasRetBase createInstance() {
        DasRetBase result = new DasRetBase();
        try {
            JavaTestPlugin plugin = new JavaTestPlugin();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(plugin);
            return result;
        } catch (Exception e) {
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_OUT_OF_MEMORY);
            return result;
        }
    }

    /**
     * 对照 C++ IpcTestPlugin2::SetSessionId
     */
    public void SetSessionId(int sessionId) {
        sessionId_ = sessionId;
    }

    /**
     * 对照 C++ IpcTestPlugin2::EnumFeature
     */
    @Override
    public DasRetDasPluginFeature EnumFeature(BigInteger index) {
        DasRetDasPluginFeature result = new DasRetDasPluginFeature();
        int idx = index.intValue();

        if (idx == 0) {
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(FEATURES[0]);
        } else {
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_OUT_OF_RANGE);
        }

        return result;
    }

    /**
     * 对照 C++ IpcTestPlugin2::CreateFeatureInterface
     * index=0 时创建 DasComponentFactoryImpl 实例
     */
    @Override
    public DasRetDasBase CreateFeatureInterface(BigInteger index) {
        DasRetDasBase result = new DasRetDasBase();
        int idx = index.intValue();

        if (idx == 0) {
            DasComponentFactoryImpl factory =
                    new DasComponentFactoryImpl(sessionId_);
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(factory);
        } else {
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_OUT_OF_RANGE);
            result.setValue(null);
        }

        return result;
    }

    /**
     * 对照 C++ IpcTestPlugin2::CanUnloadNow
     */
    @Override
    public DasRetBool CanUnloadNow() {
        DasRetBool result = new DasRetBool();
        result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
        result.setValue(true);
        return result;
    }

    // ==========================================================================
    // DasComponentImpl - 对应 C++ DasComponentImpl
    // ==========================================================================

    /**
     * 对照 C++ DasComponentImpl，实现 IDasComponent。
     * 支持 echo/compute/getSessionInfo 三个方法（当前均为 TODO stub）。
     */
    public static class DasComponentImpl extends ISwigDasComponent {

        private final int sessionId_;

        public DasComponentImpl(int sessionId) {
            super();
            sessionId_ = sessionId;
        }

        /**
         * 对照 C++ DasComponentImpl::GetGuid
         */
        @Override
        public DasRetDasGuid GetGuid() {
            DasRetDasGuid result = new DasRetDasGuid();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(IDasComponent.IID());
            return result;
        }

        /**
         * 对照 C++ DasComponentImpl::GetRuntimeClassName
         */
        @Override
        public DasRetReadOnlyString GetRuntimeClassName() {
            DasRetReadOnlyString result = new DasRetReadOnlyString();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(DasReadOnlyString.fromString("Das.ComponentImpl"));
            return result;
        }

        /**
         * 对照 C++ DasComponentImpl::Dispatch
         */
        @Override
        public DasRetDasVariantVector Dispatch(
                DasReadOnlyString p_function_name,
                IDasVariantVector p_arguments) {
            if (p_function_name == null) {
                DasRetDasVariantVector result = new DasRetDasVariantVector();
                result.setErrorCode(
                        DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
                return result;
            }

            String method = p_function_name.toJavaString();
            if (method == null) {
                DasRetDasVariantVector result = new DasRetDasVariantVector();
                result.setErrorCode(
                        DuskAutoScriptConstants.DAS_E_INVALID_POINTER);
                return result;
            }

            if (method.equals("echo")) {
                return HandleEcho(p_arguments);
            } else if (method.equals("compute")) {
                return HandleCompute(p_arguments);
            } else if (method.equals("getSessionInfo")) {
                return HandleGetSessionInfo(p_arguments);
            }

            DasRetDasVariantVector result = new DasRetDasVariantVector();
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
            return result;
        }

        private DasRetDasVariantVector HandleEcho(IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);

            if (args == null || args.GetSize() < 1) {
                DuskAutoScript.DasLogInfoU8(
                        "[JavaTestPlugin] echo: no arguments provided");
                return result;
            }

            try {
                DasReadOnlyString input =
                        args.GetStringEz(BigInteger.ZERO);
                String inputStr = input.toJavaString();
                DuskAutoScript.DasLogInfoU8(
                        "[JavaTestPlugin] echo: input=\"" + inputStr + "\"");
            } catch (Exception e) {
                DuskAutoScript.DasLogErrorU8(
                        "[JavaTestPlugin] echo: failed to read argument 0: "
                                + e.getMessage());
            }

            return result;
        }

        // TODO: Implement compute - perform calculation and return result
        private DasRetDasVariantVector HandleCompute(IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            return result;
        }

        // TODO: Return session_id as part of result
        private DasRetDasVariantVector HandleGetSessionInfo(
                IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            return result;
        }
    }

    // ==========================================================================
    // DasComponentFactoryImpl - 对应 C++ DasComponentFactoryImpl
    // ==========================================================================

    /**
     * 对照 C++ DasComponentFactoryImpl，实现 IDasComponentFactory。
     * 负责创建 DasComponentImpl 实例。
     */
    public static class DasComponentFactoryImpl extends ISwigDasComponentFactory {

        private final int sessionId_;

        public DasComponentFactoryImpl(int sessionId) {
            super();
            sessionId_ = sessionId;
        }

        /**
         * 对照 C++ DasComponentFactoryImpl::GetGuid
         */
        @Override
        public DasRetDasGuid GetGuid() {
            DasRetDasGuid result = new DasRetDasGuid();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(IDasComponentFactory.IID());
            return result;
        }

        /**
         * 对照 C++ DasComponentFactoryImpl::GetRuntimeClassName
         */
        @Override
        public DasRetReadOnlyString GetRuntimeClassName() {
            DasRetReadOnlyString result = new DasRetReadOnlyString();
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(
                    DasReadOnlyString.fromString("Das.ComponentFactoryImpl"));
            return result;
        }

        /**
         * 对照 C++ DasComponentFactoryImpl::IsSupported
         */
        @Override
        public int IsSupported(DasGuid component_iid) {
            if (DuskAutoScript.IsDasGuidEqual(
                        component_iid, IDasComponent.IID())) {
                return DuskAutoScriptConstants.DAS_S_OK;
            }
            return DuskAutoScriptConstants.DAS_E_NO_IMPLEMENTATION;
        }

        /**
         * 对照 C++ DasComponentFactoryImpl::CreateInstance
         */
        @Override
        public DasRetDasComponent CreateInstance(DasGuid component_iid) {
            DasRetDasComponent result = new DasRetDasComponent();

            if (!DuskAutoScript.IsDasGuidEqual(
                        component_iid, IDasComponent.IID())) {
                result.setErrorCode(
                        DuskAutoScriptConstants.DAS_E_NO_IMPLEMENTATION);
                return result;
            }

            DasComponentImpl instance = new DasComponentImpl(sessionId_);
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(instance);
            return result;
        }
    }
}
