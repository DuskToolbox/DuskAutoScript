#ifndef DAS_CORE_UTILS_DASJSONIMPL_H
#define DAS_CORE_UTILS_DASJSONIMPL_H

#include <boost/signals2.hpp>
#include <das/Core/Utils/Config.h>
#include <das/_autogen/idl/abi/DasJson.h>
#include <das/_autogen/idl/wrapper/Das.ExportInterface.IDasJson.Implements.hpp>
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

struct DasJsonImplJsonIsNotArray : public std::exception
{
    [[nodiscard]]
    const char* what() const noexcept override
    {
        return "Json is not an array!";
    }
};

class IDasJsonImpl final
    : public DAS::ExportInterface::DasJsonImplBase<IDasJsonImpl>
{
public:
    struct Object
    {
        nlohmann::json                  json_{};
        boost::signals2::signal<void()> signal_{};
    };

    struct Ref
    {
        nlohmann::json*                    json_{nullptr};
        boost::signals2::scoped_connection connection_{};
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
    IDasJsonImpl(const char* p_json_string);
    IDasJsonImpl(nlohmann::json& ref_json);
    explicit IDasJsonImpl(nlohmann::json&& json);
    ~IDasJsonImpl();

    // IDasBase — AddRef/Release inherited from DasJsonImplBase (atomic + delete
    // this)
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

    DasResult GetTypeByName(
        IDasReadOnlyString*            key,
        Das::ExportInterface::DasType* p_out_type) override;
    DasResult GetTypeByIndex(
        size_t                         index,
        Das::ExportInterface::DasType* p_out_type) override;
    DasResult ToString(int32_t indent, IDasReadOnlyString** pp_out_string)
        override;
    DasResult GetSize(uint64_t* p_out_size) override;
    DasResult Clear() override;

    void           SetConnection(const boost::signals2::connection& connection);
    void           OnExpired();
    nlohmann::json ExtractJson() const;
};

DAS_CORE_UTILS_NS_END

#endif // DAS_CORE_UTILS_DASJSONIMPL_H
