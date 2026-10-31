#include "PCH.h"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <Windows.h>
#include <chrono>

namespace SAFCompanion::Overlay
{
    // ── Simple Debug Overlay State ─────────────────────────────────────
    static bool g_showDemo = false;
    static float g_testFloat = 0.0f;
    static int g_testInt = 0;
    static char g_testText[256] = "";

    void RenderDebugOverlay()
    {
        // Always render when called - visibility is checked in D3D12Hook

        ImGui::SetNextWindowSize(ImVec2(400, 300), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(50, 50), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("SAF Companion Debug Overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }

        ImGui::Text("=== YOUCANDANCE - SAF COMPANION DEBUG ===");
        ImGui::Separator();
        
        ImGui::Text("ImGui Initialized: YES");
        ImGui::Text("Overlay Visible: YES");
        
        ImGui::Separator();
        
        // Test controls
        if (ImGui::Button("Test Button")) {
            g_testInt++;
            snprintf(g_testText, sizeof(g_testText), "Button clicked %d times", g_testInt);
        }
        
        ImGui::SliderFloat("Test Float", &g_testFloat, 0.0f, 100.0f);
        ImGui::InputInt("Test Int", &g_testInt);
        ImGui::InputText("Test Text", g_testText, sizeof(g_testText));
        
        ImGui::Separator();
        
        if (ImGui::Button("Show ImGui Demo")) {
            g_showDemo = !g_showDemo;
        }
        
        if (g_showDemo) {
            ImGui::ShowDemoWindow(&g_showDemo);
        }
        
        ImGui::Separator();
        ImGui::Text("Press INSERT to toggle overlay");
        
        ImGui::End();
    }
}
