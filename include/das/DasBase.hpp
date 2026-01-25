#ifndef DAS_DASBASE_HPP
#define DAS_DASBASE_HPP

#include <das/DasConfig.h>
#include <das/DasPtr.hpp>
#include <das/DasTypes.hpp>
#include <das/IDasBase.h>

class DasBase
{
private:
    DAS::DasPtr<IDasBase> p_impl_;

public:
    DasBase(IDasBase* p, bool add_ref = true) : p_impl_(p)
    {
        // DasPtr 构造函数会自动调用 AddRef
        // 如果是 Attach 语义（add_ref=false），需要抵消这个 AddRef
        if (!add_ref && p != nullptr)
        {
            p->Release();
        }
    }

    static DasBase Attach(IDasBase* p) { return {p, false}; }

    [[nodiscard]]
    IDasBase* Get() const
    {
        return p_impl_.Get();
    }

    IDasBase** Put() { return p_impl_.Put(); }

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

#endif // DAS_DASBASE_HPP
