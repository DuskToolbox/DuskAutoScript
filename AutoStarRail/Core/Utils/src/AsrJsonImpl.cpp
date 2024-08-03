#include <AutoStarRail/Core/ForeignInterfaceHost/AsrStringImpl.h>
#include <AutoStarRail/Core/Logger/Logger.h>
#include <AutoStarRail/Core/Utils/Config.h>
#include <AutoStarRail/ExportInterface/AsrJson.h>
#include <AutoStarRail/Utils/CommonUtils.hpp>
#include <AutoStarRail/Utils/Expected.h>
#include <AutoStarRail/Utils/QueryInterface.hpp>
#include <boost/signals2.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <variant>

// {A9EC9C65-66E1-45B1-9C73-C95A6620BA6A}
ASR_DEFINE_CLASS_IN_NAMESPACE(
    Asr::Core::Utils,
    IAsrJsonImpl,
    0xa9ec9c65,
    0x66e1,
    0x45b1,
    0x9c,
    0x73,
    0xc9,
    0x5a,
    0x66,
    0x20,
    0xba,
    0x6a);

ASR_CORE_UTILS_NS_BEGIN

struct AsrJsonImplRefExpiredException : public std::exception
{
    const char* what() const noexcept override
    {
        return "Dangling reference detected!";
    }
};

class IAsrJsonImpl final : public IAsrJson
{
public:
    struct Object
    {
        nlohmann::json                  json_;
        boost::signals2::signal<void()> signal_;
    };

    struct Ref
    {
        nlohmann::json*                    json_;
        boost::signals2::scoped_connection connection_;
    };

private:
    std::recursive_mutex      mutex_;
    std::variant<Object, Ref> impl_;

    template <class T>
    AsrResult GetToImpl(IAsrReadOnlyString* p_string, T* obj);

    AsrResult GetToImpl(IAsrReadOnlyString* p_string, IAsrReadOnlyString** obj);

    template <class T>
    AsrResult GetToImpl(size_t index, T* obj);

    AsrResult GetToImpl(size_t index, IAsrReadOnlyString** pp_out_obj);

    template <class T>
    AsrResult SetImpl(IAsrReadOnlyString* p_string, const T& value);

    template <class T>
    AsrResult SetImpl(size_t index, const T& value);

public:
    IAsrJsonImpl();
    IAsrJsonImpl(nlohmann::json& ref_json);
    ~IAsrJsonImpl();

    ASR_UTILS_IASRBASE_AUTO_IMPL(IAsrJsonImpl)
    AsrResult QueryInterface(const AsrGuid& iid, void** pp_out_object) override;
    AsrResult GetIntByName(IAsrReadOnlyString* key, int64_t* p_out_int)
        override;
    AsrResult GetFloatByName(IAsrReadOnlyString* key, float* p_out_float)
        override;
    AsrResult GetStringByName(
        IAsrReadOnlyString*  key,
        IAsrReadOnlyString** pp_out_string) override;
    AsrResult GetBoolByName(IAsrReadOnlyString* key, bool* p_out_bool) override;
    AsrResult GetObjectRefByName(
        IAsrReadOnlyString* key,
        IAsrJson**          pp_out_asr_json) override;

    AsrResult SetIntByName(IAsrReadOnlyString* key, int64_t in_int) override;
    AsrResult SetFloatByName(IAsrReadOnlyString* key, float in_float) override;
    AsrResult SetStringByName(
        IAsrReadOnlyString* key,
        IAsrReadOnlyString* p_in_string) override;
    AsrResult SetBoolByName(IAsrReadOnlyString* key, bool in_bool) override;
    AsrResult SetObjectByName(IAsrReadOnlyString* key, IAsrJson* p_in_asr_json)
        override;

    AsrResult GetIntByIndex(size_t index, int64_t* p_out_int) override;
    AsrResult GetFloatByIndex(size_t index, float* p_out_float) override;
    AsrResult GetStringByIndex(size_t index, IAsrReadOnlyString** pp_out_string)
        override;
    AsrResult GetBoolByIndex(size_t index, bool* p_out_bool) override;
    AsrResult GetObjectRefByIndex(size_t index, IAsrJson** pp_out_asr_json)
        override;

    AsrResult SetIntByIndex(size_t index, int64_t in_int) override;
    AsrResult SetFloatByIndex(size_t index, float in_float) override;
    AsrResult SetStringByIndex(size_t index, IAsrReadOnlyString* p_in_string)
        override;
    AsrResult SetBoolByIndex(size_t index, bool in_bool) override;
    AsrResult SetObjectByIndex(size_t index, IAsrJson* p_in_asr_json) override;

    void SetConnection(const boost::signals2::connection& connection);
    void OnExpired();
};

ASR_NS_ANONYMOUS_DETAILS_BEGIN

template <class Arg>
class CallOperatorSquareBrackets
{
    const Arg arg_;

public:
    CallOperatorSquareBrackets(Arg arg) : arg_{arg} {}

    decltype(auto) operator()(IAsrJsonImpl::Object& object) const
    {
        return object.json_[arg_];
    }

    decltype(auto) operator()(const IAsrJsonImpl::Object& object) const
    {
        return object.json_[arg_];
    }

    decltype(auto) operator()(IAsrJsonImpl::Ref& ref) const
    {
        if (ref.json_ != nullptr)
        {
            return (*ref.json_)[arg_];
        }
        throw AsrJsonImplRefExpiredException{};
    }

    decltype(auto) operator()(const IAsrJsonImpl::Ref& ref) const
    {
        if (ref.json_ != nullptr)
        {
            return (*ref.json_)[arg_];
        }
        throw AsrJsonImplRefExpiredException{};
    }
};

template <class Arg, class Value>
class CallOperatorSquareBracketsForAssign
{
    Arg    arg_;
    Value& value_;

public:
    CallOperatorSquareBracketsForAssign(Arg arg, Value& value)
        : arg_{arg}, value_{value}
    {
    }

    void operator()(IAsrJsonImpl::Object& object) const
    {
        object.json_[arg_] = value_;
    }

    void operator()(IAsrJsonImpl::Ref& ref) const
    {
        if (ref.json_ != nullptr)
        {
            (*ref.json_)[arg_] = value_;
        }
        throw AsrJsonImplRefExpiredException{};
    }
};

ASR_NS_ANONYMOUS_DETAILS_END

template <class T>
AsrResult IAsrJsonImpl::GetToImpl(IAsrReadOnlyString* p_string, T* obj)
{
    ASR_UTILS_CHECK_POINTER(p_string)
    ASR_UTILS_CHECK_POINTER(obj)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        std::visit(Details::CallOperatorSquareBrackets{p_u8_key}, impl_)
            .get_to(*obj);
        return ASR_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
}

template <class T>
AsrResult IAsrJsonImpl::GetToImpl(size_t index, T* obj)
{
    ASR_UTILS_CHECK_POINTER(obj)

    std::scoped_lock _{mutex_};
    try
    {
        return std::visit(Details::CallOperatorSquareBrackets{index}, impl_);
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
}

template <class T>
AsrResult IAsrJsonImpl::SetImpl(IAsrReadOnlyString* p_string, const T& value)
{

    ASR_UTILS_CHECK_POINTER(p_string)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        std::visit(
            Details::CallOperatorSquareBracketsForAssign{p_u8_key, value},
            impl_);
        return ASR_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
}

template <class T>
AsrResult IAsrJsonImpl::SetImpl(size_t index, const T& value)
{
    std::scoped_lock _{mutex_};
    try
    {
        std::visit(
            Details::CallOperatorSquareBracketsForAssign{index, value},
            impl_);
        return ASR_S_OK;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult IAsrJsonImpl::GetToImpl(
    IAsrReadOnlyString*  p_string,
    IAsrReadOnlyString** obj)
{
    ASR_UTILS_CHECK_POINTER(p_string)
    ASR_UTILS_CHECK_POINTER(obj)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        return ::CreateIAsrReadOnlyStringFromUtf8(
            std::visit(Details::CallOperatorSquareBrackets{p_u8_key}, impl_)
                .get_ref<const std::string&>()
                .c_str(),
            obj);
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
}

AsrResult IAsrJsonImpl::GetToImpl(size_t index, IAsrReadOnlyString** pp_out_obj)
{
    ASR_UTILS_CHECK_POINTER(pp_out_obj)

    std::scoped_lock _{mutex_};
    try
    {
        return ::CreateIAsrReadOnlyStringFromUtf8(
            std::visit(Details::CallOperatorSquareBrackets{index}, impl_)
                .get_ref<const std::string&>()
                .c_str(),
            pp_out_obj);
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
}

IAsrJsonImpl::IAsrJsonImpl() : impl_{Object{}} {}

IAsrJsonImpl::IAsrJsonImpl(nlohmann::json& ref_json) : impl_{Ref{&ref_json, {}}}
{
}

IAsrJsonImpl::~IAsrJsonImpl() {}

AsrResult IAsrJsonImpl::QueryInterface(const AsrGuid& iid, void** pp_out_object)
{
    return Utils::QueryInterfaceAsLastClassInInheritanceInfo<
        Utils::IAsrJsonInheritanceInfo,
        IAsrJsonImpl>(this, iid, pp_out_object);
}

AsrResult IAsrJsonImpl::GetIntByName(
    IAsrReadOnlyString* key,
    int64_t*            p_out_int)
{
    return GetToImpl(key, p_out_int);
}

AsrResult IAsrJsonImpl::GetFloatByName(
    IAsrReadOnlyString* key,
    float*              p_out_float)
{
    return GetToImpl(key, p_out_float);
}

AsrResult IAsrJsonImpl::GetStringByName(
    IAsrReadOnlyString*  key,
    IAsrReadOnlyString** pp_out_string)
{
    return GetToImpl(key, pp_out_string);
}

AsrResult IAsrJsonImpl::GetBoolByName(IAsrReadOnlyString* key, bool* p_out_bool)
{
    return GetToImpl(key, p_out_bool);
}

AsrResult IAsrJsonImpl::GetObjectRefByName(
    IAsrReadOnlyString* key,
    IAsrJson**          pp_out_asr_json)
{
    ASR_UTILS_CHECK_POINTER(key)
    ASR_UTILS_CHECK_POINTER(pp_out_asr_json)

    const auto expected_u8_key = ToU8StringWithoutOwnership(key);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    try
    {
        std::scoped_lock _{mutex_};
        auto&&           object =
            std::visit(Details::CallOperatorSquareBrackets{p_u8_key}, impl_);
        const auto ref_object = MakeAsrPtr<IAsrJsonImpl>(object);
        auto&      ref_out_asr_json = *pp_out_asr_json;
        ref_out_asr_json = ref_object.Get();
        ref_out_asr_json->AddRef();

        if (auto* const p_internal_object = std::get_if<Object>(&impl_);
            p_internal_object != nullptr) [[likely]]
        {
            const auto connection = p_internal_object->signal_.connect(
                [p_ref = ref_object.Get()] { p_ref->OnExpired(); });
            ref_object->SetConnection(connection);
            return ASR_S_OK;
        }
        ASR_CORE_LOG_ERROR("Can not get object from impl_.");
        return ASR_E_INTERNAL_FATAL_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
    return ASR_S_OK;
}

AsrResult IAsrJsonImpl::SetIntByName(IAsrReadOnlyString* key, int64_t in_int)
{
    return SetImpl(key, in_int);
}

AsrResult IAsrJsonImpl::SetFloatByName(IAsrReadOnlyString* key, float in_float)
{
    return SetImpl(key, in_float);
}

AsrResult IAsrJsonImpl::SetStringByName(
    IAsrReadOnlyString* key,
    IAsrReadOnlyString* p_in_string)
{
    const auto expected_u8_key = ToU8StringWithoutOwnership(p_in_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_in_u8_key = expected_u8_key.value();

    return SetImpl(key, p_in_u8_key);
}

AsrResult IAsrJsonImpl::SetBoolByName(IAsrReadOnlyString* key, bool in_bool)
{
    return SetImpl(key, in_bool);
}

AsrResult IAsrJsonImpl::SetObjectByName(
    IAsrReadOnlyString* key,
    IAsrJson*           p_in_asr_json)
{
    AsrPtr<IAsrJsonImpl> p_impl;

    if (const auto qi_result = p_in_asr_json->QueryInterface(
            AsrIidOf<IAsrJsonImpl>(),
            p_impl.PutVoid());
        ASR::IsFailed(qi_result))
    {
        return qi_result;
    }

    return std::visit(
        Utils::overload_set{
            [this, key](const Object& j) { return SetImpl(key, j.json_); },
            [this, key](const Ref& j)
            {
                if (j.json_ != nullptr)
                {
                    return SetImpl(key, *j.json_);
                }
                return ASR_E_DANGLING_REFERENCE;
            }},
        impl_);
}

AsrResult IAsrJsonImpl::GetIntByIndex(size_t index, int64_t* p_out_int)
{
    return GetToImpl(index, p_out_int);
}

AsrResult IAsrJsonImpl::GetFloatByIndex(size_t index, float* p_out_float)
{
    return GetToImpl(index, p_out_float);
}

AsrResult IAsrJsonImpl::GetStringByIndex(
    size_t               index,
    IAsrReadOnlyString** pp_out_string)
{
    return GetToImpl(index, pp_out_string);
}

AsrResult IAsrJsonImpl::GetBoolByIndex(size_t index, bool* p_out_bool)
{
    return GetToImpl(index, p_out_bool);
}

AsrResult IAsrJsonImpl::GetObjectRefByIndex(
    size_t     index,
    IAsrJson** pp_out_asr_json)
{
    ASR_UTILS_CHECK_POINTER(pp_out_asr_json)

    try
    {
        std::scoped_lock _{mutex_};
        auto&&           object =
            std::visit(Details::CallOperatorSquareBrackets{index}, impl_);
        const auto ref_object = MakeAsrPtr<IAsrJsonImpl>(object);
        auto&      ref_out_asr_json = *pp_out_asr_json;
        ref_out_asr_json = ref_object.Get();
        ref_out_asr_json->AddRef();

        if (auto* const p_internal_object = std::get_if<Object>(&impl_);
            p_internal_object != nullptr) [[likely]]
        {
            const auto connection = p_internal_object->signal_.connect(
                [p_ref = ref_object.Get()] { p_ref->OnExpired(); });
            ref_object->SetConnection(connection);
            return ASR_S_OK;
        }
        ASR_CORE_LOG_ERROR("Can not get object from impl_.");
        return ASR_E_INTERNAL_FATAL_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_INVALID_JSON;
    }
    catch (const AsrJsonImplRefExpiredException& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        ASR_CORE_LOG_EXCEPTION(ex);
        return ASR_E_OUT_OF_MEMORY;
    }
    return ASR_S_OK;
}

AsrResult IAsrJsonImpl::SetIntByIndex(size_t index, int64_t in_int)
{
    return SetImpl(index, in_int);
}

AsrResult IAsrJsonImpl::SetFloatByIndex(size_t index, float in_float)
{
    return SetImpl(index, in_float);
}

AsrResult IAsrJsonImpl::SetStringByIndex(
    size_t              index,
    IAsrReadOnlyString* p_in_string)
{
    ASR_UTILS_CHECK_POINTER(p_in_string)
    const auto expected_u8_value = ToU8StringWithoutOwnership(p_in_string);
    if (!expected_u8_value)
    {
        return expected_u8_value.error();
    }
    const auto p_u8_key = expected_u8_value.value();

    return SetImpl(index, p_u8_key);
}

AsrResult IAsrJsonImpl::SetBoolByIndex(size_t index, bool in_bool)
{
    return SetImpl(index, in_bool);
}

AsrResult IAsrJsonImpl::SetObjectByIndex(size_t index, IAsrJson* p_in_asr_json)
{
    AsrPtr<IAsrJsonImpl> p_impl;

    if (const auto qi_result = p_in_asr_json->QueryInterface(
            AsrIidOf<IAsrJsonImpl>(),
            p_impl.PutVoid());
        ASR::IsFailed(qi_result))
    {
        return qi_result;
    }

    return std::visit(
        Utils::overload_set{
            [this, index](const Object& j) { return SetImpl(index, j.json_); },
            [this, index](const Ref& j)
            {
                if (j.json_ != nullptr)
                {
                    return SetImpl(index, *j.json_);
                }
                return ASR_E_DANGLING_REFERENCE;
            }},
        impl_);
}

void IAsrJsonImpl::SetConnection(const boost::signals2::connection& connection)
{
    if (auto* const p_ref = std::get_if<Ref>(&impl_); p_ref)
    {
        p_ref->connection_ = connection;
    }
    ASR_CORE_LOG_ERROR("Expect Ref but found Object!");
}

void IAsrJsonImpl::OnExpired()
{
    std::visit(
        Utils::overload_set{
            [](const Object&)
            {
                ASR_CORE_LOG_ERROR(
                    "Type not matched. Expected reference but instance found.");
            },
            [](Ref& ref) { ref.json_ = nullptr; }},
        impl_);
}

ASR_CORE_UTILS_NS_END
