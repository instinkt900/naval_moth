// naval_moth — top-down naval combat on moth_graphics + Box2D + EnTT.
// See PLAN.md for the design and milestones.

#include "naval_application.h"

#include <moth_graphics/platform/glfw/glfw_platform.h>
#include <moth_graphics/platform/sdl/sdl_platform.h>
#include <moth_ui/ilogger.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string_view>

namespace {
    class UILogger : public moth_ui::ILogger {
    public:
        void Log(moth_ui::LogLevel level, std::string_view message) override {
            switch (level) {
                case moth_ui::LogLevel::Debug:
                    spdlog::debug("{}", message);
                    return;
                case moth_ui::LogLevel::Info:
                    spdlog::info("{}", message);
                    return;
                case moth_ui::LogLevel::Warning:
                    spdlog::warn("{}", message);
                    return;
                case moth_ui::LogLevel::Error:
                    spdlog::error("{}", message);
                    return;
            }
        }
    };
}

int main(int argc, char** argv) {
    UILogger uiLogger;
    moth_ui::SetLogger(&uiLogger);

    // SDL is the default backend; pass --vulkan to run on GLFW + Vulkan.
    bool useVulkan = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--vulkan") {
            useVulkan = true;
        }
    }

    std::unique_ptr<moth_graphics::platform::IPlatform> platform;
    if (useVulkan) {
        platform = std::make_unique<moth_graphics::platform::glfw::Platform>();
    } else {
        platform = std::make_unique<moth_graphics::platform::sdl::Platform>();
    }

    platform->Startup();
    naval::NavalApplication application(*platform);
    application.Init();
    application.Run();
    platform->Shutdown();
}
