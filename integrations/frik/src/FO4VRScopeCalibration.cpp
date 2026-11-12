#include "FO4VRScopeCalibration.h"
#include "TrueScopes/CalibrationMath.h"
#include "UIPlan.h"
#include "external/FRIKApiV2.h"
#include "render/PrimitiveDrawRenderer.h"
#include "vrui/UIManager.h"
#include "vrui/UIButtonPanel.h"
#include "vrui/UITextPanel.h"
#include "vrcf/VRControllersManager.h"
#include "vrcf/VRControllersSuppressor.h"
#include <filesystem>
#include <fstream>
#include <cctype>

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface* q, F4SE::PluginInfo* i)
{ return g_mod->onF4SEPluginQuery(q, i); }
extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface* l)
{ return g_mod->onF4SEPluginLoad(l); }

namespace my_mod
{
    using namespace TrueScopes::Calibration;
    using frik::api::FRIKApiV2;
    namespace {
        constexpr std::uint32_t kOpenMessage = 0x5343414c;
        constexpr auto kInputOwner="TrueScopesCalibration";
        std::filesystem::path profilePath() { return std::filesystem::path(BASE_MOD_PATH) / "profiles.json"; }
        std::string lower(std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        std::string formKey(std::uint32_t id) {
            const auto* f = RE::TESForm::GetFormByID(id);
            const auto* file = f ? f->GetFile(0) : nullptr;
            if (!file || id >= 0xff000000) return {};
            return std::format("{}:{:06x}", lower(std::string(file->GetFilename())), id & (file->IsLight() ? 0xfff : 0xffffff));
        }
        class Adapter : public vrui::UIModAdapter {
            RE::NiPoint3 getInteractionBoneWorldPosition() override {
                return FRIKApiV2::inst->getIndexFingerTipPosition(FRIKApiV2::Hand::Offhand);
            }
            void setInteractionHandPointing(bool, bool) override {
                // Do not override ROCK's grip or FRIK's weapon hand poses.
            }
        };
    }
    void FO4VRScopeCalibration::onModLoaded(const F4SE::LoadInterface*) {}
    void FO4VRScopeCalibration::message(F4SE::MessagingInterface::Message* msg) {
        if (msg && msg->type == kOpenMessage) g_myMod._openRequested = true;
    }
    void FO4VRScopeCalibration::onGameLoaded()
    {
        const auto h = ::GetModuleHandleW(L"truescopes_vr.dll");
        const auto get = h ? reinterpret_cast<GetAPI>(::GetProcAddress(h, "TrueScopes_GetCalibrationAPI")) : nullptr;
        _api = get ? get(kVersion, sizeof(API)) : nullptr;
        if (!_api || _api->bytes != sizeof(API) || _api->version != kVersion || !_api->read || !_api->set) {
            _api = nullptr;
            logger::info("True Scopes calibration backend unavailable");
            return;
        }
        if (FRIKApiV2::initialize(2) != 0) { logger::info("FRIK external-menu API unavailable"); return; }
        _messaging->RegisterListener(message, FRIKApiV2::FRIK_F4SE_MOD_NAME);
        const FRIKApiV2::OpenExternalModConfigData button{
            "FO4VRScopeCalibration\\config\\btn-true-scopes.nif", "FO4VRScopeCalibration", kOpenMessage};
        _menuRegistered = FRIKApiV2::inst->registerOpenModSettingButtonToMainConfig(button);
        logger::info("Scope Calibration: FRIK button {}, backend {}", _menuRegistered, _api != nullptr);
        if (!_menuRegistered) return;
        render::PrimitiveDrawRenderer::preload();
        try {
            const auto p = profilePath();
            if (std::filesystem::exists(p)) {
                if (std::filesystem::file_size(p) > 4 * 1024 * 1024) throw std::runtime_error("Profile file too large");
                std::ifstream in(p);
                const auto data = nlohmann::json::parse(in);
                if ((data.at("version") != 1 && data.at("version") != 2) || !data.at("profiles").is_object()) throw std::runtime_error("Unknown profile format");
                _profiles = data.at("profiles");
            }
        } catch (const std::exception& e) {
            _storageOK = false; logger::error("Profiles preserved; writes disabled: {}", e.what());
        }
    }
    void FO4VRScopeCalibration::onGameSessionLoaded()
    {
        if (!_menuRegistered) return;
        close();
        _seenEpoch = 0;
        _key.clear();
    }
    std::string FO4VRScopeCalibration::key(const State& s)
    {
        const auto weapon = formKey(s.weapon);
        if (weapon.empty()) return {};
        std::vector<std::string> mods;
        for (std::uint32_t i = 0; i < s.modCount; ++i) {
            const auto k = formKey(s.mods[i]);
            if (k.empty()) return {};
            mods.push_back(k);
        }
        std::sort(mods.begin(), mods.end());
        // Full strings are stored, not a collision-prone hash or load-order-specific IDs.
        return nlohmann::json::array({weapon, lower(s.optic), mods}).dump();
    }
    bool FO4VRScopeCalibration::apply(const Values& value)
    {
        if (!_api || !_state.ready || !Valid(value) || !_api->set(_state.epoch, &value)) {
            _status = "Adjustment refused: scope changed or limit reached"; return false;
        }
        _state.values = value;
        return true;
    }
    void FO4VRScopeCalibration::updateSticks()
    {
        using namespace vrcf;
        const auto now=std::chrono::steady_clock::now();
        const auto dt=std::chrono::duration<float>(now-_lastStickFrame).count();
        _lastStickFrame=now;
        const auto support=VRControllers.getThumbstickValue(Hand::Offhand);
        const auto primary=VRControllers.getThumbstickValue(Hand::Primary);
        Values value{};
        const bool grip=VRControllers.isPressHeldDown(Hand::Offhand,vr::k_EButton_Grip);
        const bool trigger=VRControllers.isPressHeldDown(Hand::Offhand,vr::k_EButton_SteamVR_Trigger);
        const bool tilt=_tiltSelected || grip || trigger;
        _lastTilt = tilt;
        if (_motion.update(_state.values,{support.x,support.y,primary.x,primary.y,tilt},dt,_fine,value)) {
            if(apply(value)) _status="LIVE PREVIEW - press SAVE to keep";
        } else if (!_motion.armed()) _status="Center both sticks to start adjusting";
    }
    void FO4VRScopeCalibration::save()
    {
        if (!_storageOK || _key.empty()) { _status = "Cannot save: profile identity/storage unavailable"; return; }
        try {
            State fresh{};
            if (!_api || !_api->read(&fresh) || !fresh.ready || fresh.epoch != _state.epoch)
                throw std::runtime_error("Scope changed before Save");
            auto next = _profiles;
            const auto& v = fresh.values;
            next[_key] = {v.right, v.forward, v.up, v.size, v.pitch, v.yaw};
            const auto p = profilePath();
            const auto temp = std::filesystem::path(p.wstring() + L".tmp");
            std::filesystem::create_directories(p.parent_path());
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.exceptions(std::ios::badbit | std::ios::failbit);
                out << nlohmann::json{{"version",2},{"profiles",next}}.dump(2);
                out.flush();
            }
            if (std::filesystem::exists(p)) {
                const auto bak = std::filesystem::path(p.wstring() + L".bak");
                if (!::CopyFileW(p.c_str(), bak.c_str(), FALSE)) throw std::runtime_error("Backup failed");
            }
            if (!::MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("Atomic profile save failed");
            _profiles = std::move(next); _start = v; _status = "SAVED for this gun and attachment configuration";
            logger::info("Calibration saved: {} R={} F={} U={} size={}", _key, v.right, v.forward, v.up, v.size);
        } catch (const std::exception& e) { _status = "Save failed - previous profile retained"; logger::error("{}", e.what()); }
    }
    void FO4VRScopeCalibration::close(bool restorePreview)
    {
        vrcf::VRControllersSuppress.release(kInputOwner);
        if(_inputClaimed && FRIKApiV2::inst)FRIKApiV2::inst->blockOffHandWeaponGripping(kInputOwner,false);
        _inputClaimed=false;
        _motion.reset();
        if (_panel) {
            if(restorePreview) apply(_start);
            vrui::g_uiManager->detachElement(_panel, true);
            _panel.reset();
        }
        _closeRequested = false;_rebuildRequested=false;_positionButton.reset();_tiltButton.reset();
    }
#include "Panel.inc"
    void FO4VRScopeCalibration::open()
    {
        if (_panel) { _closeRequested = true; return; }
        if(!vrcf::VRControllersSuppress.isInitialized()) {
            logger::error("Cannot open calibration safely: controller suppression is unavailable");
            return;
        }
        _start = _state.values;
        _status = _api ? (_state.ready ? "LIVE - changes are not saved until Save" : "Equip a scoped weapon") : "Calibration backend NOT loaded";
        _help=false;_rebuildRequested=false;_tiltSelected=false;_lastTilt=false;
        buildPanel();
        _motion.reset(); _lastStickFrame=std::chrono::steady_clock::now();
        // Consume controls only while open. Our raw manager still reads the
        // held modifier before game-facing suppression; no weapon input hooks.
        _inputClaimed=FRIKApiV2::inst->blockOffHandWeaponGripping(kInputOwner,true);
        for(const auto hand:{vrcf::Hand::Primary,vrcf::Hand::Offhand}) {
            vrcf::VRControllersSuppress.suppress(kInputOwner,hand,vr::k_EButton_SteamVR_Touchpad);
            vrcf::VRControllersSuppress.suppress(kInputOwner,hand,vr::k_EButton_SteamVR_Trigger);
            vrcf::VRControllersSuppress.suppress(kInputOwner,hand,vr::k_EButton_Grip);
            vrcf::VRControllersSuppress.suppressAllAxes(kInputOwner,hand);
        }
    }
    void FO4VRScopeCalibration::onFrameUpdate()
    {
        if (!_menuRegistered) return;
        try {
        const auto* f = FRIKApiV2::inst;
        if (!f->isSkeletonReady()) { close(); _seenEpoch=0; return; }
        const bool valid = _api->read(&_state);
        if (valid && _state.epoch != _seenEpoch) {
            // The old preview has no authority over a new optic/receiver.
            close(false);
            _seenEpoch = _state.epoch; _key.clear(); _weaponName.clear();
            if (_state.ready) {
                _key = key(_state);
                const auto* form = RE::TESForm::GetFormByID(_state.weapon);
                _weaponName = form ? std::string(RE::TESFullName::GetFullName(*form)) : "Scoped weapon";
                Values v{};
                try {
                    if (!_key.empty() && _profiles.contains(_key)) {
                        const auto data = _profiles.at(_key).get<std::vector<float>>();
                        if (data.size()!=4 && data.size()!=6) throw std::runtime_error("Invalid calibration profile");
                        v={data[0],data[1],data[2],data[3]};
                        if(data.size()==6){v.pitch=data[4];v.yaw=data[5];}
                        if (!Valid(v)) throw std::runtime_error("Out-of-range calibration profile");
                    }
                } catch (const std::exception& e) { logger::warn("Profile ignored: {}", e.what()); v={}; }
                apply(v);
            }
        }
        if (_panel && f->isConfigOpen()) close();
        const auto ui=RE::UI::GetSingleton();
        if(_panel && (!valid || !ui || ui->GetMenuOpen("PauseMenu") || ui->GetMenuOpen("PipboyMenu") ||
            ui->GetMenuOpen("LoadingMenu") || ui->GetMenuOpen("MainMenu"))) close();
        if (_openRequested) { _openRequested=false; open(); }
        if (_closeRequested) close();
        if(_panel && _rebuildRequested){
            _rebuildRequested=false;vrui::g_uiManager->detachElement(_panel,true);_panel.reset();
            _positionButton.reset();_tiltButton.reset();buildPanel();
        }
        if(_panel) {
            const auto system=vr::VRSystem();
            const auto left=system ? system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand) : vr::k_unTrackedDeviceIndexInvalid;
            const auto right=system ? system->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) : vr::k_unTrackedDeviceIndexInvalid;
            if(!system || left==vr::k_unTrackedDeviceIndexInvalid || right==vr::k_unTrackedDeviceIndexInvalid ||
                !system->IsTrackedDeviceConnected(left) || !system->IsTrackedDeviceConnected(right)) close();
            else if(!_help)updateSticks();
        }
        // Callbacks only request closure; detaching during the manager's iteration is unsafe.
        Adapter adapter; vrui::g_uiManager->onFrameUpdate(&adapter);
        } catch (...) {
            close(false); // never retain our input claim after a failed update
            throw;
        }
    }
}
