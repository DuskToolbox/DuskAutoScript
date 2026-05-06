-- LuaTestPlugin.lua
-- Lua Test Plugin for IPC integration testing
-- 被 C++ 端 LuaHost::LoadPlugin 加载
--
-- 对照 JavaTestPlugin.java 实现，用于 IPC 集成测试。
-- 继承 ILuaDasPluginPackage 以支持 Director 模式。
--
-- 包含三个组件（与 Java 版对齐）：
-- - PluginPackage: 插件包入口，提供 DasComponentFactory 特性
-- - ComponentFactory: 组件工厂，创建 Component 实例
-- - Component: 组件实现，支持 echo/compute/getSessionInfo/bridgeLifecycleTest
--
-- Lua Director [out] 参数约定:
--   Director override 方法中，[out] 参数通过 Lua 多返回值传递。
--   Lua 函数签名: return DasResult, out_val1, out_val2, ...
--   例: CreateInstance(const DasGuid&, [out] IDasComponent**)
--       → Lua: function(self, guid) return DAS_S_OK, component end

local LuaTestPlugin = {}

-- ==========================================================================
-- BridgeLifecycleDirector — 桥接生命周期验证 Director
-- 对照 Java BridgeLifecycleDirector (ISwigDasComponent)
--
-- 验证 C++ 层面发起的释放操作能否正确触发桥接释放链：
-- C++ proxy Release → IPC Release → bridge Release → LuaDirector Release
-- → __gc 元方法 → Director 析构 → 触发回调
--
-- 只有 bridge 正确释放后，Lua 对象才能被 GC。
-- 如果 bridge 释放链路有 bug，对象永远不会被 GC → 回调永远不会触发 → 测试超时失败。
-- ==========================================================================

local bridge_callback = nil
local callback_fired = false

--- 设置回调（C++ 侧传入的 IDasComponent）
--- 对照 Java: BridgeLifecycleDirector.SetCallback(callback)
function LuaTestPlugin.SetBridgeCallback(callback)
    bridge_callback = callback
    callback_fired = false
end

--- 触发回调（__gc 时调用）
--- 对照 Java: BridgeLifecycleDirector.dispatchCallback(source)
local function dispatch_callback(source)
    if callback_fired then
        return
    end
    callback_fired = true
    if bridge_callback then
        -- 构造 lifecycle_callback 参数并调用 Dispatch
        local out_args = CreateIDasVariantVector()
        if out_args then
            out_args:PushBackString("bridge_released:" .. source)
            bridge_callback:Dispatch("lifecycle_callback", out_args)
        end
    end
end

--- 创建 BridgeLifecycleDirector 实例
--- 对照 Java: new BridgeLifecycleDirector() extends ISwigDasComponent
local function create_bridge_lifecycle_director()
    local director = ILuaDasComponent({
        Dispatch = function(self, function_name, arguments)
            -- stub: 不需要实际功能
            return 1  -- DAS_E_NO_IMPLEMENTATION
        end,
    })

    -- 注意: Lua Director 的 __gc 元方法由 C++ 侧 LuaDirector 基类管理
    -- 当 C++ bridge Release 后 ref_count 降到 0 → 析构 → __gc 触发
    -- 回调机制需要在 C++ 侧 LuaHost::OnDirectorRelease 中触发
    return director
end

-- ==========================================================================
-- createInstance — 插件入口方法
-- 对照 Java: JavaTestPlugin.createInstance()
-- ==========================================================================

function LuaTestPlugin.createInstance()
    local session_id = 0

    -- PluginPackage: ILuaDasPluginPackage Director
    -- 对照 Java: JavaTestPlugin extends ISwigDasPluginPackage
    local package = ILuaDasPluginPackage({
        SetSessionId = function(self, sid)
            session_id = sid
        end,

        EnumFeature = function(self, index)
            -- 对照 Java: EnumFeature(BigInteger index)
            -- [out] DasPluginFeature* 通过多返回值传递
            if index == 0 then
                return 0  -- DAS_S_OK (feature 未填充)
            end
            return 1  -- DAS_E_NOT_FOUND
        end,

        CreateFeatureInterface = function(self, index)
            -- 对照 Java: CreateFeatureInterface(BigInteger index)
            -- [out] IDasBase** 通过多返回值传递
            if index == 0 then
                local factory = LuaTestPlugin._create_factory(session_id)
                return 0, factory  -- DAS_S_OK + [out] IDasBase*
            end
            return 1  -- DAS_E_NOT_FOUND
        end,

        CanUnloadNow = function(self)
            -- 对照 Java: CanUnloadNow() → true
            return 0, true  -- DAS_S_OK + [out] bool
        end,
    })
    return package
end

-- ==========================================================================
-- ComponentFactory — 对照 Java DasComponentFactoryImpl
-- ==========================================================================

function LuaTestPlugin._create_factory(session_id)
    return ILuaDasComponentFactory({
        IsSupported = function(self, component_iid)
            -- 对照 Java: IsSupported(DasGuid)
            return 0  -- DAS_S_OK
        end,

        CreateInstance = function(self, component_iid)
            -- 对照 Java: CreateInstance(DasGuid) → DasRetDasComponent
            -- [out] IDasComponent** 通过多返回值传递
            local component = LuaTestPlugin._create_component(session_id)
            return 0, component  -- DAS_S_OK + [out] IDasComponent*
        end,
    })
end

-- ==========================================================================
-- Component — 对照 Java DasComponentImpl
-- 支持 echo/compute/getSessionInfo/bridgeLifecycleTest
-- ==========================================================================

function LuaTestPlugin._create_component(session_id)
    return ILuaDasComponent({
        Dispatch = function(self, function_name, arguments)
            -- 对照 Java: Dispatch(DasReadOnlyString, IDasVariantVector)
            -- [out] IDasVariantVector** 通过多返回值传递

            local name_str = nil
            if function_name then
                local hr, str = function_name:GetUtf8()
                if hr >= 0 and str then
                    name_str = str
                end
            end

            if not name_str then
                return 1  -- DAS_E_INVALID_ARGUMENT
            end

            -- 路由到具体处理函数
            if name_str == "echo" then
                return LuaTestPlugin._handle_echo(arguments)
            elseif name_str == "compute" then
                return LuaTestPlugin._handle_compute(arguments)
            elseif name_str == "getSessionInfo" then
                return LuaTestPlugin._handle_get_session_info(session_id, arguments)
            elseif name_str == "bridgeLifecycleTest" then
                return LuaTestPlugin._handle_bridge_lifecycle_test(arguments)
            end

            return 1  -- DAS_E_INVALID_ARGUMENT
        end,
    })
end

-- ==========================================================================
-- echo: 接收字符串，返回带前缀的字符串
-- 对照 Java: HandleEcho(IDasVariantVector)
-- ==========================================================================

function LuaTestPlugin._handle_echo(arguments)
    if not arguments then
        return 1  -- DAS_E_INVALID_ARGUMENT
    end

    local hr, input_str = arguments:GetString(0)
    if hr < 0 or not input_str then
        return 1
    end

    local echo_result = "[Lua] echo: " .. input_str

    local out_args = CreateIDasVariantVector()
    if out_args then
        out_args:PushBackString(echo_result)
        return 0, out_args  -- DAS_S_OK + [out] IDasVariantVector*
    end
    return 1
end

-- ==========================================================================
-- compute: 执行简单计算并返回结果
-- 对照 Java: HandleCompute(IDasVariantVector)
-- 参数: [操作符, 左操作数, 右操作数]
--   操作符: "add" | "sub" | "mul" | "div"
--   操作数: int64
-- 返回: [结果int64]
-- ==========================================================================

function LuaTestPlugin._handle_compute(arguments)
    if not arguments then
        return 1
    end

    local hr1, op = arguments:GetString(0)
    if hr1 < 0 or not op then
        return 1
    end

    local hr2, a = arguments:GetInt(1)
    local hr3, b = arguments:GetInt(2)
    if hr2 < 0 or hr3 < 0 then
        return 1
    end

    local computed = 0
    if op == "add" then
        computed = a + b
    elseif op == "sub" then
        computed = a - b
    elseif op == "mul" then
        computed = a * b
    elseif op == "div" then
        if b == 0 then
            return 1  -- division by zero
        end
        computed = a / b
    else
        return 1  -- unknown op
    end

    local out_args = CreateIDasVariantVector()
    if out_args then
        out_args:PushBackInt(computed)
        return 0, out_args
    end
    return 1
end

-- ==========================================================================
-- getSessionInfo: 返回当前 session 信息
-- 对照 Java: HandleGetSessionInfo(IDasVariantVector)
-- 返回: [sessionId(int64), language(string), componentName(string)]
-- ==========================================================================

function LuaTestPlugin._handle_get_session_info(session_id, arguments)
    local out_args = CreateIDasVariantVector()
    if out_args then
        out_args:PushBackInt(session_id)
        out_args:PushBackString("Lua")
        out_args:PushBackString("Das.ComponentImpl")
        return 0, out_args
    end
    return 1
end

-- ==========================================================================
-- bridgeLifecycleTest: 验证 Lua Director 桥接生命周期管理
-- 对照 Java: HandleBridgeLifecycleTest(IDasVariantVector)
--
-- 参数: [callback(IDasBase), marker(string)]
-- 返回: VariantVector[BridgeLifecycleDirector]
--
-- 核心验证:
-- 1. Lua 侧通过 as() 将 IDasBase 向下转换为 IDasComponent
-- 2. 返回继承 ILuaDasComponent 的 Director 对象
-- 3. C++ 不持有返回值 → bridge 释放链触发 → GC → 回调
-- ==========================================================================

function LuaTestPlugin._handle_bridge_lifecycle_test(arguments)
    if not arguments then
        return 1
    end

    -- 验证点 1: IDasBase → IDasComponent 向下转换
    -- C++ 侧用 PushBackBase 传 IDasBase*（验证 QI 恢复），
    -- Lua 侧通过 GetBase 获取后可直接作为 IDasComponent 使用
    local hr1, callback_base = arguments:GetBase(0)
    if hr1 < 0 or not callback_base then
        return 1
    end

    local hr2, marker = arguments:GetString(1)
    if hr2 < 0 or not marker then
        return 1
    end

    -- 设置回调（GC 时使用）
    LuaTestPlugin.SetBridgeCallback(callback_base)

    -- 验证点 2: 创建继承 ILuaDasComponent 的 Director
    -- C++ bridge 持有引用 → 对象不能被 GC
    local director = create_bridge_lifecycle_director()

    -- 将 Director 通过返回值传给 C++
    -- C++ 收到后不处理返回值 → proxy 释放 → bridge 释放链
    local out_args = CreateIDasVariantVector()
    if out_args then
        out_args:PushBackBase(director)
        out_args:PushBackString("director_created:" .. marker)
        return 0, out_args
    end
    return 1
end

return LuaTestPlugin
