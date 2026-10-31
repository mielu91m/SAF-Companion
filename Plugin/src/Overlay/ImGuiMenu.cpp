#include "PCH.h"
#include "Overlay/ImGuiMenu.h"
#include "Overlay/D3D12Hook.h"
#include "Papyrus/SAFCompanionAPI.h"
#include "ActorUtils.h"
#include "RE/P/PlayerCharacter.h"
#include <imgui.h>
#include <Windows.h>
#include <atomic>
#include <vector>
#include <string>

namespace RE {
    class Actor;
}

namespace SAFCompanion::Overlay
{
    // ── Pending play action (processed on main thread) ───────────────────
    static std::atomic<bool>     g_hasPending{ false };
    static PendingPlay         g_pending{};

    void QueuePlayAnimation(const std::string& animId, RE::Actor* targetActor)
    {
        g_pending.animId = animId;
        g_pending.actor  = targetActor;
        g_hasPending.store(true);
    }

    bool PopPendingPlay(PendingPlay& out)
    {
        if (g_hasPending.exchange(false)) {
            out = g_pending;
            return true;
        }
        return false;
    }

    // Actor helpers from ActorUtils.cpp
    using SAFCompanion::GetNearbyActorsNative;

    // ── UI State ─────────────────────────────────────────────────────────
    static std::vector<std::string> g_animations;
    static std::vector<RE::Actor*>  g_actors;
    static int                      g_selectedAnim = -1;
    static int                      g_selectedActor = 0;
    static float                    g_maxDistance = 500.0f;
    static int                      g_maxActors   = 4;
    static bool                     g_needsRefresh = true;

    enum class Step {
        SelectAnimation,
        SelectActor
    };
    static Step g_currentStep = Step::SelectAnimation;

    // External visibility state (defined in D3D12Hook.cpp)
    extern void ToggleOverlay();
    extern bool IsOverlayVisible();

    void RefreshData()
    {
        g_animations = SAFCompanion::Papyrus::GetAvailableAnimations_Impl();
        g_actors.clear();

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            g_actors.push_back(player);
        }

        // TODO-CTD: traversal celi wywalal gre (Num7) - wylaczone do wyjasnienia.
        // Diagnostyka przez CountNearbyActorsNative (sam licznik, bez obiektow).
        // auto nearby = GetNearbyActorsNative(g_maxDistance, g_maxActors);
        // for (auto* actor : nearby) {
        //     if (actor != player) {
        //         g_actors.push_back(actor);
        //     }
        // }

        if (g_selectedAnim >= static_cast<int>(g_animations.size())) {
            g_selectedAnim = -1;
        }
        if (g_selectedActor >= static_cast<int>(g_actors.size())) {
            g_selectedActor = 0;
        }
        g_needsRefresh = false;
    }

    void ProcessAction(MenuAction action)
    {
        if (action == MenuAction::Toggle) {
            ToggleOverlay();
            if (IsOverlayVisible()) {
                g_currentStep = Step::SelectAnimation;
                g_needsRefresh = true;
            }
            return;
        }

        if (!IsOverlayVisible()) return;

        auto& currentIdx = (g_currentStep == Step::SelectAnimation) ? g_selectedAnim : g_selectedActor;
        size_t currentListSize = (g_currentStep == Step::SelectAnimation) ? g_animations.size() : g_actors.size();

        switch (action) {
        case MenuAction::Up:
            if (currentIdx > 0) currentIdx--;
            else if (currentListSize > 0) currentIdx = (int)currentListSize - 1;
            break;
        case MenuAction::Down:
            if (currentIdx < (int)currentListSize - 1) currentIdx++;
            else currentIdx = 0;
            break;
        case MenuAction::PageUp:
            currentIdx = std::max(0, currentIdx - 10);
            break;
        case MenuAction::PageDown:
            currentIdx = std::min((int)currentListSize - 1, currentIdx + 10);
            break;
        case MenuAction::Next:
        case MenuAction::Select:
            if (g_currentStep == Step::SelectAnimation && g_selectedAnim != -1) {
                g_currentStep = Step::SelectActor;
                g_needsRefresh = true;
            } else if (g_currentStep == Step::SelectActor && g_selectedActor != -1 && g_selectedAnim != -1) {
                QueuePlayAnimation(g_animations[g_selectedAnim], g_actors[g_selectedActor]);
                ToggleOverlay(); // Auto-close
            }
            break;
        case MenuAction::Back:
            if (g_currentStep == Step::SelectActor) {
                g_currentStep = Step::SelectAnimation;
            } else {
                ToggleOverlay();
            }
            break;
        }
    }

    void HandleKeyEvent(WORD keyCode)
    {
        // Global toggle keys (work even if overlay is hidden)
        if (keyCode == VK_F8 || keyCode == VK_NUMPAD7 || keyCode == VK_INSERT) {
            ProcessAction(MenuAction::Toggle);
            return;
        }

        // Navigation keys (only work if overlay is visible)
        if (!IsOverlayVisible()) return;

        switch (keyCode) {
        case VK_NUMPAD8: ProcessAction(MenuAction::Up);       break;
        case VK_NUMPAD2: ProcessAction(MenuAction::Down);     break;
        case VK_NUMPAD4: ProcessAction(MenuAction::PageUp);   break;
        case VK_NUMPAD6: ProcessAction(MenuAction::PageDown); break;
        case VK_NUMPAD5: ProcessAction(MenuAction::Select);   break;
        case VK_NUMPAD3: ProcessAction(MenuAction::Next);     break;
        case VK_NUMPAD1: ProcessAction(MenuAction::Back);     break;
        }
    }

    void RenderMenu()
    {
        if (g_needsRefresh) {
            RefreshData();
        }

        static bool wasVisible = false;
        bool isVisible = IsOverlayVisible();
        
        if (isVisible && !wasVisible) {
            ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(550, 500), ImGuiCond_Appearing);
        }
        wasVisible = isVisible;

        if (!isVisible) return;

        // Apply a bit of style for a "premium" feel
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.1f, 0.3f, 0.5f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 0.7f, 1.0f));


        bool open = true;
        if (!ImGui::Begin("SAF Companion - MENU", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::End();
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
            if (!open) ToggleOverlay();
            return;
        }
        if (!open) ToggleOverlay();


        // Stepper indicator
        float winWidth = ImGui::GetWindowWidth();
        ImGui::PushStyleColor(ImGuiCol_Text, g_currentStep == Step::SelectAnimation ? ImVec4(1, 1, 0, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1));
        ImGui::Text("1. ANIMACJA");
        ImGui::PopStyleColor();
        ImGui::SameLine(winWidth * 0.4f);
        ImGui::Text(" >> ");
        ImGui::SameLine(winWidth * 0.6f);
        ImGui::PushStyleColor(ImGuiCol_Text, g_currentStep == Step::SelectActor ? ImVec4(1, 1, 0, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1));
        ImGui::Text("2. POSTAC");
        ImGui::PopStyleColor();
        
        ImGui::Separator();
        ImGui::Spacing();

        if (g_currentStep == Step::SelectAnimation) {
            ImGui::Text("WYBIERZ ANIMACJE (Klawisze: NUM 8/2, NUM 3 dalej)");
            if (ImGui::BeginChild("ListPane", ImVec2(0, -60), true)) {
                for (int i = 0; i < static_cast<int>(g_animations.size()); ++i) {
                    bool isSelected = (g_selectedAnim == i);
                    if (isSelected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
                    
                    std::string label = (isSelected ? " [X] " : " [ ] ") + g_animations[i];
                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        g_selectedAnim = i;
                    }
                    if (isSelected) {
                        ImGui::SetScrollHereY();
                        ImGui::PopStyleColor();
                    }
                }
            }
            ImGui::EndChild();
        } else {
            ImGui::Text("WYBIERZ CEL (Klawisze: NUM 8/2, NUM 3 start)");
            if (ImGui::BeginChild("ListPane", ImVec2(0, -60), true)) {
                for (int i = 0; i < static_cast<int>(g_actors.size()); ++i) {
                    bool isSelected = (g_selectedActor == i);
                    if (isSelected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
                    
                    std::string label;
                    if (i == 0) label = " [!] GRACZ";
                    else label = " [NPC] Obiekt " + std::to_string(i);

                    if (ImGui::Selectable(label.c_str(), isSelected)) {
                        g_selectedActor = i;
                    }
                    if (isSelected) {
                        ImGui::SetScrollHereY();
                        ImGui::PopStyleColor();
                    }
                }
            }
            ImGui::EndChild();
        }

        // Navigation Footer
        ImGui::Separator();
        if (g_currentStep == Step::SelectAnimation) {
            if (ImGui::Button("Dalej (NUM 3)", ImVec2(-1, 40)) && g_selectedAnim != -1) {
                ProcessAction(MenuAction::Next);
            }
        } else {
            if (ImGui::Button("START (NUM 3 / NUM 5)", ImVec2(-1, 40)) && g_selectedActor != -1) {
                ProcessAction(MenuAction::Select);
            }
            if (ImGui::Button("Powrot (NUM 1)", ImVec2(-1, 30))) {
                ProcessAction(MenuAction::Back);
            }
        }

        ImGui::End();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
    }
}
