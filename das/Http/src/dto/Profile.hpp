#ifndef DAS_HTTP_DTO_PROFILE_HPP
#define DAS_HTTP_DTO_PROFILE_HPP

#include "Global.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 *  定义配置文件相关数据类型
 *  Define profile related data types
 */

// 配置文件描述符
// Profile descriptor
class ProfileDesc : public oatpp::DTO
{

    DTO_INIT(ProfileDesc, DTO)

    DTO_FIELD(String, name, "name");
    DTO_FIELD(String, profile_id, "profile_id");

};

using ProfileDescList = ApiResponse<oatpp::List<oatpp::Object<ProfileDesc>>>;

// 配置文件描状态
// Profile status
class ProfileStatus : public oatpp::DTO
{

    DTO_INIT(ProfileStatus, DTO)

    DTO_FIELD(String, profile_id, "profile_id");
    DTO_FIELD(Boolean, run, "run");
    DTO_FIELD(Boolean, enable, "enable");
};

using ProfileStatusList = ApiResponse<oatpp::List<oatpp::Object<ProfileStatus>>>;

// 配置文件运行状态
// Profile is runing
class ProfileRunning : public oatpp::DTO
{

    DTO_INIT(ProfileRunning, DTO)

    DTO_FIELD(String, profile_id, "profile_id");
    DTO_FIELD(Boolean, run, "run");
};

class ProfileId : public oatpp::DTO
{

    DTO_INIT(ProfileRunning, DTO)

    DTO_FIELD(String, profile_id, "profile_id");
};

#include OATPP_CODEGEN_END(DTO)

#endif // DAS_HTTP_DTO_PROFILE_HPP
