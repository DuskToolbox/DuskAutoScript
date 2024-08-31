#ifndef ASR_JSON_H
#define ASR_JSON_H

#include <AutoStarRail/AsrString.hpp>
#include <AutoStarRail/IAsrBase.h>

typedef enum AsrType
{
    ASR_TYPE_INT = 0,
    ASR_TYPE_UINT = 1,
    ASR_TYPE_FLOAT = 2,
    ASR_TYPE_STRING = 4,
    ASR_TYPE_BOOL = 8,
    ASR_TYPE_JSON_OBJECT = 16,
    ASR_TYPE_JSON_ARRAY = 32,
    // 不常见
    ASR_TYPE_NULL = 0x20000000,
    ASR_TYPE_UNSUPPORTED = 0x40000000,
    ASR_TYPE_FORCE_DWORD = 0x7FFFFFFF
} AsrType;

ASR_DEFINE_RET_TYPE(AsrRetType, AsrType);

#ifndef SWIG

ASR_INTERFACE IAsrJson;

// {A1243A5D-53E4-4C4A-B250-9A8871185D64}
ASR_DEFINE_GUID(
    ASR_IID_JSON,
    IAsrJson,
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
ASR_INTERFACE IAsrJson : public IAsrBase
{
    ASR_METHOD GetIntByName(IAsrReadOnlyString * key, int64_t * p_out_int) = 0;
    ASR_METHOD GetFloatByName(IAsrReadOnlyString * key, float* p_out_float) = 0;
    ASR_METHOD GetStringByName(
        IAsrReadOnlyString * key,
        IAsrReadOnlyString * *pp_out_string) = 0;
    ASR_METHOD GetBoolByName(IAsrReadOnlyString * key, bool* p_out_bool) = 0;
    ASR_METHOD GetObjectRefByName(
        IAsrReadOnlyString * key,
        IAsrJson * *pp_out_asr_json) = 0;

    ASR_METHOD SetIntByName(IAsrReadOnlyString * key, int64_t in_int) = 0;
    ASR_METHOD SetFloatByName(IAsrReadOnlyString * key, float in_float) = 0;
    ASR_METHOD SetStringByName(
        IAsrReadOnlyString * key,
        IAsrReadOnlyString * pin_string) = 0;
    ASR_METHOD SetBoolByName(IAsrReadOnlyString * key, bool in_bool) = 0;
    ASR_METHOD SetObjectByName(
        IAsrReadOnlyString * key,
        IAsrJson * pin_asr_json) = 0;

    ASR_METHOD GetIntByIndex(size_t index, int64_t * p_out_int) = 0;
    ASR_METHOD GetFloatByIndex(size_t index, float* p_out_float) = 0;
    ASR_METHOD GetStringByIndex(
        size_t index,
        IAsrReadOnlyString * *pp_out_string) = 0;
    ASR_METHOD GetBoolByIndex(size_t index, bool* p_out_bool) = 0;
    ASR_METHOD GetObjectRefByIndex(size_t index, IAsrJson * *pp_out_asr_json) =
        0;

    ASR_METHOD SetIntByIndex(size_t index, int64_t in_int) = 0;
    ASR_METHOD SetFloatByIndex(size_t index, float in_float) = 0;
    ASR_METHOD SetStringByIndex(size_t index, IAsrReadOnlyString * pin_string) =
        0;
    ASR_METHOD SetBoolByIndex(size_t index, bool in_bool) = 0;
    ASR_METHOD SetObjectByIndex(size_t index, IAsrJson * pin_asr_json) = 0;

    ASR_METHOD GetTypeByName(IAsrReadOnlyString * key, AsrType * p_out_type) =
        0;
    ASR_METHOD GetTypeByIndex(size_t index, AsrType * p_out_type) = 0;
};

#endif // SWIG

struct AsrRetJson;

class AsrJson
{
private:
    ASR::AsrPtr<IAsrJson> p_impl_;

public:
    [[nodiscard]]
    AsrRetInt GetIntByName(const AsrReadOnlyString& key) const
    {
        AsrRetInt result{};
        result.error_code = p_impl_->GetIntByName(key.Get(), &result.value);
        return result;
    }
    [[nodiscard]]
    AsrRetFloat GetFloatByName(const AsrReadOnlyString& key) const
    {
        AsrRetFloat result{};
        result.error_code = p_impl_->GetFloatByName(key.Get(), &result.value);
        return result;
    }
    [[nodiscard]]
    AsrRetReadOnlyString GetStringByName(const AsrReadOnlyString& key) const
    {
        AsrRetReadOnlyString            result{};
        ASR::AsrPtr<IAsrReadOnlyString> p_value{};
        result.error_code = p_impl_->GetStringByName(key.Get(), p_value.Put());
        result.value = std::move(p_value);
        return result;
    }
    [[nodiscard]]
    AsrRetBool GetBoolByName(const AsrReadOnlyString& key) const
    {
        AsrRetBool result{};
        result.error_code = p_impl_->GetBoolByName(key.Get(), &result.value);
        return result;
    }
    [[nodiscard]]
    AsrRetJson GetObjectByName(const AsrReadOnlyString& key) const;

    AsrResult SetIntByName(const AsrReadOnlyString& key, int64_t in_int) const
    {
        return p_impl_->SetIntByName(key.Get(), in_int);
    }

    AsrResult SetFloatByName(const AsrReadOnlyString& key, float in_float) const
    {
        return p_impl_->SetFloatByName(key.Get(), in_float);
    }

    AsrResult SetStringByName(
        const AsrReadOnlyString& key,
        const AsrReadOnlyString& in_string) const
    {
        return p_impl_->SetStringByName(key.Get(), in_string.Get());
    }

    AsrResult SetBoolByName(const AsrReadOnlyString& key, bool in_bool) const
    {
        return p_impl_->SetBoolByName(key.Get(), in_bool);
    }

    AsrResult SetObjectByName(
        const AsrReadOnlyString& key,
        const AsrJson&           in_asr_json)
    {
        return p_impl_->SetObjectByName(key.Get(), in_asr_json.p_impl_.Get());
    }

    [[nodiscard]]
    AsrRetInt GetIntByIndex(size_t index) const
    {
        AsrRetInt result{};
        result.error_code = p_impl_->GetIntByIndex(index, &result.value);
        return result;
    }
    [[nodiscard]]
    AsrRetFloat GetFloatByIndex(size_t index) const
    {
        AsrRetFloat result{};
        result.error_code = p_impl_->GetFloatByIndex(index, &result.value);
        return result;
    }
    [[nodiscard]]
    AsrRetReadOnlyString GetStringByIndex(size_t index) const
    {
        AsrRetReadOnlyString            result{};
        ASR::AsrPtr<IAsrReadOnlyString> p_value{};
        result.error_code = p_impl_->GetStringByIndex(index, p_value.Put());
        return result;
    }
    [[nodiscard]]
    AsrRetBool GetBoolByIndex(size_t index) const
    {
        AsrRetBool result{};
        result.error_code = p_impl_->GetBoolByIndex(index, &result.value);
        return result;
    }
    [[nodiscard]]
    AsrRetJson GetObjectRefByIndex(size_t index) const;

    AsrResult SetIntByIndex(size_t index, int64_t in_int)
    {
        return p_impl_->SetIntByIndex(index, in_int);
    }
    AsrResult SetFloatByIndex(size_t index, float in_float)
    {
        return p_impl_->SetFloatByIndex(index, in_float);
    }
    AsrResult SetStringByIndex(size_t index, const AsrReadOnlyString& in_string)
    {
        return p_impl_->SetStringByIndex(index, in_string.Get());
    }
    AsrResult SetBoolByIndex(size_t index, bool in_bool)
    {
        return p_impl_->SetBoolByIndex(index, in_bool);
    }
    AsrResult SetObjectByIndex(size_t index, AsrJson in_asr_json)
    {
        return p_impl_->SetObjectByIndex(index, in_asr_json.p_impl_.Get());
    }

#ifndef SWIG
    AsrResult GetTo(const AsrReadOnlyString& key, AsrReadOnlyString& OUTPUT)
    {
        ASR::AsrPtr<IAsrReadOnlyString> p_value{};
        const auto                      error_code =
            p_impl_->GetStringByName(key.Get(), p_value.Put());
        if (ASR::IsOk(error_code))
        {
            OUTPUT = {std::move(p_value)};
        }
        return error_code;
    }
    AsrResult GetTo(const AsrReadOnlyString& key, float& OUTPUT)
    {
        return p_impl_->GetFloatByName(key.Get(), &OUTPUT);
    }
    AsrResult GetTo(const AsrReadOnlyString& key, int64_t& OUTPUT)
    {
        return p_impl_->GetIntByName(key.Get(), &OUTPUT);
    }
    AsrResult GetTo(const AsrReadOnlyString& key, bool& OUTPUT)
    {
        return p_impl_->GetBoolByName(key.Get(), &OUTPUT);
    }
    AsrResult GetTo(const AsrReadOnlyString& key, AsrJson& OUTPUT)
    {
        ASR::AsrPtr<IAsrJson> p_OUTPUT{};
        const auto            error_code =
            p_impl_->GetObjectRefByName(key.Get(), p_OUTPUT.Put());
        if (ASR::IsOk(error_code))
        {
            OUTPUT.p_impl_ = p_OUTPUT;
        }
        return error_code;
    }

    AsrResult GetTo(size_t index, AsrReadOnlyString& OUTPUT)
    {
        ASR::AsrPtr<IAsrReadOnlyString> p_OUTPUT{};
        const auto error_code = p_impl_->GetStringByIndex(index, p_OUTPUT.Put());
        if (ASR::IsOk(error_code))
        {
            OUTPUT = {std::move(p_OUTPUT)};
        }
        return error_code;
    }
    AsrResult GetTo(size_t index, float& OUTPUT)
    {
        return p_impl_->GetFloatByIndex(index, &OUTPUT);
    }
    AsrResult GetTo(size_t index, int64_t& OUTPUT)
    {
        return p_impl_->GetIntByIndex(index, &OUTPUT);
    }
    AsrResult GetTo(size_t index, bool& OUTPUT)
    {
        return p_impl_->GetBoolByIndex(index, &OUTPUT);
    }
    AsrResult GetTo(size_t index, AsrJson& OUTPUT)
    {
        ASR::AsrPtr<IAsrJson> p_OUTPUT{};
        const auto            error_code =
            p_impl_->GetObjectRefByIndex(index, p_OUTPUT.Put());
        if (ASR::IsOk(error_code))
        {
            OUTPUT.p_impl_ = p_OUTPUT;
        }
        return error_code;
    }
#endif // SWIG
};

ASR_DEFINE_RET_TYPE(AsrRetJson, AsrJson);

inline AsrRetJson AsrJson::GetObjectByName(const AsrReadOnlyString& key) const
{
    AsrRetJson            result{};
    ASR::AsrPtr<IAsrJson> p_value{};
    result.error_code = p_impl_->GetObjectRefByName(key.Get(), p_value.Put());
    return result;
}

inline AsrRetJson AsrJson::GetObjectRefByIndex(size_t index) const
{
    AsrRetJson            result{};
    ASR::AsrPtr<IAsrJson> p_value{};
    result.error_code = p_impl_->GetObjectRefByIndex(index, p_value.Put());
    return result;
}

#endif // ASR_JSON_H
