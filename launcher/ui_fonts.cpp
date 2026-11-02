#include "ui_fonts.h"
#include <imgui.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include "system_fonts.h"
namespace uc::ui {
using uc::UiFont; using uc::ui_font_files;
Fonts load_fonts(ImFontAtlas& atlas) {
    Fonts fonts;
    const auto sans_files=ui_font_files(UiFont::sans),serif_files=ui_font_files(UiFont::serif);
    if(sans_files.empty() || serif_files.empty())throw std::runtime_error("system font directory unavailable");
    // System fonts keep the distribution small; ImGui rasterizes glyphs on demand.
    fonts.bytes.reserve(5);   // the atlas keeps pointers into these buffers
    for(const auto& file:sans_files) {
        std::ifstream source(file,std::ios::binary);
        std::vector<char> data(std::istreambuf_iterator<char>(source),{});
        std::fprintf(stderr,"[ui-font] %s bytes=%zu%s\n",file.filename().string().c_str(),data.size(),data.empty() ? " (missing)" : "");
        if(data.empty()) {if(fonts.bytes.empty())throw std::runtime_error("UI font unavailable: "+file.string());continue;}
        if(data.size()>64*1024*1024)throw std::runtime_error("UI font too large");
        ImFontConfig config;config.FontDataOwnedByAtlas=false;config.MergeMode=!fonts.bytes.empty();
        fonts.bytes.push_back(std::move(data));auto& bytes=fonts.bytes.back();
        if(!atlas.AddFontFromMemoryTTF(bytes.data(),int(bytes.size()),16,&config))throw std::runtime_error("cannot load UI font");
    }
    fonts.sans=atlas.Fonts[0];
    std::ifstream serif_source(serif_files.front(),std::ios::binary);
    std::vector<char> serif_data(std::istreambuf_iterator<char>(serif_source),{});
    std::fprintf(stderr,"[ui-font] %s bytes=%zu%s\n",serif_files.front().filename().string().c_str(),serif_data.size(),serif_data.empty() ? " (missing)" : "");
    if(serif_data.empty())throw std::runtime_error("UI serif font unavailable: "+serif_files.front().string());
    const size_t fallback_count=fonts.bytes.size();fonts.bytes.push_back(std::move(serif_data));
    ImFontConfig serif_config;serif_config.FontDataOwnedByAtlas=false;
    fonts.serif=atlas.AddFontFromMemoryTTF(fonts.bytes.back().data(),int(fonts.bytes.back().size()),22,&serif_config);
    if(!fonts.serif)throw std::runtime_error("cannot load UI serif font");
    for(size_t i=1;i<fallback_count;++i) {
        ImFontConfig fallback;fallback.FontDataOwnedByAtlas=false;fallback.MergeMode=true;
        if(!atlas.AddFontFromMemoryTTF(fonts.bytes[i].data(),int(fonts.bytes[i].size()),22,&fallback))throw std::runtime_error("cannot merge serif fallback");
    }
    return fonts;
}
}
