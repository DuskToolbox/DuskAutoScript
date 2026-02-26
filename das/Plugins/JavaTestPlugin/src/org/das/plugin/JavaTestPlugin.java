package org.das.plugin;

import org.das.*;
import java.math.BigInteger;

/**
 * Java IPC 测试插件
 * 
 * 对照 C++ IpcTestPlugin 实现，用于 IPC 集成测试。
 * 继承 ISwigDasPluginPackage 以支持 SWIG Director 模式。
 */
public class JavaTestPlugin extends ISwigDasPluginPackage {
    
    // 支持的特性列表（对照 C++ IpcTestPlugin）
    private static final DasPluginFeature[] FEATURES = {
        DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY
    };
    
    /**
     * 插件入口方法 - 由 entryPoint 配置指定
     * 对应 C++ 的 DasCoCreatePlugin 函数
     * 
     * @return DasRetBase 包装的插件实例
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
     * 枚举插件特性
     * 
     * 对照 C++ IpcTestPlugin::EnumFeature 实现
     * 
     * @param index 特性索引
     * @return 包含错误码和特性值的返回对象
     */
    @Override
    public DasRetDasPluginFeature EnumFeature(BigInteger index) {
        DasRetDasPluginFeature result = new DasRetDasPluginFeature();
        int idx = index.intValue();
        
        if (idx >= 0 && idx < FEATURES.length) {
            result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
            result.setValue(FEATURES[idx]);
        } else {
            result.setErrorCode(DuskAutoScriptConstants.DAS_E_OUT_OF_RANGE);
        }
        
        return result;
    }
    
    /**
     * 创建特性接口
     * 
     * 对照 C++ IpcTestPlugin::CreateFeatureInterface 实现
     * 当前返回 DAS_E_NO_IMPLEMENTATION（与 C++ 实现一致）
     * 
     * @param index 特性索引
     * @return 包含错误码和接口对象的返回对象
     */
    @Override
    public DasRetDasBase CreateFeatureInterface(BigInteger index) {
        DasRetDasBase result = new DasRetDasBase();
        
        // 对照 C++ IpcTestPlugin 返回 DAS_E_NO_IMPLEMENTATION
        result.setErrorCode(DuskAutoScriptConstants.DAS_E_NO_IMPLEMENTATION);
        result.setValue(null);
        
        return result;
    }
    
    /**
     * 检查插件是否可以卸载
     * 
     * 对照 C++ IpcTestPlugin::CanUnloadNow 实现
     * 
     * @return 包含错误码和布尔值的返回对象
     */
    @Override
    public DasRetBool CanUnloadNow() {
        DasRetBool result = new DasRetBool();
        result.setErrorCode(DuskAutoScriptConstants.DAS_S_OK);
        // 简单实现：总是返回 true（可以卸载）
        result.setValue(true);
        return result;
    }
}
