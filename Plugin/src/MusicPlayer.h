#pragma once

#include <string>
#include <vector>

namespace SAFCompanion::Music
{
    // Lista utworow z Data/Music (bez rozszerzen, posortowana).
    std::vector<std::string> ListTracks();

    // Odtwarza utwor w petli. Zatrzymuje poprzedni. True = audio wystartowalo.
    bool Play(const std::string& a_track);

    // Zatrzymuje odtwarzanie i przywraca dzwiek gry.
    void Stop();

    // Poziom glosnosci 0..1 (domyslnie 0.35).
    void SetVolume(float a_volume);

    // Jednorazowo po starcie gry zdejmuje zapamietane przez Windows wyciszenie
    // sesji audio gry (pozostawione przez przerwane odtwarzanie muzyki).
    // True = sprawa zamknieta (nie trzeba juz wolac).
    bool EnsureGameAudioUnmuted();

    bool IsPlaying();
}
