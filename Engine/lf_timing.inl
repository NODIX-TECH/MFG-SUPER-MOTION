// lf_timing.inl - timings for tuning, all off unless LF_PHASE=1: CPU time per
// step of the loop, card time per stage from timestamp queries, and one
// record per frame, printed together every few seconds.

// LF_PHASE=1: timings on.
static bool PhasesOn()
{
    if (lfPhase.enabled < 0)
    {
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_PHASE", text, sizeof(text));
        lfPhase.enabled = (len > 0 && text[0] == '1') ? 1 : 0;
        if (lfPhase.enabled) Trace("[timing] step timings on (LF_PHASE=1)");
    }
    return lfPhase.enabled == 1;
}

// Seconds, high resolution.
static double PhaseClock()
{
    if (lfRun.qpc_rate.QuadPart == 0) QueryPerformanceFrequency(&lfRun.qpc_rate);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(lfRun.qpc_rate.QuadPart);
}

// Add one step's time.
static void PhaseCount(int idx, double t0)
{
    if (!PhasesOn()) return;
    const double timeout_ms = PhaseClock() - t0;
    lfPhase.sum[idx] += timeout_ms;
    if (timeout_ms > lfPhase.peak[idx]) lfPhase.peak[idx] = timeout_ms;
    ++lfPhase.count[idx];
    if (idx == PHASE_SHOW)
    {
        int b = 0;
        while (b < 5 && timeout_ms >= kShowEdges[b]) ++b;
        ++lfPhase.bins[b];
    }
}

// Print the averages every few seconds and start over.
static void PhaseSummary()
{
    if (!PhasesOn()) return;
    const UINT64 now = GetTickCount64();
    if (lfPhase.window_start == 0) { lfPhase.window_start = now; return; }
    if (now - lfPhase.window_start < 2000) return;
    const double seconds = static_cast<double>(now - lfPhase.window_start) / 1000.0;
    lfPhase.window_start = now;
    TimingFlush();

    char line[640];
    int off = _snprintf_s(line, sizeof(line), _TRUNCATE, "[timing] steps");
    for (int i = 0; i < PHASE_COUNT && off > 0; ++i)
    {
        if (lfPhase.count[i] == 0) continue;
        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, " | %s %.1f/%.1f",
                           kPhaseLabel[i], lfPhase.sum[i] / lfPhase.count[i], lfPhase.peak[i]);
        lfPhase.sum[i] = 0.0; lfPhase.peak[i] = 0.0; lfPhase.count[i] = 0;
    }
    if (lfProf.query_n > 0)
    {
        _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                    " | on the card %.1f/%.1f", lfProf.query_total / lfProf.query_n, lfProf.query_peak);
        lfProf.query_total = 0.0; lfProf.query_peak = 0.0; lfProf.query_n = 0;
    }
    Trace("%s (average/peak ms)", line);
    for (int i = 0; i < GS_COUNT; ++i)
    {
        if (lfProf.submit_n[i] == 0 && lfProf.wait_n[i] == 0 && lfProf.gpu_n[i] == 0) continue;
        Trace("[timing] stage %s: send %.3f/%.3f wait %.3f/%.3f card %.3f/%.3f idle %.3f/%.3f ms",
            kStageLabel[i],
            lfProf.submit_n[i] ? lfProf.submit_sum[i] / lfProf.submit_n[i] : 0.0, lfProf.submit_peak[i],
            lfProf.wait_n[i] ? lfProf.wait_sum[i] / lfProf.wait_n[i] : 0.0, lfProf.wait_peak[i],
            lfProf.gpu_n[i] ? lfProf.gpu_sum[i] / lfProf.gpu_n[i] : 0.0, lfProf.gpu_peak[i],
            lfProf.gap_n[i] ? lfProf.gap_sum[i] / lfProf.gap_n[i] : 0.0, lfProf.gap_peak[i]);
        lfProf.submit_sum[i] = lfProf.submit_peak[i] = 0.0; lfProf.submit_n[i] = 0;
        lfProf.wait_sum[i] = lfProf.wait_peak[i] = 0.0; lfProf.wait_n[i] = 0;
        lfProf.gpu_sum[i] = lfProf.gpu_peak[i] = 0.0; lfProf.gpu_n[i] = 0;
        lfProf.gap_sum[i] = lfProf.gap_peak[i] = 0.0; lfProf.gap_n[i] = 0;
    }
    Trace("[timing] queue: at most %u in flight, %u allocator waits, %.3f/%.3f ms each",
        lfProf.in_flight_peak, lfProf.allocator_waits,
        lfProf.allocator_waits ? lfProf.allocator_wait_sum / lfProf.allocator_waits : 0.0,
        lfProf.allocator_wait_peak);
    lfProf.in_flight_peak = lfProf.allocator_waits = 0;
    lfProf.allocator_wait_sum = lfProf.allocator_wait_peak = 0.0;
    Trace("[timing] over %.2fs: %u asked, %u new pictures (%.1f/s), %u done, %u idle",
        seconds, lfPhase.requests, lfPhase.fresh_in, lfPhase.fresh_in / seconds,
        lfPhase.processed, lfPhase.idle);
    lfPhase.requests = lfPhase.fresh_in = lfPhase.evaluated = 0;
    lfPhase.fresh_evaluated = lfPhase.processed = lfPhase.idle = 0;
    Trace("[timing] show times in ms: under 5 %u, 5-12 %u, 12-20 %u, 20-28 %u, 28-40 %u, over 40 %u",
        lfPhase.bins[0], lfPhase.bins[1], lfPhase.bins[2], lfPhase.bins[3], lfPhase.bins[4], lfPhase.bins[5]);
    for (int b = 0; b < 6; ++b) lfPhase.bins[b] = 0;
}

// Note when a picture was taken and how old it was.
static void TimingCapture(double acquire_start, UINT64 source_qpc, const char *kind)
{
    if (!PhasesOn()) return;
    lfProf.stamp.acquire_start = acquire_start;
    lfProf.stamp.acquired = PhaseClock();
    lfProf.stamp.kind = kind;
    lfProf.stamp.source_qpc = source_qpc;
    lfProf.stamp.capture = ++lfProf.capture_serial;
    if (source_qpc && lfRun.qpc_rate.QuadPart > 0 && source_qpc >= lfProf.last_source_qpc)
    {
        const double source_ms = static_cast<double>(source_qpc) * 1000.0 / lfRun.qpc_rate.QuadPart;
        if (source_ms <= lfProf.stamp.acquired) lfProf.stamp.source_time = source_ms;
    }
    if (source_qpc) lfProf.last_source_qpc = source_qpc;
}

// Print the per-frame records gathered so far.
static void TimingFlush()
{
    for (unsigned i = 0; i < lfProf.row_count; ++i) Trace("%s", lfProf.rows[i]);
    lfProf.row_count = 0;
}

// Close one frame's record.
static void TimingResult(const Pipeline &pipe, uint32_t index, bool fresh, const char *outcome)
{
    if (!PhasesOn()) return;
    if (lfProf.row_count == _countof(lfProf.rows)) TimingFlush();
    lfProf.stamp.reported = true;
    const double age = fresh && lfProf.stamp.present_call > 0 && lfProf.stamp.acquired > 0
        ? lfProf.stamp.present_call - lfProf.stamp.acquired : -1.0;
    const double source_age = age >= 0 && lfProf.stamp.source_time > 0
        ? lfProf.stamp.present_call - lfProf.stamp.source_time : -1.0;
    const UINT pic_w = pipe.stretched ? pipe.screen_w : pipe.work_w, pic_h = pipe.stretched ? pipe.screen_h : pipe.work_h;
    sprintf_s(lfProf.rows[lfProf.row_count++],
        "[timing] frame n=%u source-gen=%llu source-n=%llu via=%s new=%u outcome=%s "
        "wait-from=%.3f got=%.3f show-at=%.3f age=%.3f source-clock=%llu source-age=%.3f "
        "timebase=%s mark=%llu picture=%ux%u motion=%ux%u",
        index, lfProf.capture_gen, lfProf.stamp.capture,
        lfProf.stamp.kind, fresh ? 1u : 0u, outcome, lfProf.stamp.acquire_start,
        lfProf.stamp.acquired, lfProf.stamp.present_call, age, lfProf.stamp.source_qpc, source_age,
        source_age >= 0 ? "desktop-clock" : "take-to-show", lfProf.stamp.fence,
        pic_w, pic_h, pipe.work_w, pipe.work_h);
}

// Read back finished card timings.
static void TimingCollect()
{
    if (lfProf.query_state != 1) return;
    const UINT64 reached = lfCore.timeline->GetCompletedValue();
    if (reached == UINT64_MAX) return;

    for (int n = 0; n < GpuCore::kRing; ++n)
    {
        int first = -1;
        for (int i = 0; i < GpuCore::kRing; ++i)
            if (lfProf.slot_mark[i] && lfProf.slot_mark[i] <= reached &&
                (first < 0 || lfProf.slot_mark[i] < lfProf.slot_mark[first])) first = i;
        if (first < 0) break;
        QueryRead(first);
        lfProf.slot_mark[first] = 0;
        lfProf.slot_stage[first] = GS_NONE;
    }
}

// The timestamp query heap and its read-back buffer.
static bool QueriesReady()
{
    if (lfProf.query_state != 0) return lfProf.query_state == 1;
    lfProf.query_state = -1;
    if (lfCore.device == nullptr || lfCore.direct_queue == nullptr) return false;
    D3D12_QUERY_HEAP_DESC queue_desc = {};
    queue_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queue_desc.Count = 4 * GpuCore::kRing;
    if (FAILED(lfCore.device->CreateQueryHeap(&queue_desc, __uuidof(ID3D12QueryHeap),
                                      reinterpret_cast<void **>(&lfProf.query_heap))))
    { Trace("[timing] no query heap - card timings off"); return false; }
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = sizeof(UINT64) * 4 * GpuCore::kRing; buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1; buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(lfCore.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&lfProf.query_readback)))) return false;
    if (FAILED(lfCore.direct_queue->GetTimestampFrequency(&lfProf.tick_rate)) || lfProf.tick_rate == 0) return false;
    lfProf.query_state = 1;
    Trace("[timing] card timings on (%llu ticks a second)", lfProf.tick_rate);
    return true;
}

// One stage's card time from the query results.
static void QueryRead(int slot)
{
    if (lfProf.query_state != 1) return;
    const int stage = lfProf.slot_stage[slot];
    D3D12_RANGE r = { sizeof(UINT64) * slot * 4, sizeof(UINT64) * (slot + 1) * 4 };
    void *view_bytes = nullptr;
    if (FAILED(lfProf.query_readback->Map(0, &r, &view_bytes)) || view_bytes == nullptr) return;
    const UINT64 *ts = static_cast<const UINT64 *>(view_bytes) + slot * 4;
    if (ts[1] > ts[0])
    {
        const double timeout_ms = static_cast<double>(ts[1] - ts[0]) * 1000.0
                          / static_cast<double>(lfProf.tick_rate);
        lfProf.gpu_sum[stage] += timeout_ms;
        if (timeout_ms > lfProf.gpu_peak[stage]) lfProf.gpu_peak[stage] = timeout_ms;
        ++lfProf.gpu_n[stage];
        if (lfProf.last_gpu_end != 0 && ts[0] >= lfProf.last_gpu_end)
        {
            const double gap = static_cast<double>(ts[0] - lfProf.last_gpu_end) * 1000.0
                               / static_cast<double>(lfProf.tick_rate);
            lfProf.gap_sum[stage] += gap;
            if (gap > lfProf.gap_peak[stage]) lfProf.gap_peak[stage] = gap;
            ++lfProf.gap_n[stage];
        }
        lfProf.last_gpu_end = ts[1];
    }
    D3D12_RANGE none = { 0, 0 };
    lfProf.query_readback->Unmap(0, &none);
}

// Start timing a card stage.
static bool GpuStageOpen(GpuStage stage)
{
    if (!PhasesOn()) return false;
    lfProf.stage_now = stage;
    if (!QueriesReady()) return false;
    lfProf.slot_stage[lfCore.ring_slot] = stage;
    lfCore.cmd->EndQuery(lfProf.query_heap, D3D12_QUERY_TYPE_TIMESTAMP, lfCore.ring_slot * 4);
    return true;
}

// Stop timing it.
static void GpuStageClose(GpuStage, unsigned query_count)
{
    if (lfProf.query_state != 1) return;
    const UINT base = lfCore.ring_slot * 4;
    lfCore.cmd->EndQuery(lfProf.query_heap, D3D12_QUERY_TYPE_TIMESTAMP, base + 1);
    lfCore.cmd->ResolveQueryData(lfProf.query_heap, D3D12_QUERY_TYPE_TIMESTAMP, base, query_count,
                             lfProf.query_readback, sizeof(UINT64) * base);
}

// Wait for a stage's fence, counting the wait.
static bool GpuStageWait(GpuStage stage, UINT64 fence, DWORD timeout_ms)
{
    const double t_wait = PhasesOn() ? PhaseClock() : 0.0;
    const bool ok = AwaitMark(lfCore.timeline, fence, timeout_ms);
    if (!PhasesOn()) return ok;
    const double spent = PhaseClock() - t_wait;
    lfProf.wait_sum[stage] += spent;
    if (spent > lfProf.wait_peak[stage]) lfProf.wait_peak[stage] = spent;
    ++lfProf.wait_n[stage];
    if (ok) TimingCollect();
    return ok;
}
