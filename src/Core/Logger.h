#pragma once

#include <spdlog/spdlog.h>
#include <memory>

namespace mf {

class Logger {
public:
    static void init();
    static std::shared_ptr<spdlog::logger>& core();

private:
    static std::shared_ptr<spdlog::logger> s_coreLogger;
};

#define MF_TRACE(...) ::mf::Logger::core()->trace(__VA_ARGS__)
#define MF_INFO(...)  ::mf::Logger::core()->info(__VA_ARGS__)
#define MF_WARN(...)  ::mf::Logger::core()->warn(__VA_ARGS__)
#define MF_ERROR(...) ::mf::Logger::core()->error(__VA_ARGS__)

} // namespace mf
