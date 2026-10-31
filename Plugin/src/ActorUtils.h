#pragma once

#include <vector>

namespace RE {
    class Actor;
}

namespace SAFCompanion
{
    std::vector<RE::Actor*> GetNearbyActorsNative(float maxDistance, int maxCount);

    // Diagnostyka: sam traversal + liczenie, BEZ zwracania obiektow do Papyrusa.
    // Rozstrzyga czy CTD lezy w traversal (pada tu) czy w pakowaniu Actor[] (nie pada tu).
    int CountNearbyActorsNative(float maxDistance);
}
