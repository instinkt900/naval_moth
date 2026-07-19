#include "game/emcon_system.h"

#include "game/components.h"

namespace naval {
    void UpdateEmcon(entt::registry& registry) {
        // Only AI ships (those with an Aggro brain) manage EMCON here; the player
        // works its own radar. A ship radiates while it holds anything on the plot —
        // a bare passive bearing is cue enough to go active and range it — and falls
        // dark once every contact, including the decaying ghosts of lost fixes, has
        // aged out of its picture. So going active is a consequence of hunting, not a
        // standing posture, and the price of it (being heard) is paid only while the
        // ship actually has prey.
        auto view = registry.view<Sensors, ContactPicture, Aggro>();
        for (auto entity : view) {
            view.get<Sensors>(entity).activeOn = !view.get<ContactPicture>(entity).contacts.empty();
        }
    }
}
