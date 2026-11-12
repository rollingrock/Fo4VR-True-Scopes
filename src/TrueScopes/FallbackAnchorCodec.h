#pragma once
#include "FallbackAnchorMath.h"
#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <stdexcept>
namespace TrueScopes::FallbackAnchor {
using Records=std::map<std::string,Anchor>;
inline Records decode(const nlohmann::json&j){
    if(j.at("version")!=1||!j.at("anchors").is_object()||j.at("anchors").size()>256)throw std::runtime_error("Invalid anchor schema");
    Records result;
    for(const auto&[key,value]:j.at("anchors").items()){
        Anchor a{value.at("position").get<V>(),value.at("rotation").get<M>(),value.at("scale").get<float>()};
        if(key.empty()||key.size()>8192||!valid(a))throw std::runtime_error("Invalid anchor record");
        result.emplace(key,a);
    }
    return result;
}
inline nlohmann::json encode(const Records&records){
    nlohmann::json entries=nlohmann::json::object();
    for(const auto&[key,a]:records)entries[key]={{"position",a.position},{"rotation",a.rotation},{"scale",a.scale}};
    return {{"version",1},{"anchors",entries}};
}
}
