#pragma once

#include <cstdint>

namespace SAFCompanion::OSFUIBridge
{
    enum class ActionKey : int
    {
        kStop = 0,
        kPlayer,
        kNpc,
        kCamera,
        kCount
    };

    // Fetch the OSF UI native bridge once (main thread, after kPostDataLoad) and
    // register our native commands, settings and hotkeys. No-op if OSF UI is
    // absent or too old.
    void Init();

    // Publish the Control animation list as OSF UI state for mod "saf.companion".
    // Safe from any thread; a no-op until the bridge is connected.
    void PublishAnims();

    // Publish the active menu mode ("player" / "npc").
    void PublishMode(const char* a_mode);

    // Close the animation menu view through the bridge.
    void RequestClose();

    // VK code of the key currently bound to an action (0 = unknown/skip).
    std::uint16_t GetActionVK(ActionKey a_action);

    // Ponowne odczytanie przypisan klawiszy z OSF UI (throttle ~2 s).
    // OSF UI sluzy tylko jako panel konfiguracji - klawisze czyta OnFrame.
    void TickBindings();
}
