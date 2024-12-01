#include <das/Core/ForeignInterfaceHost/DasStringImpl.h>
#include <das/Core/Logger/Logger.h>
#include <das/Core/Utils/Config.h>
#include <das/ExportInterface/DasJson.h>
#include <das/Utils/CommonUtils.hpp>
#include <das/Utils/Expected.h>
#include <das/Utils/QueryInterface.hpp>
#include <boost/signals2.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <variant>

// {A9EC9C65-66E1-45B1-9C73-C95A6620BA6A}
DAS_DEFINE_CLASS_IN_NAMESPACE(
    Das::Core::Utils,
    IDasJsonImpl,
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

DAS_CORE_UTILS_NS_BEGIN

struct DasJsonImplRefExpiredException : public std::exception
{
    [[nodiscard]]
    const char* what() const noexcept override
    {
        return "Dangling reference detected!";
    }
};

class IDasJsonImpl final : public IDasJson
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
    DasResult GetToImpl(IDasReadOnlyString* p_string, T* obj);

    DasResult GetToImpl(IDasReadOnlyString* p_string, IDasReadOnlyString** obj);

    template <class T>
    DasResult GetToImpl(size_t index, T* obj);

    DasResult GetToImpl(size_t index, IDasReadOnlyString** pp_out_obj);

    template <class T>
    DasResult SetImpl(IDasReadOnlyString* p_string, const T& value);

    template <class T>
    DasResult SetImpl(size_t index, const T& value);

public:
    IDasJsonImpl();
    IDasJsonImpl(nlohmann::json& ref_json);
    ~IDasJsonImpl();

    DAS_UTILS_IDASBASE_AUTO_IMPL(IDasJsonImpl)
    DasResult QueryInterface(const DasGuid& iid, void** pp_out_object) override;
    DasResult GetIntByName(IDasReadOnlyString* key, int64_t* p_out_int)
        override;
    DasResult GetFloatByName(IDasReadOnlyString* key, float* p_out_float)
        override;
    DasResult GetStringByName(
        IDasReadOnlyString*  key,
        IDasReadOnlyString** pp_out_string) override;
    DasResult GetBoolByName(IDasReadOnlyString* key, bool* p_out_bool) override;
    DasResult GetObjectRefByName(
        IDasReadOnlyString* key,
        IDasJson**          pp_out_das_json) override;

    DasResult SetIntByName(IDasReadOnlyString* key, int64_t in_int) override;
    DasResult SetFloatByName(IDasReadOnlyString* key, float in_float) override;
    DasResult SetStringByName(
        IDasReadOnlyString* key,
        IDasReadOnlyString* p_in_string) override;
    DasResult SetBoolByName(IDasReadOnlyString* key, bool in_bool) override;
    DasResult SetObjectByName(IDasReadOnlyString* key, IDasJson* p_in_das_json)
        override;

    DasResult GetIntByIndex(size_t index, int64_t* p_out_int) override;
    DasResult GetFloatByIndex(size_t index, float* p_out_float) override;
    DasResult GetStringByIndex(size_t index, IDasReadOnlyString** pp_out_string)
        override;
    DasResult GetBoolByIndex(size_t index, bool* p_out_bool) override;
    DasResult GetObjectRefByIndex(size_t index, IDasJson** pp_out_das_json)
        override;

    DasResult SetIntByIndex(size_t index, int64_t in_int) override;
    DasResult SetFloatByIndex(size_t index, float in_float) override;
    DasResult SetStringByIndex(size_t index, IDasReadOnlyString* p_in_string)
        override;
    DasResult SetBoolByIndex(size_t index, bool in_bool) override;
    DasResult SetObjectByIndex(size_t index, IDasJson* p_in_das_json) override;

    DasResult GetTypeByName(IDasReadOnlyString* key, DasType* p_out_type)
        override;
    DasResult GetTypeByIndex(size_t index, DasType* p_out_type) override;

    void SetConnection(const boost::signals2::connection& connection);
    void OnExpired();
};

DAS_NS_ANONYMOUS_DETAILS_BEGIN

DasType ToDasType(nlohmann::json::value_t type)
{
    switch (type)
    {
    case nlohmann::json::value_t::null:
        return DAS_TYPE_NULL;
    case nlohmann::json::value_t::object:
        return DAS_TYPE_JSON_OBJECT;
    case nlohmann::json::value_t::array:
        return DAS_TYPE_JSON_ARRAY;
    case nlohmann::json::value_t::string:
        return DAS_TYPE_STRING;
    case nlohmann::json::value_t::boolean:
        return DAS_TYPE_BOOL;
    case nlohmann::json::value_t::number_integer:
        return DAS_TYPE_UINT;
    case nlohmann::json::value_t::number_unsigned:
        return DAS_TYPE_INT;
    case nlohmann::json::value_t::number_float:
        return DAS_TYPE_FLOAT;
    case nlohmann::json::value_t::binary:
        [[fallthrough]];
    case nlohmann::json::value_t::discarded:
        [[fallthrough]];
    default:
        return DAS_TYPE_UNSUPPORTED;
    }
}

template <class Arg>
class CallOperatorSquareBrackets
{
    const Arg arg_;

public:
    CallOperatorSquareBrackets(Arg arg) : arg_{arg} {}

    decltype(auto) operator()(IDasJsonImpl::Object& object) const
    {
        return object.json_[arg_];
    }

    decltype(auto) operator()(const IDasJsonImpl::Object& object) const
    {
        return object.json_[arg_];
    }

    decltype(auto) operator()(IDasJsonImpl::Ref& ref) const
    {
        if (ref.json_ != nullptr)
        {
            return (*ref.json_)[arg_];
        }
        throw DasJsonImplRefExpiredException{};
    }

    decltype(auto) operator()(const IDasJsonImpl::Ref& ref) const
    {
        if (ref.json_ != nullptr)
        {
            return (*ref.json_)[arg_];
        }
        throw DasJsonImplRefExpiredException{};
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

    void operator()(IDasJsonImpl::Object& object) const
    {
        object.json_[arg_] = value_;
    }

    void operator()(IDasJsonImpl::Ref& ref) const
    {
        if (ref.json_ != nullptr)
        {
            (*ref.json_)[arg_] = value_;
        }
        throw DasJsonImplRefExpiredException{};
    }
};

DAS_NS_ANONYMOUS_DETAILS_END

template <class T>
DasResult IDasJsonImpl::GetToImpl(IDasReadOnlyString* p_string, T* obj)
{
    DAS_UTILS_CHECK_POINTER(p_string)
    DAS_UTILS_CHECK_POINTER(obj)

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
        return DAS_S_OK;
    }
    catch (const nlohmann::json::type_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_TYPE_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

template <class T>
DasResult IDasJsonImpl::GetToImpl(size_t index, T* obj)
{
    DAS_UTILS_CHECK_POINTER(obj)

    std::scoped_lock _{mutex_};
    try
    {
        return std::visit(Details::CallOperatorSquareBrackets{index}, impl_);
    }
    catch (const nlohmann::json::type_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_TYPE_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

template <class T>
DasResult IDasJsonImpl::SetImpl(IDasReadOnlyString* p_string, const T& value)
{

    DAS_UTILS_CHECK_POINTER(p_string)

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
        return DAS_S_OK;
    }
    catch (const nlohmann::json::type_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: key = {}", p_u8_key);
        return DAS_E_TYPE_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

template <class T>
DasResult IDasJsonImpl::SetImpl(size_t index, const T& value)
{
    std::scoped_lock _{mutex_};
    try
    {
        std::visit(
            Details::CallOperatorSquareBracketsForAssign{index, value},
            impl_);
        return DAS_S_OK;
    }
    catch (const nlohmann::json::type_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: index = {}", index);
        return DAS_E_TYPE_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult IDasJsonImpl::GetToImpl(
    IDasReadOnlyString*  p_string,
    IDasReadOnlyString** obj)
{
    DAS_UTILS_CHECK_POINTER(p_string)
    DAS_UTILS_CHECK_POINTER(obj)

    const auto expected_u8_key = ToU8StringWithoutOwnership(p_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    std::scoped_lock _{mutex_};
    try
    {
        return ::CreateIDasReadOnlyStringFromUtf8(
            std::visit(Details::CallOperatorSquareBrackets{p_u8_key}, impl_)
                .get_ref<const std::string&>()
                .c_str(),
            obj);
    }
    catch (const nlohmann::json::type_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: key = {}", p_u8_key);
        return DAS_E_TYPE_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult IDasJsonImpl::GetToImpl(size_t index, IDasReadOnlyString** pp_out_obj)
{
    DAS_UTILS_CHECK_POINTER(pp_out_obj)

    std::scoped_lock _{mutex_};
    try
    {
        return ::CreateIDasReadOnlyStringFromUtf8(
            std::visit(Details::CallOperatorSquareBrackets{index}, impl_)
                .get_ref<const std::string&>()
                .c_str(),
            pp_out_obj);
    }
    catch (const nlohmann::json::type_error& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        DAS_CORE_LOG_ERROR("Note: index = {}", index);
        return DAS_E_TYPE_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

IDasJsonImpl::IDasJsonImpl() : impl_{Object{}} {}

IDasJsonImpl::IDasJsonImpl(nlohmann::json& ref_json) : impl_{Ref{&ref_json, {}}}
{
}

IDasJsonImpl::~IDasJsonImpl() {}

DasResult IDasJsonImpl::QueryInterface(const DasGuid& iid, void** pp_out_object)
{
    return Utils::QueryInterfaceAsLastClassInInheritanceInfo<
        Utils::IDasJsonInheritanceInfo,
        IDasJsonImpl>(this, iid, pp_out_object);
}

DasResult IDasJsonImpl::GetIntByName(
    IDasReadOnlyString* key,
    int64_t*            p_out_int)
{
    return GetToImpl(key, p_out_int);
}

DasResult IDasJsonImpl::GetFloatByName(
    IDasReadOnlyString* key,
    float*              p_out_float)
{
    return GetToImpl(key, p_out_float);
}

DasResult IDasJsonImpl::GetStringByName(
    IDasReadOnlyString*  key,
    IDasReadOnlyString** pp_out_string)
{
    return GetToImpl(key, pp_out_string);
}

DasResult IDasJsonImpl::GetBoolByName(IDasReadOnlyString* key, bool* p_out_bool)
{
    return GetToImpl(key, p_out_bool);
}

DasResult IDasJsonImpl::GetObjectRefByName(
    IDasReadOnlyString* key,
    IDasJson**          pp_out_das_json)
{
    DAS_UTILS_CHECK_POINTER(key)
    DAS_UTILS_CHECK_POINTER(pp_out_das_json)

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
        const auto ref_object = MakeDasPtr<IDasJsonImpl>(object);
        auto&      ref_out_das_json = *pp_out_das_json;
        ref_out_das_json = ref_object.Get();
        ref_out_das_json->AddRef();

        if (auto* const p_internal_object = std::get_if<Object>(&impl_);
            p_internal_object != nullptr) [[likely]]
        {
            const auto connection = p_internal_object->signal_.connect(
                [p_ref = ref_object.Get()] { p_ref->OnExpired(); });
            ref_object->SetConnection(connection);
            return DAS_S_OK;
        }
        DAS_CORE_LOG_ERROR("Can not get object from impl_.");
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult IDasJsonImpl::SetIntByName(IDasReadOnlyString* key, int64_t in_int)
{
    return SetImpl(key, in_int);
}

DasResult IDasJsonImpl::SetFloatByName(IDasReadOnlyString* key, float in_float)
{
    return SetImpl(key, in_float);
}

DasResult IDasJsonImpl::SetStringByName(
    IDasReadOnlyString* key,
    IDasReadOnlyString* p_in_string)
{
    const auto expected_u8_key = ToU8StringWithoutOwnership(p_in_string);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_in_u8_key = expected_u8_key.value();

    return SetImpl(key, p_in_u8_key);
}

DasResult IDasJsonImpl::SetBoolByName(IDasReadOnlyString* key, bool in_bool)
{
    return SetImpl(key, in_bool);
}

DasResult IDasJsonImpl::SetObjectByName(
    IDasReadOnlyString* key,
    IDasJson*           p_in_das_json)
{
    DasPtr<IDasJsonImpl> p_impl;

    if (const auto qi_result = p_in_das_json->QueryInterface(
            DasIidOf<IDasJsonImpl>(),
            p_impl.PutVoid());
        DAS::IsFailed(qi_result))
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
                return DAS_E_DANGLING_REFERENCE;
            }},
        impl_);
}

DasResult IDasJsonImpl::GetIntByIndex(size_t index, int64_t* p_out_int)
{
    return GetToImpl(index, p_out_int);
}

DasResult IDasJsonImpl::GetFloatByIndex(size_t index, float* p_out_float)
{
    return GetToImpl(index, p_out_float);
}

DasResult IDasJsonImpl::GetStringByIndex(
    size_t               index,
    IDasReadOnlyString** pp_out_string)
{
    return GetToImpl(index, pp_out_string);
}

DasResult IDasJsonImpl::GetBoolByIndex(size_t index, bool* p_out_bool)
{
    return GetToImpl(index, p_out_bool);
}

DasResult IDasJsonImpl::GetObjectRefByIndex(
    size_t     index,
    IDasJson** pp_out_das_json)
{
    DAS_UTILS_CHECK_POINTER(pp_out_das_json)

    try
    {
        std::scoped_lock _{mutex_};
        auto&&           object =
            std::visit(Details::CallOperatorSquareBrackets{index}, impl_);
        const auto ref_object = MakeDasPtr<IDasJsonImpl>(object);
        auto&      ref_out_das_json = *pp_out_das_json;
        ref_out_das_json = ref_object.Get();
        ref_out_das_json->AddRef();

        if (auto* const p_internal_object = std::get_if<Object>(&impl_);
            p_internal_object != nullptr) [[likely]]
        {
            const auto connection = p_internal_object->signal_.connect(
                [p_ref = ref_object.Get()] { p_ref->OnExpired(); });
            ref_object->SetConnection(connection);
            return DAS_S_OK;
        }
        DAS_CORE_LOG_ERROR("Can not get object from impl_.");
        return DAS_E_INTERNAL_FATAL_ERROR;
    }
    catch (const nlohmann::json::exception& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_INVALID_JSON;
    }
    catch (const DasJsonImplRefExpiredException& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_DANGLING_REFERENCE;
    }
    catch (const std::bad_alloc& ex)
    {
        DAS_CORE_LOG_EXCEPTION(ex);
        return DAS_E_OUT_OF_MEMORY;
    }
}

DasResult IDasJsonImpl::SetIntByIndex(size_t index, int64_t in_int)
{
    return SetImpl(index, in_int);
}

DasResult IDasJsonImpl::SetFloatByIndex(size_t index, float in_float)
{
    return SetImpl(index, in_float);
}

DasResult IDasJsonImpl::SetStringByIndex(
    size_t              index,
    IDasReadOnlyString* p_in_string)
{
    DAS_UTILS_CHECK_POINTER(p_in_string)
    const auto expected_u8_value = ToU8StringWithoutOwnership(p_in_string);
    if (!expected_u8_value)
    {
        return expected_u8_value.error();
    }
    const auto p_u8_key = expected_u8_value.value();

    return SetImpl(index, p_u8_key);
}

DasResult IDasJsonImpl::SetBoolByIndex(size_t index, bool in_bool)
{
    return SetImpl(index, in_bool);
}

DasResult IDasJsonImpl::SetObjectByIndex(size_t index, IDasJson* p_in_das_json)
{
    DasPtr<IDasJsonImpl> p_impl;

    if (const auto qi_result = p_in_das_json->QueryInterface(
            DasIidOf<IDasJsonImpl>(),
            p_impl.PutVoid());
        DAS::IsFailed(qi_result))
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
                return DAS_E_DANGLING_REFERENCE;
            }},
        impl_);
}

DasResult IDasJsonImpl::GetTypeByName(
    IDasReadOnlyString* key,
    DasType*            p_out_type)
{
    DAS_UTILS_CHECK_POINTER(key)

    const auto expected_u8_key = ToU8StringWithoutOwnership(key);
    if (!expected_u8_key)
    {
        return expected_u8_key.error();
    }
    const auto p_u8_key = expected_u8_key.value();

    const auto raw_type =
        std::visit(Details::CallOperatorSquareBrackets{p_u8_key}, impl_).type();
    *p_out_type = Details::ToDasType(raw_type);

    return DAS_S_OK;
}

DasResult IDasJsonImpl::GetTypeByIndex(size_t index, DasType* p_out_type)
{
    DAS_UTILS_CHECK_POINTER(p_out_type)

    const auto raw_type =
        std::visit(Details::CallOperatorSquareBrackets{index}, impl_).type();
    *p_out_type = Details::ToDasType(raw_type);

    return DAS_S_OK;
}

void IDasJsonImpl::SetConnection(const boost::signals2::connection& connection)
{
    if (auto* const p_ref = std::get_if<Ref>(&impl_); p_ref)
    {
        p_ref->connection_ = connection;
        return;
    }
    DAS_CORE_LOG_ERROR("Expect Ref but found Object!");
}

void IDasJsonImpl::OnExpired()
{
    std::visit(
        Utils::overload_set{
            [](const Object&)
            {
                DAS_CORE_LOG_ERROR(
                    "Type not matched. Expected reference but instance found.");
            },
            [](Ref& ref) { ref.json_ = nullptr; }},
        impl_);
}

DAS_CORE_UTILS_NS_END
