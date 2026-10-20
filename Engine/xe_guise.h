// xe_guise.h - a card of Intel's own that Intel's libraries turn away, shown
// to them as another maker's.
//
// XeSS Frame Generation and Xe Low Latency take any card of another maker
// that can run their shaders. Of Intel's own they take the newer ones only
// (Arc, Core Ultra): asked about the UHD 730 of an 11th to 14th generation
// processor they answer "unsupported card" and "unsupported driver" - not
// because the shaders would not run there, but because it is on no list of
// theirs. They learn who made a card from DXGI (the adapter's description).
// So while this is on, the description of that one card says another maker
// to everyone in this process who asks - and the libraries take the path they
// take for any card they know nothing about.
//
// How: the four calls that hand out an adapter's description (GetDesc,
// GetDesc1, GetDesc2, GetDesc3) are replaced in DXGI's own table of an
// adapter's calls - which every adapter object of the process shares - by
// ones that ask the real call and change two numbers for the card picked.
// Nothing here knows the engine (tests\xeprobe.cpp uses it too).
#pragma once
#include <dxgi1_6.h>
#include <windows.h>

namespace xe_guise
{
struct State
{
    bool on = false;
    LUID card = {};
    HRESULT (STDMETHODCALLTYPE *desc)(IDXGIAdapter *, DXGI_ADAPTER_DESC *) = nullptr;
    HRESULT (STDMETHODCALLTYPE *desc1)(IDXGIAdapter1 *, DXGI_ADAPTER_DESC1 *) = nullptr;
    HRESULT (STDMETHODCALLTYPE *desc2)(IDXGIAdapter2 *, DXGI_ADAPTER_DESC2 *) = nullptr;
    HRESULT (STDMETHODCALLTYPE *desc3)(IDXGIAdapter4 *, DXGI_ADAPTER_DESC3 *) = nullptr;
};
inline State &Is() { static State state; return state; }

// Another maker's numbers (an AMD card of the kind the libraries are known to run on).
template <class Description> inline void Dress(Description *d)
{
    const State &s = Is();
    if (d && s.on && d->AdapterLuid.LowPart == s.card.LowPart && d->AdapterLuid.HighPart == s.card.HighPart)
    {
        d->VendorId = 0x1002;
        d->DeviceId = 0x73BF;
    }
}
inline HRESULT STDMETHODCALLTYPE Desc(IDXGIAdapter *a, DXGI_ADAPTER_DESC *d) { const HRESULT r = Is().desc(a, d); if (SUCCEEDED(r)) Dress(d); return r; }
inline HRESULT STDMETHODCALLTYPE Desc1(IDXGIAdapter1 *a, DXGI_ADAPTER_DESC1 *d) { const HRESULT r = Is().desc1(a, d); if (SUCCEEDED(r)) Dress(d); return r; }
inline HRESULT STDMETHODCALLTYPE Desc2(IDXGIAdapter2 *a, DXGI_ADAPTER_DESC2 *d) { const HRESULT r = Is().desc2(a, d); if (SUCCEEDED(r)) Dress(d); return r; }
inline HRESULT STDMETHODCALLTYPE Desc3(IDXGIAdapter4 *a, DXGI_ADAPTER_DESC3 *d) { const HRESULT r = Is().desc3(a, d); if (SUCCEEDED(r)) Dress(d); return r; }

// The card's true description, whatever is on.
inline HRESULT True(IDXGIAdapter1 *adapter, DXGI_ADAPTER_DESC1 *d)
{
    const State &s = Is();
    return s.desc1 ? s.desc1(adapter, d) : adapter->GetDesc1(d);
}

// From now on this card is another maker's to whoever asks DXGI. False: it
// could not be done (the card stays what it is).
inline bool Put(IDXGIAdapter1 *adapter)
{
    State &s = Is();
    DXGI_ADAPTER_DESC1 d = {};
    if (!adapter || FAILED(True(adapter, &d))) return false;
    s.card = d.AdapterLuid;
    if (s.desc1) { s.on = true; return true; }      // the table is changed already: another card now
    IDXGIAdapter4 *whole = nullptr;
    if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter4), reinterpret_cast<void **>(&whole))) || !whole) return false;
    // The table of an adapter's calls: GetDesc is the 9th, GetDesc1 the 11th, GetDesc2 the 12th, GetDesc3 the 19th.
    void **table = *reinterpret_cast<void ***>(whole);
    whole->Release();
    DWORD before = 0;
    if (!VirtualProtect(table, 19 * sizeof(void *), PAGE_READWRITE, &before)) return false;
    s.desc = reinterpret_cast<decltype(s.desc)>(table[8]);
    s.desc1 = reinterpret_cast<decltype(s.desc1)>(table[10]);
    s.desc2 = reinterpret_cast<decltype(s.desc2)>(table[11]);
    s.desc3 = reinterpret_cast<decltype(s.desc3)>(table[18]);
    table[8] = reinterpret_cast<void *>(&Desc);
    table[10] = reinterpret_cast<void *>(&Desc1);
    table[11] = reinterpret_cast<void *>(&Desc2);
    table[18] = reinterpret_cast<void *>(&Desc3);
    VirtualProtect(table, 19 * sizeof(void *), before, &before);
    s.on = true;
    return true;
}

// The card is itself again.
inline void Off() { Is().on = false; }
}
