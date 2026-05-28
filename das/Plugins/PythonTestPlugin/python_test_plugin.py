# das/Plugins/PythonTestPlugin/python_test_plugin.py
"""
Python 测试插件 - 用于端到端集成测试
参考 IpcTestPlugin2 实现
"""

from DuskAutoScript import (
    ISwigDasPluginPackage,
    ISwigDasComponentFactory,
    ISwigDasComponent,
    DasRetBase,
    DasRetDasPluginFeature,
    DasRetDasBase,
    DasRetDasComponent,
    DasRetDasVariantVector,
    DasRetBool,
    DAS_S_OK,
    DAS_E_FAIL,
    DAS_E_INVALID_ARGUMENT,
    DAS_E_NO_IMPLEMENTATION,
    DAS_E_OUT_OF_RANGE,
    DAS_E_OUT_OF_MEMORY,
    DAS_PLUGIN_FEATURE_COMPONENT_FACTORY,
    CreateDasRetIDasVariantVector,
    DasReadOnlyString,
    IsFailed,
)


DETERMINISTIC_FAILURE_METHOD = "failDeterministically"
RAISE_EXCEPTION_METHOD = "raisePythonException"
CALLBACK_FAILURE_METHOD = "bridgeLifecycleCallbackFailure"


def _component_name() -> str:
    if __name__ == "python_folder_test_plugin":
        return "PythonFolderTestPlugin"
    return "PythonTestPlugin"


def _failed_variant_result(error_code: int) -> DasRetDasVariantVector:
    result = DasRetDasVariantVector()
    result.error_code = error_code
    return result


def _new_variant_vector_result():
    result = DasRetDasVariantVector()
    vector_result = CreateDasRetIDasVariantVector()
    error_code = vector_result.GetErrorCode()
    if IsFailed(error_code):
        result.error_code = error_code
        return result, None

    result.error_code = DAS_S_OK
    result.value = vector_result.GetValue()
    return result, result.value


def _read_string(value):
    if value is None:
        return None
    if isinstance(value, str):
        return value

    text = value.GetUtf8()
    if isinstance(text, bytes):
        return text.decode("utf-8")
    if isinstance(text, str):
        return text
    if hasattr(text, "GetErrorCode") and hasattr(text, "GetValue"):
        if IsFailed(text.GetErrorCode()):
            return None
        return _read_string(text.GetValue())
    return str(text)


def _get_string(args, index: int):
    value_result = args.GetString(index)
    if IsFailed(value_result.GetErrorCode()):
        return None
    return _read_string(value_result.GetValue())


def _get_int(args, index: int):
    value_result = args.GetInt(index)
    if IsFailed(value_result.GetErrorCode()):
        return None
    return int(value_result.GetValue())


def _get_component(args, index: int):
    value_result = args.GetComponent(index)
    if IsFailed(value_result.GetErrorCode()):
        return None
    return value_result.GetValue()


def _push_string(vector, value: str) -> int:
    return vector.PushBackString(DasReadOnlyString(value))


def _result_error_code(value) -> int:
    if value is None:
        return DAS_E_FAIL
    if isinstance(value, int):
        return int(value)
    if hasattr(value, "GetErrorCode"):
        return int(value.GetErrorCode())
    if hasattr(value, "error_code"):
        return int(value.error_code)
    return DAS_S_OK


def _dispatch_lifecycle_callback(callback, status: str) -> int:
    if callback is None:
        return DAS_E_INVALID_ARGUMENT

    result, vector = _new_variant_vector_result()
    if vector is None:
        return _result_error_code(result)
    if IsFailed(_push_string(vector, status)):
        return DAS_E_FAIL

    dispatch_result = callback.Dispatch(
        DasReadOnlyString("lifecycle_callback"),
        vector,
    )
    return _result_error_code(dispatch_result)


class BridgeLifecycleDirector(ISwigDasComponent):
    """Director returned to C++ so bridge release can call back into IPC."""

    def __init__(self, callback, marker: str):
        super().__init__()
        self._callback = callback
        self._marker = marker
        self._release_sent = False

    def Dispatch(self, p_function_name, p_arguments) -> DasRetDasVariantVector:
        try:
            method = _read_string(p_function_name)
            if method == "getSessionInfo":
                return self._handle_get_session_info()
            return _failed_variant_result(DAS_E_NO_IMPLEMENTATION)
        except Exception:
            return _failed_variant_result(DAS_E_FAIL)

    def _handle_get_session_info(self) -> DasRetDasVariantVector:
        result, vector = _new_variant_vector_result()
        if vector is None:
            return result
        if IsFailed(_push_string(vector, "Python")):
            return _failed_variant_result(DAS_E_FAIL)
        if IsFailed(_push_string(vector, _component_name())):
            return _failed_variant_result(DAS_E_FAIL)
        if IsFailed(_push_string(vector, "Director")):
            return _failed_variant_result(DAS_E_FAIL)
        return result

    def _das_bridge_release(self) -> int:
        return self._dispatch_release_callback()

    def __del__(self):
        try:
            self._dispatch_release_callback()
        except Exception:
            pass

    def _dispatch_release_callback(self) -> int:
        if self._release_sent:
            return DAS_S_OK
        self._release_sent = True
        return _dispatch_lifecycle_callback(
            self._callback,
            f"bridge_released:Python:{self._marker}",
        )


class SimpleComponent(ISwigDasComponent):
    """简单测试组件"""

    def __init__(self, session_id: int):
        super().__init__()
        self._session_id = session_id

    def Dispatch(self, p_function_name, p_arguments) -> DasRetDasVariantVector:
        """普通组件调度入口，所有失败都转换为 DasResult。"""
        try:
            method = _read_string(p_function_name)
            if method is None:
                return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

            if method == "getSessionInfo":
                return self._handle_get_session_info()
            if method == "echo":
                return self._handle_echo(p_arguments)
            if method == "compute":
                return self._handle_compute(p_arguments)
            if method == "bridgeLifecycleTest":
                return self._handle_bridge_lifecycle_test(p_arguments)
            if method == CALLBACK_FAILURE_METHOD:
                return self._handle_lifecycle_callback_failure(p_arguments)
            if method == DETERMINISTIC_FAILURE_METHOD:
                return _failed_variant_result(DAS_E_FAIL)
            if method == RAISE_EXCEPTION_METHOD:
                raise RuntimeError("deterministic Python exception")

            return _failed_variant_result(DAS_E_NO_IMPLEMENTATION)
        except Exception:
            return _failed_variant_result(DAS_E_FAIL)

    def _handle_get_session_info(self) -> DasRetDasVariantVector:
        result, vector = _new_variant_vector_result()
        if vector is None:
            return result

        if IsFailed(vector.PushBackInt(self._session_id)):
            return _failed_variant_result(DAS_E_FAIL)
        if IsFailed(_push_string(vector, "Python")):
            return _failed_variant_result(DAS_E_FAIL)
        if IsFailed(_push_string(vector, _component_name())):
            return _failed_variant_result(DAS_E_FAIL)
        return result

    def _handle_echo(self, args) -> DasRetDasVariantVector:
        if args is None or args.GetSize() < 1:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        input_value = _get_string(args, 0)
        if input_value is None:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        result, vector = _new_variant_vector_result()
        if vector is None:
            return result

        if IsFailed(_push_string(vector, f"[Python] echo: {input_value}")):
            return _failed_variant_result(DAS_E_FAIL)
        return result

    def _handle_compute(self, args) -> DasRetDasVariantVector:
        if args is None or args.GetSize() < 3:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        operation = _get_string(args, 0)
        left = _get_int(args, 1)
        right = _get_int(args, 2)
        if operation is None or left is None or right is None:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        if operation == "add":
            computed = left + right
        elif operation == "sub":
            computed = left - right
        elif operation == "mul":
            computed = left * right
        elif operation == "div" and right != 0:
            computed = int(left / right)
        else:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        result, vector = _new_variant_vector_result()
        if vector is None:
            return result

        if IsFailed(vector.PushBackInt(computed)):
            return _failed_variant_result(DAS_E_FAIL)
        return result

    def _handle_bridge_lifecycle_test(self, args) -> DasRetDasVariantVector:
        if args is None or args.GetSize() < 2:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        callback = _get_component(args, 0)
        marker = _get_string(args, 1)
        if callback is None or marker is None:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        result, vector = _new_variant_vector_result()
        if vector is None:
            return result

        if IsFailed(_push_string(vector, f"director_created:{marker}")):
            return _failed_variant_result(DAS_E_FAIL)

        director = BridgeLifecycleDirector(callback, marker)
        if IsFailed(vector.PushBackComponent(director)):
            return _failed_variant_result(DAS_E_FAIL)
        return result

    def _handle_lifecycle_callback_failure(self, args) -> DasRetDasVariantVector:
        if args is None or args.GetSize() < 2:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        callback = _get_component(args, 0)
        marker = _get_string(args, 1)
        if callback is None or marker is None:
            return _failed_variant_result(DAS_E_INVALID_ARGUMENT)

        callback_result = _dispatch_lifecycle_callback(
            callback,
            f"bridge_setup_failure:Python:{marker}",
        )
        if IsFailed(callback_result):
            return _failed_variant_result(callback_result)
        return _failed_variant_result(DAS_E_FAIL)


class TestComponentFactory(ISwigDasComponentFactory):
    """组件工厂实现 - 参考 IpcTestPlugin2 的 DasComponentFactoryImpl"""

    def __init__(self, session_id: int):
        super().__init__()
        self._session_id = session_id
        self._components = []

    def IsSupported(self, component_iid):
        """检查是否支持指定组件接口"""
        return DAS_S_OK

    def CreateInstance(self, component_iid) -> DasRetDasComponent:
        """创建组件实例"""
        result = DasRetDasComponent()
        try:
            component = SimpleComponent(self._session_id)
            self._components.append(component)
            result.error_code = DAS_S_OK
            result.value = component
        except Exception:
            result.error_code = DAS_E_OUT_OF_MEMORY
        return result


class PythonTestPlugin(ISwigDasPluginPackage):
    """Python 测试插件 - 参考 IpcTestPlugin2 实现"""

    FEATURES = [DAS_PLUGIN_FEATURE_COMPONENT_FACTORY]
    _factory: TestComponentFactory = None

    def __init__(self):
        super().__init__()
        self._session_id = 0

    def SetSessionId(self, session_id: int):
        self._session_id = int(session_id)
        return DAS_S_OK

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
                self._factory = TestComponentFactory(self._session_id)
            result.error_code = DAS_S_OK
            result.value = self._factory
        else:
            result.error_code = DAS_E_OUT_OF_RANGE
        return result

    def CanUnloadNow(self) -> DasRetBool:
        """检查是否可以卸载"""
        result = DasRetBool()
        result.error_code = DAS_S_OK
        result.value = True
        return result


# 模块级入口函数
def create_plugin() -> DasRetBase:
    """入口函数 - 由 PythonHost 调用"""
    result = DasRetBase()
    try:
        plugin = PythonTestPlugin()
        result.error_code = DAS_S_OK
        result.value = plugin
    except Exception:
        result.error_code = DAS_E_OUT_OF_MEMORY
    return result
