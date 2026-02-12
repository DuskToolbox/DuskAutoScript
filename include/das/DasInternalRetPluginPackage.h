#ifndef DAS_INTERNAL_RET_PLUGINPACKAGE
#define DAS_INTERNAL_RET_PLUGINPACKAGE

#include "IDasPluginPackage.h"
#include <utility>

struct DasInternalRetPluginPackage
{
    DasResult                                error_code;
    Das::PluginInterface::IDasPluginPackage* value;

    template <class T>
    static void InternalDelayAddRef(T* p)
    {
        p->AddRef();
    }

    // 默认构造函数
    DasInternalRetPluginPackage()
        : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr)
    {
    }

    // 删除复制构造函数和复制赋值运算符
    DasInternalRetPluginPackage(const DasInternalRetPluginPackage&) = delete;
    DasInternalRetPluginPackage& operator=(const DasInternalRetPluginPackage&) =
        delete;

    // 移动构造函数
    DasInternalRetPluginPackage(DasInternalRetPluginPackage&& other) noexcept
        : error_code(DAS_E_UNDEFINED_RETURN_VALUE), value(nullptr)
    {
        std::swap(error_code, other.error_code);
        std::swap(value, other.value);
    }

    // 移动赋值运算符（仅C++使用，SWIG忽略）
#ifndef SWIG
    DasInternalRetPluginPackage& operator=(
        DasInternalRetPluginPackage&& other) noexcept
    {
        if (this != &other)
        {
            // 释放当前资源
            if (value)
            {
                value->Release();
            }
            error_code = DAS_E_UNDEFINED_RETURN_VALUE;
            // 移动新资源
            std::swap(error_code, other.error_code);
            std::swap(value, other.value);
        }
        return *this;
    }
#endif // SWIG

    // 析构函数
    ~DasInternalRetPluginPackage()
    {
        if (value)
        {
            value->Release();
        }
    }

    void SetErrorCode(DasResult code) { error_code = code; }

    void SetValue(Das::PluginInterface::IDasPluginPackage* v)
    {
        if (value)
        {
            value->Release();
        }
        value = v;
        InternalDelayAddRef(value);
    }
};
#endif // DAS_INTERNAL_RET_PLUGINPACKAGE