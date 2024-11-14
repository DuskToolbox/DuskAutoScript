#ifndef DAS_JSON_H
#define DAS_JSON_H

#include <das/DasString.hpp>
#include <das/IDasBase.h>

typedef enum DasType
{
    DAS_TYPE_INT = 0,
    DAS_TYPE_UINT = 1,
    DAS_TYPE_FLOAT = 2,
    DAS_TYPE_STRING = 4,
    DAS_TYPE_BOOL = 8,
    DAS_TYPE_JSON_OBJECT = 16,
    DAS_TYPE_JSON_ARRAY = 32,
    // 不常见
    DAS_TYPE_NULL = 0x20000000,
    DAS_TYPE_UNSUPPORTED = 0x40000000,
    DAS_TYPE_FORCE_DWORD = 0x7FFFFFFF
} DasType;

DAS_DEFINE_RET_TYPE(DasRetType, DasType);

#ifndef SWIG

DAS_INTERFACE IDasJson;

// {A1243A5D-53E4-4C4A-B250-9A8871185D64}
DAS_DEFINE_GUID(
    DAS_IID_JSON,
    IDasJson,
    0xa1243a5d,
    0x53e4,
    0x4c4a,
    0xb2,
    0x50,
    0x9a,
    0x88,
    0x71,
    0x18,
    0x5d,
    0x64)
DAS_INTERFACE IDasJson : public IDasBase
{
    DAS_METHOD GetIntByName(IDasReadOnlyString * key, int64_t * p_out_int) = 0;
    DAS_METHOD GetFloatByName(IDasReadOnlyString * key, float* p_out_float) = 0;
    DAS_METHOD GetStringByName(
        IDasReadOnlyString * key,
        IDasReadOnlyString * *pp_out_string) = 0;
    DAS_METHOD GetBoolByName(IDasReadOnlyString * key, bool* p_out_bool) = 0;
    DAS_METHOD GetObjectRefByName(
        IDasReadOnlyString * key,
        IDasJson * *pp_out_asr_json) = 0;

    DAS_METHOD SetIntByName(IDasReadOnlyString * key, int64_t in_int) = 0;
    DAS_METHOD SetFloatByName(IDasReadOnlyString * key, float in_float) = 0;
    DAS_METHOD SetStringByName(
        IDasReadOnlyString * key,
        IDasReadOnlyString * pin_string) = 0;
    DAS_METHOD SetBoolByName(IDasReadOnlyString * key, bool in_bool) = 0;
    DAS_METHOD SetObjectByName(
        IDasReadOnlyString * key,
        IDasJson * pin_asr_json) = 0;

    DAS_METHOD GetIntByIndex(size_t index, int64_t * p_out_int) = 0;
    DAS_METHOD GetFloatByIndex(size_t index, float* p_out_float) = 0;
    DAS_METHOD GetStringByIndex(
        size_t index,
        IDasReadOnlyString * *pp_out_string) = 0;
    DAS_METHOD GetBoolByIndex(size_t index, bool* p_out_bool) = 0;
    DAS_METHOD GetObjectRefByIndex(size_t index, IDasJson * *pp_out_asr_json) =
        0;

    DAS_METHOD SetIntByIndex(size_t index, int64_t in_int) = 0;
    DAS_METHOD SetFloatByIndex(size_t index, float in_float) = 0;
    DAS_METHOD SetStringByIndex(size_t index, IDasReadOnlyString * pin_string) =
        0;
    DAS_METHOD SetBoolByIndex(size_t index, bool in_bool) = 0;
    DAS_METHOD SetObjectByIndex(size_t index, IDasJson * pin_asr_json) = 0;

    DAS_METHOD GetTypeByName(IDasReadOnlyString * key, DasType * p_out_type) =
        0;
    DAS_METHOD GetTypeByIndex(size_t index, DasType * p_out_type) = 0;
};

#endif // SWIG

struct DasRetJson;

class DasJson
{
private:
    DAS::DasPtr<IDasJson> p_impl_;

public:
    [[nodiscard]]
    DasRetInt GetIntByName(const DasReadOnlyString& key) const
    {
        DasRetInt result{};
        result.error_code = p_impl_->GetIntByName(key.Get(), &result.value);
        return result;
    }
    [[nodiscard]]
    DasRetFloat GetFloatByName(const DasReadOnlyString& key) const
    {
        DasRetFloat result{};
        result.error_code = p_impl_->GetFloatByName(key.Get(), &result.value);
        return result;
    }
    [[nodiscard]]
    DasRetReadOnlyString GetStringByName(const DasReadOnlyString& key) const
    {
        DasRetReadOnlyString            result{};
        DAS::DasPtr<IDasReadOnlyString> p_value{};
        result.error_code = p_impl_->GetStringByName(key.Get(), p_value.Put());
        result.value = std::move(p_value);
        return result;
    }
    [[nodiscard]]
    DasRetBool GetBoolByName(const DasReadOnlyString& key) const
    {
        DasRetBool result{};
        result.error_code = p_impl_->GetBoolByName(key.Get(), &result.value);
        return result;
    }
    [[nodiscard]]
    DasRetJson GetObjectByName(const DasReadOnlyString& key) const;

    DasResult SetIntByName(const DasReadOnlyString& key, int64_t in_int) const
    {
        return p_impl_->SetIntByName(key.Get(), in_int);
    }

    DasResult SetFloatByName(const DasReadOnlyString& key, float in_float) const
    {
        return p_impl_->SetFloatByName(key.Get(), in_float);
    }

    DasResult SetStringByName(
        const DasReadOnlyString& key,
        const DasReadOnlyString& in_string) const
    {
        return p_impl_->SetStringByName(key.Get(), in_string.Get());
    }

    DasResult SetBoolByName(const DasReadOnlyString& key, bool in_bool) const
    {
        return p_impl_->SetBoolByName(key.Get(), in_bool);
    }

    DasResult SetObjectByName(
        const DasReadOnlyString& key,
        const DasJson&           in_asr_json)
    {
        return p_impl_->SetObjectByName(key.Get(), in_asr_json.p_impl_.Get());
    }

    [[nodiscard]]
    DasRetInt GetIntByIndex(size_t index) const
    {
        DasRetInt result{};
        result.error_code = p_impl_->GetIntByIndex(index, &result.value);
        return result;
    }
    [[nodiscard]]
    DasRetFloat GetFloatByIndex(size_t index) const
    {
        DasRetFloat result{};
        result.error_code = p_impl_->GetFloatByIndex(index, &result.value);
        return result;
    }
    [[nodiscard]]
    DasRetReadOnlyString GetStringByIndex(size_t index) const
    {
        DasRetReadOnlyString            result{};
        DAS::DasPtr<IDasReadOnlyString> p_value{};
        result.error_code = p_impl_->GetStringByIndex(index, p_value.Put());
        return result;
    }
    [[nodiscard]]
    DasRetBool GetBoolByIndex(size_t index) const
    {
        DasRetBool result{};
        result.error_code = p_impl_->GetBoolByIndex(index, &result.value);
        return result;
    }
    [[nodiscard]]
    DasRetJson GetObjectRefByIndex(size_t index) const;

    DasResult SetIntByIndex(size_t index, int64_t in_int)
    {
        return p_impl_->SetIntByIndex(index, in_int);
    }
    DasResult SetFloatByIndex(size_t index, float in_float)
    {
        return p_impl_->SetFloatByIndex(index, in_float);
    }
    DasResult SetStringByIndex(size_t index, const DasReadOnlyString& in_string)
    {
        return p_impl_->SetStringByIndex(index, in_string.Get());
    }
    DasResult SetBoolByIndex(size_t index, bool in_bool)
    {
        return p_impl_->SetBoolByIndex(index, in_bool);
    }
    DasResult SetObjectByIndex(size_t index, DasJson in_asr_json)
    {
        return p_impl_->SetObjectByIndex(index, in_asr_json.p_impl_.Get());
    }

#ifndef SWIG
    DasResult GetTo(const DasReadOnlyString& key, DasReadOnlyString& OUTPUT)
    {
        DAS::DasPtr<IDasReadOnlyString> p_value{};
        const auto                      error_code =
            p_impl_->GetStringByName(key.Get(), p_value.Put());
        if (DAS::IsOk(error_code))
        {
            OUTPUT = {std::move(p_value)};
        }
        return error_code;
    }
    DasResult GetTo(const DasReadOnlyString& key, float& OUTPUT)
    {
        return p_impl_->GetFloatByName(key.Get(), &OUTPUT);
    }
    DasResult GetTo(const DasReadOnlyString& key, int64_t& OUTPUT)
    {
        return p_impl_->GetIntByName(key.Get(), &OUTPUT);
    }
    DasResult GetTo(const DasReadOnlyString& key, bool& OUTPUT)
    {
        return p_impl_->GetBoolByName(key.Get(), &OUTPUT);
    }
    DasResult GetTo(const DasReadOnlyString& key, DasJson& OUTPUT)
    {
        DAS::DasPtr<IDasJson> p_OUTPUT{};
        const auto            error_code =
            p_impl_->GetObjectRefByName(key.Get(), p_OUTPUT.Put());
        if (DAS::IsOk(error_code))
        {
            OUTPUT.p_impl_ = p_OUTPUT;
        }
        return error_code;
    }

    DasResult GetTo(size_t index, DasReadOnlyString& OUTPUT)
    {
        DAS::DasPtr<IDasReadOnlyString> p_OUTPUT{};
        const auto error_code = p_impl_->GetStringByIndex(index, p_OUTPUT.Put());
        if (DAS::IsOk(error_code))
        {
            OUTPUT = {std::move(p_OUTPUT)};
        }
        return error_code;
    }
    DasResult GetTo(size_t index, float& OUTPUT)
    {
        return p_impl_->GetFloatByIndex(index, &OUTPUT);
    }
    DasResult GetTo(size_t index, int64_t& OUTPUT)
    {
        return p_impl_->GetIntByIndex(index, &OUTPUT);
    }
    DasResult GetTo(size_t index, bool& OUTPUT)
    {
        return p_impl_->GetBoolByIndex(index, &OUTPUT);
    }
    DasResult GetTo(size_t index, DasJson& OUTPUT)
    {
        DAS::DasPtr<IDasJson> p_OUTPUT{};
        const auto            error_code =
            p_impl_->GetObjectRefByIndex(index, p_OUTPUT.Put());
        if (DAS::IsOk(error_code))
        {
            OUTPUT.p_impl_ = p_OUTPUT;
        }
        return error_code;
    }
#endif // SWIG
};

DAS_DEFINE_RET_TYPE(DasRetJson, DasJson);

inline DasRetJson DasJson::GetObjectByName(const DasReadOnlyString& key) const
{
    DasRetJson            result{};
    DAS::DasPtr<IDasJson> p_value{};
    result.error_code = p_impl_->GetObjectRefByName(key.Get(), p_value.Put());
    return result;
}

inline DasRetJson DasJson::GetObjectRefByIndex(size_t index) const
{
    DasRetJson            result{};
    DAS::DasPtr<IDasJson> p_value{};
    result.error_code = p_impl_->GetObjectRefByIndex(index, p_value.Put());
    return result;
}

#endif // DAS_JSON_H
