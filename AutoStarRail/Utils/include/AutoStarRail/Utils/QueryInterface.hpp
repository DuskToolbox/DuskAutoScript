#ifndef ASR_UTILS_QUERYINTERFACE_HPP
#define ASR_UTILS_QUERYINTERFACE_HPP

#include <AutoStarRail/Utils/Config.h>
#include <AutoStarRail/Utils/PresetTypeInheritanceInfo.h>
#include <type_traits>

ASR_UTILS_NS_BEGIN

template <class T>
struct _asr_internal_QueryInterfaceContext
{
    T*             p_this;
    const AsrGuid& iid;
    void**         pp_out_object;

    _asr_internal_QueryInterfaceContext(
        T*             p_this,
        const AsrGuid& iid,
        void**         pp_out_object)
        : p_this{p_this}, iid{iid}, pp_out_object{pp_out_object}
    {
    }
};

template <class T>
_asr_internal_QueryInterfaceContext(T*, const AsrGuid&, void**)
    -> _asr_internal_QueryInterfaceContext<T>;

template <class T, class First>
AsrResult InternalQueryInterfaceImpl(T context)
{
    if (AsrIidOf<First>() == context.iid)
    {
        auto* const pointer = static_cast<First*>(context.p_this);
        *context.pp_out_object = pointer;
        pointer->AddRef();
        return ASR_S_OK;
    }
    *context.pp_out_object = nullptr;
    return ASR_E_NO_INTERFACE;
}

template <class T, class First, class... Other>
auto InternalQueryInterfaceImpl(T context)
    -> std::enable_if_t<(sizeof...(Other) > 0), AsrResult>
{
    if (AsrIidOf<First>() == context.iid)
    {
        auto* const pointer = static_cast<First*>(context.p_this);
        *context.pp_out_object = pointer;
        pointer->AddRef();
        return ASR_S_OK;
    }
    return InternalQueryInterfaceImpl<T, Other...>(context);
}

template <class T, class... Ts>
AsrResult InternalQueryInterface(T context, internal_type_holder<Ts...>)
{
    if (!context.pp_out_object)
    {
        return ASR_E_INVALID_POINTER;
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
AsrResult QueryInterfaceAsLastClassInInheritanceInfo(
    std::add_pointer_t<typename internal_type_holder<
        AdditionalTs...>::template At<sizeof...(AdditionalTs) - 1>> p_this,
    const AsrGuid&                                                  iid,
    void** pp_out_object)
{
    using FullType =
        decltype(PresetTypeInheritanceInfo{} + internal_type_holder<AdditionalTs...>{});

    _asr_internal_QueryInterfaceContext context{p_this, iid, pp_out_object};
    return InternalQueryInterface(context, FullType{});
}

template <class PresetTypeInheritanceInfo>
AsrResult QueryInterfaceAsLastClassInInheritanceInfo(
    std::add_pointer_t<typename PresetTypeInheritanceInfo::template At<
        PresetTypeInheritanceInfo::size - 1>> p_this,
    const AsrGuid&                            iid,
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
AsrResult
QueryInterface(TImpl* p_this, const AsrGuid& iid, void** pp_out_object)
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
 * @return 成功时为 ASR_S_OK ，失败为 ASR_E_NO_INTERFACE
 * ，注意：指针已经AddRef,析构时需释放。
 */
template <
    class T,
    class TImpl,
    class = std::enable_if_t<std::is_base_of_v<IAsrSwigBase, T>>>
AsrRetSwigBase QueryInterface(TImpl* p_this, const AsrGuid& iid)
{
    void*      pointer{};
    const auto error_code = QueryInterfaceAsLastClassInInheritanceInfo<
        typename PresetTypeInheritanceInfo<T>::TypeInfo>(p_this, iid, &pointer);
    return AsrRetSwigBase{error_code, pointer};
}

ASR_UTILS_NS_END

#endif // ASR_UTILS_QUERYINTERFACE_HPP
