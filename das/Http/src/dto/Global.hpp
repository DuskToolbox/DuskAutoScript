#ifndef DAS_HTTP_DTO_GLOBAL_HPP
#define DAS_HTTP_DTO_GLOBAL_HPP

/**
 *  API全局类型
 *  API global type
 */

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

// 定义统一响应包装类型
// Define unified response wrapper type
template <class T>
class ApiResponse : public oatpp::DTO
{

    DTO_INIT(ApiResponse, DTO)

    DTO_FIELD(Int32, code);
    DTO_FIELD(String, message);

    DTO_FIELD(T, data);
};

template <>
class ApiResponse<void> : public oatpp::DTO
{
    DTO_INIT(ApiResponse, DTO)

    DTO_FIELD(Int32, code);
    DTO_FIELD(String, message);
};

#include OATPP_CODEGEN_END(DTO)

#endif // DAS_HTTP_DTO_GLOBAL_HPP