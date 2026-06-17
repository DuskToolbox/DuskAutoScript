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
 *   - echo: 接收字符串，返回带前缀的字符串
 *   - compute: 接收 [op, a, b]，返回计算结果 (add/sub/mul/div)
 *   - getSessionInfo: 返回 [sessionId, language, componentName]
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

    // ==========================================================================
    // DasComponentImpl - 对应 C++ DasComponentImpl
    // ==========================================================================

    /**
     * 对照 C++ DasComponentImpl，实现 IDasComponent。
     * 支持 echo/compute/getSessionInfo 三个方法。
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
            System.err.println("[JavaTestPlugin] Dispatch received: method='" + method + "'");
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
            } else if (method.equals("bridgeLifecycleTest")) {
                return HandleBridgeLifecycleTest(p_arguments);
            }

            DasRetDasVariantVector result = new DasRetDasVariantVector();
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
            return result;
        }

        private DasRetDasVariantVector HandleEcho(IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();

            if (args == null || args.GetSize() < 1) {
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
                DuskAutoScript.DasLogErrorU8(
                        "[JavaTestPlugin] echo: no arguments provided");
                return result;
            }

            try {
                String inputStr = args.GetStringEz(BigInteger.ZERO).toString();
                String echoResult = "[Java] echo: " + inputStr;
                DuskAutoScript.DasLogInfoU8(
                        "[JavaTestPlugin] echo: input=\"" + inputStr + "\"");

                DasRetIDasVariantVector vecResult =
                        DuskAutoScript.CreateDasRetIDasVariantVector();
                if (DuskAutoScript.IsFailed(vecResult.getErrorCode())) {
                    result.setErrorCode(vecResult.getErrorCode());
                    return result;
                }
                IDasVariantVector outVec = vecResult.getValue();
                outVec.PushBackString(
                        DasReadOnlyString.fromString(echoResult));
                result.setValue(outVec);
                result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            } catch (Exception e) {
                DuskAutoScript.DasLogErrorU8(
                        "[JavaTestPlugin] echo: failed: " + e.getMessage());
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_FAIL);
            }

            return result;
        }

        /**
         * compute: 执行简单计算并返回结果
         * 参数: [操作符, 左操作数, 右操作数]
         *   操作符: "add" | "sub" | "mul" | "div"
         *   操作数: int64
         * 返回: [结果int64]
         */
        private DasRetDasVariantVector HandleCompute(IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();

            if (args == null || args.GetSize() < 3) {
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
                DuskAutoScript.DasLogErrorU8(
                        "[JavaTestPlugin] compute: need 3 args [op, a, b]");
                return result;
            }

            try {
                String op = args.GetStringEz(BigInteger.ZERO).toString();
                long a = args.GetIntEz(BigInteger.ONE);
                long b = args.GetIntEz(BigInteger.valueOf(2));
                long computed = 0;

                if (op.equals("add")) {
                    computed = a + b;
                } else if (op.equals("sub")) {
                    computed = a - b;
                } else if (op.equals("mul")) {
                    computed = a * b;
                } else if (op.equals("div")) {
                    if (b == 0) {
                        result.setErrorCode(
                                DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
                        DuskAutoScript.DasLogErrorU8(
                                "[JavaTestPlugin] compute: division by zero");
                        return result;
                    }
                    computed = a / b;
                } else {
                    result.setErrorCode(
                            DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
                    DuskAutoScript.DasLogErrorU8(
                            "[JavaTestPlugin] compute: unknown op \"" + op
                                    + "\"");
                    return result;
                }

                DuskAutoScript.DasLogInfoU8(
                        "[JavaTestPlugin] compute: " + a + " " + op + " " + b
                                + " = " + computed);

                DasRetIDasVariantVector vecResult =
                        DuskAutoScript.CreateDasRetIDasVariantVector();
                if (DuskAutoScript.IsFailed(vecResult.getErrorCode())) {
                    result.setErrorCode(vecResult.getErrorCode());
                    return result;
                }
                IDasVariantVector outVec = vecResult.getValue();
                outVec.PushBackInt(computed);
                result.setValue(outVec);
                result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            } catch (Exception e) {
                DuskAutoScript.DasLogErrorU8(
                        "[JavaTestPlugin] compute: failed: " + e.getMessage());
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_FAIL);
            }

            return result;
        }

        /**
         * getSessionInfo: 返回当前 session 信息
         * 返回: [sessionId(int64), language(string), componentName(string)]
         */
        private DasRetDasVariantVector HandleGetSessionInfo(
                IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();

            try {
                DuskAutoScript.DasLogInfoU8(
                        "[JavaTestPlugin] getSessionInfo: session_id="
                                + sessionId_);

                DasRetIDasVariantVector vecResult =
                        DuskAutoScript.CreateDasRetIDasVariantVector();
                if (DuskAutoScript.IsFailed(vecResult.getErrorCode())) {
                    result.setErrorCode(vecResult.getErrorCode());
                    return result;
                }
                IDasVariantVector outVec = vecResult.getValue();
                outVec.PushBackInt(sessionId_);
                outVec.PushBackString(
                        DasReadOnlyString.fromString("Java"));
                outVec.PushBackString(
                        DasReadOnlyString.fromString("Das.ComponentImpl"));
                result.setValue(outVec);
                result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            } catch (Exception e) {
                DuskAutoScript.DasLogErrorU8(
                        "[JavaTestPlugin] getSessionInfo: failed: "
                                + e.getMessage());
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_FAIL);
            }

            return result;
        }

        /**
         * bridgeLifecycleTest: 验证 Java Director 桥接生命周期管理
         *
         * 参数: [callback(IDasBase), marker(string)]
         * 返回: VariantVector[BridgeLifecycleDirector]
         *
         * 核心验证:
         * 1. Java 侧通过 as() 将 IDasBase 向下转换为 IDasComponent
         * 2. 返回继承 ISwigDasComponent 的 Director 对象
         * 3. C++ 不持有返回值 → bridge 释放链触发 → GC → 回调
         */
        private DasRetDasVariantVector HandleBridgeLifecycleTest(IDasVariantVector args) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();

            if (args == null || args.GetSize() < 2) {
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_INVALID_ARGUMENT);
                return result;
            }

            try {
                System.err.println("[JavaTestPlugin] bridgeLifecycleTest: ENTERED try block");

                // 验证点 1: IDasBase → IDasComponent 向下转换
                // C++ 侧用 PushBackBase 传 IDasBase*（验证 QI 恢复），
                // Java 侧用 as(IDasComponent.class) 向下转换
                System.err.println("[JavaTestPlugin] bridgeLifecycleTest: calling GetBaseEz");
                IDasBase callbackBase = args.GetBaseEz(BigInteger.ZERO);
                System.err.println("[JavaTestPlugin] bridgeLifecycleTest: GetBaseEz returned, calling as()");
                IDasComponent callback = callbackBase.as(IDasComponent.class);
                System.err.println("[JavaTestPlugin] bridgeLifecycleTest: as() returned");
                String marker = args.GetStringEz(BigInteger.ONE).toString();

                DuskAutoScript.DasLogInfoU8(
                    "[JavaTestPlugin] bridgeLifecycleTest: marker=" + marker
                    + ", as() conversion succeeded");

                // 设置回调（GC 时使用）
                BridgeLifecycleDirector.SetCallback(callback);

                // 验证点 2: 创建继承 ISwigDasComponent 的 Director
                // SWIG 会创建 C++ bridge + NewGlobalRef → 对象不能被 GC
                BridgeLifecycleDirector director = new BridgeLifecycleDirector();

                // 将 Director 通过返回值传给 C++
                // 注意：这不是为了方便，而是为了验证功能：
                // C++ 收到后不处理返回值 → proxy 释放 → bridge 释放链
                DasRetIDasVariantVector vecResult =
                    DuskAutoScript.CreateDasRetIDasVariantVector();
                if (DuskAutoScript.IsFailed(vecResult.getErrorCode())) {
                    result.setErrorCode(vecResult.getErrorCode());
                    return result;
                }
                IDasVariantVector outVec = vecResult.getValue();
                // 故意用 PushBackBase：Director 继承 ISwigDasComponent → IDasComponent → IDasBase
                outVec.PushBackBase(director);
                outVec.PushBackString(
                    DasReadOnlyString.fromString("director_created:" + marker));

                result.setValue(outVec);
                result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);

                // 开线程持续触发 GC（bridge 释放后 Director 才能被 GC）
                final String sentinelMarker = marker;
                new Thread(() -> {
                    try {
                        for (int i = 0; i < 30; i++) {
                            Thread.sleep(500);
                            System.gc();
                            System.runFinalization();
                        }
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                    }
                }, "bridge-lifecycle-gc-thread").start();

                DuskAutoScript.DasLogInfoU8(
                    "[JavaTestPlugin] bridgeLifecycleTest: Director returned via PushBackBase, "
                    + "GC thread started");
            } catch (Exception e) {
                DuskAutoScript.DasLogErrorU8(
                    "[JavaTestPlugin] bridgeLifecycleTest: failed: " + e.getMessage());
                e.printStackTrace();
                // 将异常信息通过返回值传回 C++ 侧（方便诊断）
                // 故意不设 error code，确保 value 被传回
                try {
                    DasRetIDasVariantVector errResult =
                        DuskAutoScript.CreateDasRetIDasVariantVector();
                    if (DuskAutoScript.IsOk(errResult.getErrorCode())) {
                        IDasVariantVector errVec = errResult.getValue();
                        errVec.PushBackString(
                            DasReadOnlyString.fromString(
                                "EXCEPTION: " + e.getClass().getName()
                                + ": " + e.getMessage()));
                        result.setValue(errVec);
                        result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
                        return result;
                    }
                } catch (Exception e2) {
                    // ignore
                }
                result.setErrorCode(DuskAutoScriptConstants.DAS_E_FAIL);
            }

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

    // ==========================================================================
    // BridgeLifecycleDirector - 桥接生命周期验证 Director
    // ==========================================================================

    /**
     * 桥接生命周期验证 Director — 继承 ISwigDasComponent，有 C++ bridge + 全局 JNI ref。
     *
     * 验证 C++ 层面发起的释放操作能否正确触发桥接释放链：
     * C++ proxy Release → IPC Release → bridge Release → ref_count=0
     * → __das_bridge_release → swigTakeOwnership → WeakGlobalRef → GC 可达
     *
     * 只有 bridge 正确释放后，Java 对象才能被 GC。
     * 如果 bridge 释放链路有 bug，对象永远不会被 GC → 回调永远不会触发 → 测试超时失败。
     */
    public static class BridgeLifecycleDirector extends ISwigDasComponent {

        private static IDasComponent CALLBACK_ = null;
        private static final java.util.concurrent.atomic.AtomicBoolean CALLBACK_FIRED_ =
            new java.util.concurrent.atomic.AtomicBoolean(false);
        /** Cleaner 实例（Java 9+），null 表示使用 finalize fallback */
        private static final Object CLEANER_;

        static {
            Object cleaner = null;
            try {
                cleaner = Class.forName("java.lang.ref.Cleaner")
                    .getMethod("create")
                    .invoke(null);
            } catch (Exception e) {
                // fallback to finalize
            }
            CLEANER_ = cleaner;
        }

        public static void SetCallback(IDasComponent callback) {
            CALLBACK_ = callback;
            CALLBACK_FIRED_.set(false);
        }

        private static void dispatchCallback(String source) {
            if (!CALLBACK_FIRED_.compareAndSet(false, true)) {
                return;
            }
            try {
                if (CALLBACK_ != null) {
                    DasRetIDasVariantVector argsResult =
                        DuskAutoScript.CreateDasRetIDasVariantVector();
                    IDasVariantVector outArgs = argsResult.getValue();
                    outArgs.PushBackString(
                        DasReadOnlyString.fromString("bridge_released:" + source));
                    CALLBACK_.Dispatch(
                        DasReadOnlyString.fromString("lifecycle_callback"),
                        outArgs);
                }
            } catch (Exception e) {
                System.err.println("[BRIDGE_LIFECYCLE] callback failed: " + e.getMessage());
            }
        }

        private static void registerCleanerAction(Object cleaner, BridgeLifecycleDirector self) {
            try {
                Runnable action = () -> dispatchCallback("Cleaner");
                Class<?> cleanerClass = Class.forName("java.lang.ref.Cleaner");
                java.lang.reflect.Method registerMethod = cleanerClass.getMethod(
                    "register", Object.class, Runnable.class);
                registerMethod.invoke(cleaner, self, action);
            } catch (Exception e) {
                // fallback to finalize
            }
        }

        public BridgeLifecycleDirector() {
            super();
            if (CLEANER_ != null) {
                registerCleanerAction(CLEANER_, this);
            }
            System.err.println("[BRIDGE_LIFECYCLE] Director created, bridge + GlobalRef active");
        }

        @Override
        @SuppressWarnings({"deprecation", "removal"})
        protected void finalize() {
            dispatchCallback("finalize");
            System.err.println("[BRIDGE_LIFECYCLE] finalize called, bridge was released");
            try {
                super.finalize();
            } catch (Throwable t) {}
        }

        // ===== ISwigDasComponent 必须实现的接口方法（测试不需要功能，提供 stub） =====

        @Override
        public DasRetDasGuid GetGuid() {
            DasRetDasGuid result = new DasRetDasGuid();
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_NO_IMPLEMENTATION);
            return result;
        }

        @Override
        public DasRetReadOnlyString GetRuntimeClassName() {
            DasRetReadOnlyString result = new DasRetReadOnlyString();
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_NO_IMPLEMENTATION);
            return result;
        }

        @Override
        public DasRetDasVariantVector Dispatch(
                DasReadOnlyString p_function_name,
                IDasVariantVector p_arguments) {
            DasRetDasVariantVector result = new DasRetDasVariantVector();
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_NO_IMPLEMENTATION);
            return result;
        }
    }
}
