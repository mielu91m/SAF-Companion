#pragma once

#include "PCH.h"

#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace RE::BSScript
{
    class IVirtualMachine;
}

namespace SAFCompanion::Papyrus
{
    // Skanuje Data/SAF/Animations[/subDir] po plikach .fbx (rekurencyjnie).
    // Zwraca sciezki wzgledne bez rozszerzenia, np. "Control/taniec1".
    // subDir == "" -> caly folder Animations (stare zachowanie).
    inline std::vector<std::string> GetAvailableAnimations_Impl(const std::string& subDir = "")
    {
        std::vector<std::string> result;

        try {
            std::filesystem::path dir = std::filesystem::path("Data") / "SAF" / "Animations";
            if (!subDir.empty()) {
                dir /= subDir;
            }
            std::error_code ec;
            (void)ec;
            if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
                return result;
            }

            std::set<std::string> names;

            for (const auto& e : std::filesystem::recursive_directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied)) {
                if (!e.is_regular_file()) {
                    continue;
                }

                std::string ext = e.path().extension().string();
                if (ext.size() < 2) {
                    continue;
                }

                for (auto& c : ext) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }

                if (ext == ".fbx") {
                    auto rel = std::filesystem::relative(e.path(), dir);
                    rel.replace_extension("");
                    names.insert(rel.string());
                }
            }

            result.assign(names.begin(), names.end());
        } catch (...) {
        }

        return result;
    }

    // Nowy tryb Control: tylko Data/SAF/Animations/Control/*.fbx.
    inline std::vector<std::string> GetControlAnimations_Impl()
    {
        auto bare = GetAvailableAnimations_Impl("Control");
        for (auto& s : bare) {
            s = std::string("Control\\") + s;
        }
        return bare;
    }

    // --- Internal helpers (native side) ---
    void SendBSInputEvent(std::uint16_t keyCode, const char* controlName, const char* friendlyName, bool pressed, float heldTime);
    // Akcja z konfigurowalnego klawisza: SAFStop / SAFPlayer / SAFNPCMenu / SAFCamera.
    void SendActionEvent(const char* a_action);
    // Tryb mini-odtwarzacza ustawiany z Papyrusa:
    // 0 = brak, 1 = interaktywne menu, 2 = pasywny HUD kamery.
    int GetMiniVisibleMode();
    void ResetMiniVisible();
    void TickSlotExpiry();
    void HideDialogueMenu();
    void StartCrosshairPollThread();
    void NotifyEPressed();
    void LatchNearestNPC(float a_maxDist);
    bool HasPendingNPC();
    void CaptureNPCOnE();
    bool IsNPCArmed();
    void TickAutoPlay();
    bool HasSpeakerNow();
    bool IsWaitingDialogueClose();
    void ClearWaitingDialogueClose();
    bool GuardedIsDialogueMenuOpen();

    // Wolna kamera (tfc) wymaga layoutu PlayerCamera zweryfikowanego na 1.16.244.
    void SetFreeCamSupported(bool a_supported);

    // Wykryte wczytanie zapisu (main.cpp: nowa instancja gracza). Silnik
    // potrafi przywrocic tfc/AI-driven - czyscimy flagi i key-upy, a Papyrus
    // dostaje zdarzenie SAFLoadGame, zeby zdjac Game.SetPlayerAIDriven.
    void OnGameLoaded();

    // Registracja funkcji natywnych dla skryptow Papyrus.
    bool Bind(RE::BSScript::IVirtualMachine* a_vm);

    // Rejestracja komendy MCF "safc" (wymaga ModernCommandFramework.dll).
    void RegisterMCFCommand();
}
