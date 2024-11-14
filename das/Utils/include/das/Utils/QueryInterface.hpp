#ifndef DAS_UTILS_QUERYINTERFACE_HPP
#define DAS_UTILS_QUERYINTERFACE_HPP

#include <das/Utils/Config.h>
#include <das/Utils/PresetTypeInheritanceInfo.h>
#include <type_traits>

DAS_UTILS_NS_BEGIN

template <class T>
struct _asr_internal_QueryInterfaceContext
{
    T*             p_this;
    const DasGuid& iid;
    void**         pp_out_object;

    _asr_internal_QueryInterfaceContext(
        T*             p_this,
        const DasGuid& iid,
        void**         pp_out_object)
        : p_this{p_this}, iid{iid}, pp_out_object{pp_out_object}
    {
    }
};

template <class T>
_asr_internal_QueryInterfaceContext(T*, const DasGuid&, void**)
    -> _asr_internal_QueryInterfaceContext<T>;

template <class T, class First>
DasResult InternalQueryInterfaceImpl(T context)
{
    if (DasIidOf<First>() == context.iid)
    {
        auto* const pointer = static_cast<First*>(context.p_this);
        *context.pp_out_object = pointer;
        pointer->AddRef();
        return DAS_S_OK;
    }
    *context.pp_out_object = nullptr;
    return DAS_E_NO_INTERFACE;
}

template <class T, class First, class... Other>
auto InternalQueryInterfaceImpl(T context)
    -> std::enable_if_t<(sizeof...(Other) > 0), DasResult>
{
    if (DasIidOf<First>() == context.iid)
    {
        auto* const pointer = static_cast<First*>(context.p_this);
        *context.pp_out_object = pointer;
        pointer->AddRef();
        return DAS_S_OK;
    }
    return InternalQueryInterfaceImpl<T, Other...>(context);
}

template <class T, class... Ts>
DasResult InternalQueryInterface(T context, internal_type_holder<Ts...>)
{
    if (!context.pp_out_object)
    {
        return DAS_E_INVALID_POINTER;
    }

    return InternalQueryInterfaceImpl<T, Ts...>(context);
}

/**
 * @brief 支持直接补充继承关系的QueryInterface
 * @tparam PresetTypeInheritanceInfo
 * @tparam AdditionalTs
 * @param p_this
 * @param iid
 * @param pp_out_object
 * @return
 */
template <class PresetTypeInheritanceInfo, class... AdditionalTs>
DasResult QueryInterfaceAsLastClassInInheritanceInfo(
    std::add_pointer_t<typename internal_type_holder<
        AdditionalTs...>::template At<sizeof...(AdditionalTs) - 1>> p_this,
    const DasGuid&                                                  iid,
    void** pp_out_object)
{
    using FullType =
        decltype(PresetTypeInheritanceInfo{} + internal_type_holder<AdditionalTs...>{});

    _asr_internal_QueryInterfaceContext context{p_this, iid, pp_out_object};
    return InternalQueryInterface(context, FullType{});
}

template <class PresetTypeInheritanceInfo>
DasResult QueryInterfaceAsLastClassInInheritanceInfo(
    std::add_pointer_t<typename PresetTypeInheritanceInfo::template At<
        PresetTypeInheritanceInfo::size - 1>> p_this,
    const DasGuid&                            iid,
    void**                                    pp_out_object)
{
    _asr_internal_QueryInterfaceContext context{p_this, iid, pp_out_object};
    return InternalQueryInterface(context, PresetTypeInheritanceInfo{});
}

/**
 * @brief 不需要TImpl类能被Query出来时，使用这个函数
 * @tparam T
 * @tparam TImpl
 * @param p_this
 * @param iid
 * @param pp_out_object
 * @return
 */
template <class T, class TImpl>
DasResult
QueryInterface(TImpl* p_this, const DasGuid& iid, void** pp_out_object)
{
    return QueryInterfaceAsLastClassInInheritanceInfo<
        typename PresetTypeInheritanceInfo<T>::TypeInfo>(
        p_this,
        iid,
        pp_out_object);
}

/**
 *
 * @tparam T 要执行QueryInterface的类型
 * @tparam TImpl this的类型，即T的子类
 * @param p_this this指针
 * @param iid 要搜索的iid
 * @return 成功时为 DAS_S_OK ，失败为 DAS_E_NO_INTERFACE
 * ，注意：指针已经AddRef,析构时需释放。
 */
template <
    class T,
    class TImpl,
    class = std::enable_if_t<std::is_base_of_v<IDasSwigBase, T>>>
DasRetSwigBase QueryInterface(TImpl* p_this, const DasGuid& iid)
{
    void*      pointer{};
    const auto error_code = QueryInterfaceAsLastClassInInheritanceInfo<
        typename PresetTypeInheritanceInfo<T>::TypeInfo>(p_this, iid, &pointer);
    return DasRetSwigBase{error_code, pointer};
}

DAS_UTILS_NS_END

#endif // DAS_UTILS_QUERYINTERFACE_HPP
