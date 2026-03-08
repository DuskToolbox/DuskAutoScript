# tests/plugins/PythonTestPlugin/python_test_plugin.py
"""
Python 测试插件 - 用于端到端集成测试
参考 IpcTestPlugin2 实现
"""

from DuskAutoScript import (
    ISwigDasPluginPackage,
    IDasComponentFactory,
    IDasComponent,
    DasPluginFeature,
    DasRetBase,
    DasRetDasPluginFeature,
    DasRetDasBase,
    DasRetBool,
    DasGuid,
    DAS_S_OK,
    DAS_E_OUT_OF_RANGE,
    DAS_E_OUT_OF_MEMORY,
)


class SimpleComponent(IDasComponent):
    """简单测试组件"""

    def __init__(self):
        super().__init__()


class TestComponentFactory(IDasComponentFactory):
    """组件工厂实现 - 参考 IpcTestPlugin2 的 DasComponentFactoryImpl"""

    def IsSupported(self, component_iid: DasGuid):
        """检查是否支持指定组件接口"""
        # 简单实现：支持所有请求
        result = DasRetBase()
        result.error_code = DAS_S_OK
        return result

    def CreateInstance(self, component_iid: DasGuid) -> DasRetBase:
        """创建组件实例"""
        result = DasRetBase()
        try:
            component = SimpleComponent()
            result.error_code = DAS_S_OK
            result.value = component
        except Exception:
            result.error_code = DAS_E_OUT_OF_MEMORY
        return result


class PythonTestPlugin(ISwigDasPluginPackage):
    """Python 测试插件 - 参考 IpcTestPlugin2 实现"""

    FEATURES = [DasPluginFeature.DAS_PLUGIN_FEATURE_COMPONENT_FACTORY]
    _factory: TestComponentFactory = None

    @staticmethod
    def create_plugin() -> DasRetBase:
        """入口函数"""
        result = DasRetBase()
        try:
            plugin = PythonTestPlugin()
            result.error_code = DAS_S_OK
            result.value = plugin
        except Exception:
            result.error_code = DAS_E_OUT_OF_MEMORY
        return result

    def EnumFeature(self, index: int) -> DasRetDasPluginFeature:
        """枚举插件特性"""
        result = DasRetDasPluginFeature()

        if 0 <= index < len(self.FEATURES):
            result.error_code = DAS_S_OK
            result.value = self.FEATURES[index]
        else:
            result.error_code = DAS_E_OUT_OF_RANGE
        return result

    def CreateFeatureInterface(self, index: int) -> DasRetDasBase:
        """创建特性接口 - 返回组件工厂"""
        result = DasRetDasBase()

        if index == 0 and len(self.FEATURES) > 0:
            if self._factory is None:
                self._factory = TestComponentFactory()
            result.error_code = DAS_S_OK
            result.value = self._factory
        else:
            result.error_code = DAS_E_OUT_OF_RANGE
        return result

    def CanUnloadNow(self) -> DasRetBool:
        """检查是否可以卸载"""
        result = DasRetBool()
        result.error_code = DAS_S_OK
        # 如果工厂存在，表示有活跃引用，不能卸载
        result.value = (self._factory is None)
        return result
