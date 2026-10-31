#include "PCH.h"
#include "Papyrus/SAFCompanionAPI.h"
#include "MusicPlayer.h"
#include "RE/A/Array.h"
#include "RE/B/BSFixedString.h"
#include "RE/M/MemoryManager.h"
#include "RE/B/BSScriptUtil.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/V/VirtualMachine.h"
#include <Windows.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "Overlay/DebugOverlay.h"
#include "Overlay/D3D12Hook.h" 
#include "OSFUI_Bridge.h"


namespace SAFCompanion::Overlay {
    bool InitD3D12Hook();
    void ShutdownD3D12Hook();
    bool IsOverlayVisible();
    void ToggleOverlay();
}

#ifndef DLLEXPORT
#define DLLEXPORT __declspec(dllexport)
#endif

extern "C" SFSE::PluginVersionData SFSEPlugin_Version;

namespace
{
    void TryBindPapyrus(bool verbose = true)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (vm) {
            // Po wczytaniu zapisu Papyrus VM jest nowa instancja - mini-odtwarzacz
            // nie jest wtedy otwarty, wiec zerujemy tryb widocznosci.
            static RE::BSScript::Internal::VirtualMachine* lastVm = nullptr;
            if (vm != lastVm) {
                lastVm = vm;
                SAFCompanion::Papyrus::ResetMiniVisible();
                // Nowa VM = wczytany zapis. Muzyka z poprzedniej sesji nie moze
                // grac dalej (i trzymac wyciszonego dzwieku gry).
                SAFCompanion::Music::Stop();
            }
            SAFCompanion::Papyrus::Bind(vm);
            if (verbose) {
            }
        } else if (verbose) {
        }
    }

    void MessageHandler(SFSE::MessagingInterface::Message* a_msg)
    {
        if (!a_msg) return;
        if (a_msg->type == SFSE::MessagingInterface::kPostDataLoad) {
            TryBindPapyrus();
            SAFCompanion::OSFUIBridge::Init();
        }
    }

    // Process frame updates
    void OnFrame()
    {
        // Wczytanie zapisu = nowa instancja gracza. Silnik potrafi wtedy
        // przywrocic tfc/AI-driven i gracz nie rusza sie z miejsca - reset
        // robimy w pierwszej klatce po loadzie.
        static RE::PlayerCharacter* lastPlayer = nullptr;
        if (auto* player = RE::PlayerCharacter::GetSingleton(); player != lastPlayer) {
            lastPlayer = player;
            if (player) {
                SAFCompanion::Papyrus::OnGameLoaded();
            }
        }

        // Obsługa INSERT dla overlay
        static bool wasPressedInsert = false;
        bool isPressedInsert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        
        if (isPressedInsert && !wasPressedInsert) {
            SAFCompanion::Overlay::ToggleOverlay();
        }
        wasPressedInsert = isPressedInsert;

        // Konfigurowalne klawisze SAF Companion. OSF UI jest TYLKO panelem
        // konfiguracji (schema settings/saf.companion.json); jego dispatcher
        // hotkeyow nie jest uzywany, wiec klawisze dzialaja zawsze: w grze,
        // w mini-odtwarzaczu i w trybie kamery.
        static bool wasPressedAction[4] = { false };
        static bool actionsSeeded = false;
        const char* actionNames[4] = { "SAFStop", "SAFPlayer", "SAFNPCMenu", "SAFCamera" };

        if (!actionsSeeded) {
            actionsSeeded = true;
            // Klawisze trzymane w chwili startu nie maja odpalac akcji.
            for (int i = 0; i < 4; ++i) {
                const WORD vk = SAFCompanion::OSFUIBridge::GetActionVK(
                    static_cast<SAFCompanion::OSFUIBridge::ActionKey>(i));
                wasPressedAction[i] = vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
            }
        }

        for (int i = 0; i < 4; ++i) {
            const WORD vk = SAFCompanion::OSFUIBridge::GetActionVK(
                static_cast<SAFCompanion::OSFUIBridge::ActionKey>(i));
            if (vk == 0) {
                wasPressedAction[i] = false;
                continue;
            }
            const bool pressed = (GetAsyncKeyState(vk) & 0x8000) != 0;
            if (pressed && !wasPressedAction[i]) {
                // Stan dla widoku zanim Papyrus otworzy menu.
                if (i == 1) {
                    SAFCompanion::OSFUIBridge::PublishMode("player");
                    SAFCompanion::OSFUIBridge::PublishAnims();
                } else if (i == 2) {
                    SAFCompanion::OSFUIBridge::PublishMode("npc");
                    SAFCompanion::OSFUIBridge::PublishAnims();
                }
                SAFCompanion::Papyrus::SendActionEvent(actionNames[i]);
            }
            wasPressedAction[i] = pressed;
        }

        // Klawisz E (aktywacja/dialog) - latch NPC co klatke (commandTarget bywa
        // ustawiany pozniej w klatce niz nasz odczyt), event do Papyrusa na zboczu.
        // Papyrus reaguje na E TYLKO w trybie wyboru (Num7).
        SAFCompanion::Papyrus::CaptureNPCOnE();
        static bool wasPressedE = false;
        bool isPressedE = (GetAsyncKeyState(0x45) & 0x8000) != 0;
        if (isPressedE && !wasPressedE) {
            // UWAGA: brak LatchNearestNPC - traversal celi zwracal wiszące aktory,
            // a GetHandleForObject na takim srube psul stan VM (opozniony CTD w silniku).
            // Zrodla NPC: celownik / mowca dialogu / cel walki. TYLKO bezpieczne wskazniki.
            if (SAFCompanion::Papyrus::IsNPCArmed()) {
                SAFCompanion::Papyrus::NotifyEPressed();
            }
            SAFCompanion::Papyrus::SendBSInputEvent(0x45, "Activate", "E", true, 0.0f);
        }
        wasPressedE = isPressedE;

        // Dopoki uzbrojone i dialog otwarty: powtarzaj odbior co ~0.5s.
        // Papyrus dociera z opoznieniem, a slot latchuje sie w miedzyczasie -
        // pierwsze E otwiera dialog, kolejne (syntetyczne) odbiera mowce.
        static int repeatTick = 0;
        if (SAFCompanion::Papyrus::IsNPCArmed() &&
            SAFCompanion::Papyrus::HasPendingNPC()) {
            if (++repeatTick >= 30) {
                repeatTick = 0;
                SAFCompanion::Papyrus::SendBSInputEvent(0x45, "ActivateRepeat", "E", true, 0.0f);
            }
        } else {
            repeatTick = 0;
        }

        // UWAGA: auto-Tab WYLACZONY NA STALE - mowca ustawia sie tez przy powitaniach
        // BEZ menu, a Tab w zywa gre otwieral pauze ("przechodzi do glownego menu").
        // Dialog zamyka recznie gracz (Tab) albo petla Papyrus (RequestDialogueExit).
        // Zamkniecie i tak wykrywamy (mowca) i dokanczamy (DlgClosed ponizej).
        // Wygasanie slotu (stare latche, np. z powitan).
        SAFCompanion::Papyrus::TickSlotExpiry();

        // Mowca/dialog sprawdzamy tylko gdy tryb wyboru NPC jest uzbrojony -
        // poza nim zapytania do MenuTopicManager co klatke byly czystym narzutem.
        const bool armed = SAFCompanion::Papyrus::IsNPCArmed();
        const bool hasPending = SAFCompanion::Papyrus::HasPendingNPC();
        static bool wasSpeaking = false;
        bool speaking = false;
        if (armed || hasPending) {
            speaking = SAFCompanion::Papyrus::HasSpeakerNow();
            if (wasSpeaking && !speaking) {
                SAFCompanion::Papyrus::ClearWaitingDialogueClose();
                SAFCompanion::Papyrus::SendBSInputEvent(201, "DlgClosed", "DialogZamkniety", true, 0.0f);
            }
            // Ukrywanie menu dialogu (kHide przez kolejke UI) dopoki uzbrojone
            // i ktos mowi. CO KLATKE - throttle 0.5s dawal widoczne migniecie okna.
            if (armed && speaking) {
                SAFCompanion::Papyrus::HideDialogueMenu();
            }
        }
        wasSpeaking = speaking;

        // Auto-play po odbiorze NPC: ~20 klatek na otwarcie dialogu, gra gdy cicho.
        SAFCompanion::Papyrus::TickAutoPlay();

        // Rebind klawiszy z OSF UI (throttle ~2 s) - zmiana w panelu ma
        // dzialac bez restartu, nawet gdy eventy ustawien nie dojda.
        SAFCompanion::OSFUIBridge::TickBindings();

        // Windows potrafi zapamietac wyciszenie sesji gry po przerwanym
        // odtwarzaniu muzyki - po starcie zdejmujemy je, zanim ruszy muzyka.
        // Kilka prob co 2 s, bo sesja audio gry powstaje dopiero z Wwise.
        static bool audioHealDone = false;
        static ULONGLONG lastAudioHeal = 0;
        const ULONGLONG nowTick = GetTickCount64();
        if (!audioHealDone && nowTick - lastAudioHeal >= 2000) {
            lastAudioHeal = nowTick;
            audioHealDone = SAFCompanion::Music::EnsureGameAudioUnmuted();
        }

        // Po wczytaniu zapisu Papyrus VM jest NOWA instancja - bindingi natywne
        // przepadaja (tak samo SAF musi wolac RebindAfterLoad). SFSE nie wysyla
        // komunikatu o loadzie, wiec samonaprawa: rebind co ~10s.
        static int rebindTick = 0;
        if (++rebindTick >= 600) {
            rebindTick = 0;
            TryBindPapyrus(false);
            SAFCompanion::OSFUIBridge::Init();  // retry until OSF UI is up
        }
    }
}

extern "C" {
DLLEXPORT bool SFSEAPI SFSEPlugin_Query(const SFSE::QueryInterface* a_sfse, SFSE::PluginInfo* a_info)
{
    a_info->infoVersion = SFSE::PluginInfo::kVersion;
    a_info->name = SFSEPlugin_Version.pluginName;
    a_info->version = SFSEPlugin_Version.pluginVersion;
    if (a_sfse->RuntimeVersion() < SFSE::RUNTIME_LATEST) {
        return false;
    }
    return true;
}

DLLEXPORT bool SFSEAPI SFSEPlugin_Load(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    SAFCompanion::Papyrus::SetFreeCamSupported(a_sfse->RuntimeVersion() == SFSE::RUNTIME_SF_1_16_244);
    SFSE::RegisterForAPIInitEvent([] {
        TryBindPapyrus();
        SAFCompanion::OSFUIBridge::Init();
    });
    if (auto* messaging = SFSE::GetMessagingInterface()) {
        messaging->RegisterListener(MessageHandler);
    }
    if (auto* tasks = SFSE::GetTaskInterface()) {
        tasks->AddPermanentTask(OnFrame);
    }
    SAFCompanion::Papyrus::RegisterMCFCommand();
    SAFCompanion::Papyrus::StartCrosshairPollThread();

    if (SAFCompanion::Overlay::InitD3D12Hook()) {
    } else {
    }

    return true;
}
}

