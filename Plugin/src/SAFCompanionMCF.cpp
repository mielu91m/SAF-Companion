#include "PCH.h"
#include "Papyrus/SAFCompanionAPI.h"
#include "RE/A/Actor.h"
#include "RE/B/BSFixedString.h"
#include "RE/F/FormTypes.h"
#include "RE/B/BSTSmartPointer.h"
#include "RE/O/Object.h"
#include "RE/O/ObjectBindPolicy.h"
#include "RE/V/Variable.h"
#include "RE/V/VirtualMachine.h"
#include "MCF_API.h"
#include <charconv>
#include <string>

namespace
{
    // Pakuje RE::Actor* do Variable bez BSScriptUtil (unika zaleznosci od Array).
    bool PackActorVariable(RE::BSScript::IVirtualMachine* vm, RE::Actor* actor, RE::BSScript::Variable& out) {
        if (!vm || !actor) { out = nullptr; return false; }
        const auto& handles = vm->GetObjectHandlePolicy();
        const std::uint32_t typeId = static_cast<std::uint32_t>(RE::FormType::kREFR);
        const std::size_t h = handles.GetHandleForObject(typeId, actor);
        if (h == handles.EmptyHandle()) { out = nullptr; return false; }
        RE::BSTSmartPointer<RE::BSScript::Object> obj;
        if (!vm->FindBoundObject(h, "Actor", false, obj, false) &&
            vm->CreateObject("Actor", obj) && obj) {
            vm->GetObjectBindPolicy().BindObject(obj, h);
        }
        if (!obj) { out = nullptr; return false; }
        out = std::move(obj);
        return true;
    }
}

namespace
{
    void SafcCommand(
        const MCF::simple_array<MCF::simple_string_view>& a_args,
        const char* /*a_fullString*/,
        MCF::ConsoleInterface* a_intfc)
    {
        if (!a_intfc) return;

        auto anims = SAFCompanion::Papyrus::GetAvailableAnimations_Impl();
        auto* vm = RE::BSScript::Internal::VirtualMachine::GetSingleton();

        auto PlayOnPlayerAnim = [&](const std::string& animId) -> bool {
            if (!vm || animId.empty()) return false;
            RE::BSFixedString scriptName("SAFScript");
            RE::BSFixedString funcName("PlayOnPlayer");
            RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> argsFn(
                [animId](RE::BSScrapArray<RE::BSScript::Variable>& a_args) {
                    RE::BSScript::Variable vId;
                    vId = RE::BSFixedString(animId.c_str());
                    RE::BSScript::Variable vSpeed;
                    vSpeed = 1.0f;
                    RE::BSScript::Variable vIndex;
                    vIndex = static_cast<std::int32_t>(0);
                    a_args.push_back(vId);
                    a_args.push_back(vSpeed);
                    a_args.push_back(vIndex);
                    return true;
                });
            return vm->DispatchStaticCall(scriptName, funcName, argsFn, nullptr, 0);
        };

        if (a_args.size() == 0 || (a_args.size() == 1 && a_args[0].get() == "list")) {
            a_intfc->PreventDefaultPrint();
            a_intfc->PrintLn("=== SAF Companion - animacje .fbx (Data/SAF/Animations) ===");
            if (anims.empty()) {
                a_intfc->PrintLn("Brak animacji.");
                return;
            }
            for (size_t i = 0; i < anims.size(); ++i) {
                std::string line = std::to_string(i + 1) + ". " + anims[i];
                a_intfc->PrintLn(MCF::simple_string_view(line.c_str()));
            }
            a_intfc->PrintLn("---");
            a_intfc->PrintLn("Na graczu:    safc play <numer>");
            a_intfc->PrintLn("Na celowniku: celuj w postac i wpisz: safc play <numer> ref");
            a_intfc->PrintLn("Po FormID:    safc play <numer> <hex_id>  np. safc play 1 14");
            return;
        }

        if (a_args.size() >= 2 && a_args[0].get() == "play") {
            int index = 0;
            auto sv = a_args[1].get();
            std::string numStr(sv);
            auto [p, ec] = std::from_chars(numStr.data(), numStr.data() + numStr.size(), index);
            if (ec != std::errc{} || index < 1 || static_cast<size_t>(index) > anims.size()) {
                a_intfc->PreventDefaultPrint();
                a_intfc->PrintLn("Nieprawidlowy numer animacji. Uzyj: safc list");
                return;
            }
            const std::string& animId = anims[static_cast<size_t>(index) - 1];

            bool useRef = false;
            bool useFormId = false;
            std::string formIdStr;
            if (a_args.size() >= 3) {
                std::string_view third = a_args[2].get();
                if (third == "ref" || third == "r") {
                    useRef = true;
                } else {
                    useFormId = true;
                    formIdStr = std::string(third);
                }
            }

            if (useRef) {
                auto ref = a_intfc->GetSelectedReference();
                if (!ref || !ref->As<RE::Actor>()) {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn("Celuj w postac (NPC) i sprobuj ponownie: safc play " + std::to_string(index) + " ref");
                    return;
                }
                RE::Actor* actor = ref->As<RE::Actor>();
                if (!vm) return;
                RE::BSScript::Variable vActor;
                if (!PackActorVariable(vm, actor, vActor)) {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn("Blad pakowania aktora.");
                    return;
                }
                RE::BSFixedString scriptName("SAFScript");
                RE::BSFixedString funcName("PlayOnActor");
                RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> argsFn(
                    [animId, vActor](RE::BSScrapArray<RE::BSScript::Variable>& a_args) {
                        a_args.push_back(vActor);
                        RE::BSScript::Variable vId;
                        vId = RE::BSFixedString(animId.c_str());
                        RE::BSScript::Variable vSpeed;
                        vSpeed = 1.0f;
                        RE::BSScript::Variable vIndex;
                        vIndex = static_cast<std::int32_t>(0);
                        a_args.push_back(vId);
                        a_args.push_back(vSpeed);
                        a_args.push_back(vIndex);
                        return true;
                    });
                if (vm->DispatchStaticCall(scriptName, funcName, argsFn, nullptr, 0)) {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn(std::string("Odtwarzanie na celowniku: ") + animId);
                } else {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn("Nie udalo sie odtworzyc na celowniku.");
                }
                return;
            }

            if (useFormId && !formIdStr.empty()) {
                RE::TESForm* form = a_intfc->HexStrToForm(MCF::simple_string_view(formIdStr.c_str()));
                RE::Actor* actor = form ? form->As<RE::Actor>() : nullptr;
                if (!actor) {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn("Nieprawidlowe FormID lub nie aktor. Uzyj hex, np. 14 dla gracza.");
                    return;
                }
                if (!vm) return;
                RE::BSScript::Variable vActor;
                if (!PackActorVariable(vm, actor, vActor)) {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn("Blad pakowania aktora (FormID).");
                    return;
                }
                RE::BSFixedString scriptName("SAFScript");
                RE::BSFixedString funcName("PlayOnActor");
                RE::BSTThreadScrapFunction<bool(RE::BSScrapArray<RE::BSScript::Variable>&)> argsFn(
                    [animId, vActor](RE::BSScrapArray<RE::BSScript::Variable>& a_args) {
                        a_args.push_back(vActor);
                        RE::BSScript::Variable vId;
                        vId = RE::BSFixedString(animId.c_str());
                        RE::BSScript::Variable vSpeed;
                        vSpeed = 1.0f;
                        RE::BSScript::Variable vIndex;
                        vIndex = static_cast<std::int32_t>(0);
                        a_args.push_back(vId);
                        a_args.push_back(vSpeed);
                        a_args.push_back(vIndex);
                        return true;
                    });
                if (vm->DispatchStaticCall(scriptName, funcName, argsFn, nullptr, 0)) {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn(std::string("Odtwarzanie: ") + animId);
                } else {
                    a_intfc->PreventDefaultPrint();
                    a_intfc->PrintLn("Nie udalo sie odtworzyc.");
                }
                return;
            }

            if (PlayOnPlayerAnim(animId)) {
                a_intfc->PreventDefaultPrint();
                a_intfc->PrintLn(std::string("Odtwarzanie na graczu: ") + animId);
            } else {
                a_intfc->PreventDefaultPrint();
                a_intfc->PrintLn("Nie udalo sie odtworzyc na graczu.");
            }
            return;
        }

        a_intfc->PreventDefaultPrint();
        a_intfc->PrintLn("Uzycie: safc | safc list | safc play <numer> [ref | <formid>]");
    }
}

namespace SAFCompanion::Papyrus
{
    void RegisterMCFCommand()
    {
        MCF::RegisterCommand("safc", SafcCommand);
    }
}
