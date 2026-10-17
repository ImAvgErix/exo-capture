// Exo Capture - shared helpers. GPL-3.0-or-later.
#include "common.h"

#include <shlobj.h>
#include <shellapi.h>
#include <wincodec.h>

#include <cstdio>

namespace
{
std::mutex g_out;
}

void Log(const std::string& line)
{
    std::lock_guard lock(g_out);
    fprintf(stderr, "%s\n", line.c_str());
    fflush(stderr);
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& text)
{
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

static void WriteLine(const std::wstring& line)
{
    auto utf8 = Narrow(line);
    std::lock_guard lock(g_out);
    fwrite(utf8.data(), 1, utf8.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void Emit(const wchar_t* name, const json::JsonObject& data)
{
    json::JsonObject line;
    line.Insert(L"event", json::JsonValue::CreateStringValue(name));
    line.Insert(L"data", data);
    WriteLine(std::wstring(line.Stringify()));
}

void Reply(double id, const json::IJsonValue& result)
{
    json::JsonObject line;
    line.Insert(L"id", json::JsonValue::CreateNumberValue(id));
    line.Insert(L"result", result);
    WriteLine(std::wstring(line.Stringify()));
}

void ReplyError(double id, const std::string& message)
{
    json::JsonObject line;
    line.Insert(L"id", json::JsonValue::CreateNumberValue(id));
    line.Insert(L"error", json::JsonValue::CreateStringValue(Widen(message)));
    WriteLine(std::wstring(line.Stringify()));
}

int64_t Now100ns()
{
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (now.QuadPart / freq) * 10'000'000 + (now.QuadPart % freq) * 10'000'000 / freq;
}

std::wstring Stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t text[64];
    swprintf_s(text, L"%04u-%02u-%02u %02u.%02u.%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return text;
}

std::wstring SafeName(const std::wstring& name)
{
    std::wstring out;
    for (wchar_t c : name)
    {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) out += L' ';
        else out += c;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(out.begin());
    if (out.size() > 80) out.resize(80);
    return out.empty() ? L"Capture" : out;
}

std::wstring FreePath(const std::wstring& folder, const std::wstring& base, const std::wstring& ext)
{
    std::wstring path = folder + L"\\" + base + ext;
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && n < 1000; ++n)
        path = folder + L"\\" + base + L" (" + std::to_wstring(n) + L")" + ext;
    return path;
}

bool EnsureFolder(const std::wstring& folder)
{
    int result = SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

json::JsonObject Obj() { return json::JsonObject(); }

std::wstring GetString(const json::JsonObject& o, const wchar_t* key, const std::wstring& fallback)
{
    if (!o || !o.HasKey(key)) return fallback;
    auto v = o.Lookup(key);
    return v.ValueType() == json::JsonValueType::String ? std::wstring(v.GetString()) : fallback;
}

double GetNumber(const json::JsonObject& o, const wchar_t* key, double fallback)
{
    if (!o || !o.HasKey(key)) return fallback;
    auto v = o.Lookup(key);
    return v.ValueType() == json::JsonValueType::Number ? v.GetNumber() : fallback;
}

bool GetBool(const json::JsonObject& o, const wchar_t* key, bool fallback)
{
    if (!o || !o.HasKey(key)) return fallback;
    auto v = o.Lookup(key);
    return v.ValueType() == json::JsonValueType::Boolean ? v.GetBoolean() : fallback;
}

void SaveImage(const std::wstring& path, const uint8_t* bgra, UINT width, UINT height, UINT stride, bool png, UINT maxWidth)
{
    com_ptr<IWICImagingFactory> wic;
    Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())), "WIC factory");
    com_ptr<IWICBitmap> bitmap;
    Check(wic->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, stride, stride * height,
        const_cast<BYTE*>(bgra), bitmap.put()), "WIC bitmap");
    com_ptr<IWICBitmapSource> source = bitmap;
    UINT outW = width, outH = height;
    if (maxWidth && width > maxWidth)
    {
        outW = maxWidth;
        outH = std::max<UINT>(1, static_cast<UINT>(static_cast<uint64_t>(height) * maxWidth / width));
        com_ptr<IWICBitmapScaler> scaler;
        Check(wic->CreateBitmapScaler(scaler.put()), "WIC scaler");
        Check(scaler->Initialize(bitmap.get(), outW, outH, WICBitmapInterpolationModeFant), "WIC scale");
        source = scaler;
    }
    std::wstring temp = path + L".part";
    com_ptr<IWICStream> stream;
    Check(wic->CreateStream(stream.put()), "WIC stream");
    Check(stream->InitializeFromFilename(temp.c_str(), GENERIC_WRITE), "image file");
    {
        com_ptr<IWICBitmapEncoder> encoder;
        Check(wic->CreateEncoder(png ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg, nullptr, encoder.put()), "image encoder");
        Check(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache), "image encoder init");
        com_ptr<IWICBitmapFrameEncode> frame;
        com_ptr<IPropertyBag2> props;
        Check(encoder->CreateNewFrame(frame.put(), props.put()), "image frame");
        if (!png && props)
        {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value{};
            value.vt = VT_R4;
            value.fltVal = 0.85f;
            props->Write(1, &option, &value);
        }
        Check(frame->Initialize(props.get()), "image frame init");
        Check(frame->SetSize(outW, outH), "image size");
        WICPixelFormatGUID format = png ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
        Check(frame->SetPixelFormat(&format), "image format");
        com_ptr<IWICBitmapSource> converted = source;
        if (format != GUID_WICPixelFormat32bppBGRA)
        {
            com_ptr<IWICFormatConverter> converter;
            Check(wic->CreateFormatConverter(converter.put()), "WIC converter");
            Check(converter->Initialize(source.get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "WIC convert");
            converted = converter;
        }
        Check(frame->WriteSource(converted.get(), nullptr), "image write");
        Check(frame->Commit(), "image commit");
        Check(encoder->Commit(), "image file commit");
    }
    stream = nullptr;
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(temp.c_str());
        throw Failure(HRESULT_FROM_WIN32(GetLastError()), "image rename");
    }
}

bool ReadImageFile(const std::wstring& path, std::vector<uint8_t>& pixels, UINT& width, UINT& height)
{
    com_ptr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())))) return false;
    com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) return false;
    com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) return false;
    com_ptr<IWICFormatConverter> converter;
    wic->CreateFormatConverter(converter.put());
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    converter->GetSize(&width, &height);
    if (!width || !height || width > 32768 || height > 32768) return false;
    pixels.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
}

static bool OpenClipboardRetry(HWND owner)
{
    for (int i = 0; i < 10; ++i)
    {
        if (OpenClipboard(owner)) return true;
        Sleep(20);
    }
    return false;
}

bool CopyImageToClipboard(HWND owner, const uint8_t* bgra, UINT width, UINT height, UINT stride)
{
    size_t rowBytes = static_cast<size_t>(width) * 4;
    size_t size = sizeof(BITMAPINFOHEADER) + rowBytes * height;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return false;
    auto* header = static_cast<BITMAPINFOHEADER*>(GlobalLock(memory));
    *header = {};
    header->biSize = sizeof(BITMAPINFOHEADER);
    header->biWidth = static_cast<LONG>(width);
    header->biHeight = static_cast<LONG>(height); // bottom-up
    header->biPlanes = 1;
    header->biBitCount = 32;
    header->biCompression = BI_RGB;
    header->biSizeImage = static_cast<DWORD>(rowBytes * height);
    auto* pixels = reinterpret_cast<uint8_t*>(header + 1);
    for (UINT y = 0; y < height; ++y)
    {
        auto* dst = pixels + (height - 1 - y) * rowBytes;
        memcpy(dst, bgra + static_cast<size_t>(y) * stride, rowBytes);
        for (UINT x = 0; x < width; ++x) dst[x * 4 + 3] = 255;
    }
    GlobalUnlock(memory);
    if (!OpenClipboardRetry(owner)) { GlobalFree(memory); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_DIB, memory) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(memory);
    return ok;
}

bool CopyFileToClipboard(HWND owner, const std::wstring& path)
{
    size_t chars = path.size() + 2; // path, terminator, list terminator
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    if (!memory) return false;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    memcpy(reinterpret_cast<uint8_t*>(drop) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(memory);

    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (effect) { *static_cast<DWORD*>(GlobalLock(effect)) = DROPEFFECT_COPY; GlobalUnlock(effect); }

    if (!OpenClipboardRetry(owner)) { GlobalFree(memory); if (effect) GlobalFree(effect); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_HDROP, memory) != nullptr;
    if (!ok) GlobalFree(memory);
    if (effect && !SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect)) GlobalFree(effect);
    CloseClipboard();
    return ok;
}

void ShowInFolder(const std::wstring& path)
{
    PIDLIST_ABSOLUTE item = nullptr;
    if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr)) && item)
    {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        CoTaskMemFree(item);
        return;
    }
    auto slash = path.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        ShellExecuteW(nullptr, L"open", path.substr(0, slash).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
