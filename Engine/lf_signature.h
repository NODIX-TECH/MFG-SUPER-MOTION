// lf_signature.h - a runtime DLL the user drops into native\libraries is
// only loaded when every check below passes; the shipped runtimes need none.
//
//   1. Windows accepts the file's Authenticode signature (revocation checked
//      along the whole chain);
//   2. the certificate that signed it is NVIDIA Corporation's;
//   3. that certificate leads to a root in this machine's own root store -
//      not to one the user's store or the file itself brought along;
//   4. the file's version resource names an NVIDIA product.
// A file that passed stays open for reading with no write sharing, so it
// cannot be swapped between the check and its use.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "version.lib")

namespace lf_trust
{
    constexpr const wchar_t *kSigner = L"NVIDIA Corporation";
    constexpr const wchar_t *kProductStart = L"NVIDIA";

    struct StoreCloser { void operator()(void *s) const { if (s) CertCloseStore(s, 0); } };
    struct CertFreer { void operator()(const CERT_CONTEXT *c) const { if (c) CertFreeCertificateContext(c); } };
    struct MsgCloser { void operator()(void *m) const { if (m) CryptMsgClose(m); } };
    using Store = std::unique_ptr<void, StoreCloser>;
    using Cert = std::unique_ptr<const CERT_CONTEXT, CertFreer>;
    using Msg = std::unique_ptr<void, MsgCloser>;

    // Files that passed, held open while the process lives.
    inline std::vector<HANDLE> &Held()
    {
        static std::vector<HANDLE> held;
        return held;
    }

    // 1. Authenticode, with revocation along the whole chain and no UI.
    inline bool WindowsAccepts(const wchar_t *path)
    {
        WINTRUST_FILE_INFO target = {};
        target.cbStruct = sizeof(target);
        target.pcwszFilePath = path;
        WINTRUST_DATA request = {};
        request.cbStruct = sizeof(request);
        request.dwUIChoice = WTD_UI_NONE;
        request.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        request.dwUnionChoice = WTD_CHOICE_FILE;
        request.pFile = &target;
        request.dwStateAction = WTD_STATEACTION_VERIFY;
        GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG verdict = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &policy, &request);
        request.dwStateAction = WTD_STATEACTION_CLOSE;         // always release the state
        WinVerifyTrust(nullptr, &policy, &request);
        return verdict == ERROR_SUCCESS;
    }

    // The certificate that signed the file, with the store it came in.
    inline Cert SigningCertificate(const wchar_t *path, Store &store)
    {
        HCERTSTORE raw_store = nullptr;
        HCRYPTMSG raw_msg = nullptr;
        DWORD encoding = 0, content = 0, format = 0;
        if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path, CERT_QUERY_CONTENT_FLAG_ALL, CERT_QUERY_FORMAT_FLAG_ALL, 0,
                              &encoding, &content, &format, &raw_store, &raw_msg, nullptr))
            return Cert();
        store.reset(raw_store);
        Msg msg(raw_msg);
        if (!msg || !store) return Cert();
        DWORD bytes = 0;
        if (!CryptMsgGetParam(msg.get(), CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &bytes) || bytes == 0)
            return Cert();
        std::vector<unsigned char> signer(bytes);
        if (!CryptMsgGetParam(msg.get(), CMSG_SIGNER_CERT_INFO_PARAM, 0, signer.data(), &bytes))
            return Cert();
        return Cert(CertFindCertificateInStore(store.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                               CERT_FIND_SUBJECT_CERT, signer.data(), nullptr));
    }

    // 2. The signer's plain name.
    inline bool SignedByNvidia(const CERT_CONTEXT *cert)
    {
        const DWORD chars = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
        if (chars == 0) return false;
        std::wstring name(chars, L'\0');
        if (CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, &name[0], chars) == 0) return false;
        return wcscmp(name.c_str(), kSigner) == 0;
    }

    // 3. A chain built only against the local machine's root store.
    inline bool ReachesMachineRoot(const CERT_CONTEXT *cert)
    {
        Store roots(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                  CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG, L"Root"));
        if (!roots) return false;
        CERT_CHAIN_ENGINE_CONFIG setup = {};
        setup.cbSize = sizeof(setup);
        setup.hExclusiveRoot = roots.get();
        HCERTCHAINENGINE engine = nullptr;
        if (!CertCreateCertificateChainEngine(&setup, &engine)) return false;
        bool good = false;
        CERT_CHAIN_PARA want = {};
        want.cbSize = sizeof(want);
        PCCERT_CHAIN_CONTEXT chain = nullptr;
        if (CertGetCertificateChain(engine, cert, nullptr, nullptr, &want, 0, nullptr, &chain))
        {
            CERT_CHAIN_POLICY_PARA rules = {};
            rules.cbSize = sizeof(rules);
            CERT_CHAIN_POLICY_STATUS result = {};
            result.cbSize = sizeof(result);
            good = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_AUTHENTICODE, chain, &rules, &result) &&
                   result.dwError == ERROR_SUCCESS;
            CertFreeCertificateChain(chain);
        }
        CertFreeCertificateChainEngine(engine);
        return good;
    }

    // 4. ProductName of the first language the version resource lists.
    inline bool NamesNvidiaProduct(const wchar_t *path)
    {
        DWORD ignored = 0;
        const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
        if (bytes == 0) return false;
        std::vector<unsigned char> info(bytes);
        if (!GetFileVersionInfoW(path, 0, bytes, info.data())) return false;
        struct Language { WORD id; WORD codepage; } *languages = nullptr;
        UINT languages_bytes = 0;
        if (!VerQueryValueW(info.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&languages),
                            &languages_bytes) || languages_bytes < sizeof(Language))
            return false;
        wchar_t key[256];
        _snwprintf_s(key, 256, _TRUNCATE, L"\\StringFileInfo\\%04x%04x\\ProductName", languages[0].id,
                     languages[0].codepage);
        wchar_t *product = nullptr;
        UINT product_chars = 0;
        return VerQueryValueW(info.data(), key, reinterpret_cast<void **>(&product), &product_chars) &&
               product != nullptr && product_chars > 0 && wcsncmp(product, kProductStart, wcslen(kProductStart)) == 0;
    }

    // Keep the file from being replaced while it is in use (at most eight).
    inline void HoldOpen(const wchar_t *path)
    {
        if (Held().size() >= 8) return;
        const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) Held().push_back(file);
    }
}

// All four checks, in order; the file is held open when they pass.
static bool TrustLoadDll(const wchar_t *path)
{
    if (!lf_trust::WindowsAccepts(path)) return false;
    lf_trust::Store store;
    lf_trust::Cert signer = lf_trust::SigningCertificate(path, store);
    if (!signer || !lf_trust::SignedByNvidia(signer.get()) || !lf_trust::ReachesMachineRoot(signer.get()))
        return false;
    signer.reset();
    store.reset();
    if (!lf_trust::NamesNvidiaProduct(path)) return false;
    lf_trust::HoldOpen(path);
    return true;
}

// A user-supplied runtime: the same checks (what names it, for the caller's log).
static bool TrustUserDll(const wchar_t *path, const char *what)
{
    (void)what;
    return TrustLoadDll(path);
}
