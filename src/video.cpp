// Exo Capture - GPU device, Windows.Graphics.Capture, color conversion and the hardware video encoder.
// GPL-3.0-or-later.
#include "video.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <algorithm>

using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// ---------------------------------------------------------------- device

std::shared_ptr<GpuDevice> GpuDevice::Create()
{
    auto gpu = std::make_shared<GpuDevice>();
    com_ptr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "DXGI factory");
    com_ptr<IDXGIAdapter1> adapter;
    factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.put()));
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    Check(D3D11CreateDevice(adapter.get(), adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, gpu->device.put(), nullptr, gpu->context.put()), "D3D11 device");
    gpu->device.as<ID3D10Multithread>()->SetMultithreadProtected(TRUE);

    auto dxgi = gpu->device.as<IDXGIDevice>();
    // The game's frames go first; a converted frame waits for a gap (each session sets its own).
    dxgi->SetGPUThreadPriority(-7);
    com_ptr<IDXGIAdapter> used;
    dxgi->GetAdapter(used.put());
    DXGI_ADAPTER_DESC desc{};
    used->GetDesc(&desc);
    gpu->luid = desc.AdapterLuid;
    gpu->adapter = desc.Description;

    UINT token = 0;
    Check(MFCreateDXGIDeviceManager(&token, gpu->manager.put()), "DXGI device manager");
    Check(gpu->manager->ResetDevice(gpu->device.get(), token), "device manager reset");

    winrt::com_ptr<::IInspectable> inspectable;
    Check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()), "WinRT device");
    gpu->winrt = inspectable.as<IDirect3DDevice>();
    return gpu;
}

const wchar_t* CodecName(Codec codec)
{
    switch (codec)
    {
    case Codec::AV1: return L"AV1";
    case Codec::HEVC: return L"HEVC";
    default: return L"H.264";
    }
}

static GUID Subtype(Codec codec)
{
    switch (codec)
    {
    case Codec::AV1: return MFVideoFormat_AV1;
    case Codec::HEVC: return MFVideoFormat_HEVC;
    default: return MFVideoFormat_H264;
    }
}

namespace
{
/// The MFTs for a category and types; true and the first one's name when any matches the adapter.
bool FindMft(const GUID& category, UINT32 flags, const GUID& inSub, const GUID& outSub, const LUID* luid,
    std::wstring* name, com_ptr<IMFActivate>* chosen)
{
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, inSub };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, outSub };
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(category, flags, inSub == GUID_NULL ? nullptr : &in, outSub == GUID_NULL ? nullptr : &out, &list, &count)))
        return false;
    bool found = false;
    for (UINT32 i = 0; i < count; ++i)
    {
        if (!found)
        {
            bool match = true;
            if (luid)
            {
                LUID adapter{};
                UINT32 size = 0;
                if (SUCCEEDED(list[i]->GetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&adapter), sizeof(adapter), &size)))
                    match = adapter.LowPart == luid->LowPart && adapter.HighPart == luid->HighPart;
            }
            if (match)
            {
                found = true;
                if (name)
                {
                    wchar_t* text = nullptr;
                    UINT32 length = 0;
                    if (SUCCEEDED(list[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &text, &length)))
                    {
                        *name = text;
                        CoTaskMemFree(text);
                    }
                }
                if (chosen) chosen->copy_from(list[i]);
            }
        }
        list[i]->Release();
    }
    CoTaskMemFree(list);
    return found;
}
}

Codec EncoderSupport::Best() const
{
    if (av1 && av1Playback) return Codec::AV1;
    if (hevc && hevcPlayback) return Codec::HEVC;
    return Codec::H264;
}

EncoderSupport ProbeEncoders(const GpuDevice& gpu)
{
    EncoderSupport s;
    const UINT32 hw = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    std::wstring name;
    s.h264 = FindMft(MFT_CATEGORY_VIDEO_ENCODER, hw, MFVideoFormat_NV12, MFVideoFormat_H264, &gpu.luid, &name, nullptr);
    if (s.h264) s.name = name;
    s.hevc = FindMft(MFT_CATEGORY_VIDEO_ENCODER, hw, MFVideoFormat_NV12, MFVideoFormat_HEVC, &gpu.luid, &name, nullptr);
    if (s.hevc) s.name = name;
    s.av1 = FindMft(MFT_CATEGORY_VIDEO_ENCODER, hw, MFVideoFormat_NV12, MFVideoFormat_AV1, &gpu.luid, &name, nullptr);
    if (s.av1) s.name = name;
    const UINT32 any = MFT_ENUM_FLAG_ALL;
    s.hevcPlayback = FindMft(MFT_CATEGORY_VIDEO_DECODER, any, MFVideoFormat_HEVC, GUID_NULL, nullptr, nullptr, nullptr);
    s.av1Playback = FindMft(MFT_CATEGORY_VIDEO_DECODER, any, MFVideoFormat_AV1, GUID_NULL, nullptr, nullptr, nullptr);
    return s;
}

UINT BitrateFor(Codec codec, UINT width, UINT height, UINT fps, int quality)
{
    // Bits per pixel per frame from "Smaller files" (0.04) to "Best quality" (0.20).
    double bpp = 0.04 + std::clamp(quality, 0, 100) / 100.0 * 0.16;
    if (codec == Codec::H264) bpp *= 1.4;
    else if (codec == Codec::AV1) bpp *= 0.85;
    // Frames past 60 per second differ less from each other.
    double rate = std::min<double>(fps, 60) + std::max<double>(0, fps - 60.0) * 0.5;
    double bits = bpp * width * height * rate;
    return static_cast<UINT>(std::clamp(bits, 2e6, 130e6));
}

UINT RefreshRate(HMONITOR monitor)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return 60;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency < 24) return 60;
    return mode.dmDisplayFrequency;
}

bool MonitorIsHdr(HMONITOR monitor)
{
    com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) return false;
    com_ptr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a, adapter = nullptr)
    {
        com_ptr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o, output = nullptr)
        {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (desc.Monitor != monitor) continue;
            if (auto six = output.try_as<IDXGIOutput6>())
            {
                DXGI_OUTPUT_DESC1 desc1{};
                if (SUCCEEDED(six->GetDesc1(&desc1))) return desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------- encoder

namespace
{
struct ReleaseCallback : winrt::implements<ReleaseCallback, IMFAsyncCallback>
{
    std::function<void()> done;
    explicit ReleaseCallback(std::function<void()> fn) : done(std::move(fn)) {}
    HRESULT __stdcall GetParameters(DWORD*, DWORD*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall Invoke(IMFAsyncResult*) noexcept override
    {
        if (done) done();
        done = nullptr;
        return S_OK;
    }
};

void SetApi(ICodecAPI* api, const GUID& key, UINT32 value)
{
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = value;
    api->SetValue(&key, &v);
}

void SetApiBool(ICodecAPI* api, const GUID& key, bool value)
{
    VARIANT v{};
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &v);
}

/// The parameter sets (VPS/SPS/PPS) at the head of an Annex B key frame.
std::vector<uint8_t> ParameterSets(const std::vector<uint8_t>& data, Codec codec)
{
    std::vector<uint8_t> out;
    size_t i = 0, n = data.size();
    auto startCode = [&](size_t at, size_t& length) {
        if (at + 3 <= n && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1) { length = 3; return true; }
        if (at + 4 <= n && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 0 && data[at + 3] == 1) { length = 4; return true; }
        return false;
    };
    size_t sc = 0;
    while (i < n && !startCode(i, sc)) ++i;
    while (i < n)
    {
        size_t begin = i + sc;
        size_t j = begin;
        while (j < n && !startCode(j, sc)) ++j;
        if (begin < j)
        {
            uint8_t header = data[begin];
            bool keep = codec == Codec::HEVC
                ? ((header >> 1) & 0x3F) >= 32 && ((header >> 1) & 0x3F) <= 34
                : (header & 0x1F) == 7 || (header & 0x1F) == 8;
            if (keep)
            {
                static const uint8_t code[] = { 0, 0, 0, 1 };
                out.insert(out.end(), code, code + 4);
                out.insert(out.end(), data.begin() + begin, data.begin() + j);
            }
        }
        i = j;
    }
    return out;
}
}

VideoEncoder::VideoEncoder(GpuDevice& gpu, Codec codec, UINT width, UINT height, UINT fps, UINT bitrate, bool hdr,
    std::function<void(PacketPtr)> sink)
    : m_gpu(gpu), m_codec(codec), m_fps(fps), m_width(width), m_height(height), m_sink(std::move(sink))
{
    const GUID input = hdr ? MFVideoFormat_P010 : MFVideoFormat_NV12;
    com_ptr<IMFActivate> activate;
    if (!FindMft(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, input, Subtype(codec),
        &gpu.luid, &m_name, &activate) &&
        !FindMft(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, input, Subtype(codec),
        nullptr, &m_name, &activate))
        throw Failure(MF_E_TOPO_CODEC_NOT_FOUND, "no hardware video encoder");
    Check(activate->ActivateObject(IID_PPV_ARGS(m_mft.put())), "start the video encoder");

    com_ptr<IMFAttributes> attrs;
    if (SUCCEEDED(m_mft->GetAttributes(attrs.put())))
    {
        attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    Check(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(gpu.manager.get())), "encoder GPU");
    if (m_mft->GetStreamIDs(1, &m_in, 1, &m_out) != S_OK) { m_in = 0; m_out = 0; }

    m_api = m_mft.as<ICodecAPI>();
    if (m_api)
    {
        VARIANT mode{};
        mode.vt = VT_UI4;
        mode.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
        if (FAILED(m_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &mode)))
        {
            mode.ulVal = eAVEncCommonRateControlMode_UnconstrainedVBR;
            if (FAILED(m_api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &mode)))
                SetApi(m_api.get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
        }
        SetApi(m_api.get(), CODECAPI_AVEncCommonMeanBitRate, bitrate);
        SetApi(m_api.get(), CODECAPI_AVEncCommonMaxBitRate, static_cast<UINT32>(std::min<double>(bitrate * 1.5, 200e6)));
        SetApi(m_api.get(), CODECAPI_AVEncMPVGOPSize, fps * 2);
        SetApi(m_api.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        SetApiBool(m_api.get(), CODECAPI_AVLowLatencyMode, true);
        // Leans to speed: NVENC, AMF and Quick Sync run beside the game's own GPU work.
        SetApi(m_api.get(), CODECAPI_AVEncCommonQualityVsSpeed, 40);
    }

    com_ptr<IMFMediaType> out;
    Check(MFCreateMediaType(out.put()), "media type");
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, Subtype(codec));
    out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    if (hdr)
    {
        out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT2020);
        out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_2084);
        out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT2020_10);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, codec == Codec::AV1 ? eAVEncAV1VProfile_Main_420_10 : eAVEncH265VProfile_Main_420_10);
    }
    else
    {
        out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, codec == Codec::AV1 ? eAVEncAV1VProfile_Main_420_8
            : codec == Codec::HEVC ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
    }
    Check(m_mft->SetOutputType(m_out, out.get(), 0), "encoder output type");

    com_ptr<IMFMediaType> in;
    for (DWORD i = 0;; ++i)
    {
        com_ptr<IMFMediaType> candidate;
        if (FAILED(m_mft->GetInputAvailableType(m_in, i, candidate.put()))) break;
        GUID sub{};
        candidate->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == input) { in = candidate; break; }
    }
    if (!in)
    {
        Check(MFCreateMediaType(in.put()), "media type");
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, input);
    }
    MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    Check(m_mft->SetInputType(m_in, in.get(), 0), "encoder input type");

    m_events = m_mft.as<IMFMediaEventGenerator>();
    m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    Check(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "encoder start");
    Check(m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "encoder stream start");
    m_thread = std::thread([this] { EventLoop(); });
}

VideoEncoder::~VideoEncoder()
{
    {
        std::lock_guard lock(m_lock);
        m_queue.clear();
    }
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, m_in);
    m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, m_in);
    {
        std::unique_lock lock(m_lock);
        m_changed.wait_for(lock, std::chrono::seconds(2), [this] { return m_drained; });
    }
    if (auto shutdown = m_mft.try_as<IMFShutdown>()) shutdown->Shutdown();
    if (m_thread.joinable()) m_thread.join();
    m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
}

void VideoEncoder::Submit(ID3D11Texture2D* texture, int64_t time, std::function<void()> released)
{
    com_ptr<IMFMediaBuffer> buffer;
    Check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, buffer.put()), "frame buffer");
    if (auto twoD = buffer.try_as<IMF2DBuffer>())
    {
        DWORD length = 0;
        if (SUCCEEDED(twoD->GetContiguousLength(&length))) buffer->SetCurrentLength(length);
    }
    com_ptr<IMFTrackedSample> tracked;
    Check(MFCreateTrackedSample(tracked.put()), "frame sample");
    auto sample = tracked.as<IMFSample>();
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(time);
    sample->SetSampleDuration(10'000'000 / m_fps);
    auto callback = winrt::make<ReleaseCallback>(std::move(released));
    tracked->SetAllocator(callback.as<IMFAsyncCallback>().get(), nullptr);

    std::lock_guard lock(m_lock);
    m_queue.push_back(sample);
    // The encoder fell behind: drop the oldest waiting frame rather than grow.
    while (m_queue.size() > 3) m_queue.pop_front();
    FeedLocked();
}

void VideoEncoder::FeedLocked()
{
    while (m_credits > 0 && !m_queue.empty())
    {
        auto sample = m_queue.front();
        if (m_forceKey.exchange(false) && m_api) SetApi(m_api.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        HRESULT hr = m_mft->ProcessInput(m_in, sample.get(), 0);
        if (hr == MF_E_NOTACCEPTING) break;
        m_queue.pop_front();
        --m_credits;
        if (FAILED(hr))
        {
            Log("Encoder refused a frame: " + std::string(Failure(hr, "ProcessInput").what()));
            break;
        }
    }
}

void VideoEncoder::EventLoop()
{
    for (;;)
    {
        com_ptr<IMFMediaEvent> event;
        if (FAILED(m_events->GetEvent(0, event.put()))) break;
        MediaEventType type = MEUnknown;
        event->GetType(&type);
        if (type == METransformNeedInput)
        {
            std::lock_guard lock(m_lock);
            ++m_credits;
            FeedLocked();
        }
        else if (type == METransformHaveOutput)
        {
            try { TakeOutput(); }
            catch (const std::exception& e) { Log(std::string("Encoder output: ") + e.what()); }
        }
        else if (type == METransformDrainComplete)
        {
            std::lock_guard lock(m_lock);
            m_drained = true;
            m_changed.notify_all();
        }
    }
}

void VideoEncoder::TakeOutput()
{
    MFT_OUTPUT_STREAM_INFO info{};
    m_mft->GetOutputStreamInfo(m_out, &info);
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = m_out;
    com_ptr<IMFSample> own;
    if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
    {
        com_ptr<IMFMediaBuffer> buffer;
        Check(MFCreateMemoryBuffer(std::max<DWORD>(info.cbSize, 4 * 1024 * 1024), buffer.put()), "output buffer");
        Check(MFCreateSample(own.put()), "output sample");
        own->AddBuffer(buffer.get());
        output.pSample = own.get();
    }
    DWORD status = 0;
    HRESULT hr = m_mft->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();
    com_ptr<IMFSample> sample;
    if (own) sample = own;
    else if (output.pSample) sample.attach(output.pSample);
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        com_ptr<IMFMediaType> type;
        if (SUCCEEDED(m_mft->GetOutputAvailableType(m_out, 0, type.put()))) m_mft->SetOutputType(m_out, type.get(), 0);
        return;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return;
    Check(hr, "encode");
    if (!sample) return;

    auto packet = std::make_shared<Packet>();
    packet->track = 0;
    LONGLONG time = 0, duration = 0;
    sample->GetSampleTime(&time);
    sample->GetSampleDuration(&duration);
    packet->time = time;
    packet->duration = duration;
    packet->key = MFGetAttributeUINT32(sample.get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
    com_ptr<IMFMediaBuffer> buffer;
    Check(sample->ConvertToContiguousBuffer(buffer.put()), "encoded data");
    BYTE* bytes = nullptr;
    DWORD length = 0;
    Check(buffer->Lock(&bytes, nullptr, &length), "encoded lock");
    packet->data.assign(bytes, bytes + length);
    buffer->Unlock();

    bool needType = false;
    {
        std::lock_guard lock(m_lock);
        needType = !m_outputType && packet->key;
    }
    if (needType)
    {
        com_ptr<IMFMediaType> current, type;
        Check(m_mft->GetOutputCurrentType(m_out, current.put()), "encoder type");
        Check(MFCreateMediaType(type.put()), "media type");
        current->CopyAllItems(type.get());
        // The file states the picture's own size; the encoder reports its padded block size.
        MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, m_width, m_height);
        type->DeleteItem(MF_MT_MINIMUM_DISPLAY_APERTURE);
        type->DeleteItem(MF_MT_GEOMETRIC_APERTURE);
        UINT32 headerSize = 0;
        if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &headerSize)) || headerSize == 0)
        {
            auto sets = ParameterSets(packet->data, m_codec);
            if (!sets.empty()) type->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sets.data(), static_cast<UINT32>(sets.size()));
        }
        std::lock_guard lock(m_lock);
        m_outputType = type;
        m_changed.notify_all();
    }
    if (m_sink) m_sink(packet);
}

com_ptr<IMFMediaType> VideoEncoder::OutputType()
{
    std::unique_lock lock(m_lock);
    m_changed.wait_for(lock, std::chrono::seconds(3), [this] { return m_outputType != nullptr; });
    return m_outputType;
}

// ---------------------------------------------------------------- capture

namespace
{
GraphicsCaptureItem ItemFor(HWND window, HMONITOR monitor)
{
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    if (window)
        Check(interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)), "capture the window");
    else
        Check(interop->CreateForMonitor(monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)), "capture the screen");
    return item;
}
}

CaptureSession::CaptureSession(std::shared_ptr<GpuDevice> gpu, const Options& options, std::function<void(PacketPtr)> sink)
    : m_gpu(std::move(gpu)), m_options(options), m_codec(options.codec)
{
    m_fps = std::clamp<UINT>(options.fps, 24, 120);
    m_interval = 10'000'000 / m_fps;
    HMONITOR monitor = options.window ? MonitorFromWindow(options.window, MONITOR_DEFAULTTONEAREST) : options.monitor;
    m_hdr = m_codec != Codec::H264 && MonitorIsHdr(monitor);
    winrt::Windows::Graphics::SizeInt32 size{};
    if (options.window && options.gameHook)
    {
        try { m_hook = GameHook::Start(options.window, 1'000'000'000ull / m_fps, m_gpu->device.get()); }
        catch (const std::exception& e) { Log(std::string("Game hook: ") + e.what()); }
    }
    if (m_hook)
    {
        size = { m_hook->Size().cx, m_hook->Size().cy };
        // An SDR game on an HDR screen stays SDR: its frames are what the game drew.
        D3D11_TEXTURE2D_DESC desc{};
        if (auto frame = m_hook->Frame()) frame->GetDesc(&desc);
        bool wide = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT || desc.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
            desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
        m_hdr = m_hdr && wide;
    }
    else
    {
        m_item = ItemFor(options.window, options.monitor);
        size = m_item.Size();
    }

    // Encode at the game's own size, within what the encoder takes.
    double w = std::max(64, size.Width), h = std::max(64, size.Height);
    double maxSide = m_codec == Codec::H264 ? 4096 : 8192;
    double scale = std::min({ 1.0, maxSide / w, maxSide / h, std::sqrt(3840.0 * 2160.0 / (w * h)) });
    m_width = static_cast<UINT>(w * scale) & ~1u;
    m_height = static_cast<UINT>(h * scale) & ~1u;
    m_bitrate = BitrateFor(m_codec, m_width, m_height, m_fps, options.quality);

    m_videoDevice = m_gpu->device.as<ID3D11VideoDevice>();
    m_videoContext = m_gpu->context.as<ID3D11VideoContext>();

    auto makeEncoder = [&] {
        m_encoder = std::make_unique<VideoEncoder>(*m_gpu, m_codec, m_width, m_height, m_fps, m_bitrate, m_hdr, sink);
    };
    try { makeEncoder(); }
    catch (const std::exception& e)
    {
        Log(std::string("Encoder with first choice failed: ") + e.what());
        if (m_hdr) m_hdr = false;
        else if (m_codec != Codec::H264)
        {
            m_codec = Codec::H264;
            m_bitrate = BitrateFor(m_codec, m_width, m_height, m_fps, options.quality);
        }
        makeEncoder();
    }

    DXGI_FORMAT targetFormat = m_hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    for (int i = 0; i < 6; ++i)
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = targetFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Target target;
        Check(m_gpu->device->CreateTexture2D(&desc, nullptr, target.texture.put()), "frame texture");
        m_targets.push_back(std::move(target));
        m_free->push_back(i);
    }

    // Through Windows, the converter waits its turn behind the game. From inside the game it reads
    // a texture the game writes next, so at low priority it would hold the game up.
    m_gpu->device.as<IDXGIDevice>()->SetGPUThreadPriority(m_hook ? 0 : -7);
    if (m_hook) m_pump = std::thread([this] { Pump(); });
    else StartWindows(options);
    m_repeat = std::thread([this] { Repeat(); });
}

void CaptureSession::StartWindows(const Options& options)
{
    auto size = m_item.Size();
    auto pixelFormat = m_hdr ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized;
    m_pool = Direct3D11CaptureFramePool::CreateFreeThreaded(m_gpu->winrt, pixelFormat, 2, size);
    m_poolSize = size;
    // Windows draws a glowing outline around anything being captured, and it lands in the
    // recording. Turning it off takes borderless access, asked once (no prompt for a desktop app).
    static const bool borderless = [] {
        try { return GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless).get() == winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapabilityAccessStatus::Allowed; }
        catch (...) { return false; }
    }();
    m_session = m_pool.CreateCaptureSession(m_item);
    try { m_session.IsCursorCaptureEnabled(options.cursor); } catch (...) {}
    if (borderless) { try { m_session.IsBorderRequired(false); } catch (...) {} }
    // Windows hands over no more frames than the clip keeps: a game drawing faster costs nothing extra.
    try { m_session.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan(m_interval * 9 / 10)); } catch (...) {}
    m_item.Closed([this](auto&&, auto&&) { m_closed = true; });
    m_pool.FrameArrived([this](Direct3D11CaptureFramePool const& pool, auto&&) {
        try { OnFrame(pool); }
        catch (const std::exception& e) { Log(std::string("Frame: ") + e.what()); }
        catch (winrt::hresult_error const& e) { Log("Frame: " + Narrow(std::wstring(e.message()))); }
    });
    m_session.StartCapture();
}

void CaptureSession::Pump()
{
    // The hook sends no signal per frame: take the newest one on the clip's own clock.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    int64_t next = Now100ns();
    while (!m_stop)
    {
        next += m_interval;
        int64_t wait = next - Now100ns();
        if (wait < -m_interval) { next = Now100ns(); wait = 0; }
        if (wait > 0)
        {
            LARGE_INTEGER due{};
            due.QuadPart = -wait;
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 1000);
            else Sleep(static_cast<DWORD>(wait / 10'000));
        }
        if (m_stop) break;
        try
        {
            std::lock_guard lock(m_convert);
            if (m_closed) break;
            auto frame = m_hook->Frame();
            if (!frame) { m_closed = true; break; }
            D3D11_TEXTURE2D_DESC desc{};
            frame->GetDesc(&desc);
            EnsureProcessor(desc.Width, desc.Height, desc.Format);
            m_content = { static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height) };
            Convert(Now100ns(), InputFor(frame, desc.Width, desc.Height));
        }
        catch (const std::exception& e) { Log(std::string("Game frame: ") + e.what()); }
    }
    if (timer) CloseHandle(timer);
}

CaptureSession::~CaptureSession()
{
    m_stop = true;
    if (m_repeat.joinable()) m_repeat.join();
    if (m_pump.joinable()) m_pump.join();
    {
        std::lock_guard lock(m_convert);
        m_closed = true;
        if (m_session) { try { m_session.Close(); } catch (...) {} }
        if (m_pool) { try { m_pool.Close(); } catch (...) {} }
        m_inputs.clear();
        m_copy = nullptr;
        m_copyView = nullptr;
        m_hook.reset();
    }
    m_encoder.reset();
}

SIZE CaptureSession::ContentSize() const
{
    std::lock_guard lock(const_cast<std::mutex&>(m_convert));
    return m_content;
}

void CaptureSession::OnFrame(Direct3D11CaptureFramePool const& pool)
{
    auto frame = pool.TryGetNextFrame();
    if (!frame || m_stop) return;
    ++m_arrived;
    auto size = frame.ContentSize();
    if (size.Width != m_poolSize.Width || size.Height != m_poolSize.Height)
    {
        {
            std::lock_guard lock(m_convert);
            m_inputs.clear();
        }
        m_poolSize = size;
        pool.Recreate(m_gpu->winrt, m_hdr ? DirectXPixelFormat::R16G16B16A16Float : DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
    }
    int64_t time = frame.SystemRelativeTime().count();
    // Hold the frame rate: the game may draw faster than the clip records.
    if (m_lastFrame && time < m_lastFrame - m_interval / 8) return;
    if (time - m_lastFrame > m_interval) m_lastFrame = time;
    m_lastFrame += m_interval;

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    Check(access->GetInterface(IID_PPV_ARGS(texture.put())), "frame texture");
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    UINT cw = std::min<UINT>(std::max(size.Width, 1), desc.Width);
    UINT ch = std::min<UINT>(std::max(size.Height, 1), desc.Height);

    std::lock_guard lock(m_convert);
    if (m_closed) return;
    EnsureProcessor(desc.Width, desc.Height, desc.Format);
    m_content = { static_cast<LONG>(cw), static_cast<LONG>(ch) };
    Convert(time, InputFor(texture.get(), cw, ch));
}

/// A format the converter reads, of the same family (game frames may be typeless or sRGB).
static DXGI_FORMAT TypedFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

ID3D11VideoProcessorInputView* CaptureSession::InputFor(ID3D11Texture2D* texture, UINT width, UINT height)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
    inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (m_direct)
    {
        // The pool reuses two or three surfaces: one view each, made once.
        for (auto& input : m_inputs)
            if (input.texture.get() == texture) return input.view.get();
        Input input;
        input.texture.copy_from(texture);
        if (SUCCEEDED(m_videoDevice->CreateVideoProcessorInputView(texture, m_vpEnum.get(), &inDesc, input.view.put())))
        {
            if (m_inputs.size() >= 4) m_inputs.erase(m_inputs.begin());
            m_inputs.push_back(std::move(input));
            return m_inputs.back().view.get();
        }
        Log("The converter cannot read capture surfaces here; copying each frame.");
        m_direct = false;
        m_inputs.clear();
    }
    if (!m_copy)
    {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Format = TypedFormat(desc.Format);
        desc.Width = m_inW;
        desc.Height = m_inH;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        Check(m_gpu->device->CreateTexture2D(&desc, nullptr, m_copy.put()), "frame copy");
        Check(m_videoDevice->CreateVideoProcessorInputView(m_copy.get(), m_vpEnum.get(), &inDesc, m_copyView.put()), "converter input");
    }
    D3D11_BOX box{ 0, 0, 0, width, height, 1 };
    m_gpu->context->CopySubresourceRegion(m_copy.get(), 0, 0, 0, 0, texture, 0, &box);
    return m_copyView.get();
}

void CaptureSession::EnsureProcessor(UINT width, UINT height, DXGI_FORMAT format)
{
    if (m_vp && m_inW == width && m_inH == height && m_inFormat == format) return;
    m_inputs.clear();
    m_copy = nullptr;
    m_copyView = nullptr;
    m_vp = nullptr;
    m_vpEnum = nullptr;
    for (auto& t : m_targets) t.view = nullptr;
    m_inW = width;
    m_inH = height;
    m_inFormat = format;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputWidth = width;
    content.InputHeight = height;
    content.OutputWidth = m_width;
    content.OutputHeight = m_height;
    content.InputFrameRate = { m_fps, 1 };
    content.OutputFrameRate = { m_fps, 1 };
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    Check(m_videoDevice->CreateVideoProcessorEnumerator(&content, m_vpEnum.put()), "color converter");
    Check(m_videoDevice->CreateVideoProcessor(m_vpEnum.get(), 0, m_vp.put()), "color converter");

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
    outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    for (auto& t : m_targets)
        Check(m_videoDevice->CreateVideoProcessorOutputView(t.texture.get(), m_vpEnum.get(), &outDesc, t.view.put()), "converter output");

    if (auto context1 = m_videoContext.try_as<ID3D11VideoContext1>())
    {
        auto typed = TypedFormat(format);
        auto input = typed == DXGI_FORMAT_R16G16B16A16_FLOAT ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709
            : typed == DXGI_FORMAT_R10G10B10A2_UNORM && m_hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        context1->VideoProcessorSetStreamColorSpace1(m_vp.get(), 0, input);
        context1->VideoProcessorSetOutputColorSpace1(m_vp.get(),
            m_hdr ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    }
    else
    {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.RGB_Range = 0;
        out.YCbCr_Matrix = 1;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        m_videoContext->VideoProcessorSetStreamColorSpace(m_vp.get(), 0, &in);
        m_videoContext->VideoProcessorSetOutputColorSpace(m_vp.get(), &out);
    }
    m_videoContext->VideoProcessorSetStreamFrameFormat(m_vp.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    m_videoContext->VideoProcessorSetStreamAutoProcessingMode(m_vp.get(), 0, FALSE);
    D3D11_VIDEO_COLOR black{};
    black.RGBA = { 0, 0, 0, 1 };
    m_videoContext->VideoProcessorSetOutputBackgroundColor(m_vp.get(), FALSE, &black);
}

void CaptureSession::Convert(int64_t time, ID3D11VideoProcessorInputView* input)
{
    if (!m_vp || !input || m_content.cx <= 0) return;
    int index = -1;
    {
        std::lock_guard lock(*m_freeLock);
        if (!m_free->empty()) { index = m_free->back(); m_free->pop_back(); }
    }
    if (index < 0) { ++m_dropped; return; }
    if (time <= m_lastSubmit) time = m_lastSubmit + 1;

    // Fit the picture in the clip's frame, centred, black around it.
    double scale = std::min(double(m_width) / m_content.cx, double(m_height) / m_content.cy);
    LONG dw = static_cast<LONG>(m_content.cx * scale) & ~1L, dh = static_cast<LONG>(m_content.cy * scale) & ~1L;
    LONG x = ((static_cast<LONG>(m_width) - dw) / 2) & ~1L, y = ((static_cast<LONG>(m_height) - dh) / 2) & ~1L;
    RECT source{ 0, 0, m_content.cx, m_content.cy };
    RECT dest{ x, y, x + dw, y + dh };
    RECT full{ 0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height) };
    m_videoContext->VideoProcessorSetStreamSourceRect(m_vp.get(), 0, TRUE, &source);
    m_videoContext->VideoProcessorSetStreamDestRect(m_vp.get(), 0, TRUE, &dest);
    m_videoContext->VideoProcessorSetOutputTargetRect(m_vp.get(), TRUE, &full);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    HRESULT hr = m_videoContext->VideoProcessorBlt(m_vp.get(), m_targets[index].view.get(), 0, 1, &stream);
    if (FAILED(hr))
    {
        std::lock_guard lock(*m_freeLock);
        m_free->push_back(index);
        ++m_dropped;
        return;
    }
    m_lastIndex = index;
    m_lastDest = dest;
    Submit(index, time);
}

void CaptureSession::Submit(int index, int64_t time)
{
    // A key frame every second: clips and trims start within a second of where asked.
    if (time - m_lastKey >= 10'000'000)
    {
        m_encoder->ForceKeyFrame();
        m_lastKey = time;
    }
    auto free = m_free;
    auto freeLock = m_freeLock;
    m_encoder->Submit(m_targets[index].texture.get(), time, [free, freeLock, index] {
        std::lock_guard lock(*freeLock);
        free->push_back(index);
    });
    m_lastSubmit = time;
    ++m_frames;
}

void CaptureSession::Repeat()
{
    // A still picture sends no frames; repeat the last one ten times a second so
    // key frames keep coming and the sound stays in step.
    while (!m_stop)
    {
        Sleep(50);
        std::lock_guard lock(m_convert);
        if (m_closed || m_lastIndex < 0) continue;
        int64_t now = Now100ns();
        if (now - m_lastSubmit <= 1'000'000) continue;
        int index = -1;
        {
            std::lock_guard free(*m_freeLock);
            if (!m_free->empty()) { index = m_free->back(); m_free->pop_back(); }
        }
        if (index < 0) continue;
        // The newest picture again: no new frame means nothing on screen changed.
        if (index != m_lastIndex) m_gpu->context->CopyResource(m_targets[index].texture.get(), m_targets[m_lastIndex].texture.get());
        m_lastIndex = index;
        try { Submit(index, std::max(now, m_lastSubmit + 1)); }
        catch (const std::exception& e) { Log(std::string("Repeat frame: ") + e.what()); }
    }
}

bool CaptureSession::LastFrame(std::vector<uint8_t>& pixels, UINT& width, UINT& height)
{
    // Only the copy holds the converter: the read back waits on the GPU, and frames keep coming.
    D3D11_TEXTURE2D_DESC desc{};
    com_ptr<ID3D11Texture2D> staging;
    RECT r{};
    {
        std::lock_guard lock(m_convert);
        if (m_hdr || m_lastIndex < 0) return false;
        m_targets[m_lastIndex].texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(m_gpu->device->CreateTexture2D(&desc, nullptr, staging.put()))) return false;
        m_gpu->context->CopyResource(staging.get(), m_targets[m_lastIndex].texture.get());
        r = m_lastDest;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_gpu->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    // The newest converted (NV12) frame's picture area as BGRA, sampled to at most 480 across:
    // it only becomes a thumbnail, which is 480 wide.
    UINT fullW = static_cast<UINT>(r.right - r.left), fullH = static_cast<UINT>(r.bottom - r.top);
    UINT step = std::max(1u, (fullW + 479) / 480);
    width = fullW / step;
    height = fullH / step;
    pixels.resize(static_cast<size_t>(width) * height * 4);
    auto* base = static_cast<const uint8_t*>(mapped.pData);
    const uint8_t* chroma = base + static_cast<size_t>(mapped.RowPitch) * desc.Height;
    auto clamp = [](double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0)); };
    for (UINT y = 0; y < height; ++y)
    {
        UINT sy = r.top + y * step;
        const uint8_t* luma = base + static_cast<size_t>(mapped.RowPitch) * sy;
        const uint8_t* uv = chroma + static_cast<size_t>(mapped.RowPitch) * (sy / 2);
        uint8_t* out = pixels.data() + static_cast<size_t>(y) * width * 4;
        for (UINT x = 0; x < width; ++x)
        {
            UINT sx = r.left + x * step;
            // BT.709, studio range.
            double Y = 1.164 * (luma[sx] - 16);
            double U = uv[sx & ~1u] - 128.0, V = uv[(sx & ~1u) + 1] - 128.0;
            out[x * 4 + 0] = clamp(Y + 2.112 * U);
            out[x * 4 + 1] = clamp(Y - 0.213 * U - 0.533 * V);
            out[x * 4 + 2] = clamp(Y + 1.793 * V);
            out[x * 4 + 3] = 255;
        }
    }
    m_gpu->context->Unmap(staging.get(), 0);
    return width > 0 && height > 0;
}
