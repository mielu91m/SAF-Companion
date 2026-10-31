#include "Papyrus/SAFCompanionAPI.h"
#include "ActorUtils.h"
#include "MusicPlayer.h"
#include <RE/Starfield.h>
#include "RE/A/Actor.h"
#include "RE/A/Array.h"
#include "RE/B/BSFixedString.h"
#include "RE/B/BSScriptUtil.h"
#include "RE/B/BSPointerHandle.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/P/PlayerCamera.h"
#include "RE/E/Events.h"
#include "RE/B/BSTEvent.h"
#include "RE/B/BGSObjectInstance.h"
#include "RE/M/MenuTopicManager.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/U/UI.h"
#include "RE/V/VirtualMachine.h"
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <iterator>
#include <thread>
#include <vector>
#include <fstream>
#include <chrono>
#include <iomanip>
#include "Overlay/ImGuiMenu.h"


namespace {
}

namespace SAFCompanion::Papyrus
{
    // Wyślij event BSInputEvent do skryptu SAFControlMenu (+ legacy YouCanDance, wymaga Cassiopeia)
    void SendBSInputEvent(std::uint16_t keyCode, const char* controlName, const char* friendlyName, bool pressed, float heldTime)
    {
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) return;

        const char* targets[] = {"YouCanDance", "SAFControlMenu"};
        RE::BSFixedString eventName("BSInputEvent");

        RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> argsFn(
            [keyCode, controlName, friendlyName, pressed, heldTime](RE::BSScrapArray<RE::BSScript::Variable>& a_args) {
                RE::BSScript::Variable vKeyCode;
                vKeyCode = static_cast<std::int32_t>(keyCode);
                a_args.push_back(vKeyCode);

                RE::BSScript::Variable vControlName;
                vControlName = RE::BSFixedString(controlName);
                a_args.push_back(vControlName);

                RE::BSScript::Variable vFriendlyName;
                vFriendlyName = RE::BSFixedString(friendlyName);
                a_args.push_back(vFriendlyName);

                RE::BSScript::Variable vPressed;
                vPressed = pressed;
                a_args.push_back(vPressed);

                RE::BSScript::Variable vHeldTime;
                vHeldTime = heldTime;
                a_args.push_back(vHeldTime);

                return true;
            });

        for (const char* t : targets) {
            RE::BSFixedString scriptName(t);
            vm->DispatchStaticCall(scriptName, eventName, argsFn, nullptr, 0);
        }
    }

    // Akcja z konfigurowalnego klawisza (keyCode 0 - YouCanDance ja ignoruje).
    void SendActionEvent(const char* a_action)
    {
        if (!a_action || !*a_action) {
            return;
        }
        SendBSInputEvent(0, a_action, a_action, true, 0.0f);
    }

    // Native Papyrus: String[] Function GetAvailableAnimations() native global
    // CommonLibSF BindNativeMethod accepts std::vector as return type (script_traits::is_array).
    std::vector<RE::BSFixedString> GetAvailableAnimations(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        std::vector<RE::BSFixedString> arr;

        auto strings = GetAvailableAnimations_Impl();
        arr.reserve(strings.size());
        for (auto& s : strings) {
            arr.push_back(RE::BSFixedString(s.c_str()));
        }

        return arr;
    }

    // Native Papyrus: String[] Function GetControlAnimations() native global
    // Tylko Data/SAF/Animations/Control/*.fbx (nowy tryb numeryczny).
    std::vector<RE::BSFixedString> GetControlAnimations(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        std::vector<RE::BSFixedString> arr;

        auto strings = GetControlAnimations_Impl();
        arr.reserve(strings.size());
        for (auto& s : strings) {
            arr.push_back(RE::BSFixedString(s.c_str()));
        }

        return arr;
    }

    // Native Papyrus: String[] Function ListMusicTracks() native global
    // Utwory z Data/Music (mp3/wav/m4a/wma/aac), bez rozszerzen.
    std::vector<RE::BSFixedString> ListMusicTracks(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        std::vector<RE::BSFixedString> arr;
        try {
            auto tracks = SAFCompanion::Music::ListTracks();
            arr.reserve(tracks.size());
            for (auto& t : tracks) {
                arr.push_back(RE::BSFixedString(t.c_str()));
            }
        } catch (...) {
        }
        return arr;
    }

    // Native Papyrus: Bool Function MusicPlay(String asTrack) native global
    bool MusicPlay(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate,
        RE::BSFixedString a_track)
    {
        try {
            return SAFCompanion::Music::Play(a_track.c_str());
        } catch (...) {
            return false;
        }
    }

    // Native Papyrus: Function MusicStop() native global
    void MusicStop(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        SAFCompanion::Music::Stop();
    }

    // Native Papyrus: Bool Function MusicIsPlaying() native global
    bool MusicIsPlaying(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        return SAFCompanion::Music::IsPlaying();
    }

    // Native Papyrus: Function MusicSetVolume(Float afVolume) native global
    void MusicSetVolume(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate,
        float a_volume)
    {
        SAFCompanion::Music::SetVolume(a_volume);
    }

    // Native Papyrus: Bool Function IsKeyPressed(Int keyCode) native global
    bool IsKeyPressed(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate,
        WORD a_keyCode)
    {
        return (GetAsyncKeyState(a_keyCode) & 0x8000) != 0;
    }

    // Native Papyrus: Actor[] Function GetNearbyActors(Float maxDistance, Int maxCount) native global
    std::vector<RE::Actor*> GetNearbyActors(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate,
        float a_maxDistance,
        std::int32_t a_maxCount)
    {
        auto result = SAFCompanion::GetNearbyActorsNative(a_maxDistance, a_maxCount);
        return result;
    }

    // Native Papyrus: Int Function CountNearbyActors(Float maxDistance) native global
    // Diagnostyka CTD: sam traversal, zwraca tylko liczbe (pakowanie int = bezpieczne).
    std::int32_t CountNearbyActors(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate,
        float a_maxDistance)
    {
        int count = SAFCompanion::CountNearbyActorsNative(a_maxDistance);
        return static_cast<std::int32_t>(count);
    }

    // ── Przechwyt NPC na E (styl SnuSnuField) ──────────────────────────
    // Tylko inty/boole przez granice C++/Papyrus - zero pakowania obiektow (zero CTD).
    namespace {
        std::atomic<bool> g_npcArmed{ false };
        std::atomic<std::uint32_t> g_pendingNPCFormID{ 0 };
        std::atomic<std::uint64_t> g_pendingNPCtime{ 0 };
        std::atomic<bool> g_menuProbed{ false };
        // Czekamy na zamkniecie dialogu: przeniesenie NPC W TRAKCIE dialogu
        // rozjezdza silnik (NPC laduje poza mapa). Bring+play dopiero po Tab.
        std::atomic<bool> g_waitingClose{ false };
        // Okno 500ms po E: tylko wtedy watek sondy moze latchowac (intencja gracza).
        std::atomic<std::uint64_t> g_eWindowUntil{ 0 };
        // Generacja E. Latch sprzed pierwszego E po uzbrojeniu nalezy do
        // POPRZEDNIEGO wyboru (bug: druga selekcja grala na pierwszym NPC).
        std::atomic<std::uint32_t> g_eGeneration{ 0 };
        std::atomic<std::uint32_t> g_armGeneration{ 0 };
        // Papyrus odebral NPC: nie latchuj ponownie tego samego mowcy, dopoki
        // nie uzbroimy kolejnego wyboru (stary latch nadpisywal nowy wybor).
        std::atomic<bool> g_captureDone{ false };
        std::atomic<bool> g_pollRunning{ true };
        // Tryb mini-odtwarzacza (0/1/2) ustawiany z Papyrusa.
        std::atomic<int> g_miniVisible{ 0 };
    }

    int GetMiniVisibleMode()
    {
        return g_miniVisible.load(std::memory_order_relaxed);
    }

    void ResetMiniVisible()
    {
        g_miniVisible.store(0, std::memory_order_relaxed);
    }

    // Znacznik czasu slotu (do wygasania). Wywolywac PRZY KAZDYM zapisie slotu.
    static void TouchSlot()
    {
        g_pendingNPCtime.store(GetTickCount64());
    }

    static void ExpireSlot()
    {
        std::uint32_t id = g_pendingNPCFormID.load();
        if (id != 0 && GetTickCount64() - g_pendingNPCtime.load() > 8000) {
            std::uint32_t expect = id;
            if (g_pendingNPCFormID.compare_exchange_strong(expect, 0)) {
                // slot wygasla - po cichu
            }
        }
    }

    // Publiczne tykniecie wygasania (wolane co klatke z OnFrame).
    void TickSlotExpiry()
    {
        ExpireSlot();
    }

    // Ukrycie menu dialogu przez kolejke UI (dziala W TRAKCIE dialogu, bez klawiszy).
    // Wzorzec SEH jak wyzej: silnik w zwyklej funkcji, straznik w wrapperze.
    static void QueryHideDialogue()
    {
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (!queue) {
            return;
        }
        queue->AddMessage(RE::BSFixedString("DialogueMenu"), RE::UI_MESSAGE_TYPE::kHide);
    }

    static void GuardedHideDialogue()
    {
        __try {
            QueryHideDialogue();
        } __except (1) {
        }
    }

    void HideDialogueMenu()
    {
        GuardedHideDialogue();
    }

    // Sonda celownika w oknie po E (200Hz, osobny watek): lapie nawet
    // jednoklatkowe ustawienie commandTarget/crosshairRef przy aktywacji.
    // Sam int do slotu - Papyrus odbiera przez TakePendingNPC.
    static bool QueryEWindowTarget(std::uint32_t& a_idOut, int& a_srcOut)
    {
        a_idOut = 0;
        a_srcOut = -1;
        if (!g_npcArmed.load()) {
            return false;
        }
        if (GetTickCount64() >= g_eWindowUntil.load()) {
            return false;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return false;
        }
        __try {
            auto* r90 = *reinterpret_cast<RE::TESObjectREFR**>(reinterpret_cast<char*>(player) + 0x0F90);
            RE::Actor* a90 = r90 ? r90->As<RE::Actor>() : nullptr;
            if (a90 && a90 != player) {
                std::uint32_t id = a90->GetFormID();
                if (id != 0) {
                    a_idOut = id;
                    a_srcOut = 0;
                    return true;
                }
            }
            auto* r30 = *reinterpret_cast<RE::TESObjectREFR**>(reinterpret_cast<char*>(player) + 0x0F30);
            RE::Actor* a30 = r30 ? r30->As<RE::Actor>() : nullptr;
            if (a30 && a30 != player) {
                std::uint32_t id = a30->GetFormID();
                if (id != 0) {
                    a_idOut = id;
                    a_srcOut = 1;
                    return true;
                }
            }
        } __except (1) {
            return false;
        }
        return false;
    }

    static void CrosshairPollLoop()
    {
        while (g_pollRunning.load()) {
            if (g_npcArmed.load(std::memory_order_relaxed)) {
                std::uint32_t id = 0;
                int src = -1;
                if (QueryEWindowTarget(id, src) && id != 0) {
                    g_pendingNPCFormID.exchange(id);
                    TouchSlot();
                }
            }
            // 200 Hz tylko w oknie 500 ms po E; poza nim 20 Hz (mniej pobudek watku).
            const bool inWindow =
                GetTickCount64() < g_eWindowUntil.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(inWindow ? 5 : 50));
        }
    }

    void StartCrosshairPollThread()
    {
        static std::atomic<bool> started{ false };
        if (started.exchange(true)) {
            return;
        }
        std::thread(&CrosshairPollLoop).detach();
    }

    // Otwarcie okna 500ms po E (wolane z OnFrame na zboczu E).
    void NotifyEPressed()
    {
        g_eGeneration.fetch_add(1, std::memory_order_relaxed);
        g_eWindowUntil.store(GetTickCount64() + 500);
    }

    // Przechwyt najbliższego NPC na zboczu E (interakcja BEZ dialogu).
    // Uzywa naprawionego traversal (GuardedTraverse) i zwraca TYLKO FormID (int).
    void LatchNearestNPC(float a_maxDist)
    {
        if (!g_npcArmed.load()) {
            return;
        }
        auto nearby = SAFCompanion::GetNearbyActorsNative(a_maxDist, 1);
        if (nearby.empty() || !nearby[0]) {
            return;
        }
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || nearby[0] == player) {
            return;
        }
        std::uint32_t id = nearby[0]->GetFormID();
        if (id == 0) {
            return;
        }
        std::uint32_t prev = g_pendingNPCFormID.exchange(id);
        TouchSlot();
        (void)prev;
    }

    // ZASADA SEH w tym pliku: wywolan SILNIKOWYCH (inlinowane statyczne
    // Relocation) NIE WOLNO wkladac do __try (C2712). Wzorzec: zwykla funkcja
    // robi robote, chudy wrapper z __try ja wywoluje - AV wedruje do handlera.
    static bool QuerySpeakerFormID(std::uint32_t& a_idOut)
    {
        a_idOut = 0;
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) {
            return false;
        }
        RE::BSPointerHandle<RE::TESObjectREFR> h = mtm->speaker;
        if (!h) {
            return false;
        }
        RE::NiPointer<RE::TESObjectREFR> ptr = h.get();
        RE::Actor* actor = ptr ? ptr->As<RE::Actor>() : nullptr;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!actor || actor == player) {
            return false;
        }
        std::uint32_t id = actor->GetFormID();
        if (id == 0) {
            return false;
        }
        a_idOut = id;
        return true;
    }

    static bool GuardedQuerySpeaker(std::uint32_t& a_idOut)
    {
        __try {
            return QuerySpeakerFormID(a_idOut);
        } __except (1) {
            a_idOut = 0;
            return false;
        }
    }

    struct ProbeRes
    {
        const char* name;
        bool reg;
        bool open;
    };

    // Native Papyrus: Function ArmNPCOn() / ArmNPCDisarm() native global
    // (nullary void - jedyny ksztalt void jaki BindNativeMethod wspiera, por. ToggleOverlay)
    void NPCArm(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        // Nowy wybor zaczyna od czystego slotu - stary NPC nie moze wygrac.
        g_pendingNPCFormID.store(0);
        g_pendingNPCtime.store(0);
        g_captureDone.store(false, std::memory_order_relaxed);
        g_armGeneration.store(g_eGeneration.load(std::memory_order_relaxed));
        g_npcArmed.store(true);
    }

    void NPCDisarm(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        g_npcArmed.store(false);
        g_pendingNPCFormID.store(0);
        g_captureDone.store(false, std::memory_order_relaxed);
    }

    // Native Papyrus: Int Function TakePendingNPC() native global
    // Zwraca pelny FormID zlapaneNPC (0 = brak), CZYSCI slot. Papyrus robi Game.GetForm(id).
    std::int32_t TakePendingNPC(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        std::uint32_t id = g_pendingNPCFormID.exchange(0);
        if (id != 0) {
            g_captureDone.store(true, std::memory_order_relaxed);
        }
        return static_cast<std::int32_t>(id);
    }

    // Czy slot trzyma nieprzeczytanego NPC (do bramki auto-Tab w OnFrame).
    bool HasPendingNPC()
    {
        return g_pendingNPCFormID.load() != 0;
    }

    // (sonda offsetow 0F30/0F90 usunieta - oba martwe w 1.16.244, log to potwierdzil)

    // Wolane CO KLATKE z OnFrame. Latchujemy WYLACZNIE mowce dialogu:
    // commandTarget po zamknietym dialogu trzyma ostatni cel i nadpisywal
    // nowego NPC (objaw: druga selekcja grala na pierwszym aktorze).
    // Przed pierwszym E po uzbrojeniu nie latchujemy nic - stary uchwyt
    // mowcy mogl jeszcze wskazywac poprzedniego NPC.
    void CaptureNPCOnE()
    {
        if (!g_npcArmed.load()) {
            return;
        }
        if (g_eGeneration.load(std::memory_order_relaxed) ==
            g_armGeneration.load(std::memory_order_relaxed)) {
            return;
        }
        if (g_captureDone.load(std::memory_order_relaxed)) {
            return;
        }

        // MOWCA DIALOGU (plan C): dialog otwarty = speaker ustawiony przez dlugi czas,
        // brak wrazliwosci na timing klatki. Uchwyt pusty poza dialogiem -> pomijamy.
        std::uint32_t speakerID = 0;
        if (GuardedQuerySpeaker(speakerID) && speakerID != 0) {
            g_pendingNPCFormID.exchange(speakerID);
            TouchSlot();
        }
    }

    // Czy tryb wyboru uzbrojony (do logiki powtorzen w OnFrame).
    bool IsNPCArmed()
    {
        return g_npcArmed.load();
    }

    // Tykanie auto-play (wolane co klatke z OnFrame): po odbiorze slotu przez
    // Papyrus (Take = adopt) strzelamy zdarzeniem 202: raz po ~20 klatkach,
    // potem co 60 klatek DOPOKI Papyrus nie potwierdzi (AckPlayed).
    // Bez bramki na mowce: przy BringNPC=False gra w miejscu jest bezpieczna
    // nawet w trakcie dialogu; przypadek BringNPC filtruje sam Papyrus.
    static bool s_waitPlay = false;
    static int s_waitFrames = 0;
    static int s_repeatTick = 0;

    void TickAutoPlay()
    {
        std::uint32_t cur = g_pendingNPCFormID.load();
        static std::uint32_t lastSlot = 0;
        if (lastSlot != 0 && cur == 0) {
            s_waitPlay = true;
            s_waitFrames = 0;
            s_repeatTick = 0;
        }
        lastSlot = cur;

        if (!s_waitPlay) {
            return;
        }
        ++s_waitFrames;
        if (s_waitFrames == 20) {
            SAFCompanion::Papyrus::SendBSInputEvent(202, "AutoPlay", "AutoPlay", true, 0.0f);
            return;
        }
        if (s_waitFrames > 20 && ++s_repeatTick >= 60) {
            s_repeatTick = 0;
            SAFCompanion::Papyrus::SendBSInputEvent(202, "AutoPlay", "AutoPlay", true, 0.0f);
        }
    }

    // Potwierdzenie Papyrusa: zagrane/rezygnacja - stop powtorek.
    void AckPlayed(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        s_waitPlay = false;
    }
    // Czy ktos aktualnie mowi (dialog otwarty). Ta sama sciezka co dzialajacy
    // latch mowcy - udowodnione bezpieczna wieloma sesjami (zero crashy).
    bool HasSpeakerNow()
    {
        auto* mtm = RE::MenuTopicManager::GetSingleton();
        if (!mtm) {
            return false;
        }
        RE::BSPointerHandle<RE::TESObjectREFR> h = mtm->speaker;
        if (!h) {
            return false;
        }
        RE::NiPointer<RE::TESObjectREFR> ptr = h.get();
        return ptr != nullptr;
    }

    // Flaga oczekiwania na zamkniecie dialogu (ustawia TakePendingNPC, kasuje one-shot).
    bool IsWaitingDialogueClose()
    {
        return g_waitingClose.load();
    }

    void ClearWaitingDialogueClose()
    {
        g_waitingClose.store(false);
    }

    static bool QueryDialogueMenuOpen()
    {
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            return false;
        }
        return ui->IsMenuOpen("DialogueMenu");
    }

    // Czy menu dialogu otwarte (straznik SEH - nieznany REL-ID nie moze polożyć gry).
    bool GuardedIsDialogueMenuOpen()
    {
        __try {
            return QueryDialogueMenuOpen();
        } __except (1) {
            return false;
        }
    }

    // Native Papyrus: Function ToggleOverlay() native global
    void ToggleOverlay(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        SAFCompanion::Overlay::ToggleOverlay();
    }

    // Native Papyrus: Function SetMiniVisible(Int aiMode) native global
    void SetMiniVisible(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate,
        std::int32_t a_mode)
    {
        if (a_mode < 0) {
            a_mode = 0;
        } else if (a_mode > 2) {
            a_mode = 2;
        }
        g_miniVisible.store(a_mode, std::memory_order_relaxed);
    }

    // ── Natywna wolna kamera (tfc) ────────────────────────────────────
    // ToggleFreeCameraMode ustawia kFreeWalk (13) + flagi renderera/inputu,
    // dzieki czemu WASD przesuwa kamere, a gracz stoi (AI-driven).
    // Offsety flag zweryfikowane na 1.16.244 (ten sam layout uzywa
    // osf-animation); na innej wersji gry wchodzimy tylko przez REL-ID,
    // wiec gate po wersji chroni przed pisaniem w nieznana pamiec.
    namespace {
        std::atomic<bool> g_freeCamActive{ false };
        std::atomic<bool> g_freeCamSupported{ false };
        bool g_freeCamPriorFirstPerson = false;

        constexpr std::uint32_t kFreeWalkStateIndex = 13;  // RE::CameraState::kFreeWalk
        constexpr std::ptrdiff_t kFreeCamGateOffset = 0x2DD;     // free-cam aktywna
        constexpr std::ptrdiff_t kFreeCamEnteredOffset = 0x2E5;  // weszlismy w tfc

        // ThirdPersonState (layout zweryfikowany na 1.16.244, ten sam co osf-animation).
        constexpr std::ptrdiff_t kThirdPersonStateIdOffset = 0x50;    // musi == 20 (kThirdPerson)
        constexpr std::ptrdiff_t kThirdPersonTargetZoomOffset = 0x224;
        constexpr std::ptrdiff_t kThirdPersonCurrentZoomOffset = 0x228;
        constexpr std::uint32_t kThirdPersonStateId = 20;
        constexpr float kThirdPersonZoomSeed = 1.0f;  // domyslna odleglosc 3. osoby

        // ForceThirdPerson zeruje zoom, a zoom 0.0 natychmiast wraca do 1. osoby.
        // Piszemy TARGET i CURRENT, zeby kamera nie "uciekala" do tylu po wyjsciu z tfc.
        void SeedThirdPersonZoom()
        {
            auto* camera = RE::PlayerCamera::GetSingleton();
            if (!camera) {
                return;
            }
            void* state = camera->cameraStates[RE::CameraState::kThirdPerson];
            if (!state) {
                return;
            }
            auto* base = reinterpret_cast<std::byte*>(state);
            if (*reinterpret_cast<std::uint32_t*>(base + kThirdPersonStateIdOffset) != kThirdPersonStateId) {
                return;
            }
            *reinterpret_cast<float*>(base + kThirdPersonTargetZoomOffset) = kThirdPersonZoomSeed;
            *reinterpret_cast<float*>(base + kThirdPersonCurrentZoomOffset) = kThirdPersonZoomSeed;
        }

        // tfc potrafi zostawic wciśniete WASD/Shift. Po wyjsciu gracz "biegnie"
        // do tylu, dopoki klawisz nie zostanie fizycznie puszczony - wysylamy key-up.
        // Ten sam problem robi pauza menu OSF UI: klawisz puszczony w trakcie
        // pauzy nie dociera do silnika, wiec po powrocie gracz biegnie dalej.
        void ReleaseLatchedMovementKeys()
        {
            constexpr WORD kVks[] = { 0x57, 0x41, 0x53, 0x44, 0x10, 0x11, 0x20 };  // W A S D Shift Ctrl Space
            INPUT inputs[std::size(kVks)]{};
            for (std::size_t i = 0; i < std::size(kVks); ++i) {
                inputs[i].type = INPUT_KEYBOARD;
                inputs[i].ki.wVk = kVks[i];
                inputs[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(kVks[i], MAPVK_VK_TO_VSC));
                inputs[i].ki.dwFlags = KEYEVENTF_KEYUP;
            }
            SendInput(static_cast<UINT>(std::size(kVks)), inputs, sizeof(INPUT));
        }
    }

    // Native Papyrus: Function ReleaseMovementKeys() native global
    // Dosyla key-up W/A/S/D/Shift/Ctrl/Space - czysci ruch "zapamietany"
    // przez pauze menu albo tfc.
    void ReleaseMovementKeys(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        ReleaseLatchedMovementKeys();
    }

    void SetFreeCamSupported(bool a_supported)
    {
        g_freeCamSupported.store(a_supported, std::memory_order_relaxed);
    }

    // Native Papyrus: Bool Function FreeCamEnter() native global
    // False = brak wsparcia (inna wersja gry) - skrypt wlacza fallback.
    bool FreeCamEnter(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        if (g_freeCamActive.load(std::memory_order_relaxed)) {
            return true;
        }
        if (!g_freeCamSupported.load(std::memory_order_relaxed)) {
            return false;
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera) {
            return false;
        }
        auto* base = reinterpret_cast<std::byte*>(camera);
        auto* entered = reinterpret_cast<std::uint8_t*>(base + kFreeCamEnteredOffset);
        if (*entered == 0) {
            // tfc przejmuje WASD: klawisz trzymany w chwili wejscia zostaje
            // "wcisniety" w silniku i po wyjsciu gracz biegnie sam.
            ReleaseLatchedMovementKeys();
            g_freeCamPriorFirstPerson = camera->IsInFirstPerson();
            camera->ToggleFreeCameraMode(kFreeWalkStateIndex, false);
        }
        if (*entered == 0) {
            return false;
        }
        g_freeCamActive.store(true, std::memory_order_relaxed);
        return true;
    }

    // Native Papyrus: Bool Function FreeCamExit() native global
    // True = tfc bylo aktywne i zostalo wylaczone; Papyrus musi wtedy zdjac
    // Game.SetPlayerAIDriven(False) - tfc ustawia te flage, a jej wylaczenie
    // samo w sobie jej nie cofa (gracz zostalby unieruchomiony).
    bool FreeCamExit(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        const bool wasActive = g_freeCamActive.exchange(false, std::memory_order_relaxed);
        if (!g_freeCamSupported.load(std::memory_order_relaxed)) {
            return false;
        }
        auto* camera = RE::PlayerCamera::GetSingleton();
        if (!camera) {
            return false;
        }
        auto* base = reinterpret_cast<std::byte*>(camera);
        auto* entered = reinterpret_cast<std::uint8_t*>(base + kFreeCamEnteredOffset);
        // Po wczytaniu zapisu silnik sam przywraca tfc: nasza flaga jest wtedy
        // false, ale gracz siedzi w wolnej kamerze i nie chodzi. Wyjscie
        // opieramy na stanie silnika, nie tylko na wlasnym.
        if (!wasActive && *entered == 0) {
            return false;
        }
        *reinterpret_cast<std::uint8_t*>(base + kFreeCamGateOffset) = 0;
        *entered = 0;
        // Same flagi nie przywracaja kamery - silnik zostaje w kFreeWalk,
        // wiec jawnie wracamy do POV sprzed wejscia w tfc.
        if (g_freeCamPriorFirstPerson) {
            camera->ForceFirstPerson();
        } else if (!camera->IsInThirdPerson()) {
            camera->ForceThirdPerson();
            SeedThirdPersonZoom();
            // ForceThirdPerson zeruje zoom dopiero w kolejnej klatce - dosiewamy
            // po niej, inaczej kamera cofa sie do gracza z predkoscia lerpu.
            if (auto* tasks = SFSE::GetTaskInterface()) {
                tasks->AddTask([]() { SeedThirdPersonZoom(); });
            }
        }
        ReleaseLatchedMovementKeys();
        return true;
    }

    // Native Papyrus: Function ResetPlayerMotion() native global
    // SAF zaklada graczowi kHeadingFixed przy LockActorForAnimation, ale swoja
    // petla zdejmuje te flage tylko dla FormID != 0x14 - gracz zostaje
    // z zablokowanym kierunkiem ("biegnie do tylu" do saf stop). Czyscimy
    // wszystkie trzy flagi natywnie, bez ruszania grafu animacji.
    void ResetPlayerMotion(
        RE::BSScript::IVirtualMachine& /*a_vm*/,
        std::uint32_t /*a_stackID*/,
        std::monostate)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }
        player->boolBits.reset(RE::Actor::BOOL_BITS::kHeadingFixed);
        player->boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);
        player->boolFlags2.reset(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
        ReleaseLatchedMovementKeys();
    }

    // Po kazdym wczytaniu zapisu (pociag, smierc, reczny load) silnik potrafi
    // przywrocic tfc i AI-driven - gracz nie rusza sie z miejsca, dopoki ktos
    // tego nie wyzeruje. Flagami i klawiszami zajmujemy sie tutaj, kamere
    // i SetPlayerAIDriven domyka Papyrus (SAFLoadGame -> ResetAfterLoad).
    void OnGameLoaded()
    {
        ReleaseLatchedMovementKeys();
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (player) {
            player->boolBits.reset(RE::Actor::BOOL_BITS::kHeadingFixed);
            player->boolFlags.reset(RE::Actor::BOOL_FLAGS::kMovementBlocked);
            player->boolFlags2.reset(RE::Actor::BOOL_FLAGS2::kAnimationDriven);
        }
        SendActionEvent("SAFLoadGame");
    }

    bool Bind(RE::BSScript::IVirtualMachine* a_vm)
    {
        if (!a_vm) {
            return false;
        }

        constexpr auto scriptNameSAFCompanion = "SAFCompanion";

        try {
            // Rejestracja dla SAFCompanion
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "GetAvailableAnimations",
                GetAvailableAnimations,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "GetControlAnimations",
                GetControlAnimations,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "ListMusicTracks",
                ListMusicTracks,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "MusicPlay",
                MusicPlay,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "MusicStop",
                MusicStop,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "MusicIsPlaying",
                MusicIsPlaying,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "MusicSetVolume",
                MusicSetVolume,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "IsKeyPressed",
                IsKeyPressed,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "GetNearbyActors",
                GetNearbyActors,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "CountNearbyActors",
                CountNearbyActors,
                std::nullopt,
                false);
            // TEMP-DIAG-OFF
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "ArmNPCOn",
                NPCArm,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "ArmNPCDisarm",
                NPCDisarm,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "TakePendingNPC",
                TakePendingNPC,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "AckPlayed",
                AckPlayed,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "ToggleOverlay",
                ToggleOverlay,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "SetMiniVisible",
                SetMiniVisible,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "FreeCamEnter",
                FreeCamEnter,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "FreeCamExit",
                FreeCamExit,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "ResetPlayerMotion",
                ResetPlayerMotion,
                std::nullopt,
                false);
            a_vm->BindNativeMethod(
                scriptNameSAFCompanion,
                "ReleaseMovementKeys",
                ReleaseMovementKeys,
                std::nullopt,
                false);

            return true;
        } catch (const std::exception&) {
            return false;
        } catch (...) {
            return false;
        }
    }
}

