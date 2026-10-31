#include "PCH.h"
#include "MusicPlayer.h"

#include <Windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr REFERENCE_TIME kBufferDuration = 3000000;  // 300 ms

    // Osobny GUID sesji: muzyka NIE dzieli sesji z Wwise/gra, wiec wyciszenie
    // sesji gry (wyciszanie dzwiekow podczas tancow) nie tlumi odtwarzacza.
    const GUID kMusicSessionGuid = {
        0xb7c0e1a2, 0x3f4d, 0x4e5f, { 0x9a, 0x6b, 0x1c, 0x2d, 0x3e, 0x4f, 0x5a, 0x6b }
    };

    std::mutex g_mutex;
    std::thread g_worker;
    std::atomic<bool> g_stopRequested{ false };
    std::atomic<bool> g_playing{ false };
    std::atomic<float> g_volume{ 0.35f };
    std::atomic<bool> g_audioHealDone{ false };
    std::string g_track;

    void ApplyGain(BYTE* a_dst, UINT32 a_bytes, const WAVEFORMATEX& a_fmt)
    {
        const float gain = g_volume.load(std::memory_order_relaxed);
        if (!a_dst || a_bytes == 0 || gain >= 1.0f) {
            return;
        }
        if (gain <= 0.0f) {
            std::memset(a_dst, 0, a_bytes);
            return;
        }
        if (a_fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT && a_fmt.wBitsPerSample == 32) {
            auto* samples = reinterpret_cast<float*>(a_dst);
            const size_t count = a_bytes / sizeof(float);
            for (size_t i = 0; i < count; ++i) {
                samples[i] *= gain;
            }
        } else if (a_fmt.wBitsPerSample == 16) {
            auto* samples = reinterpret_cast<std::int16_t*>(a_dst);
            const size_t count = a_bytes / sizeof(std::int16_t);
            for (size_t i = 0; i < count; ++i) {
                samples[i] = static_cast<std::int16_t>(samples[i] * gain);
            }
        }
    }

    // Domkniecie watku przy wyladowaniu DLL: bez tego joinable std::thread
    // w statycznym destruktorze wolalby std::terminate (crash przy wyjsciu z gry).
    struct ShutdownGuard
    {
        ~ShutdownGuard()
        {
            g_stopRequested.store(true);
            if (g_worker.joinable()) {
                try {
                    g_worker.join();
                } catch (...) {
                }
            }
        }
    };
    ShutdownGuard g_shutdownGuard;

    std::filesystem::path MusicDir()
    {
        static const std::filesystem::path dir = [] {
            wchar_t buf[MAX_PATH]{};
            HMODULE mod = nullptr;
            if (::GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&MusicDir),
                    &mod) &&
                mod && ::GetModuleFileNameW(mod, buf, MAX_PATH) > 0) {
                auto p = std::filesystem::path(buf);
                p = p.parent_path();  // Plugins
                p = p.parent_path();  // SFSE
                p = p.parent_path();  // Data
                return p / "Music";
            }
            return std::filesystem::path(L"Data") / L"Music";
        }();
        return dir;
    }

    struct TrackFile
    {
        std::string name;
        std::filesystem::path path;
    };

    // Strumien PCM z SourceReadera + bufor resztek miedzy probkami.
    struct Decoder
    {
        IMFSourceReader* reader = nullptr;
        WAVEFORMATEX fmt{};
        bool exactMix = false;
        std::vector<BYTE> pending;
        size_t pos = 0;

        size_t Available() const
        {
            return pending.size() - pos;
        }

        void Compact()
        {
            if (pos > 0) {
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pos));
                pos = 0;
            }
        }

        bool Rewind()
        {
            if (!reader) {
                return false;
            }
            PROPVARIANT var;
            PropVariantInit(&var);
            var.vt = VT_I8;
            var.hVal.QuadPart = 0;
            const HRESULT hr = reader->SetCurrentPosition(GUID_NULL, var);
            PropVariantClear(&var);
            // `pending` zostaje celowo: ogon utworu jest dogrywany przed
            // poczatkiem nastepnej petli. Czyszczenie go ucinalo koncowke
            // i dawalo trzask na sklejeniu (co ~3 min).
            return SUCCEEDED(hr);
        }

        bool Ensure(size_t a_want)
        {
            if (!reader) {
                return false;
            }
            int starved = 0;
            while (Available() < a_want) {
                DWORD flags = 0;
                LONGLONG ts = 0;
                IMFSample* sample = nullptr;
                const HRESULT hr = reader->ReadSample(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &sample);
                if (FAILED(hr)) {
                    if (sample) {
                        sample->Release();
                    }
                    return false;
                }

                bool appended = false;
                if (sample) {
                    IMFMediaBuffer* buffer = nullptr;
                    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && buffer) {
                        BYTE* data = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(buffer->Lock(&data, &len, nullptr))) {
                            Compact();
                            if (len > 0) {
                                pending.insert(pending.end(), data, data + len);
                                appended = true;
                            }
                            buffer->Unlock();
                        }
                        buffer->Release();
                    }
                    sample->Release();
                }

                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                    if (!Rewind()) {
                        return false;
                    }
                    if (++starved > 3 && !appended) {
                        return false;
                    }
                } else if (appended) {
                    starved = 0;
                }
            }
            return true;
        }

        void Fill(BYTE* a_dst, UINT32 a_bytes)
        {
            UINT32 done = 0;
            const UINT32 align = fmt.nBlockAlign ? fmt.nBlockAlign : 4;
            while (done < a_bytes) {
                const size_t want = (std::min)(static_cast<size_t>(8192), static_cast<size_t>(a_bytes - done));
                if (!Ensure(want)) {
                    std::memset(a_dst + done, 0, a_bytes - done);
                    return;
                }
                const size_t take = (std::min)(Available(), static_cast<size_t>(a_bytes - done)) / align * align;
                if (take == 0) {
                    if (!Ensure(align)) {
                        std::memset(a_dst + done, 0, a_bytes - done);
                        return;
                    }
                    continue;
                }
                std::memcpy(a_dst + done, pending.data() + pos, take);
                pos += take;
                done += static_cast<UINT32>(take);
            }
        }
    };

    std::vector<TrackFile> ScanTracks()
    {
        std::vector<TrackFile> out;
        try {
            std::error_code ec;
            const auto dir = MusicDir();
            if (!std::filesystem::is_directory(dir, ec)) {
                return out;
            }
            for (const auto& entry : std::filesystem::directory_iterator(
                     dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
                if (ec) {
                    break;
                }
                std::error_code fec;
                if (!entry.is_regular_file(fec)) {
                    continue;
                }
                const auto& p = entry.path();
                std::string ext = p.extension().string();
                for (auto& c : ext) {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (ext != ".mp3" && ext != ".wav" && ext != ".m4a" && ext != ".wma" && ext != ".aac") {
                    continue;
                }
                out.push_back({ p.stem().string(), p });
            }
        } catch (...) {
        }

        std::sort(out.begin(), out.end(), [](const TrackFile& a, const TrackFile& b) {
            return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        out.erase(std::unique(out.begin(), out.end(), [](const TrackFile& a, const TrackFile& b) {
            return _stricmp(a.name.c_str(), b.name.c_str()) == 0;
        }), out.end());
        return out;
    }

    std::string NormalizeTrackName(std::string a_name)
    {
        std::filesystem::path p(a_name);
        a_name = p.stem().string();
        for (auto& c : a_name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (c == ' ') {
                c = '_';
            }
        }
        return a_name;
    }

    bool ResolveTrack(const std::string& a_name, std::filesystem::path& a_out)
    {
        const std::string want = NormalizeTrackName(a_name);
        if (want.empty()) {
            return false;
        }
        for (const auto& t : ScanTracks()) {
            if (NormalizeTrackName(t.name) == want) {
                a_out = t.path;
                return true;
            }
        }
        return false;
    }

    std::wstring SessionIdentifier(IAudioSessionControl* a_ctl)
    {
        if (!a_ctl) {
            return {};
        }
        IAudioSessionControl2* ctl2 = nullptr;
        if (FAILED(a_ctl->QueryInterface(IID_PPV_ARGS(&ctl2))) || !ctl2) {
            return {};
        }
        LPWSTR sid = nullptr;
        std::wstring out;
        if (SUCCEEDED(ctl2->GetSessionIdentifier(&sid)) && sid) {
            out.assign(sid);
            ::CoTaskMemFree(sid);
        }
        ctl2->Release();
        return out;
    }

    std::wstring UpperWide(std::wstring a_s)
    {
        for (auto& c : a_s) {
            c = static_cast<wchar_t>(std::towupper(c));
        }
        return a_s;
    }

    std::wstring OurSessionGuidString()
    {
        wchar_t buf[64]{};
        ::StringFromGUID2(kMusicSessionGuid, buf, 64);
        return UpperWide(buf);
    }

    template <typename Fn>
    void EnumerateSessions(IAudioSessionManager2* a_mgr, Fn&& a_fn)
    {
        IAudioSessionEnumerator* sessions = nullptr;
        if (!a_mgr || FAILED(a_mgr->GetSessionEnumerator(&sessions)) || !sessions) {
            return;
        }
        int count = 0;
        if (SUCCEEDED(sessions->GetCount(&count))) {
            for (int i = 0; i < count; ++i) {
                IAudioSessionControl* ctl = nullptr;
                if (FAILED(sessions->GetSession(i, &ctl)) || !ctl) {
                    continue;
                }
                DWORD pid = 0;
                IAudioSessionControl2* ctl2 = nullptr;
                if (SUCCEEDED(ctl->QueryInterface(IID_PPV_ARGS(&ctl2))) && ctl2) {
                    ctl2->GetProcessId(&pid);
                    ctl2->Release();
                }
                a_fn(ctl, pid);
                ctl->Release();
            }
        }
        sessions->Release();
    }

    // Wycisza sesje audio TEGO procesu (gra/Wwise). Muzyka ma osobny GUID sesji,
    // wiec zostaje slyszalna. a_known zbiera ID wszystkich sesji gry (takze
    // naszej), dzieki czemu FindOurSession rozpoznaje nasza po roznicy zbiorow.
    void MuteGameSessions(
        IAudioSessionManager2* a_mgr,
        DWORD a_pid,
        const std::wstring& a_ourId,
        std::set<std::wstring>& a_known)
    {
        if (!a_mgr) {
            return;
        }
        const std::wstring ourGuid = OurSessionGuidString();
        EnumerateSessions(a_mgr, [&](IAudioSessionControl* a_ctl, DWORD a_sessionPid) {
            if (a_sessionPid != a_pid) {
                return;
            }
            const std::wstring id = SessionIdentifier(a_ctl);
            if (id.empty()) {
                return;
            }
            a_known.insert(id);
            if (!a_ourId.empty() && id == a_ourId) {
                return;
            }
            if (UpperWide(id).find(ourGuid) != std::wstring::npos) {
                return;
            }
            ISimpleAudioVolume* vol = nullptr;
            if (FAILED(a_ctl->QueryInterface(IID_PPV_ARGS(&vol))) || !vol) {
                return;
            }
            BOOL muted = FALSE;
            if (SUCCEEDED(vol->GetMute(&muted)) && !muted) {
                vol->SetMute(TRUE, nullptr);
            }
            vol->Release();
        });
    }

    // Przywraca dzwiek gry: zdejmuje wyciszenie ze WSZYSTKICH sesji tego procesu
    // poza nasza sesja muzyki. Windows potrafi zapamietac wyciszenie per aplikacja,
    // wiec nie polegamy tylko na ID zapisanych w chwili wyciszania.
    void RestoreGameSessions(IAudioSessionManager2* a_mgr)
    {
        if (!a_mgr) {
            return;
        }
        const DWORD pid = ::GetCurrentProcessId();
        const std::wstring ourGuid = OurSessionGuidString();
        EnumerateSessions(a_mgr, [&](IAudioSessionControl* a_ctl, DWORD a_sessionPid) {
            if (a_sessionPid != pid) {
                return;
            }
            const std::wstring id = SessionIdentifier(a_ctl);
            if (!id.empty() && UpperWide(id).find(ourGuid) != std::wstring::npos) {
                return;
            }
            ISimpleAudioVolume* vol = nullptr;
            if (SUCCEEDED(a_ctl->QueryInterface(IID_PPV_ARGS(&vol))) && vol) {
                BOOL muted = FALSE;
                if (SUCCEEDED(vol->GetMute(&muted)) && muted) {
                    vol->SetMute(FALSE, nullptr);
                }
                vol->Release();
            }
        });
    }

    // Po restarcie gry Windows moze przywrocic zapamietane wyciszenie sesji
    // (np. gra zamknieta w trakcie odtwarzania muzyki). Po starcie zdejmujemy
    // mute z sesji gry - zanim ruszy jakakolwiek muzyka. Kilka prob, bo sesja
    // audio gry powstaje dopiero po inicjalizacji Wwise.
    bool HealGameAudio()
    {
        if (g_audioHealDone.load(std::memory_order_relaxed)) {
            return true;
        }
        if (g_playing.load(std::memory_order_relaxed) || g_worker.joinable()) {
            g_audioHealDone.store(true, std::memory_order_relaxed);
            return true;
        }
        // Limit prob: brak sesji gry (inne urzadzenie audio) nie moze zamienic
        // tej diagnozy w wieczna enumeracje COM co 2 s.
        static int attempts = 0;
        if (++attempts > 30) {
            g_audioHealDone.store(true, std::memory_order_relaxed);
            return true;
        }

        const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInit = SUCCEEDED(comHr);

        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        IAudioSessionManager2* mgr = nullptr;
        bool sawGameSession = false;

        if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) &&
            enumerator &&
            SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) && device &&
            SUCCEEDED(device->Activate(
                __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) &&
            mgr) {
            const DWORD pid = ::GetCurrentProcessId();
            const std::wstring ourGuid = OurSessionGuidString();
            EnumerateSessions(mgr, [&](IAudioSessionControl* a_ctl, DWORD a_sessionPid) {
                if (a_sessionPid != pid) {
                    return;
                }
                const std::wstring id = SessionIdentifier(a_ctl);
                // Sesja bez identyfikatora tez jest sesja gry - nasza zawsze ma GUID.
                if (!id.empty() && UpperWide(id).find(ourGuid) != std::wstring::npos) {
                    return;
                }
                sawGameSession = true;
                ISimpleAudioVolume* vol = nullptr;
                if (SUCCEEDED(a_ctl->QueryInterface(IID_PPV_ARGS(&vol))) && vol) {
                    BOOL muted = FALSE;
                    if (SUCCEEDED(vol->GetMute(&muted)) && muted) {
                        vol->SetMute(FALSE, nullptr);
                    }
                    vol->Release();
                }
            });
        }

        if (mgr) {
            mgr->Release();
        }
        if (device) {
            device->Release();
        }
        if (enumerator) {
            enumerator->Release();
        }
        if (comInit) {
            ::CoUninitialize();
        }

        if (sawGameSession) {
            g_audioHealDone.store(true, std::memory_order_relaxed);
        }
        return g_audioHealDone.load(std::memory_order_relaxed);
    }

    // Sesja muzyki: najpierw po GUID (pewne), potem po roznicy zbiorow
    // (sesja pojawila sie dopiero po Initialize naszego klienta).
    std::wstring FindOurSession(
        IAudioSessionManager2* a_mgr,
        DWORD a_pid,
        const std::set<std::wstring>& a_known)
    {
        if (!a_mgr) {
            return {};
        }
        const std::wstring ourGuid = OurSessionGuidString();
        std::wstring byGuid;
        EnumerateSessions(a_mgr, [&](IAudioSessionControl* a_ctl, DWORD a_sessionPid) {
            if (!byGuid.empty() || a_sessionPid != a_pid) {
                return;
            }
            const std::wstring id = SessionIdentifier(a_ctl);
            if (!id.empty() && UpperWide(id).find(ourGuid) != std::wstring::npos) {
                byGuid = id;
            }
        });
        if (!byGuid.empty()) {
            return byGuid;
        }

        std::wstring byDiff;
        EnumerateSessions(a_mgr, [&](IAudioSessionControl* a_ctl, DWORD a_sessionPid) {
            if (!byDiff.empty() || a_sessionPid != a_pid) {
                return;
            }
            const std::wstring id = SessionIdentifier(a_ctl);
            if (!id.empty() && a_known.find(id) == a_known.end()) {
                byDiff = id;
            }
        });
        return byDiff;
    }

    bool IsFloatFormat(const WAVEFORMATEX* a_fmt)
    {
        if (!a_fmt) {
            return false;
        }
        if (a_fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            return true;
        }
        if (a_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && a_fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(a_fmt);
            return IsEqualGUID(ext->SubFormat, MFAudioFormat_Float) != 0;
        }
        return false;
    }

    bool ConfigureReader(IMFSourceReader* a_reader, const WAVEFORMATEX* a_mix, Decoder& a_dec)
    {
        if (!a_reader || !a_mix) {
            return false;
        }
        a_dec.reader = a_reader;
        const bool wantFloat = IsFloatFormat(a_mix);
        const UINT32 mixChannels = a_mix->nChannels ? a_mix->nChannels : 2;
        const UINT32 mixRate = a_mix->nSamplesPerSec ? a_mix->nSamplesPerSec : 48000;

        // Format natywny zrodla - fallback, gdy konwersja do formatu miksu
        // (np. nietypowa liczba kanalow) zostanie odrzucona przez dekoder.
        UINT32 srcChannels = 2;
        UINT32 srcRate = 44100;
        IMFMediaType* native = nullptr;
        if (SUCCEEDED(a_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &native)) && native) {
            UINT32 v = 0;
            if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &v)) && v > 0) {
                srcChannels = v;
            }
            if (SUCCEEDED(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &v)) && v > 0) {
                srcRate = v;
            }
            native->Release();
        }

        const auto configure = [&](const GUID& a_subtype, UINT32 a_bits, UINT32 a_channels, UINT32 a_rate) -> HRESULT {
            IMFMediaType* type = nullptr;
            if (FAILED(::MFCreateMediaType(&type)) || !type) {
                return E_FAIL;
            }
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetGUID(MF_MT_SUBTYPE, a_subtype);
            type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, a_bits);
            type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, a_rate);
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, a_channels);
            type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, a_channels * a_bits / 8);
            type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, a_rate * a_channels * a_bits / 8);
            const HRESULT hr = a_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type);
            type->Release();
            return hr;
        };

        HRESULT hr = configure(
            wantFloat ? MFAudioFormat_Float : MFAudioFormat_PCM,
            wantFloat ? 32 : 16,
            mixChannels,
            mixRate);
        if (FAILED(hr)) {
            hr = configure(MFAudioFormat_PCM, 16, srcChannels, srcRate);
        }
        if (FAILED(hr)) {
            return false;
        }

        IMFMediaType* actual = nullptr;
        if (FAILED(a_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) || !actual) {
            return false;
        }
        GUID sub{};
        UINT32 ch = 0;
        UINT32 sr = 0;
        UINT32 bps = 0;
        actual->GetGUID(MF_MT_SUBTYPE, &sub);
        actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
        actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
        actual->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
        actual->Release();
        if (!ch || !sr || !bps) {
            return false;
        }

        a_dec.fmt = {};
        a_dec.fmt.wFormatTag = IsEqualGUID(sub, MFAudioFormat_Float) ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        a_dec.fmt.nChannels = static_cast<WORD>(ch);
        a_dec.fmt.nSamplesPerSec = sr;
        a_dec.fmt.wBitsPerSample = static_cast<WORD>(bps);
        a_dec.fmt.nBlockAlign = static_cast<WORD>(ch * bps / 8);
        a_dec.fmt.nAvgBytesPerSec = sr * a_dec.fmt.nBlockAlign;
        a_dec.fmt.cbSize = 0;

        const bool floatMatch = (a_dec.fmt.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) == IsFloatFormat(a_mix);
        a_dec.exactMix = floatMatch && sr == a_mix->nSamplesPerSec && ch == a_mix->nChannels &&
                         bps == a_mix->wBitsPerSample;
        return true;
    }

    void Worker(std::filesystem::path a_file)
    {
        // MMCSS "Audio": planista traktuje watek jak audio, wiec watki gry nie
        // zaglodza bufora WASAPI. THREAD_PRIORITY_BELOW_NORMAL dawal trzaski
        // przy obciazeniu (probki nie dojezdzaly na koniec bufora).
        DWORD mmcssTask = 0;
        HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &mmcssTask);
        if (mmcss) {
            ::AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_NORMAL);
        } else {
            ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        }

        const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInit = SUCCEEDED(comHr);

        bool mfStarted = false;
        IMFSourceReader* reader = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        IAudioSessionManager2* sessions = nullptr;
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        WAVEFORMATEX* mix = nullptr;
        HANDLE audioEvent = nullptr;
        std::set<std::wstring> knownSessions;
        std::wstring ourSession;
        UINT32 bufferFrames = 0;
        bool started = false;
        std::atomic<bool> watchStop{ false };
        std::thread watcher;

        Decoder dec;

        auto cleanup = [&] {
            watchStop.store(true);
            if (watcher.joinable()) {
                watcher.join();
            }
            if (client) {
                client->Stop();
            }
            if (audioEvent) {
                ::CloseHandle(audioEvent);
                audioEvent = nullptr;
            }
            if (render) {
                render->Release();
                render = nullptr;
            }
            if (client) {
                client->Release();
                client = nullptr;
            }
            if (sessions) {
                RestoreGameSessions(sessions);
                sessions->Release();
                sessions = nullptr;
            }
            if (mix) {
                ::CoTaskMemFree(mix);
                mix = nullptr;
            }
            if (device) {
                device->Release();
                device = nullptr;
            }
            if (enumerator) {
                enumerator->Release();
                enumerator = nullptr;
            }
            g_playing.store(false);
            if (reader) {
                reader->Release();
                reader = nullptr;
            }
            if (mfStarted) {
                ::MFShutdown();
                mfStarted = false;
            }
            if (comInit) {
                ::CoUninitialize();
            }
        };

        do {
            if (FAILED(::MFStartup(MF_VERSION))) {
                break;
            }
            mfStarted = true;

            IMFByteStream* stream = nullptr;
            HRESULT hr = ::MFCreateFile(
                MF_ACCESSMODE_READ,
                MF_OPENMODE_FAIL_IF_NOT_EXIST,
                MF_FILEFLAGS_NONE,
                a_file.wstring().c_str(),
                &stream);
            if (FAILED(hr) || !stream) {
                break;
            }
            hr = ::MFCreateSourceReaderFromByteStream(stream, nullptr, &reader);
            stream->Release();
            if (FAILED(hr) || !reader) {
                reader = nullptr;
                break;
            }
            reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
            reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

            hr = ::CoCreateInstance(
                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
            if (FAILED(hr) || !enumerator) {
                break;
            }
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            if (FAILED(hr) || !device) {
                break;
            }
            hr = device->Activate(
                __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
            if (FAILED(hr) || !client) {
                break;
            }
            hr = device->Activate(
                __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessions));
            if (FAILED(hr) || !sessions) {
                break;
            }
            hr = client->GetMixFormat(&mix);
            if (FAILED(hr) || !mix) {
                break;
            }
            if (!ConfigureReader(reader, mix, dec)) {
                break;
            }

            const DWORD pid = ::GetCurrentProcessId();

            // Najpierw wyciszamy sesje gry - nasza sesja jeszcze nie istnieje.
            MuteGameSessions(sessions, pid, ourSession, knownSessions);

            auto initClient = [&](DWORD a_flags, const WAVEFORMATEX* a_fmt) -> HRESULT {
                if (client) {
                    client->Release();
                    client = nullptr;
                }
                HRESULT ar = device->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
                if (FAILED(ar) || !client) {
                    return FAILED(ar) ? ar : E_FAIL;
                }
                return client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED, a_flags, kBufferDuration, 0, a_fmt, &kMusicSessionGuid);
            };

            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            const WAVEFORMATEX* format = mix;
            if (!dec.exactMix) {
                flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                format = &dec.fmt;
            }
            hr = initClient(flags, format);
            if (FAILED(hr)) {
                flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                hr = initClient(flags, &dec.fmt);
            }
            if (FAILED(hr)) {
                break;
            }

            if (FAILED(client->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
                break;
            }
            hr = client->GetService(IID_PPV_ARGS(&render));
            if (FAILED(hr) || !render) {
                break;
            }
            audioEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!audioEvent || FAILED(client->SetEventHandle(audioEvent))) {
                break;
            }

            for (int attempt = 0; attempt < 3 && ourSession.empty(); ++attempt) {
                ourSession = FindOurSession(sessions, pid, knownSessions);
                if (ourSession.empty()) {
                    ::Sleep(20);
                }
            }

            // Zapas zdekodowanego audio (~0.5 s) trzymany z gory: pojedynczy
            // ReadSample nie moze wtedy wstrzymac podawania probek do WASAPI.
            const size_t aheadBytes = dec.fmt.nAvgBytesPerSec
                ? static_cast<size_t>(dec.fmt.nAvgBytesPerSec) / 2
                : static_cast<size_t>(dec.fmt.nBlockAlign) * 24000;

            BYTE* data = nullptr;
            if (SUCCEEDED(render->GetBuffer(bufferFrames, &data)) && data) {
                dec.Fill(data, bufferFrames * dec.fmt.nBlockAlign);
                ApplyGain(data, bufferFrames * dec.fmt.nBlockAlign, dec.fmt);
                render->ReleaseBuffer(bufferFrames, 0);
                dec.Ensure(aheadBytes);
            }
            if (FAILED(client->Start())) {
                break;
            }
            started = true;
            g_playing.store(true);

            // Niektore sterowniki publikuja sesje dopiero po Start - druga proba.
            for (int attempt = 0; attempt < 2 && ourSession.empty(); ++attempt) {
                ourSession = FindOurSession(sessions, pid, knownSessions);
                if (ourSession.empty()) {
                    ::Sleep(20);
                }
            }

            // Sesje mogly powstac w trakcie inicjalizacji - dociagnij je teraz.
            // Bez rozpoznanej wlasnej sesji NIE wyciszamy ponownie (ryzyko
            // wyciszenia muzyki); pierwszy przebieg i tak objal sesje gry.
            if (!ourSession.empty()) {
                MuteGameSessions(sessions, pid, ourSession, knownSessions);
            }

            // Nowe sesje gier wyciszamy z osobnego watku: enumeracja COM na
            // watku audio potrafi stanac na dziesiatki ms i oproznic bufor.
            if (!ourSession.empty()) {
                try {
                    watcher = std::thread([pid, ourId = ourSession, &watchStop] {
                        const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        const bool comInit = SUCCEEDED(comHr);
                        IMMDeviceEnumerator* en = nullptr;
                        IMMDevice* dev = nullptr;
                        IAudioSessionManager2* mgr = nullptr;
                        if (SUCCEEDED(::CoCreateInstance(
                                __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
                            en && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev &&
                            SUCCEEDED(dev->Activate(
                                __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&mgr))) &&
                            mgr) {
                            while (!watchStop.load(std::memory_order_relaxed)) {
                                for (int i = 0; i < 50 && !watchStop.load(std::memory_order_relaxed); ++i) {
                                    ::Sleep(100);
                                }
                                if (watchStop.load(std::memory_order_relaxed)) {
                                    break;
                                }
                                std::set<std::wstring> known;
                                MuteGameSessions(mgr, pid, ourId, known);
                            }
                        }
                        if (mgr) {
                            mgr->Release();
                        }
                        if (dev) {
                            dev->Release();
                        }
                        if (en) {
                            en->Release();
                        }
                        if (comInit) {
                            ::CoUninitialize();
                        }
                    });
                } catch (...) {
                }
            }

            bool underrunLogged = false;
            while (!g_stopRequested.load(std::memory_order_relaxed)) {
                UINT32 padding = 0;
                if (FAILED(client->GetCurrentPadding(&padding))) {
                    break;
                }
                const UINT32 available = padding < bufferFrames ? bufferFrames - padding : 0;
                // Pusty bufor w trakcie gry = slyszalna dziura (trzask). Jedna
                // linia w logu wystarczy, zeby odroznic to od problemu z dekoderem.
                if (padding == 0 && !underrunLogged) {
                    underrunLogged = true;
                    REX::WARN("[Music] WASAPI buffer underrun");
                }
                if (available > 0) {
                    BYTE* out = nullptr;
                    if (FAILED(render->GetBuffer(available, &out)) || !out) {
                        break;
                    }
                    dec.Fill(out, available * dec.fmt.nBlockAlign);
                    ApplyGain(out, available * dec.fmt.nBlockAlign, dec.fmt);
                    if (FAILED(render->ReleaseBuffer(available, 0))) {
                        break;
                    }
                    dec.Ensure(aheadBytes);
                }
                ::WaitForSingleObject(audioEvent, 20);
            }
        } while (false);

        if (!started) {
            REX::WARN("[Music] playback init failed");
        }
        cleanup();
        if (mmcss) {
            ::AvRevertMmThreadCharacteristics(mmcss);
        }
    }
}

namespace SAFCompanion::Music
{
    std::vector<std::string> ListTracks()
    {
        std::vector<std::string> out;
        for (const auto& t : ScanTracks()) {
            out.push_back(t.name);
        }
        return out;
    }

    bool Play(const std::string& a_track)
    {
        std::filesystem::path file;
        if (!ResolveTrack(a_track, file)) {
            return false;
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        const std::string track = file.stem().string();
        if (g_playing.load() && _stricmp(g_track.c_str(), track.c_str()) == 0) {
            return true;
        }

        g_stopRequested.store(true);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_playing.store(false);

        g_stopRequested.store(false);
        g_track = track;

        // Start asynchroniczny: Papyrus nie czeka na inicjalizacje MF/WASAPI
        // (potrafi trwac setki ms i wstrzymywac gre przy starcie animacji).
        // Niepowodzenie zglasza worker; IsPlaying() wtedy wraca do false,
        // a kolejne EnsureMusicPlaying synchronizuje stan.
        try {
            g_worker = std::thread(Worker, file);
        } catch (...) {
            g_track.clear();
            return false;
        }
        return true;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stopRequested.store(true);
        if (g_worker.joinable()) {
            g_worker.join();
        }
        g_playing.store(false);
        g_track.clear();
    }

    void SetVolume(float a_volume)
    {
        if (a_volume < 0.0f) {
            a_volume = 0.0f;
        } else if (a_volume > 1.0f) {
            a_volume = 1.0f;
        }
        g_volume.store(a_volume, std::memory_order_relaxed);
    }

    bool EnsureGameAudioUnmuted()
    {
        return HealGameAudio();
    }

    bool IsPlaying()
    {
        return g_playing.load();
    }
}
