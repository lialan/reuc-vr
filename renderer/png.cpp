#include "vulkan_renderer.h"
#include <algorithm>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#else
#include "file_open.h"
#include "miniz.h"
#include <cstdio>
#include <memory>
#endif

namespace uc {
void save_png(const RenderPixels& pixels,const std::filesystem::path& path) {
    if (!pixels.width || !pixels.height || pixels.width>UINT32_MAX/4 ||
        pixels.height>std::numeric_limits<size_t>::max()/(size_t(pixels.width)*4) ||
        pixels.rgba.size()!=size_t(pixels.width)*pixels.height*4 || pixels.rgba.size()>UINT32_MAX)
        throw std::invalid_argument("invalid PNG readback dimensions/data");
#ifndef _WIN32
    // miniz (already linked for mod archives): the same opaque 8-bit RGBA PNG the WIC encoder writes.
    auto rgba=pixels.rgba;
    for (size_t i=3;i<rgba.size();i+=4) rgba[i]=255;   // presentation is opaque; EFB alpha carries game data rather than window transparency
    size_t size=0;
    std::unique_ptr<void,void(*)(void*)> png(tdefl_write_image_to_png_file_in_memory_ex(rgba.data(),int(pixels.width),int(pixels.height),4,&size,6,MZ_FALSE),mz_free);
    if (!png) throw std::runtime_error("PNG encoding failed");
    std::unique_ptr<std::FILE,int(*)(std::FILE*)> file(uc_fopen(path,"wb"),std::fclose);
    if (!file || std::fwrite(png.get(),1,size,file.get())!=size) throw std::runtime_error("cannot write PNG "+path.string());
#else
    auto require=[](HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("PNG encoding failed: HRESULT "+std::to_string(static_cast<uint32_t>(hr))); };
    struct ComSession {
        HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        ~ComSession() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com;
    if (com.result!=RPC_E_CHANGED_MODE) require(com.result);
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    require(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(factory.GetAddressOf())));
    ComPtr<IWICStream> stream; require(factory->CreateStream(&stream));
    require(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; require(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));
    require(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; require(encoder->CreateNewFrame(&frame,nullptr));
    require(frame->Initialize(nullptr)); require(frame->SetSize(pixels.width,pixels.height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    require(frame->SetPixelFormat(&format));
    if (format!=GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG encoder changed required pixel format");
    auto bgra=pixels.rgba;
    // Presentation is opaque; EFB alpha carries game data rather than window transparency.
    for (size_t i=0;i<bgra.size();i+=4) { std::swap(bgra[i],bgra[i+2]); bgra[i+3]=255; }
    require(frame->WritePixels(pixels.height,pixels.width*4,UINT(bgra.size()),bgra.data()));
    require(frame->Commit()); require(encoder->Commit());
#endif
}
}
