// sm_banner.inl - two lines of text at the top of the frame, in the middle,
// drawn by the engine itself: what Adaptive Motion Learning has to say while
// it studies a game (sm_learn.inl) - that it is studying, how far along it
// is, and what the player can do to help it.
//
// It is drawn the way the FPS counter is (lf_counter.inl), and for the same
// reason: a window of its own over the output would have the compositor
// blend the two for every frame shown. The text is drawn with GDI when it
// changes (which also shapes Arabic and sets it right to left), kept as two
// bytes a pixel - its dark edge and its fill - and blended onto the back
// buffer by the counter's own pass, in that buffer's format.
//
// BannerSay is called by the loop's thread, BannerStamp by the thread that
// presents.

static constexpr UINT kBannerW = 1920, kBannerH = 176, kBannerPitch = (kBannerW * 2u + 255u) & ~255u;

static struct BannerState
{
    std::mutex lock;
    wchar_t line[2][192] = {};
    unsigned asked = 0, drawn = ~0u;        // which text is asked for, which one is on the card
    bool on = false;
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    uint32_t *pixels = nullptr;
    HFONT font[2] = {};
    int scale_for = 0;
    winrt::com_ptr<ID3D12Resource> upload[2], glyphs;
    BYTE *mapped[2] = {};
    unsigned front = 0;
    winrt::com_ptr<ID3D12DescriptorHeap> views, target;
    UINT w = 0, h = 0;
    bool failed = false;
} g_banner;

// What is to stand there: a first line, and a second under it (either may be empty; both empty: nothing).
static void BannerSay(const wchar_t *first, const wchar_t *second)
{
    BannerState &b = g_banner;
    std::lock_guard<std::mutex> hold(b.lock);
    if (wcsncmp(b.line[0], first, 191) == 0 && wcsncmp(b.line[1], second, 191) == 0) return;
    wcsncpy_s(b.line[0], first, _TRUNCATE);
    wcsncpy_s(b.line[1], second, _TRUNCATE);
    b.on = first[0] != 0 || second[0] != 0;
    ++b.asked;
}

// The two lines as GDI draws them: the edge in red, the letters in green over it, each line in the middle of
// the box. Under g_banner.lock.
static bool BannerDraw(UINT screen_h)
{
    BannerState &b = g_banner;
    if (!b.dc)
    {
        b.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = kBannerW;
        info.bmiHeader.biHeight = -static_cast<LONG>(kBannerH);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        b.dib = b.dc ? CreateDIBSection(b.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        b.pixels = static_cast<uint32_t *>(bits);
        if (!b.dib || !b.pixels) return false;
        SelectObject(b.dc, b.dib);
        SetBkMode(b.dc, TRANSPARENT);
    }
    if (!b.pixels) return false;
    // 30 and 26 pixels tall on 1440 lines; other screens in proportion.
    const int scale = std::clamp(static_cast<int>(screen_h * 100u / 1440u), 60, 200);
    if (scale != b.scale_for)
    {
        for (HFONT &font : b.font) if (font) DeleteObject(font);
        b.font[0] = CreateFontW(-(30 * scale / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        b.font[1] = CreateFontW(-(26 * scale / 100), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        b.scale_for = scale;
    }
    const int edge = std::max(1, 2 * scale / 100);
    SIZE size[2] = {};
    int w = 0, h = 2 * edge;
    for (int i = 0; i < 2; ++i)
    {
        SelectObject(b.dc, b.font[i]);
        const int length = static_cast<int>(wcslen(b.line[i]));
        if (length) GetTextExtentPoint32W(b.dc, b.line[i], length, &size[i]);
        w = std::max<int>(w, size[i].cx);
        h += size[i].cy + (i && size[i].cy ? edge : 0);
    }
    w = std::min<int>((w + 2 * edge + 3) & ~1, kBannerW);
    h = std::min<int>((h + 1) & ~1, kBannerH);
    RECT all = {0, 0, w, h};
    FillRect(b.dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    int top = edge;
    for (int i = 0; i < 2; ++i)
    {
        const int length = static_cast<int>(wcslen(b.line[i]));
        if (!length) continue;
        SelectObject(b.dc, b.font[i]);
        const int left = std::max(edge, (w - static_cast<int>(size[i].cx)) / 2);
        // A line with Arabic or Hebrew in it is read from the right: its parts are set in that order.
        bool from_right = false;
        for (int at = 0; at < length; ++at) if (b.line[i][at] >= 0x0590 && b.line[i][at] <= 0x08FF) from_right = true;
        SetTextAlign(b.dc, from_right ? TA_RTLREADING : 0);
        SetTextColor(b.dc, RGB(255, 0, 0));
        for (int dy = -edge; dy <= edge; ++dy)
            for (int dx = -edge; dx <= edge; ++dx)
                if ((dx || dy) && dx * dx + dy * dy <= edge * edge + 1) TextOutW(b.dc, left + dx, top + dy, b.line[i], length);
        SetTextColor(b.dc, RGB(0, 255, 0));
        TextOutW(b.dc, left, top, b.line[i], length);
        top += size[i].cy + edge;
    }
    GdiFlush();
    b.w = static_cast<UINT>(w);
    b.h = static_cast<UINT>(h);
    return true;
}

// The banner onto a back buffer in COPY_DEST, by the thread that presents; the buffer is in COPY_DEST again
// afterwards. (Called right after CounterStamp, wherever that is.)
static void BannerStamp(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer)
{
    BannerState &b = g_banner;
    std::lock_guard<std::mutex> hold(b.lock);
    if (!b.on || b.failed || !list || !back_buffer || !lfCore.device) return;
    const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    // The counter's pass does the blending (made for this format, whether the counter itself is shown or not).
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> counter(c.lock);
    if (!CounterReady(desc.Format)) return;
    if (!b.glyphs)
    {
        auto fail = [&](const char *what) { b.failed = true; Trace("[banner] off: %s", what); };
        D3D12_DESCRIPTOR_HEAP_DESC views = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        D3D12_DESCRIPTOR_HEAP_DESC target = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
        if (FAILED(lfCore.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(b.views.put()))) ||
            FAILED(lfCore.device->CreateDescriptorHeap(&target, IID_PPV_ARGS(b.target.put())))) return fail("its descriptor heaps");
        D3D12_HEAP_PROPERTIES card = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC texture = {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = kBannerW; texture.Height = kBannerH; texture.DepthOrArraySize = 1; texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8_UNORM; texture.SampleDesc.Count = 1;
        if (FAILED(lfCore.device->CreateCommittedResource(&card, D3D12_HEAP_FLAG_NONE, &texture,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(b.glyphs.put())))) return fail("its texture");
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = DXGI_FORMAT_R8G8_UNORM; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
        lfCore.device->CreateShaderResourceView(b.glyphs.get(), &view, b.views->GetCPUDescriptorHandleForHeapStart());
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = UINT64(kBannerPitch) * kBannerH;
        buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (unsigned i = 0; i < 2; ++i)
        {
            D3D12_RANGE none = {0, 0};
            void *at = nullptr;
            if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(b.upload[i].put()))) ||
                FAILED(b.upload[i]->Map(0, &none, &at))) return fail("its upload buffers");
            b.mapped[i] = static_cast<BYTE *>(at);
        }
        b.drawn = ~0u;
    }
    if (b.drawn != b.asked)
    {
        if (!BannerDraw(desc.Height)) return;
        // The other buffer: the one in front may still be read by a list on its way.
        const unsigned next = b.front ^ 1u;
        for (UINT y = 0; y < b.h; ++y)
        {
            const uint32_t *from = b.pixels + static_cast<size_t>(y) * kBannerW;
            BYTE *row = b.mapped[next] + static_cast<size_t>(y) * kBannerPitch;
            for (UINT x = 0; x < b.w; ++x)
            {
                row[x * 2] = static_cast<BYTE>((from[x] >> 16) & 0xFF);
                row[x * 2 + 1] = static_cast<BYTE>((from[x] >> 8) & 0xFF);
            }
        }
        b.front = next;
        b.drawn = b.asked;
        auto to_write = Barrier(b.glyphs.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_write);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = b.upload[b.front].get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8_UNORM;
        from.PlacedFootprint.Footprint.Width = b.w;
        from.PlacedFootprint.Footprint.Height = b.h;
        from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = kBannerPitch;
        to.pResource = b.glyphs.get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto to_read = Barrier(b.glyphs.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_read);
    }
    if (b.w == 0 || b.h == 0 || desc.Width < b.w || desc.Height < b.h + 64) return;
    // In the middle, a little under the top edge.
    const UINT x = static_cast<UINT>((desc.Width - b.w) / 2), y = static_cast<UINT>(desc.Height * 3 / 100);
    auto to_target = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &to_target);
    const D3D12_CPU_DESCRIPTOR_HANDLE target = b.target->GetCPUDescriptorHandleForHeapStart();
    lfCore.device->CreateRenderTargetView(back_buffer, nullptr, target);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {float(x), float(y), float(b.w), float(b.h), 0.0f, 1.0f};
    const D3D12_RECT box = {LONG(x), LONG(y), LONG(x + b.w), LONG(y + b.h)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &box);
    ID3D12DescriptorHeap *heaps[] = {b.views.get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(c.root);
    list->SetPipelineState(c.blend);
    struct { int x, y; UINT encoding; float white; } numbers = {
        int(x), int(y),
        desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2u : 0u,
        lfHdr.keep ? 2.5375f : lfHdr.paper_white};
    list->SetGraphicsRoot32BitConstants(0, 4, &numbers, 0);
    list->SetGraphicsRootDescriptorTable(1, b.views->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    auto to_copy = Barrier(back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_copy);
}
