// «Телефон рядом», HTTPS: TLS 1.2 средствами Windows (Schannel) поверх сокета соединения. Рукопожатие -
// AcceptSecurityContext, дальше DecryptMessage / EncryptMessage записями; сертификат сервера - phone/cert.
#include <winsock2.h>
#define SECURITY_WIN32
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <algorithm>

#include "phone/phone_internal.h"

namespace phone::detail {

struct Tls {
    SOCKET s = INVALID_SOCKET;
    CtxtHandle ctx{};
    bool haveCtx = false;
    SecPkgContext_StreamSizes sizes{};
    std::string in;     // пришедшее и ещё не расшифрованное
    std::string plain;  // расшифрованное и ещё не прочитанное
    std::string out;    // буфер записи
};

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
CredHandle g_cred{};
bool g_haveCred = false;
std::vector<CredHandle> g_old;  // прежние (адрес поменялся) - соединения могли взять их; освобождаются в TlsReset

bool RecvMore(Tls& t) {
    char buf[16384];
    int k = recv(t.s, buf, sizeof(buf), 0);
    if (k <= 0) return false;
    t.in.append(buf, (size_t)k);
    return true;
}

bool RawSend(SOCKET s, const char* p, size_t n) {
    while (n > 0) {
        int k = send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

bool Handshake(Tls& t, CredHandle cred) {
    bool needRecv = true;
    for (int round = 0; round < 40; ++round) {
        if (needRecv || t.in.empty()) {
            if (!RecvMore(t)) return false;
            if (round == 0 && (unsigned char)t.in[0] != 0x16) return false;  // не TLS (http на порт https)
        }
        SecBuffer ib[2] = {{(ULONG)t.in.size(), SECBUFFER_TOKEN, t.in.data()}, {0, SECBUFFER_EMPTY, nullptr}};
        SecBuffer ob[1] = {{0, SECBUFFER_TOKEN, nullptr}};
        SecBufferDesc id{SECBUFFER_VERSION, 2, ib}, od{SECBUFFER_VERSION, 1, ob};
        ULONG attr = 0;
        SECURITY_STATUS st =
            AcceptSecurityContext(&cred, t.haveCtx ? &t.ctx : nullptr, &id,
                                  ASC_REQ_STREAM | ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_CONFIDENTIALITY |
                                      ASC_REQ_REPLAY_DETECT | ASC_REQ_SEQUENCE_DETECT | ASC_REQ_EXTENDED_ERROR,
                                  0, &t.ctx, &od, &attr, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            needRecv = true;
            continue;
        }
        if (st == SEC_E_OK || st == SEC_I_CONTINUE_NEEDED || (FAILED(st) && (attr & ASC_RET_EXTENDED_ERROR)))
            t.haveCtx = true;
        if (ob[0].pvBuffer) {  // ответ клиенту (или сообщение об ошибке)
            bool sent = ob[0].cbBuffer == 0 || RawSend(t.s, (const char*)ob[0].pvBuffer, ob[0].cbBuffer);
            FreeContextBuffer(ob[0].pvBuffer);
            if (!sent) return false;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) return false;
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer)
            t.in.erase(0, t.in.size() - ib[1].cbBuffer);
        else
            t.in.clear();
        if (st == SEC_E_OK) return QueryContextAttributesW(&t.ctx, SECPKG_ATTR_STREAM_SIZES, &t.sizes) == SEC_E_OK;
        needRecv = t.in.empty();
    }
    return false;
}

}  // namespace

bool TlsSetCert(PCCERT_CONTEXT c) {
    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.cCreds = 1;
    sc.paCred = &c;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
    sc.dwFlags = SCH_USE_STRONG_CRYPTO;
    CredHandle h;
    TimeStamp exp;
    if (AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_INBOUND, nullptr, &sc, nullptr, nullptr,
                                  &h, &exp) != SEC_E_OK)
        return false;
    AcquireSRWLockExclusive(&g_lock);
    if (g_haveCred) g_old.push_back(g_cred);
    g_cred = h;
    g_haveCred = true;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

bool TlsReady() {
    AcquireSRWLockShared(&g_lock);
    bool r = g_haveCred;
    ReleaseSRWLockShared(&g_lock);
    return r;
}

void TlsReset() {  // сервер остановлен, соединений нет
    AcquireSRWLockExclusive(&g_lock);
    for (CredHandle& h : g_old) FreeCredentialsHandle(&h);
    g_old.clear();
    if (g_haveCred) FreeCredentialsHandle(&g_cred);
    g_haveCred = false;
    ReleaseSRWLockExclusive(&g_lock);
}

Tls* TlsAccept(UINT_PTR s) {
    AcquireSRWLockShared(&g_lock);
    bool have = g_haveCred;
    CredHandle cred = g_cred;
    ReleaseSRWLockShared(&g_lock);
    if (!have) return nullptr;
    Tls* t = new Tls;
    t->s = (SOCKET)s;
    if (!Handshake(*t, cred)) {
        TlsClose(t);
        return nullptr;
    }
    return t;
}

int TlsRecv(Tls* t, char* buf, int n) {
    while (t->plain.empty()) {
        if (t->in.empty() && !RecvMore(*t)) return -1;
        SecBuffer b[4] = {{(ULONG)t->in.size(), SECBUFFER_DATA, t->in.data()},
                          {0, SECBUFFER_EMPTY, nullptr},
                          {0, SECBUFFER_EMPTY, nullptr},
                          {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        SECURITY_STATUS st = DecryptMessage(&t->ctx, &d, 0, nullptr);
        if (st == SEC_E_INCOMPLETE_MESSAGE) {
            if (!RecvMore(*t)) return -1;
            continue;
        }
        if (st == SEC_I_CONTEXT_EXPIRED) return 0;  // клиент закрыл (close_notify)
        if (st != SEC_E_OK) return -1;              // и повторное рукопожатие не поддерживаем
        std::string extra;
        for (SecBuffer& x : b) {
            if (x.BufferType == SECBUFFER_DATA) t->plain.append((const char*)x.pvBuffer, x.cbBuffer);
            if (x.BufferType == SECBUFFER_EXTRA) extra.assign((const char*)x.pvBuffer, x.cbBuffer);
        }
        t->in.swap(extra);
    }
    int k = (int)std::min<size_t>((size_t)n, t->plain.size());
    memcpy(buf, t->plain.data(), (size_t)k);
    t->plain.erase(0, (size_t)k);
    return k;
}

bool TlsSend(Tls* t, const char* p, size_t n) {
    const SecPkgContext_StreamSizes& z = t->sizes;
    t->out.resize(z.cbHeader + z.cbMaximumMessage + z.cbTrailer);
    while (n > 0) {
        size_t len = std::min<size_t>(n, z.cbMaximumMessage);
        char* base = t->out.data();
        memcpy(base + z.cbHeader, p, len);
        SecBuffer b[4] = {{z.cbHeader, SECBUFFER_STREAM_HEADER, base},
                          {(ULONG)len, SECBUFFER_DATA, base + z.cbHeader},
                          {z.cbTrailer, SECBUFFER_STREAM_TRAILER, base + z.cbHeader + len},
                          {0, SECBUFFER_EMPTY, nullptr}};
        SecBufferDesc d{SECBUFFER_VERSION, 4, b};
        if (EncryptMessage(&t->ctx, 0, &d, 0) != SEC_E_OK) return false;
        if (!RawSend(t->s, base, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
        p += len;
        n -= len;
    }
    return true;
}

void TlsClose(Tls* t) {
    if (!t) return;
    if (t->haveCtx) DeleteSecurityContext(&t->ctx);
    delete t;
}

}  // namespace phone::detail
