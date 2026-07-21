#include "naval_application.h"

#include "layers/game_layer.h"

#include <moth_graphics/graphics/surface_context.h>
#include <moth_graphics/platform/window.h>

#include <memory>

namespace naval {
    namespace {
        constexpr int kLogicalWidth = 1280;
        constexpr int kLogicalHeight = 720;
    }

    NavalApplication::NavalApplication(moth_graphics::platform::IPlatform& platform)
        : moth_graphics::platform::Application(platform, "Naval Moth", kLogicalWidth, kLogicalHeight) {
    }

    void NavalApplication::PostCreateWindow() {
        m_window->PushLayer(std::make_unique<GameLayer>(
            m_window->GetGraphics(),
            m_window->GetSurfaceContext().GetAssetContext(),
            kLogicalWidth, kLogicalHeight));
    }
}
