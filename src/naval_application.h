#pragma once

#include <moth_graphics/platform/application.h>

namespace naval {
    // moth_graphics application: creates the window and pushes the GameLayer,
    // which owns the ECS world and drives the simulation.
    class NavalApplication : public moth_graphics::platform::Application {
    public:
        explicit NavalApplication(moth_graphics::platform::IPlatform& platform);

        NavalApplication(NavalApplication const&) = delete;
        NavalApplication& operator=(NavalApplication const&) = delete;

    private:
        void PostCreateWindow() override;
    };
}
