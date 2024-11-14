#ifndef DAS_GUIDHOLDER_H
#define DAS_GUIDHOLDER_H

template <class T>
struct DasIidHolder;

template <class T>
[[nodiscard]]
auto DasIidOf() -> const struct _asr_GUID&;

#define DAS_DEFINE_GUID_HOLDER(                                                \
    type,                                                                      \
    l,                                                                         \
    w1,                                                                        \
    w2,                                                                        \
    b1,                                                                        \
    b2,                                                                        \
    b3,                                                                        \
    b4,                                                                        \
    b5,                                                                        \
    b6,                                                                        \
    b7,                                                                        \
    b8)                                                                        \
    extern "C++"                                                               \
    {                                                                          \
        template <>                                                            \
        struct DasIidHolder<struct type>                                       \
        {                                                                      \
            static constexpr DasGuid iid =                                     \
                {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};                 \
        };                                                                     \
        template <>                                                            \
        constexpr const DasGuid& DasIidOf<struct type>()                       \
        {                                                                      \
            return DasIidHolder<type>::iid;                                    \
        }                                                                      \
        template <>                                                            \
        constexpr const DasGuid& DasIidOf<struct type*>()                      \
        {                                                                      \
            return DasIidHolder<type>::iid;                                    \
        }                                                                      \
    }

#define DAS_DEFINE_CLASS_GUID_HOLDER(                                          \
    type,                                                                      \
    l,                                                                         \
    w1,                                                                        \
    w2,                                                                        \
    b1,                                                                        \
    b2,                                                                        \
    b3,                                                                        \
    b4,                                                                        \
    b5,                                                                        \
    b6,                                                                        \
    b7,                                                                        \
    b8)                                                                        \
    extern "C++"                                                               \
    {                                                                          \
        template <>                                                            \
        struct DasIidHolder<class type>                                        \
        {                                                                      \
            static constexpr DasGuid iid =                                     \
                {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};                 \
        };                                                                     \
        template <>                                                            \
        constexpr const DasGuid& DasIidOf<class type>()                        \
        {                                                                      \
            return DasIidHolder<type>::iid;                                    \
        }                                                                      \
        template <>                                                            \
        constexpr const DasGuid& DasIidOf<class type*>()                       \
        {                                                                      \
            return DasIidHolder<type>::iid;                                    \
        }                                                                      \
    }

#define DAS_TO_NAMESPACE_QUALIFIER(ns) ns## ::

#define DAS_DEFINE_CLASS_GUID_HOLDER_IN_NAMESPACE(                             \
    ns,                                                                        \
    type,                                                                      \
    l,                                                                         \
    w1,                                                                        \
    w2,                                                                        \
    b1,                                                                        \
    b2,                                                                        \
    b3,                                                                        \
    b4,                                                                        \
    b5,                                                                        \
    b6,                                                                        \
    b7,                                                                        \
    b8)                                                                        \
    extern "C++"                                                               \
    {                                                                          \
        template <>                                                            \
        struct DasIidHolder<ns::type>                                          \
        {                                                                      \
            static constexpr DasGuid iid =                                     \
                {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}};                 \
        };                                                                     \
        template <>                                                            \
        constexpr const DasGuid& DasIidOf<ns::type>()                          \
        {                                                                      \
            return DasIidHolder<ns::type>::iid;                                \
        }                                                                      \
        template <>                                                            \
        constexpr const DasGuid& DasIidOf<ns::type*>()                         \
        {                                                                      \
            return DasIidHolder<ns::type>::iid;                                \
        }                                                                      \
    }

#define DAS_UUID_OF(type) DasIidOf<decltype(type)>()

#endif // DAS_GUIDHOLDER_H