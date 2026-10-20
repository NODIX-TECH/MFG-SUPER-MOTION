// lf_shader_warm.inl - the shaders this run will need, compiled on background
// threads from the worker's first moment (round 24; lf_shader_cache.inl).
//
// The card, NGX and the network take about 1.3 s to come up, and none of it
// keeps the processor busy - that time now compiles, at below-normal priority,
// what LIVE FLOW otherwise compiled inside its first frames: the Denoiser, the
// finish, the rim reduction, the still check, Flow Super Resolution, the
// capture's own small passes, and FLOW MFG's kernels and motion estimate when
// FLOW MFG is asked for. From the cache files (every start after the first) it
// is a few milliseconds.
//
// Each job is exactly the call its feature makes (same source, entry point,
// profile and flags), so the feature finds it done - or, if it asks while a
// job is still running, waits for that job instead of compiling it twice.

struct ShaderJob
{
    const char *source;
    size_t size;
    const char *name;
    const char *entry;
    UINT flags;
};

static void ShaderWarmStart()
{
    static std::vector<ShaderJob> *jobs = new std::vector<ShaderJob>();
    static std::atomic<size_t> *next = new std::atomic<size_t>(0);
    static std::atomic<bool> *stop = new std::atomic<bool>(false);
    const UINT o3 = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    auto add = [&](const char *source, size_t size, const char *name, const char *entry, UINT flags) {
        jobs->push_back({ source, size, name, entry, flags });
    };
    // The motion estimate's first: they are the longest.
    if (NgxSelected() || MotionGiven())
        for (const char *entry : { "FlowCheck", "FlowRefine", "FlowFinal", "FlowCoarse", "FlowSpread", "EstHalf", "EstBase" })
            add(kFlowEstimateHlsl, sizeof(kFlowEstimateHlsl) - 1, "flow_estimate.hlsl", entry, o3);
    if (MsmSelected())
        for (const char *entry : { "SearchMain", "RefineMain", "RelaxMain", "FarMain", "SubpelMain", "FillMain", "MarkMain", "SplatMain", "ResolveMain", "EdgeMain",
                                    "WarpMain", "LumaMain", "HalveMain", "ClearMain" })
            add(kMsmHlsl, sizeof(kMsmHlsl) - 1, "msm-fg.hlsl", entry, o3);
    add(kUpscaleHlsl, sizeof(kUpscaleHlsl) - 1, "upscale.hlsl", "ScaleMain", o3);
    add(kUpscaleHlsl, sizeof(kUpscaleHlsl) - 1, "upscale.hlsl", "SharpMain", o3);
    add(kUpscaleHlsl, sizeof(kUpscaleHlsl) - 1, "upscale.hlsl", "AnimePushMain", o3);
    add(kUpscaleHlsl, sizeof(kUpscaleHlsl) - 1, "upscale.hlsl", "AnimeGradMain", o3);
    add(kHdrInHlsl, sizeof(kHdrInHlsl) - 1, "lf-order.hlsl", "CSMain", 0);
    add(kLumaHlsl, sizeof(kLumaHlsl) - 1, "lf-luma.hlsl", "CSMain", 0);
    if (!MsmSelected()) add(kOfaGrow, sizeof(kOfaGrow) - 1, nullptr, "CSMain", o3);
    if (HdrSwitchOn()) add(kHdrOutHlsl, sizeof(kHdrOutHlsl) - 1, "lf-hdr-out", "CSMain", 0);

    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = std::clamp(cores / 2, 1u, 6u);
    static std::atomic<unsigned> *left = new std::atomic<unsigned>(threads);
    static const auto started = std::chrono::steady_clock::now();
    atexit([] { stop->store(true); });
    for (unsigned t = 0; t < threads; ++t)
    {
        try
        {
            std::thread([] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                for (;;)
                {
                    if (stop->load()) return;
                    const size_t at = next->fetch_add(1);
                    if (at >= jobs->size()) break;
                    const ShaderJob &job = (*jobs)[at];
                    ID3DBlob *code = nullptr, *errors = nullptr;
                    CachedCompile(job.source, job.size, job.name, nullptr, nullptr, job.entry, "cs_5_0", job.flags, 0,
                                  &code, &errors);
                    if (code) code->Release();
                    if (errors) errors->Release();
                }
                if (left->fetch_sub(1) == 1 && !stop->load())
                {
                    ShaderCacheState &sc = ShaderCache();
                    unsigned compiled = 0, from_file = 0;
                    double seconds = 0.0;
                    {
                        std::lock_guard<std::mutex> hold(sc.lock);
                        compiled = sc.compiled;
                        from_file = sc.hits_file;
                        seconds = sc.compile_seconds;
                    }
                    Trace("[shader] %zu shaders ready in %.2f s (%u from the cache, %u compiled, %.2f s of compiler time)",
                          jobs->size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                          from_file, compiled, seconds);
                }
            }).detach();
        }
        catch (...)
        {
            left->fetch_sub(1);
        }
    }
}
