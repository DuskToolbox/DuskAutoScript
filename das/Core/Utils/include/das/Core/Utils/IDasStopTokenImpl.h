#ifndef DAS_CORE_UTILS_IDASSTOPTOKENIMPL_H
#define DAS_CORE_UTILS_IDASSTOPTOKENIMPL_H

#include <atomic>
#include <das/Core/Utils/Config.h>
#include <das/PluginInterface/IDasTask.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/QueryInterface.hpp>

DAS_CORE_UTILS_NS_BEGIN

class DasStopTokenImplOnStack;

class IDasStopTokenImplOnStack final : public IDasStopToken
{
    DasStopTokenImplOnStack& impl_;

public:
    IDasStopTokenImplOnStack(DasStopTokenImplOnStack& impl);
    int64_t   AddRef() override;
    int64_t   Release() override;
    DasResult QueryInterface(const DasGuid& iid, void** pp_object) override;
    DasBool   StopRequested() override;
};

class IDasSwigStopTokenImplOnStack final : public IDasSwigStopToken
{
    DasStopTokenImplOnStack& impl_;

public:
    IDasSwigStopTokenImplOnStack(DasStopTokenImplOnStack& impl);
    int64_t        AddRef() override;
    int64_t        Release() override;
    DasRetSwigBase QueryInterface(const DasGuid& iid) override;
    DasBool        StopRequested() override;
};

/**
 * @brief 构造复制拷贝移动必须保证线程安全
 */
class DasStopTokenImplOnStack : DAS_UTILS_MULTIPLE_PROJECTION_GENERATORS(
                                    DasStopTokenImplOnStack,
                                    IDasStopTokenImplOnStack,
                                    IDasSwigStopTokenImplOnStack)
{
public:
    int64_t   AddRef() { return 1; };
    int64_t   Release() { return 1; };
    DasResult QueryInterface(const DasGuid& iid, void** pp_object)
    {
        const auto qi_result = Utils::QueryInterface<IDasStopToken>(
            static_cast<IDasStopTokenImplOnStack*>(*this),
            iid,
            pp_object);
        if (IsFailed(qi_result))
        {
            const auto qi_swig_result =
                Utils::QueryInterface<IDasSwigStopToken>(
                    static_cast<IDasSwigStopTokenImplOnStack*>(*this),
                    iid);
            if (IsOk(qi_swig_result))
            {
                *pp_object = qi_swig_result.value;
            }
            return qi_swig_result.error_code;
        }
        return qi_result;
    }
    DasBool StopRequested() { return is_stop_requested_; }
    void    RequestStop() { is_stop_requested_ = true; }
    void    Reset() { is_stop_requested_ = false; }

private:
    std::atomic_bool is_stop_requested_{false};
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_IDASSTOPTOKENIMPL_H
