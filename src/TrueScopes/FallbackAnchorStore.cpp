#include "FallbackAnchorStore.h"
#include "FallbackAnchorCodec.h"
#include <ShlObj.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
namespace TrueScopes::FallbackAnchor {
namespace {
std::mutex mutex;
std::map<std::string,Anchor> anchors;
bool loaded=false,writable=true;
std::uint64_t revision=0,flushed=0;
std::filesystem::path file;
}
std::optional<Anchor> get(const std::string& key){
    std::scoped_lock guard(mutex);
    const auto i=anchors.find(key);
    return i==anchors.end()?std::nullopt:std::optional(i->second);
}
bool insert(const std::string& key,const Anchor&a){
    if(key.empty()||key.size()>8192||!valid(a))return false;
    std::scoped_lock guard(mutex);
    if(!loaded||anchors.size()>=256)return false;
    const bool added=anchors.try_emplace(key,a).second;
    if(added){++revision;logger::info(FMT_STRING("FALLBACK ANCHOR captured (weapon-local baseline): {}"),key);}
    return added;
}
void service(){
    // Called on one game thread only, before taking Calibration's session lock.
    try {
        if(!loaded){
            PWSTR docs=nullptr;
            if(FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&docs)))throw std::runtime_error("Documents path unavailable");
            file=std::filesystem::path(docs)/"My Games/Fallout4VR/Mods_Config/FO4VRScopeCalibration/fallback-anchors-v1.json";
            CoTaskMemFree(docs);
            std::map<std::string,Anchor> recovered;
            if(std::filesystem::exists(file)){
                if(std::filesystem::file_size(file)>4*1024*1024)throw std::runtime_error("Anchor cache too large");
                std::ifstream in(file);auto j=nlohmann::json::parse(in);
                recovered=decode(j);
            }
            const auto count=recovered.size();
            {std::scoped_lock guard(mutex);anchors=std::move(recovered);loaded=true;}
            logger::info(FMT_STRING("FALLBACK ANCHOR cache ready: {} baselines; personal profiles untouched"),count);
        }
        nlohmann::json records=nlohmann::json::object();std::uint64_t pending=0;
        {
            std::scoped_lock guard(mutex);
            if(!writable||revision==flushed)return;
            pending=revision;
            for(const auto&[key,a]:anchors)records[key]={{"position",a.position},{"rotation",a.rotation},{"scale",a.scale}};
        }
        std::filesystem::create_directories(file.parent_path());
        auto tmp=file;tmp+=".tmp";
        if(std::filesystem::exists(file)){auto backup=file;backup+=".bak";std::filesystem::copy_file(file,backup,std::filesystem::copy_options::overwrite_existing);}
        {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);out<<nlohmann::json{{"version",1},{"anchors",records}}.dump(2);out.flush();if(!out)throw std::runtime_error("Anchor cache write failed");}
        if(!MoveFileExW(tmp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Anchor cache replacement failed");
        {std::scoped_lock guard(mutex);flushed=pending;}
        logger::info(FMT_STRING("FALLBACK ANCHOR cache saved at revision {}"),pending);
    }catch(const std::exception&e){
        // Preserve malformed/failed storage. RAM anchoring still stabilizes this
        // session; never overwrite a user profile or retry I/O every frame.
        std::scoped_lock guard(mutex);loaded=true;writable=false;
        logger::error(FMT_STRING("FALLBACK ANCHOR storage preserved; writes disabled: {}"),e.what());
    }
}
}
