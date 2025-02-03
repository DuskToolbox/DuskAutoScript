#ifndef DAS_GATEWAY_LOGGER_H
#define DAS_GATEWAY_LOGGER_H

#include <memory>
#include <spdlog/spdlog.h>

#include <das/gateway/Config.h>

DAS_GATEWAY_NS_BEGIN

std::shared_ptr<spdlog::logger>& GetLogger();

DAS_GATEWAY_NS_END

#endif // DAS_GATEWAY_LOGGER_H
