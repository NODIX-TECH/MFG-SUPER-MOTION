// lf_declare.h - every function of the engine that another topic file calls,
// declared before any is defined, so the topic files do not depend on each
// other's order. Default arguments live here and only here.
#pragma once

static void Trace(const char *pixel_format, ...);
static const char *NgxCodeText(NVSDK_NGX_Result r);
static bool PatchBytes(void *at, const void *from, size_t bytes);
static bool ArchHookInstall();
static int __cdecl ArchAnswer(void *gpu_at, ArchRecord *info);
static bool ArchWanted();
static int ArchPrepare();
static void TimingFlush();
static void TimingCapture(double acquire_start, UINT64 source_qpc, const char *kind);
static bool CmdOpen();
static void CmdCarry();
static void CmdCarryFlush();
static void CmdCarryDrop();
static void CmdDrain();
static void FgCostRead();
static unsigned FgStillCounted();
static void Luma11Stop();
static void ReportDeviceLoss(const char *where);
static UINT64 CmdSubmit();
static bool AwaitMark(ID3D12Fence *f, UINT64 v, DWORD timeout_ms);
static void CmdCloseSafe();
static void DropCommands();
static bool CmdSubmitShow(DWORD timeout_ms);
static int CardWanted();
static void ReportVram(const char *where);
static int CardInUse();
static bool GpuCoreStart();
static void CardsTell();
static void NVSDK_CONV NgxQuietLog(const char *, NVSDK_NGX_Logging_Level,
                                          NVSDK_NGX_Feature);
static NVSDK_NGX_FeatureCommonInfo *NgxSearchInfo(const wchar_t *data_dir);
static bool NgxStart();
static void NgxShutdown();
static ID3D12Resource *NewTexture(UINT w, UINT h_, DXGI_FORMAT pixel_format, bool uav_view);
static int ShownCap();
static void PaceCapture();
static void ShowAffinityApply();
static LRESULT CALLBACK ShowWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp);
static void ShowOriginRead();
static DWORD WINAPI ShowThread(LPVOID);
static void ShowStop();
static bool ShowStart(UINT width, UINT height, uint32_t flags);
static bool ShowDirect(const Pipeline &pipe);
static float WindowCoverage(const RECT &target);
static bool WindowInView(const RECT &target);
static bool WindowWantsOverlay(const RECT &target);
static bool WindowBounds(RECT &r);
static void WindowTrack();
static void ShowStayOnTop();
static void ShowRevealOnce();
static bool ShowResult(HRESULT status, const char *where);
static bool ShowRebuildIfStale();
static bool ShowPassThrough(Pipeline &pipe);
static bool StagedCreate(StagedTexture &staged, UINT w, UINT hgt, DXGI_FORMAT pixel_format, UINT tight_pitch,
                           D3D12_RESOURCE_FLAGS tex_flags = D3D12_RESOURCE_FLAG_NONE);
static void ClampWorkSize(UINT src_w, UINT src_h, UINT &w, UINT &height);
static bool PipelineBuild(Pipeline &pipe, UINT w, UINT hgt, UINT full_w = 0, UINT full_h = 0);
static bool StagedFill(StagedTexture &staged, const BYTE *from, UINT from_pitch, UINT hgt);
static void StagedCopy(ID3D12GraphicsCommandList *cmdl, StagedTexture &staged);
static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after);
static void StagedRelease(StagedTexture &staged);
static void LumaStop();
static void DeskStop();
static bool SwizzleReady();
static void SwizzleBind(ID3D12Resource *from);
static UINT FlowLumaDivisor();
static void FlowLumaStop();
static bool FlowLumaReady(UINT capture_w, UINT capture_h);
static bool LumaPipelineReady();
static bool LumaStart(UINT width, UINT height);
static bool LumaFromPicture();
static bool CaptureDeviceReady();
static HMONITOR MonitorByName(const wchar_t *name);
static IDXGIOutput *FindCaptureOutput(IDXGIAdapter1 *card, HMONITOR *elsewhere = nullptr);
static bool DeskStart(UINT w, UINT hgt);
static BridgeResult BridgeFrame(ID3D11Texture2D *frame, UINT *out_w, UINT *out_h,
                                      DXGI_FORMAT *out_format = nullptr);
static bool SwizzleIntoPicture(Pipeline &pipe);
static bool DeskTake(Pipeline &pipe);
static void WindowStop();
static bool SourceLive();
static bool WindowStart(HWND hwnd);
static void ScreenViaWgcStop();
static bool ScreenViaWgcStart(HMONITOR monitor);
static bool ScreenViaWgcTake(Pipeline &pipe);
static bool WindowTake(Pipeline &pipe);
static bool QueriesReady();
static bool GpuStageOpen(GpuStage stage);
static void GpuStageClose(GpuStage stage, unsigned query_count = 2);
static void QueryRead(int slot);
static void TimingCollect();
static bool GpuStageWait(GpuStage stage, UINT64 fence, DWORD timeout_ms);
static void PipelineRelease(Pipeline &pipe);
static bool PhasesOn();
static double PhaseClock();
static void PhaseCount(int idx, double t0);
static void TimingResult(const Pipeline &pipe, uint32_t index, bool fresh, const char *outcome);
static void PhaseSummary();
static bool ShowHdr(Pipeline &v);
static bool ShowFormatReady(bool hdr, bool pq = false);
static void HdrRelease();
static bool FgRequested();
// XeSS MFG, the second generator (xe_mfg.inl): asked by lf_dlssg.inl where NGX is asked for DLSS MFG.
static bool XeStart();
static bool XeReady(UINT w, UINT h, DXGI_FORMAT format);
static bool XeLive();
static unsigned XeMost();
static unsigned XeCount(unsigned wanted, bool exact);
static void XeFeed(ID3D12GraphicsCommandList *list, ID3D12Resource *picture, D3D12_RESOURCE_STATES state, const float (*moved)[2]);
static bool XeRunsOn(IDXGIAdapter1 *card);
static bool XeMake(unsigned frames, ID3D12Resource **into, ID3D12Resource *motion, ID3D12Resource *depth,
                   UINT field_w, UINT field_h, bool restart, double interval, UINT64 *taken);
static void XeAfter(ID3D12CommandQueue *queue, UINT64 taken);
static void XeTell(UINT64 taken, HANDLE event);
static void XeForget();
static void XeClose();
static bool StopAsked();
static unsigned FgExtraFrames();
static void CloseFgResources();
static bool FgPresent(Pipeline &v, ID3D12Resource *color, D3D12_RESOURCE_STATES state);
// A window scaled onto its whole screen (lf_window_fill.inl).
static bool FillWanted();
static bool Dlss5LinkOn();
static bool Dlss5LinkTake(Pipeline &pipe);
static void Dlss5LinkPause();
static void FillSet(HWND hwnd, bool on);
static bool UpscaleWanted();
static bool UpscaleMonitor(RECT &monitor);
static void UpscaleTrack(const RECT &frame);
static bool UpscaleWantsOverlay();
static void UpscaleFollowStart();
static void ShowBlit(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *src);
static bool VrrWanted();
static HRESULT ShowPresentNow(double source = -1.0);
// The FPS counter, drawn into the frame (lf_counter.inl).
static void CounterPresented(double source, bool own_capture);
static void CounterStamp(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer);
static void ShowProof(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, bool real);
static double LatencyNow();
static void VrrObserve();
static bool LowLatencyWanted();
static void LowLatencyWait();
static void LatencyPresented(double source);
static double LumaChangeNow();
static bool SceneCutNow(double change);
static void SourceSeen(bool repeated);
static void FgGateRepeat(double now);
static void FgGateFresh(double stamp);
static bool NetMotionRun(Pipeline &pipe, bool reset, UINT64 *sent_mark);
static double OutputRefresh();
static double VrrCeiling(double refresh);
static int VrrCap();
// The output as a Spout sender (sm_spout.inl).
static bool SpoutWanted();
static void SpoutCopy(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer);
static void SpoutAfter(ID3D12CommandQueue *queue);
static int SyncEvery();
static bool TearingWanted();
static double ShownRefresh();
static int PresentCap();
// What the app is told (sm_control.inl).
static void Status(const char *format, ...);
static bool GuardWideShift(float &across, float &down);     // sm_guard.inl (round 53)
