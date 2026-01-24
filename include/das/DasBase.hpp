#ifndef DAS_DASBASE_HPP
#define DAS_DASBASE_HPP

#include <das/DasPtr.hpp>
#include <das/IDasBase.h>

namespace DAS
{

    class DasBase
    {
    private:
        DasPtr<IDasBase> p_impl_;

    public:
        explicit DasBase(IDasBase* p) : p_impl_(p) {}

        static DasBase Attach(IDasBase* p)
        {
            return DasBase(DasPtr<IDasBase>::Attach(p));
        }

        // 获取原始接口指针
        IDasBase* Get() const { return p_impl_.Get(); }

        // Put 方法：使用 DasPtr::Put
        IDasBase** Put() { return p_impl_.Put(); }

        // As 模板方法
        template <class T>
        DasResult As(T& other) const
        {
            return p_impl_.As(other);
        }

        template <class T>
        DasResult As(T** pp_out_other) const
        {
            return p_impl_.As(pp_out_other);
        }
    };

} // namespace DAS

#endif // DAS_DASBASE_HPP
