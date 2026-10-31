#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <string>
#include <vector>

namespace RE {
    class Actor;
}

namespace SAFCompanion::Overlay
{
    struct PendingPlay {
        std::string animId;
        RE::Actor*  actor = nullptr;
    };

    enum class MenuAction {
        Toggle,
        Select,
        Next,
        Back,
        Up,
        Down,
        PageUp,
        PageDown
    };

    void RenderMenu();
    void ProcessAction(MenuAction action);
    void HandleKeyEvent(WORD keyCode);
    bool IsOverlayVisible();
    void ToggleOverlay();

    // Called from C++ overlay when user clicks "Play"
    void QueuePlayAnimation(const std::string& animId, RE::Actor* targetActor);

    // Called from main thread to process pending play request
    bool PopPendingPlay(PendingPlay& out);
}
