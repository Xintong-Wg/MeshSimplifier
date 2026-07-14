#include "Core/Logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace mf {

std::shared_ptr<spdlog::logger> Logger::s_coreLogger;

void Logger::init() {
    if (s_coreLogger) return; // already initialized
    s_coreLogger = spdlog::stdout_color_mt("MeshForge");
    s_coreLogger->set_pattern("[%T] [%^%l%$] %v");
    s_coreLogger->set_level(spdlog::level::trace);
}

std::shared_ptr<spdlog::logger>& Logger::core() {
    return s_coreLogger;
}

} // namespace mf
