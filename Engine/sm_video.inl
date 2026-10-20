// sm_video.inl - Super Video (round 47): a video file made smoother - and, if asked, larger - with the engine's own
// frames in between, into a new file. Not a live picture: every frame of the video is taken, none passed over, and
// as fast as the card goes.
//
//   nvngx.dll --video <job file>      (the app's Super Video page; one video a run)
//
// The job file is lines of key=value: video_input, video_output (the paths, UTF-8), video_multiplier (1 none, 2..6),
// video_height (0 the video's own, else the height of the new one - the width follows), video_codec (0 H.264,
// 1 HEVC), video_share (the motion search's size, percent), and the engine's own keys that apply here (upscaler,
// sharpness, over_sharpness, msm_search, gpu, log).
//
// The way through, all on one card:
//   Media Foundation decodes the video on the card (its D3D11 device), each frame copied into one of two pictures
//   both Direct3D 11 and 12 see; SMFG (msm_fg.h) is told each pair and draws its frames in between; each frame - the
//   real one turned to the generator's channel order (BGRA -> RGBA), the ones in between as they are - goes through
//   the upscaler (lf_window_fill.inl, UpscaleRecord) when a size or sharpening is asked, into a picture Direct3D 11
//   sees again, is copied into a texture of its own and handed to Media Foundation's encoder (the card's own, MP4).
//   The sound is copied as it is when it is AAC, else decoded and encoded as AAC. Each step waits on the one before
//   on the card itself (one fence both APIs share); the processor waits only before a list is used again.
//   Lines to the app: "@video <frames done> <frames in all>", "@video done", "@error video <what>".
//
// DLSS MFG and XeSS MFG are not here: theirs is the live presenter's way (a swap chain, frames paced to a screen);
// SMFG works on pictures and nothing else, so a video is just pictures to it.

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace supervideo
{
    struct Job
    {
        std::wstring input, output;
        int multiplier = 2, height = 0, codec = 0, share = 75;
    };
    static Job g_job;
    static std::atomic<bool> g_stop{false};
    // MFVideoFormat_ABGR32 (D3DFMT_A8B8G8R8, 32 - R8G8B8A8): declared by mfapi.h only for newer Windows targets than the engine.s.
    static const GUID kAbgr32 = { 0x00000020, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

    static std::wstring Wide(const char *text)
    {
        const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        std::wstring out(n > 0 ? n - 1 : 0, L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), n);
        return out;
    }

    // The job's own lines (the engine's other keys go through ApplyLine as a start file's do).
    static void ReadJob(const wchar_t *path)
    {
        FILE *file = _wfsopen(path, L"rb", _SH_DENYNO);
        if (!file) return;
        char line[4096];
        while (fgets(line, sizeof(line), file))
        {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            // (A file written with a byte order mark: the mark is not part of the first key.)
            if (n >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF) memmove(line, line + 3, n - 2);
            const char *eq = strchr(line, '=');
            if (!eq) continue;
            const std::string key(line, eq - line);
            const char *value = eq + 1;
            if (key == "video_input") g_job.input = Wide(value);
            else if (key == "video_output") g_job.output = Wide(value);
            else if (key == "video_multiplier") g_job.multiplier = std::clamp(atoi(value), 1, 6);
            else if (key == "video_height") g_job.height = std::clamp(atoi(value), 0, 4320);
            else if (key == "video_codec") g_job.codec = std::clamp(atoi(value), 0, 1);
            else if (key == "video_share") g_job.share = std::clamp(atoi(value), 25, 100);
        }
        fclose(file);
    }

    // "stop" (or the app gone) ends the video where it is - the file is closed as a whole one.
    static DWORD WINAPI StdinThread(LPVOID)
    {
        char line[256];
        while (fgets(line, sizeof(line), stdin))
        {
            // (A byte order mark or other stray bytes in front of the first line, as ControlThread has it.)
            const char *word = line;
            while (*word && !isalnum(static_cast<unsigned char>(*word))) ++word;
            if (_strnicmp(word, "stop", 4) == 0) break;
        }
        g_stop = true;
        return 0;
    }

    // BGRA -> RGBA (the real frames into the generator's order): one read and one write a pixel.
    static const char kConvertHlsl[] =
        "Texture2D<float4> src : register(t0); RWTexture2D<float4> dst : register(u0);\n"
        "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) { dst[id.xy] = src[id.xy]; }\n";

    static void Fail(const char *what, HRESULT hr = S_OK)
    {
        Trace("[video] %s (0x%08X)", what, (unsigned)hr);
        Status("@error video %s", what);
    }

    static int Run()
    {
        using winrt::com_ptr;
        const Job &job = g_job;
        if (job.input.empty() || job.output.empty()) { Fail("no input or output named"); return 2; }
        CreateThread(nullptr, 0, StdinThread, nullptr, 0, nullptr);
        Trace("[video] %ls -> %ls, x%d, height %d, %s", job.input.c_str(), job.output.c_str(), job.multiplier, job.height,
              job.codec ? "HEVC" : "H.264");

        // Direct3D 11 on the engine's card (the decoder's and the encoder's).
        com_ptr<IDXGIFactory1> factory;
        com_ptr<IDXGIAdapter1> adapter;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put())))) { Fail("no DXGI factory"); return 3; }
        {
            com_ptr<IDXGIFactory4> f4;
            const LUID luid = lfCore.device->GetAdapterLuid();
            if (!factory.try_as(f4) || FAILED(f4->EnumAdapterByLuid(luid, IID_PPV_ARGS(adapter.put())))) { Fail("the engine's card is not found"); return 3; }
        }
        com_ptr<ID3D11Device> d11;
        com_ptr<ID3D11DeviceContext> ctx;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 2,
                                       D3D11_SDK_VERSION, d11.put(), nullptr, ctx.put());
        if (FAILED(hr)) { Fail("no Direct3D 11 device for video", hr); return 3; }
        if (com_ptr<ID3D10Multithread> mt = d11.try_as<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);
        com_ptr<ID3D11Device5> d5 = d11.try_as<ID3D11Device5>();
        com_ptr<ID3D11DeviceContext4> ctx4 = ctx.try_as<ID3D11DeviceContext4>();
        if (!d5 || !ctx4) { Fail("this Windows has no shared fences for Direct3D 11"); return 3; }

        if (FAILED(hr = MFStartup(MF_VERSION, MFSTARTUP_FULL))) { Fail("Media Foundation did not start", hr); return 3; }
        struct MfDown { ~MfDown() { MFShutdown(); } } mf_down;
        UINT token = 0;
        com_ptr<IMFDXGIDeviceManager> manager;
        if (FAILED(hr = MFCreateDXGIDeviceManager(&token, manager.put())) || FAILED(hr = manager->ResetDevice(d11.get(), token)))
        { Fail("no Media Foundation device manager", hr); return 3; }

        // The reader: frames decoded on the card, turned into BGRA there.
        com_ptr<IMFAttributes> ra;
        MFCreateAttributes(ra.put(), 4);
        ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.get());
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        com_ptr<IMFSourceReader> reader;
        if (FAILED(hr = MFCreateSourceReaderFromURL(job.input.c_str(), ra.get(), reader.put()))) { Fail("the video could not be opened", hr); return 4; }
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        {
            com_ptr<IMFMediaType> want;
            MFCreateMediaType(want.put());
            want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
            if (FAILED(hr = reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, want.get())))
            { Fail("the video's frames cannot be decoded here", hr); return 4; }
        }
        com_ptr<IMFMediaType> vtype;
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), vtype.put());
        UINT32 w = 0, h = 0, rate_num = 30, rate_den = 1;
        MFGetAttributeSize(vtype.get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(vtype.get(), MF_MT_FRAME_RATE, &rate_num, &rate_den);
        if (w < 64 || h < 64 || rate_num == 0 || rate_den == 0) { Fail("the video's size or rate is not known"); return 4; }
        PROPVARIANT dur;
        PropVariantInit(&dur);
        UINT64 duration = 0;
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &dur))) duration = dur.uhVal.QuadPart;
        PropVariantClear(&dur);
        const UINT64 total = duration ? static_cast<UINT64>(double(duration) * 1e-7 * rate_num / rate_den + 0.5) : 0;

        // The sound: as it is (AAC), or decoded and made AAC.
        bool audio = false, audio_copy = false;
        com_ptr<IMFMediaType> atype;
        {
            com_ptr<IMFMediaType> native;
            if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, native.put())))
            {
                GUID sub = GUID_NULL;
                native->GetGUID(MF_MT_SUBTYPE, &sub);
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
                if (sub == MFAudioFormat_AAC && SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, native.get())))
                    audio = audio_copy = true;
                else
                {
                    com_ptr<IMFMediaType> pcm;
                    MFCreateMediaType(pcm.put());
                    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    audio = SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm.get()));
                }
                if (audio) reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), atype.put());
                else reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), FALSE);
            }
        }

        // The new video's size: the asked height, the width following (both even, as encoders want them).
        UINT ow = w & ~1u, oh = h & ~1u;
        if (job.height > 0 && UINT(job.height) != h)
        {
            oh = UINT(job.height) & ~1u;
            ow = static_cast<UINT>(std::lround(double(w) * oh / h)) & ~1u;
        }
        const UINT n = static_cast<UINT>(job.multiplier);
        const UINT out_num = rate_num * n, out_den = rate_den;
        const double out_fps = double(out_num) / out_den;
        // The writer: MP4, the card's encoder. H.264 as asked - or HEVC, when H.264 does not go that large or that
        // fast (1440p at 180 frames a second is beyond the levels H.264 has; HEVC's go further).
        com_ptr<IMFSinkWriter> writer;
        DWORD vs = 0, as = 0;
        UINT32 bitrate = 0;
        int codec = job.codec;
        auto make_writer = [&](int kind) -> HRESULT {
            writer = nullptr;
            bitrate = static_cast<UINT32>(std::clamp(double(ow) * oh * std::min(out_fps, 240.0) * (kind ? 0.05 : 0.08), 4e6, 150e6));
            com_ptr<IMFAttributes> wa;
            MFCreateAttributes(wa.put(), 4);
            wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            wa->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, manager.get());
            wa->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            HRESULT r = MFCreateSinkWriterFromURL(job.output.c_str(), nullptr, wa.get(), writer.put());
            if (FAILED(r)) return r;
            com_ptr<IMFMediaType> out;
            MFCreateMediaType(out.put());
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, kind ? MFVideoFormat_HEVC : MFVideoFormat_H264);
            out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
            out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(out.get(), MF_MT_FRAME_SIZE, ow, oh);
            MFSetAttributeRatio(out.get(), MF_MT_FRAME_RATE, out_num, out_den);
            // (Video's own range and colours, said outright: without them the encoder made full-range YUV that players
            // read as limited - dark greys darker, colours too strong.)
            out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
            MFSetAttributeRatio(out.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (FAILED(r = writer->AddStream(out.get(), &vs))) return r;
            com_ptr<IMFMediaType> in;
            MFCreateMediaType(in.put());
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in->SetGUID(MF_MT_SUBTYPE, kAbgr32);
            in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(in.get(), MF_MT_FRAME_SIZE, ow, oh);
            MFSetAttributeRatio(in.get(), MF_MT_FRAME_RATE, out_num, out_den);
            MFSetAttributeRatio(in.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_0_255);
            return writer->SetInputMediaType(vs, in.get(), nullptr);
        };
        hr = make_writer(codec);
        if (FAILED(hr) && codec == 0)
        {
            Trace("[video] H.264 does not take %ux%u at %.1f FPS (0x%08X) - HEVC instead", ow, oh, out_fps, (unsigned)hr);
            codec = 1;
            hr = make_writer(codec);
        }
        if (FAILED(hr))
        {
            writer = nullptr;
            DeleteFileW(job.output.c_str());
            Fail("the card's encoder does not take this size at this many frames a second - a smaller size or fewer frames", hr);
            return 5;
        }
        if (audio)
        {
            com_ptr<IMFMediaType> out;
            if (audio_copy) out = atype;
            else
            {
                UINT32 rate = MFGetAttributeUINT32(atype.get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
                UINT32 channels = MFGetAttributeUINT32(atype.get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
                if ((rate != 44100 && rate != 48000) || channels < 1 || channels > 6) audio = false;
                else
                {
                    MFCreateMediaType(out.put());
                    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
                }
            }
            if (audio && (FAILED(writer->AddStream(out.get(), &as)) || FAILED(writer->SetInputMediaType(as, atype.get(), nullptr))))
                audio = false;
            if (!audio)
            {
                reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), FALSE);
                Trace("[video] the sound is left out (a kind this writer does not take)");
            }
        }
        if (FAILED(hr = writer->BeginWriting())) { Fail("the new video could not be begun", hr); return 5; }
        Trace("[video] %ux%u at %.3f FPS -> %ux%u at %.3f FPS, %s %.1f Mbit/s; sound %s", w, h, double(rate_num) / rate_den, ow, oh,
              out_fps, codec ? "HEVC" : "H.264", bitrate / 1e6, audio ? (audio_copy ? "copied" : "made AAC") : "none");
        Status("@video codec %s", codec ? "HEVC" : "H.264");

        // The pictures both APIs see, and the one fence.
        auto shared11 = [&](UINT tw, UINT th, DXGI_FORMAT format, com_ptr<ID3D11Texture2D> &t11, com_ptr<ID3D12Resource> &t12) {
            D3D11_TEXTURE2D_DESC d = {};
            d.Width = tw; d.Height = th; d.MipLevels = 1; d.ArraySize = 1; d.Format = format; d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (FAILED(d11->CreateTexture2D(&d, nullptr, t11.put()))) return false;
            com_ptr<IDXGIResource1> r1 = t11.try_as<IDXGIResource1>();
            HANDLE handle = nullptr;
            if (!r1 || FAILED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) return false;
            const bool ok = SUCCEEDED(lfCore.device->OpenSharedHandle(handle, IID_PPV_ARGS(t12.put())));
            CloseHandle(handle);
            return ok;
        };
        com_ptr<ID3D11Texture2D> in11[2], out11;
        com_ptr<ID3D12Resource> in12[2], out12;
        if (!shared11(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, in11[0], in12[0]) || !shared11(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, in11[1], in12[1]) ||
            !shared11(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, out11, out12))
        { Fail("no pictures shared between Direct3D 11 and 12"); return 3; }
        com_ptr<ID3D12Fence> fence12;
        com_ptr<ID3D11Fence> fence11;
        {
            HANDLE handle = nullptr;
            if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence12.put()))) ||
                FAILED(lfCore.device->CreateSharedHandle(fence12.get(), nullptr, GENERIC_ALL, nullptr, &handle)))
            { Fail("no shared fence"); return 3; }
            hr = d5->OpenSharedFence(handle, IID_PPV_ARGS(fence11.put()));
            CloseHandle(handle);
            if (FAILED(hr)) { Fail("Direct3D 11 cannot open the shared fence", hr); return 3; }
        }
        UINT64 fv = 0;
        HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // The generator's picture (RGBA, written by compute) and the conversion of real frames into it.
        com_ptr<ID3D12Resource> work;
        {
            D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(work.put()))))
            { Fail("no picture for the generator"); return 3; }
        }
        com_ptr<ID3D12RootSignature> conv_root;
        com_ptr<ID3D12PipelineState> conv_pso;
        com_ptr<ID3D12DescriptorHeap> conv_heap;
        {
            D3D12_DESCRIPTOR_RANGE ranges[2] = {};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 1;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].OffsetInDescriptorsFromTableStart = 1;
            D3D12_ROOT_PARAMETER param = {};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable.NumDescriptorRanges = 2; param.DescriptorTable.pDescriptorRanges = ranges;
            D3D12_ROOT_SIGNATURE_DESC rs = {}; rs.NumParameters = 1; rs.pParameters = &param;
            com_ptr<ID3DBlob> blob, error, code;
            if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), error.put())) ||
                FAILED(lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(conv_root.put()))) ||
                FAILED(D3DCompile(kConvertHlsl, sizeof(kConvertHlsl) - 1, "convert", nullptr, nullptr, "main", "cs_5_0", 0, 0, code.put(), error.put())))
            { Fail("the conversion pass could not be made"); return 3; }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = conv_root.get(); pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
            D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(lfCore.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(conv_pso.put()))) ||
                FAILED(lfCore.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(conv_heap.put()))))
            { Fail("the conversion pass could not be made"); return 3; }
            // Two tables: real frames from either input picture into `work`.
            const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_CPU_DESCRIPTOR_HANDLE at = conv_heap->GetCPUDescriptorHandleForHeapStart();
            for (int i = 0; i < 2; ++i)
            {
                lfCore.device->CreateShaderResourceView(in12[i].get(), nullptr, at); at.ptr += each;
                lfCore.device->CreateUnorderedAccessView(work.get(), nullptr, nullptr, at); at.ptr += each;
            }
        }
        // SMFG at the video's size.
        if (n > 1 && !MsmReady(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, job.share)) { Fail("SMFG could not be made for this size"); return 6; }
        const bool scaled = ow != w || oh != h;
        const bool reshape = scaled || smOpt.upscaler.load() != 0 || smOpt.over_sharpness.load() > 0;

        com_ptr<ID3D12CommandAllocator> alloc;
        com_ptr<ID3D12GraphicsCommandList> list;
        if (FAILED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put()))) ||
            FAILED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr, IID_PPV_ARGS(list.put()))))
        { Fail("no command list"); return 3; }
        list->Close();
        ID3D12CommandQueue *queue = lfCore.direct_queue;
        // A list is used again only once the card has done the one before.
        auto open = [&] {
            if (fence12->GetCompletedValue() < fv) { fence12->SetEventOnCompletion(fv, fence_event); WaitForSingleObject(fence_event, 10000); }
            alloc->Reset();
            list->Reset(alloc.get(), nullptr);
        };
        // `work` (COPY_SOURCE) into the shared output picture, scaled or sharpened if asked, then handed to the encoder.
        UINT64 frames_out = 0;
        auto emit = [&](LONGLONG time, LONGLONG length) -> bool {
            D3D12_RESOURCE_BARRIER to_dst = Barrier(out12.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &to_dst);
            const D3D12_RESOURCE_DESC sd = work->GetDesc(), bd = out12->GetDesc();
            if (!reshape || !UpscaleRecord(list.get(), out12.get(), work.get(), sd, bd)) list->CopyResource(out12.get(), work.get());
            D3D12_RESOURCE_BARRIER back = Barrier(out12.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &back);
            if (FAILED(list->Close())) return false;
            ID3D12CommandList *lists[] = { list.get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence12.get(), ++fv);
            // On Direct3D 11: after the card's work, into a texture of its own for the encoder.
            ctx4->Wait(fence11.get(), fv);
            D3D11_TEXTURE2D_DESC td = {};
            out11->GetDesc(&td);
            td.MiscFlags = 0; td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            com_ptr<ID3D11Texture2D> frame;
            if (FAILED(d11->CreateTexture2D(&td, nullptr, frame.put()))) return false;
            ctx->CopyResource(frame.get(), out11.get());
            ctx4->Signal(fence11.get(), ++fv);
            ctx->Flush();
            queue->Wait(fence12.get(), fv);
            com_ptr<IMFMediaBuffer> buffer;
            com_ptr<IMFSample> sample;
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), frame.get(), 0, FALSE, buffer.put()))) return false;
            if (com_ptr<IMF2DBuffer> b2 = buffer.try_as<IMF2DBuffer>())
            {
                DWORD length2d = 0;
                if (SUCCEEDED(b2->GetContiguousLength(&length2d))) buffer->SetCurrentLength(length2d);
            }
            MFCreateSample(sample.put());
            sample->AddBuffer(buffer.get());
            sample->SetSampleTime(time);
            sample->SetSampleDuration(length);
            if (FAILED(hr = writer->WriteSample(vs, sample.get()))) { Trace("[video] the encoder refused frame %llu (0x%08X)", frames_out, (unsigned)hr); return false; }
            ++frames_out;
            return true;
        };
        // A real frame (input picture `slot`) into `work`.
        auto convert = [&](int slot) {
            D3D12_RESOURCE_BARRIER in[] = { Barrier(in12[slot].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                            Barrier(work.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
            list->ResourceBarrier(2, in);
            ID3D12DescriptorHeap *heaps[] = { conv_heap.get() };
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(conv_root.get());
            list->SetPipelineState(conv_pso.get());
            D3D12_GPU_DESCRIPTOR_HANDLE table = conv_heap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(slot) * 2 * lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            list->SetComputeRootDescriptorTable(0, table);
            list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER out[] = { Barrier(in12[slot].get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                             Barrier(work.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
            list->ResourceBarrier(2, out);
        };

        MsmAsk ask;
        ask.fast = smOpt.msm_search.load() != 0;
        ask.marks = false;          // (a video has no read-outs of its own to keep still)
        int slot = 0;
        bool have_before = false;
        LONGLONG before_time = 0, last_length = static_cast<LONGLONG>(1e7 * rate_den / rate_num);
        UINT64 frames_in = 0;
        ULONGLONG told = 0;
        bool video_done = false, audio_done = !audio, failed = false;
        while (!g_stop && !failed && (!video_done || !audio_done))
        {
            DWORD stream = 0, flags = 0;
            LONGLONG time = 0;
            com_ptr<IMFSample> sample;
            hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &stream, &flags, &time, sample.put());
            if (FAILED(hr)) { Fail("the video could not be read on", hr); failed = true; break; }
            com_ptr<IMFMediaType> kind;
            GUID major = GUID_NULL;
            if (SUCCEEDED(reader->GetCurrentMediaType(stream, kind.put()))) kind->GetGUID(MF_MT_MAJOR_TYPE, &major);
            const bool is_video = major == MFMediaType_Video;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { if (is_video) video_done = true; else audio_done = true; continue; }
            if (!sample) continue;
            if (!is_video)
            {
                if (audio && FAILED(writer->WriteSample(as, sample.get()))) { Trace("[video] a piece of sound was refused"); }
                continue;
            }
            // The decoded frame into this slot's picture.
            com_ptr<IMFMediaBuffer> buffer;
            if (FAILED(sample->GetBufferByIndex(0, buffer.put()))) continue;
            if (com_ptr<IMFDXGIBuffer> dx = buffer.try_as<IMFDXGIBuffer>())
            {
                com_ptr<ID3D11Texture2D> texture;
                UINT sub = 0;
                if (FAILED(dx->GetResource(IID_PPV_ARGS(texture.put()))) || FAILED(dx->GetSubresourceIndex(&sub))) continue;
                D3D11_BOX box = { 0, 0, 0, w, h, 1 };
                ctx->CopySubresourceRegion(in11[slot].get(), 0, 0, 0, 0, texture.get(), sub, &box);
            }
            else
            {
                BYTE *data = nullptr;
                LONG pitch = 0;
                com_ptr<IMF2DBuffer> b2 = buffer.try_as<IMF2DBuffer>();
                if (b2 && SUCCEEDED(b2->Lock2D(&data, &pitch)))
                {
                    ctx->UpdateSubresource(in11[slot].get(), 0, nullptr, data, static_cast<UINT>(std::abs(pitch)), 0);
                    b2->Unlock2D();
                }
                else
                {
                    DWORD length = 0;
                    if (FAILED(buffer->Lock(&data, nullptr, &length))) continue;
                    ctx->UpdateSubresource(in11[slot].get(), 0, nullptr, data, w * 4, 0);
                    buffer->Unlock();
                }
            }
            ctx4->Signal(fence11.get(), ++fv);
            ctx->Flush();
            queue->Wait(fence12.get(), fv);
            ++frames_in;
            // The pair before it: its frames in between, then this real frame.
            const LONGLONG span = have_before ? time - before_time : 0;
            if (span > 0) last_length = span;
            if (have_before && n > 1 && span > 0)
            {
                open();
                MsmPair(list.get(), in12[1 - slot].get(), in12[slot].get(), D3D12_RESOURCE_STATE_COMMON, ask);
                for (UINT k = 1; k < n && !failed; ++k)
                {
                    if (k > 1) open();
                    MsmFrame(list.get(), in12[1 - slot].get(), in12[slot].get(), D3D12_RESOURCE_STATE_COMMON, work.get(),
                             D3D12_RESOURCE_STATE_COPY_SOURCE, float(k) / float(n));
                    if (!emit(before_time + span * LONGLONG(k) / LONGLONG(n), span / LONGLONG(n))) failed = true;
                }
            }
            if (failed) break;
            open();
            convert(slot);
            if (!emit(time, have_before && span > 0 ? span / LONGLONG(n) : last_length / LONGLONG(n))) { failed = true; break; }
            have_before = true;
            before_time = time;
            slot = 1 - slot;
            const ULONGLONG now = GetTickCount64();
            if (now - told >= 250) { told = now; Status("@video %llu %llu", frames_in, std::max(total, frames_in)); }
        }
        // The last real frame's own time at the new rate is shown by the frames that follow it - none: its length
        // stays one frame of the new rate (a video ends on its last picture).
        if (fence12->GetCompletedValue() < fv) { fence12->SetEventOnCompletion(fv, fence_event); WaitForSingleObject(fence_event, 10000); }
        CloseHandle(fence_event);
        hr = writer->Finalize();
        if (failed) { Status("@state stopped"); return 7; }
        if (FAILED(hr)) { Fail("the new video could not be closed", hr); return 7; }
        Trace("[video] %s: %llu frames in, %llu out", g_stop ? "stopped" : "done", frames_in, frames_out);
        Status("@video %llu %llu", frames_in, std::max(total, frames_in));
        Status(g_stop ? "@video stopped" : "@video done");
        return 0;
    }
}
