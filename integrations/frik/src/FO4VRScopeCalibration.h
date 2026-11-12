#pragma once

#include "Config.h"
#include "ModBase.h"
#include "TrueScopes/CalibrationAPI.h"
#include "vrui/UIContainer.h"
#include "vrui/UIToggleButtonPanel.h"
#include <nlohmann/json.hpp>
#include "StickCalibration.h"
#include <chrono>

namespace my_mod
{
    class FO4VRScopeCalibration : public ModBase
    {
    public:
        FO4VRScopeCalibration()
            : ModBase(Settings("FO4VRScopeCalibration", "0.7.0", &g_config, 32, true))
        { _settings.devbenchTool = false; }

    protected:
        virtual void onModLoaded(const F4SE::LoadInterface* f4SE) override;
        virtual void onGameLoaded() override;
        virtual void onGameSessionLoaded() override;
        virtual void onFrameUpdate() override;

    private:
        void open();
        void buildPanel();
        void close(bool restorePreview=true);
        void updateSticks();
        void save();
        bool apply(const TrueScopes::Calibration::Values& value);
        std::string key(const TrueScopes::Calibration::State& state);
        static void message(F4SE::MessagingInterface::Message* msg);
        const TrueScopes::Calibration::API* _api = nullptr;
        bool _menuRegistered = false;
        TrueScopes::Calibration::State _state{};
        TrueScopes::Calibration::Values _start{};
        std::shared_ptr<f4cf::vrui::UIContainer> _panel;
        nlohmann::json _profiles = nlohmann::json::object();
        std::string _key, _status, _weaponName;
        std::uint64_t _seenEpoch = 0;
        bool _fine = true, _storageOK = true, _closeRequested = false, _openRequested = false;
        std::shared_ptr<f4cf::vrui::UIToggleButtonPanel> _positionButton,_tiltButton;
        bool _help=false,_rebuildRequested=false;
        bool _tiltSelected=false, _lastTilt=false, _inputClaimed=false;
        scope_controls::Motion _motion;
        std::chrono::steady_clock::time_point _lastStickFrame{};
        std::chrono::steady_clock::time_point _acceptAfter{};
    };

    // The ONE global to rule them ALL
    inline FO4VRScopeCalibration g_myMod;
}
