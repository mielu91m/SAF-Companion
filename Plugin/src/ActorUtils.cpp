#include "PCH.h"
#include "ActorUtils.h"
#include "RE/A/Actor.h"
#include "RE/F/FormTypes.h"
#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESObjectCELL.h"
#include "RE/T/TESObjectREFR.h"
#include "RE/V/VirtualMachine.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace SAFCompanion
{
    // ZASADA: ForEachReference jest INLajnowana w nasz DLL, a cela bywa w trakcie
    // streamingu (wiszące refy, przebudowa tablicy) - SAMA PETLA sypie AV.
    // SEH w callbacku tego nie lapie. Wzorzec: caly przebieg pod jednym __try,
    // stan w statycznym buforze POD (zero obiektow z destruktorami w funkcjach z __try).
    namespace
    {
        struct Entry
        {
            RE::Actor* actor;
            float dist;
        };
        constexpr int kMaxFound = 128;

        Entry g_found[kMaxFound];
        int g_foundCount = 0;
        int g_countOnly = 0;
        float g_px = 0.0f;
        float g_py = 0.0f;
        float g_pz = 0.0f;
        float g_maxDistSq = 0.0f;
        RE::Actor* g_playerSkip = nullptr;

        using CellCb = RE::BSContainer::ForEachResult (*)(const RE::NiPointer<RE::TESObjectREFR>&);

        RE::BSContainer::ForEachResult CellCallback(const RE::NiPointer<RE::TESObjectREFR>& ref)
        {
            if (!ref) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            RE::Actor* actor = ref->As<RE::Actor>();
            if (!actor || actor == g_playerSkip) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            float dx = ref->GetPositionX() - g_px;
            float dy = ref->GetPositionY() - g_py;
            float dz = ref->GetPositionZ() - g_pz;
            float distSq = dx * dx + dy * dy + dz * dz;
            if (distSq > g_maxDistSq) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (g_countOnly) {
                g_foundCount += 1;
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (g_foundCount < kMaxFound) {
                g_found[g_foundCount].actor = actor;
                g_found[g_foundCount].dist = std::sqrt(distSq);
                g_foundCount += 1;
            }
            return RE::BSContainer::ForEachResult::kContinue;
        }

        static const std::function<RE::BSContainer::ForEachResult(const RE::NiPointer<RE::TESObjectREFR>&)> g_cellCb{ &CellCallback };

        // Sam przebieg BEZ __try (inlinowany BSAutoReadLock wymaga normalnego unwind).
        static void DoTraverse(RE::TESObjectCELL* cell)
        {
            cell->ForEachReference(g_cellCb);
        }

        // Caly przebieg pod SEH. AV (wiszące refy w streamowanej celi) lapie handler,
        // czesciowe wyniki w buforze POD zostaja. Zwraca false przy AV.
        static bool GuardedTraverse(RE::TESObjectCELL* cell)
        {
            __try {
                DoTraverse(cell);
                return true;
            } __except (1) {
                return false;
            }
        }
    }

    // Aktor bez uchwytu w VM nie da sie bezpiecznie zapakowac do tablicy Papyrusa.
    // (Tylko POD-lokale w srodku: wymog __try. Wywolywana na zywych aktorach z bufora.)
    static bool HasValidPapyrusHandle(RE::Actor* actor)
    {
        if (!actor) {
            return false;
        }
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();
        if (!vm) {
            return false;
        }
        const auto& handles = vm->GetObjectHandlePolicy();
        const std::uint32_t typeId = static_cast<std::uint32_t>(RE::FormType::kREFR);
        __try {
            return handles.GetHandleForObject(typeId, actor) != handles.EmptyHandle();
        } __except (1) {
            return false;
        }
    }

    // Wspolny przebieg: zwraca posortowany wektor (kopie wskaznikow z bufora POD).
    static std::vector<Entry> RunScan(float maxDistance)
    {
        std::vector<Entry> out;

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return out;
        }

        auto playerPos = player->GetPosition();
        g_px = playerPos.x;
        g_py = playerPos.y;
        g_pz = playerPos.z;
        g_maxDistSq = maxDistance * maxDistance;
        g_playerSkip = player;
        g_foundCount = 0;
        g_countOnly = 0;

        GuardedTraverse(player->parentCell);

        out.reserve(g_foundCount);
        for (int i = 0; i < g_foundCount; ++i) {
            out.push_back(g_found[i]);
        }
        std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            return a.dist < b.dist;
        });
        return out;
    }

    std::vector<RE::Actor*> GetNearbyActorsNative(float maxDistance, int maxCount)
    {
        std::vector<RE::Actor*> result;

        auto scanned = RunScan(maxDistance);

        int limit = static_cast<int>(scanned.size());
        if (maxCount > 0 && maxCount < limit) {
            limit = maxCount;
        }

        result.reserve(limit);
        for (int i = 0; i < limit; ++i) {
            if (HasValidPapyrusHandle(scanned[i].actor)) {
                result.push_back(scanned[i].actor);
            }
        }
        return result;
    }

    int CountNearbyActorsNative(float maxDistance)
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->parentCell) {
            return 0;
        }

        auto playerPos = player->GetPosition();
        g_px = playerPos.x;
        g_py = playerPos.y;
        g_pz = playerPos.z;
        g_maxDistSq = maxDistance * maxDistance;
        g_playerSkip = player;
        g_foundCount = 0;
        g_countOnly = 1;

        GuardedTraverse(player->parentCell);
        return g_foundCount;
    }
}
