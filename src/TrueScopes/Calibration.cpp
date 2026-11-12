#include "Calibration.h"
#include "CalibrationMath.h"
#include "CalibrationSession.h"
#include "ScopeIdent.h"
#include "FallbackAnchorStore.h"
#include <nlohmann/json.hpp>
#include <format>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <mutex>

namespace TrueScopes::Calibration
{
    namespace {
        std::mutex lock;
        Session session;
        std::atomic_bool hasAdjustment{false};
        DWORD gameThread = 0;
        std::string anchorKey;
        const char* opticKey(const ScopeIdent::Info& info)
        {
            if (info.matched[0]) return info.matched;
            if (info.overrideKey[0]) return info.overrideKey;
            return "fallback";
        }
        std::string stableForm(std::uint32_t id){
            const auto* form=RE::TESForm::GetFormByID(id);
            const auto* file=form?form->GetFile(0):nullptr;
            if(!file||id>=0xff000000)return {};
            std::string name(file->GetFilename());
            for(auto& c:name)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return std::format("{}:{:06x}",name,id&(file->IsLight()?0xfff:0xffffff));
        }
        std::string stableKey(const State& state){
            const auto weapon=stableForm(state.weapon);if(weapon.empty())return {};
            std::vector<std::string> mods;
            for(unsigned i=0;i<state.modCount;++i){auto mod=stableForm(state.mods[i]);if(mod.empty())return {};mods.push_back(mod);}
            std::sort(mods.begin(),mods.end());
            return nlohmann::json::array({weapon,"fallback",mods}).dump();
        }

        std::uint32_t __cdecl Read(State* out)
        {
            if (!out || out->bytes != sizeof(State)) return 0;
            const auto tid = ::GetCurrentThreadId();
            if (!gameThread) gameThread = tid;
            if (gameThread != tid) return 0;
            FallbackAnchor::service();
            State next{};
            next.probe = ScopeIdent::ProbeCount();
            const auto info = ScopeIdent::Get();
            if (next.probe != ScopeIdent::ProbeCount()) return 0;
            if (info.probed && !info.faulted && !info.modOverflow && info.modCount <= 24) {
                next.ready = 1;
                next.weapon = info.weaponFormID;
                std::snprintf(next.optic, sizeof(next.optic), "%s", opticKey(info));
                next.modCount = info.modCount;
                for (std::uint32_t i = 0; i < next.modCount; ++i) next.mods[i] = info.mods[i].formID;
            }
            const std::scoped_lock guard(lock);
            const auto previous=session.state().epoch;
            session.observe(next);
            *out = session.state();
            hasAdjustment.store(out->ready && !Identity(out->values), std::memory_order_relaxed);
            if(out->epoch!=previous || (!out->ready && !anchorKey.empty()))
                anchorKey=out->ready && std::strcmp(out->optic,"fallback")==0?stableKey(*out):std::string{};
            return 1;
        }
        std::uint32_t __cdecl Set(std::uint64_t epoch, const Values* values)
        {
            if (!values || !Valid(*values) || gameThread != ::GetCurrentThreadId()) return 0;
            State fresh{};
            if (!Read(&fresh) || !fresh.ready || fresh.epoch != epoch) return 0;
            const std::scoped_lock guard(lock);
            if (!session.set(epoch, *values)) return 0;
            hasAdjustment.store(!Identity(*values), std::memory_order_relaxed);
            return 1;
        }
        const API api{sizeof(API), kVersion, Read, Set};
    }
    bool ForRender(Values& out, std::string& fallbackKey)
    {
        if (!hasAdjustment.load(std::memory_order_relaxed)) return false;
        const auto probe = ScopeIdent::ProbeCount();
        const auto info = ScopeIdent::Get();
        if (probe != ScopeIdent::ProbeCount() || !info.probed || info.faulted || info.modOverflow) return false;
        char optic[128]{};
        std::snprintf(optic, sizeof(optic), "%s", opticKey(info));
        const std::scoped_lock guard(lock);
        if (!session.render(info.weaponFormID, optic, probe, out)) return false;
        if (!info.fromTable && !info.haveFace) fallbackKey = anchorKey;
        return true;
    }
}

extern "C" __declspec(dllexport) const TrueScopes::Calibration::API* __cdecl
TrueScopes_GetCalibrationAPI(std::uint32_t version, std::uint32_t bytes)
{
    using namespace TrueScopes::Calibration;
    return version == kVersion && bytes == sizeof(API) ? &api : nullptr;
}
