#ifndef DAS_LOGGER_H
#define DAS_LOGGER_H

#include <das/DasExport.h>
#include <das/DasString.hpp>

#ifndef SWIG

typedef struct _asr_SourceLocation
{
    const char* file_name;
    int         line;
    const char* function_name;
} DasSourceLocation;

#define DAS_LOG_WITH_SOURCE_LOCATION(type, ...)                                \
    do                                                                         \
    {                                                                          \
        DasSourceLocation _asr_internal_source_location = {                    \
            __FILE__,                                                          \
            __LINE__,                                                          \
            __func__};                                                         \
        DasLog##type##U8WithSourceLocation(                                    \
            __VA_ARGS__,                                                       \
            &_asr_internal_source_location);                                   \
    } while (false)

#define DAS_LOG_ERROR(...) DAS_LOG_WITH_SOURCE_LOCATION(Error, __VA_ARGS__)
#define DAS_LOG_WARNING(...) DAS_LOG_WITH_SOURCE_LOCATION(Warning, __VA_ARGS__)
#define DAS_LOG_INFO(...) DAS_LOG_WITH_SOURCE_LOCATION(Info, __VA_ARGS__)

DAS_C_API void DasLogError(IDasReadOnlyString* p_readonly_string);
DAS_C_API void DasLogErrorU8(const char* p_string);
DAS_C_API void DasLogErrorU8WithSourceLocation(
    const char*              p_string,
    const DasSourceLocation* p_location);

DAS_C_API void DasLogWarning(IDasReadOnlyString* p_readonly_string);
DAS_C_API void DasLogWarningU8(const char* p_string);
DAS_C_API void DasLogWarningU8WithSourceLocation(
    const char*              p_string,
    const DasSourceLocation* p_location);

DAS_C_API void DasLogInfo(IDasReadOnlyString* p_readonly_string);
DAS_C_API void DasLogInfoU8(const char* p_string);
DAS_C_API void DasLogInfoU8WithSourceLocation(
    const char*              p_string,
    const DasSourceLocation* p_location);

// {9BC34D72-E442-4944-ACE6-69257D262568}
DAS_DEFINE_GUID(
    DAS_IID_LOG_READER,
    IDasLogReader,
    0x9bc34d72,
    0xe442,
    0x4944,
    0xac,
    0xe6,
    0x69,
    0x25,
    0x7d,
    0x26,
    0x25,
    0x68);
DAS_INTERFACE IDasLogReader : public IDasBase
{
    DAS_METHOD ReadOne(const char* message, size_t size) = 0;
};

// {806E244C-CCF0-4DC3-AD54-6886FDF9B1F4}
DAS_DEFINE_GUID(
    DAS_IID_LOG_REQUESTER,
    IDasLogRequester,
    0x806e244c,
    0xccf0,
    0x4dc3,
    0xad,
    0x54,
    0x68,
    0x86,
    0xfd,
    0xf9,
    0xb1,
    0xf4);
DAS_INTERFACE IDasLogRequester : public IDasBase
{
    /**
     * @brief 使用用户自定义的方法对数据进行读取
     * @param p_reader 在内部加锁后进行阅读的操作，由用户继承并实现
     * @return 指示操作是否成功
     */
    DAS_METHOD RequestOne(IDasLogReader * p_reader) = 0;
};

DAS_C_API DasResult CreateIDasLogRequester(
    uint32_t           max_line_count,
    IDasLogRequester** pp_out_requester);

#endif // SWIG

DAS_API void DasLogError(DasReadOnlyString asr_string);

DAS_API void DasLogWarning(DasReadOnlyString asr_string);

DAS_API void DasLogInfo(DasReadOnlyString asr_string);

#endif // DAS_LOGGER_H
