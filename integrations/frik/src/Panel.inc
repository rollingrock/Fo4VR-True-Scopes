    void FO4VRScopeCalibration::buildPanel()
    {
        _acceptAfter=std::chrono::steady_clock::now()+std::chrono::milliseconds(350);
        using namespace vrui;using C=optics_ui::Command;
        _panel=std::make_shared<UIContainer>("TrueScopesDock",UIContainerLayout::VerticalCenter,optics_ui::verticalGap,optics_ui::scale);
        auto text=std::make_shared<UITextPanel>("ScopeDockReadout",16.f);
        text->setTextHeight(.36f);text->setLineSpacing(1.2f);text->setAlign(f4cf::render::TextAlign::Center);
        text->setContent([this](std::vector<TextRow>& rows){
            rows.push_back({.text="True Scopes"});
            if(_help){
                rows.push_back({.text="Position: support stick = left / right and up / down."});
                rows.push_back({.text="Primary stick = size / depth. Tilt mode = pitch / yaw."});
                rows.push_back({.text="Save keeps edits. Back keeps preview. Cancel undoes."});return;
            }
            std::string title=_weaponName+" | "+_state.optic;if(title.size()>76)title=title.substr(0,73)+"...";
            rows.push_back({.text=title});
            rows.push_back({.text=_lastTilt?std::format("TILT  pitch {:+.1f}  yaw {:+.1f} | {}",_state.values.pitch,_state.values.yaw,_fine?"FINE":"COARSE"):
                std::format("XYZ {:+.2f}/{:+.2f}/{:+.2f}  Size {:.0f}% | {}",_state.values.right,_state.values.forward,_state.values.up,_state.values.size*100,_fine?"FINE":"COARSE")});
            rows.push_back({.text=_status});
        });_panel->addElement(text);
        auto activate=[this](C c){
            if(std::chrono::steady_clock::now()<_acceptAfter){
                if(_positionButton)_positionButton->setToggleState(!_tiltSelected);
                if(_tiltButton)_tiltButton->setToggleState(_tiltSelected);
                return;
            }
            switch(c){
            case C::ScopePosition:_tiltSelected=false;_motion.reset();break;
            case C::ScopeTilt:_tiltSelected=true;_motion.reset();break;
            case C::Speed:_fine=!_fine;_motion.reset();break;
            case C::Save:save();break;
            case C::Reset:if(apply({}))_status="Baseline preview - Save to keep";break;
            case C::Cancel:_closeRequested=true;break;
            case C::ScopeHelp:_help=true;_rebuildRequested=true;_motion.reset();break;
            case C::ScopeBack:_help=false;_rebuildRequested=true;_motion.reset();break;
            default:break;
            }
            if(_positionButton)_positionButton->setToggleState(!_tiltSelected);
            if(_tiltButton)_tiltButton->setToggleState(_tiltSelected);
        };
        for (const auto& cells : (_help ? optics_ui::helpRows : optics_ui::scopeRows)) {
            auto row=std::make_shared<UIContainer>("Buttons",UIContainerLayout::HorizontalCenter,optics_ui::rowGap);bool any=false;
            for(auto c:cells){
                if(c==C::None)continue;any=true;
                if(c==C::ScopePosition||c==C::ScopeTilt){
                    auto b=std::make_shared<UIToggleButtonPanel>(optics_ui::label(c));
                    b->setMiddleText(optics_ui::label(c));
                    b->setScale(optics_ui::buttonScale);
                    b->setUnToggleAllowed(false);
                    b->setToggleState(c==C::ScopeTilt?_tiltSelected:!_tiltSelected);
                    b->setOnToggleHandler([activate,c](UIToggleButtonPanel*,bool on){if(on)activate(c);});
                    if(c==C::ScopePosition)_positionButton=b;else _tiltButton=b;row->addElement(b);
                }else{
                    auto b = std::make_shared<UIButtonPanel>(optics_ui::label(c));
                    b->setMiddleText(optics_ui::label(c));
                    b->setScale(optics_ui::buttonScale);
                    b->setOnPressHandler([activate,c](UIButtonPanel*) { activate(c); });
                    row->addElement(b);
                }
            }
            if(any)_panel->addElement(row);
        }
        g_uiManager->attachPresetToPrimaryWandLeft(_panel,{g_config.panelX,g_config.panelY,g_config.panelZ});
    }
