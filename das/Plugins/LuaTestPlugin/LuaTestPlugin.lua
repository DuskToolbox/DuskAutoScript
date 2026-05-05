-- LuaTestPlugin.lua
-- Lua Test Plugin for IPC integration testing
-- 被 C++ 端 LuaHost::LoadPlugin 加载
--
-- ⚠️ 限制说明（Lua Director [out] 参数问题）:
-- 当前 Lua Director 生成器在 override 方法中调用 Lua 函数时，
-- 跳过所有 [out] 参数（只传递 IN 参数给 Lua）。这意味着：
--   - EnumFeature(uint64_t index, [out] DasPluginFeature*) → Lua 只收到 index
--   - CreateFeatureInterface(uint64_t index, [out] IDasBase**) → Lua 只收到 index
--   - CreateInstance(const DasGuid&, [out] IDasComponent**) → Lua 只收到 guid
--   - Dispatch(IDasReadOnlyString*, IDasVariantVector*, [out] IDasVariantVector**)
--     → Lua 只收到 function_name 和 arguments
--
-- Director 方法将 Lua 返回值仅映射为 DasResult，不会设置 [out] 参数。
-- 因此 EnumFeature 返回 DAS_S_OK 但 feature 未填充，CreateFeatureInterface
-- 返回 DAS_S_OK 但 pp_out_interface 为 nullptr。
--
-- 完整解决方案：扩展 Lua Director 生成器（swig_lua_generator.py），
-- 让 Lua 函数可以通过多返回值（Lua tuple）填充 [out] 参数，
-- 类似于抽象接口注册中 _generate_out_param_lambda 的做法。

local LuaTestPlugin = {}

function LuaTestPlugin.createInstance()
    -- 创建 ILuaDasPluginPackage Director 实例
    -- ILuaDasPluginPackage 由 luaopen_DasCoreApi 注册到 Lua
    local package = ILuaDasPluginPackage({
        EnumFeature = function(self, index)
            -- 注意：[out] DasPluginFeature* 参数由 Director 系统处理，
            -- Lua 端无法设置。此处返回 DAS_S_OK 表示成功。
            -- 但由于 feature out 参数未被填充，C++ 侧获取的
            -- feature 数据为零值/未初始化。
            if index == 0 then
                return 0  -- DAS_S_OK
            end
            return 1  -- DAS_E_NOT_FOUND (out of range)
        end,

        CreateFeatureInterface = function(self, index)
            -- 注意：[out] IDasBase** 参数由 Director 系统处理，
            -- Lua 端无法设置。即使返回 DAS_S_OK，
            -- pp_out_interface 仍为 nullptr。
            if index == 0 then
                -- 意图：创建 ILuaDasComponentFactory Director
                -- 实际：由于 [out] 参数限制，factory 不会被传回 C++
                return 0  -- DAS_S_OK
            end
            return 1  -- DAS_E_NOT_FOUND
        end,

        CanUnloadNow = function(self)
            -- 允许卸载
            return true
        end,
    })
    return package
end

--- 以下为预期的完整 Lua 插件实现，待 Director [out] 参数支持后启用。
--- 当前这些代码不会被调用，仅作为设计参考。

--- 预期的 ILuaDasComponentFactory 实现
--- @class LuaTestComponentFactory : ILuaDasComponentFactory
local LuaTestComponentFactory = {
    --- @param self ILuaDasComponentFactory
    --- @param component_iid DasGuid
    --- @return DasResult, IDasComponent?
    CreateInstance = function(self, component_iid)
        -- 意图：创建 ILuaDasComponent Director 并通过 [out] 返回
        -- 当前限制：Director 的 CreateInstance override 不传递 [out] 给 Lua
        local component = ILuaDasComponent({
            Dispatch = function(self_func, function_name, arguments)
                -- function_name 是 IDasReadOnlyString*
                -- arguments 是 IDasVariantVector*
                -- 注意：[out] IDasVariantVector** 不传给 Lua
                --
                -- 获取函数名字符串
                local name_str = nil
                if function_name then
                    local hr, str = function_name:GetUtf8()
                    if hr >= 0 and str then
                        name_str = str
                    end
                end

                if name_str == "bridgeLifecycleTest" then
                    -- 意图：从 arguments 中提取 callback 和 marker，
                    -- 然后直接调用 callback 的 Dispatch 发送 lifecycle_callback
                    --
                    -- arguments 结构：
                    --   [0] = IDasBase* (callback component)
                    --   [1] = string (marker)
                    if arguments then
                        -- 提取 callback（IDasBase* → 需要转为 IDasComponent）
                        local hr1, callback_base = arguments:GetBase(0)
                        if hr1 >= 0 and callback_base then
                            -- 提取 marker
                            local hr2, marker_str = arguments:GetString(1)
                            if hr2 >= 0 and marker_str then
                                -- 构造 lifecycle_callback 参数
                                local out_args = CreateIDasVariantVector()
                                if out_args then
                                    out_args:PushBackString(
                                        "bridge_released:lua_gc"
                                    )
                                    -- 调用 callback 的 Dispatch
                                    -- callback_base 是 IDasBase*，
                                    -- 需要 QueryInterface 到 IDasComponent
                                    -- 但 Lua 侧通过抽象接口可以直接调用
                                    callback_base:Dispatch(
                                        "lifecycle_callback",
                                        out_args
                                    )
                                end
                            end
                        end
                    end

                    return 0  -- DAS_S_OK
                end

                return 1  -- DAS_E_NOT_FOUND
            end,
        })
        -- 当前无法通过 [out] 返回 component，仅返回 DasResult
        return 0, component  -- 预期：DasResult + [out] IDasComponent*
    end,

    --- @param self ILuaDasComponentFactory
    --- @param component_iid DasGuid
    --- @return DasResult
    IsSupported = function(self, component_iid)
        return 0  -- DAS_S_OK
    end,
}

return LuaTestPlugin
