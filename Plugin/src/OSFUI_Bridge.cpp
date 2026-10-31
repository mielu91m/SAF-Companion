#include "PCH.h"

#include <Windows.h>

#define OSFUI_API_NO_REX 1
#include "OSFUI_Bridge.h"
#include "OSFUI_API.h"
#include "Papyrus/SAFCompanionAPI.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // Static for process life: bridge callbacks may fire after plugin teardown.
    OSFUI::API::Client g_ui;

    bool g_inited = false;
    bool g_viewState = false;

    constexpr const char* kModId = "saf.companion";
    constexpr const char* kViewId = "saf.companion/animmenu";

    std::string EscapeJson(std::string_view a_in)
    {
        std::string out;
        out.reserve(a_in.size() + 8);
        for (const char c : a_in) {
            switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += ' ';
                } else {
                    out += c;
                }
            }
        }
        return out;
    }

    std::string AnimsJson()
    {
        std::string json = "[";
        try {
            const auto anims = SAFCompanion::Papyrus::GetControlAnimations_Impl();
            bool first = true;
            for (const auto& a : anims) {
                if (!first) {
                    json += ',';
                }
                first = false;
                json += '"';
                json += EscapeJson(a);
                json += '"';
            }
        } catch (...) {
        }
        json += ']';
        return json;
    }

    std::string g_mode = "player";

    // --- Konfigurowalne klawisze (schema OSF UI: keyStop/keyPlayer/keyNpc/keyCamera) ---
    constexpr const char* kKeyStop = "keyStop";
    constexpr const char* kKeyPlayer = "keyPlayer";
    constexpr const char* kKeyNpc = "keyNpc";
    constexpr const char* kKeyCamera = "keyCamera";

    std::atomic<std::uint16_t> g_vkStop{ VK_NUMPAD4 };
    std::atomic<std::uint16_t> g_vkPlayer{ VK_NUMPAD5 };
    std::atomic<std::uint16_t> g_vkNpc{ VK_NUMPAD6 };
    std::atomic<std::uint16_t> g_vkCamera{ VK_NUMPAD7 };

    std::uint16_t VkFromKeyName(std::string_view a_name)
    {
        std::string s(a_name);
        while (!s.empty() && (s.front() == '"' || std::isspace(static_cast<unsigned char>(s.front())))) {
            s.erase(s.begin());
        }
        while (!s.empty() && (s.back() == '"' || std::isspace(static_cast<unsigned char>(s.back())))) {
            s.pop_back();
        }
        if (s.empty()) {
            return 0;
        }

        std::string up = s;
        for (auto& c : up) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        if (s.size() == 1) {
            const char c = up[0];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                return static_cast<std::uint16_t>(c);
            }
        }

        if (up.rfind("NUMPAD", 0) == 0) {
            if (up.size() == 7 && up[6] >= '0' && up[6] <= '9') {
                return static_cast<std::uint16_t>(VK_NUMPAD0 + (up[6] - '0'));
            }
            if (up == "NUMPADDECIMAL") return VK_DECIMAL;
            if (up == "NUMPADDIVIDE") return VK_DIVIDE;
            if (up == "NUMPADMULTIPLY") return VK_MULTIPLY;
            if (up == "NUMPADPLUS" || up == "NUMPADADD") return VK_ADD;
            if (up == "NUMPADMINUS" || up == "NUMPADSUBTRACT") return VK_SUBTRACT;
        }

        if (up.size() >= 2 && up.size() <= 3 && up[0] == 'F') {
            const int n = std::atoi(up.c_str() + 1);
            if (n >= 1 && n <= 24) {
                return static_cast<std::uint16_t>(VK_F1 + n - 1);
            }
        }

        struct Named
        {
            const char* name;
            std::uint16_t vk;
        };
        static constexpr Named kNames[] = {
            { "SPACE", VK_SPACE },
            { "ENTER", VK_RETURN },
            { "RETURN", VK_RETURN },
            { "ESCAPE", VK_ESCAPE },
            { "ESC", VK_ESCAPE },
            { "TAB", VK_TAB },
            { "BACKSPACE", VK_BACK },
            { "INSERT", VK_INSERT },
            { "DELETE", VK_DELETE },
            { "HOME", VK_HOME },
            { "END", VK_END },
            { "PAGEUP", VK_PRIOR },
            { "PAGEDOWN", VK_NEXT },
            { "ARROWUP", VK_UP },
            { "ARROWDOWN", VK_DOWN },
            { "ARROWLEFT", VK_LEFT },
            { "ARROWRIGHT", VK_RIGHT },
            { "CAPSLOCK", VK_CAPITAL },
            { "NUMLOCK", VK_NUMLOCK },
            { "LEFTSHIFT", VK_LSHIFT },
            { "RIGHTSHIFT", VK_RSHIFT },
            { "LEFTCTRL", VK_LCONTROL },
            { "LEFTCONTROL", VK_LCONTROL },
            { "RIGHTCTRL", VK_RCONTROL },
            { "RIGHTCONTROL", VK_RCONTROL },
            { "LEFTALT", VK_LMENU },
            { "RIGHTALT", VK_RMENU },
            { "SEMICOLON", VK_OEM_1 },
            { "COMMA", VK_OEM_COMMA },
            { "PERIOD", VK_OEM_PERIOD },
            { "SLASH", VK_OEM_2 },
            { "BACKSLASH", VK_OEM_5 },
            { "QUOTE", VK_OEM_7 },
            { "BRACKETLEFT", VK_OEM_4 },
            { "BRACKETRIGHT", VK_OEM_6 },
            { "MINUS", VK_OEM_MINUS },
            { "EQUAL", VK_OEM_PLUS },
            { "BACKQUOTE", VK_OEM_3 },
            { "TILDE", VK_OEM_3 },
        };
        for (const auto& n : kNames) {
            if (up == n.name) {
                return n.vk;
            }
        }
        return 0;
    }

    std::string ReadSettingKey(const char* a_key)
    {
        if (!g_ui.IsConnected()) {
            return {};
        }
        char buf[64] = {};
        const auto need = g_ui.GetSettingString(kModId, a_key, buf, static_cast<std::uint32_t>(sizeof(buf)));
        if (need == 0 || buf[0] == '\0') {
            return {};
        }
        return std::string(buf);
    }

    void RefreshBindings()
    {
        const auto apply = [](const char* a_key, std::atomic<std::uint16_t>& a_slot, std::uint16_t a_fallback) {
            const std::string name = ReadSettingKey(a_key);
            const std::uint16_t vk = VkFromKeyName(name);
            a_slot.store(vk != 0 ? vk : a_fallback, std::memory_order_relaxed);
        };
        apply(kKeyStop, g_vkStop, VK_NUMPAD4);
        apply(kKeyPlayer, g_vkPlayer, VK_NUMPAD5);
        apply(kKeyNpc, g_vkNpc, VK_NUMPAD6);
        apply(kKeyCamera, g_vkCamera, VK_NUMPAD7);
    }

    void OnSettingChanged(const char* a_mod, const char* a_key, const char*, void*) noexcept
    {
        if (!a_mod || !a_key || _stricmp(a_mod, kModId) != 0) {
            return;
        }
        RefreshBindings();
    }

    void PublishKeys()
    {
        if (!g_ui.IsConnected()) {
            return;
        }

        const std::string anims = AnimsJson();

        if (g_viewState) {
            g_ui.Raw()->SetViewState(kModId, "anims", anims.c_str());
        }

        // Legacy views (targetVersion < 2.0) never see state envelopes; they get
        // a plain {type, payload} message, exactly like OSF Animation does.
        std::string payload = "{\"anims\":";
        payload += anims;
        payload += ",\"mode\":\"";
        payload += EscapeJson(g_mode);
        payload += "\"}";
        g_ui.SendToWeb(kViewId, "saf.companion.anims", payload.c_str());
    }

    void OnBridgeReady(void*) noexcept
    {
        PublishKeys();
    }

    void OnRefreshCommand(const char*, const char*, const char*, void*) noexcept
    {
        PublishKeys();
    }

    void OnCloseCommand(const char*, const char*, const char*, void*) noexcept
    {
        SAFCompanion::OSFUIBridge::RequestClose();
    }
}

namespace SAFCompanion::OSFUIBridge
{
    void Init()
    {
        if (g_inited) {
            return;
        }

        if (!g_ui.Init()) {
            static int tries = 0;
            ++tries;
            const HMODULE osfMod = ::GetModuleHandleW(L"OSFUI.dll");
            const FARPROC osfProc = osfMod ? ::GetProcAddress(osfMod, "OSFUI_RequestBridge") : nullptr;
            if (osfProc) {
                // MAJOR mismatch against our 1.10 header? Try the last known-good ABI.
                const auto req = reinterpret_cast<OSFUI::API::RequestBridge_t>(osfProc);
                if (auto* raw = req((1u << 16) | 8u)) {
                    g_ui.Attach(raw);
                } else if (tries == 3) {
                    REX::WARN("[OSFUI] bridge ABI mismatch - UI disabled");
                }
            } else if (tries == 3) {
                REX::WARN("[OSFUI] OSFUI.dll not loaded - UI disabled");
            }
            if (!g_ui.IsConnected()) {
                return;
            }
        }
        g_inited = true;

        g_viewState = g_ui.Has(OSFUI::API::Feature::kViewState);

        g_ui.RegisterCommand("saf.companion.refresh", &OnRefreshCommand, nullptr);
        g_ui.RegisterCommand("saf.companion.close", &OnCloseCommand, nullptr);
        g_ui.SetReadyCallback(&OnBridgeReady, nullptr);

        if (g_ui.Has(OSFUI::API::Feature::kSettings)) {
            RefreshBindings();
            g_ui.SubscribeSettings(kModId, &OnSettingChanged, nullptr);
        }

        PublishKeys();
        PublishMode("player");
    }

    void PublishAnims()
    {
        PublishKeys();
    }

    void PublishMode(const char* a_mode)
    {
        g_mode = (a_mode && *a_mode) ? a_mode : "player";
        if (g_viewState) {
            std::string value = "\"";
            value += EscapeJson(g_mode);
            value += '"';
            g_ui.Raw()->SetViewState(kModId, "mode", value.c_str());
        }
        PublishKeys();
    }

    void RequestClose()
    {
        if (!g_ui.IsConnected()) {
            return;
        }
        g_ui.RequestMenu(kViewId, false);
    }

    std::uint16_t GetActionVK(ActionKey a_action)
    {
        switch (a_action) {
        case ActionKey::kStop:
            return g_vkStop.load(std::memory_order_relaxed);
        case ActionKey::kPlayer:
            return g_vkPlayer.load(std::memory_order_relaxed);
        case ActionKey::kNpc:
            return g_vkNpc.load(std::memory_order_relaxed);
        case ActionKey::kCamera:
            return g_vkCamera.load(std::memory_order_relaxed);
        default:
            return 0;
        }
    }

    void TickBindings()
    {
        if (!g_inited || !g_ui.IsConnected()) {
            return;
        }
        // Rebind w OSF UI ma zadzialac od razu; eventy ustawien bywaja
        // niedostepne, wiec co ~3 s czytamy wartosci ponownie.
        static std::uint64_t last = 0;
        const std::uint64_t now = GetTickCount64();
        if (last != 0 && now - last < 3000) {
            return;
        }
        last = now;
        RefreshBindings();
    }
}
