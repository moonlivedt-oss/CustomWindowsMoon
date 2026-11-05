#include "helpers/translate.h"

#include <winhttp.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#include "core/bg.h"
#include "core/text.h"
#include "core/usage.h"
#include "helpers/translate_logic.h"
#include "ring/selring.h"

namespace translate {

namespace {

// Помощник. Кавычек " в нём нет: он передаётся одним аргументом -c. Сеть выключена до загрузки Argos: что бы ни
// попыталось выйти наружу, получит ошибку. Stanza (деление на предложения в Argos) по умолчанию сверяет список моделей
// с GitHub: ему велено брать то, что лежит рядом с моделью. stdout занят обменом, всё остальное уходит в никуда.
constexpr wchar_t kHelper[] =
    LR"PY(import sys,os,time,threading
out=sys.stdout.buffer
sys.stdout=sys.stderr
def say(s):
    out.write(s.encode('utf-8','replace')+b'\n')
    out.flush()
B=chr(92)
def esc(s):
    return s.replace(B,B+B).replace('\n',B+'n').replace('\r',B+'r').replace('\t',B+'t')
def une(s):
    r=[];i=0
    while i<len(s):
        c=s[i]
        if c==B and i+1<len(s):
            i+=1;c={'n':'\n','r':'\r','t':'\t'}.get(s[i],s[i])
        r.append(c);i+=1
    return ''.join(r)
mode=sys.argv[1] if len(sys.argv)>1 else 'serve'
if mode=='install':
    try:
        import argostranslate.package as p
        p.update_package_index()
        a=[x for x in p.get_available_packages() if x.from_code==sys.argv[2] and x.to_code==sys.argv[3]]
        if not a:
            say('FAIL\tnomodel');os._exit(4)
        p.install_from_path(a[0].download())
        say('DONE');os._exit(0)
    except Exception as e:
        say('FAIL\t'+esc(str(e)));os._exit(5)
import socket
def nonet(*a,**k):
    raise OSError('network is off in the local translator')
socket.socket.connect=nonet
socket.socket.connect_ex=nonet
socket.create_connection=nonet
socket.getaddrinfo=nonet
os.environ['HF_HUB_OFFLINE']='1'
try:
    import argostranslate.translate as tr
    import stanza
    pipeline=stanza.Pipeline
    def offline(*a,**k):
        k['download_method']=None
        return pipeline(*a,**k)
    stanza.Pipeline=offline
except Exception:
    say('FAIL\tnoargos');os._exit(3)
langs={l.code:l for l in tr.get_installed_languages()}
pairs=[a+'>'+b for a in langs for b in langs if a!=b and langs[a].get_translation(langs[b])]
say('READY\t'+','.join(pairs))
last=[time.time()]
def idle():
    while True:
        time.sleep(15)
        if time.time()-last[0]>300:
            os._exit(0)
threading.Thread(target=idle,daemon=True).start()
for raw in sys.stdin.buffer:
    parts=raw.decode('utf-8','replace').rstrip('\r\n').split('\t',3)
    if len(parts)<4:
        continue
    last[0]=time.time()
    try:
        say('OK\t'+parts[0]+'\t'+esc(tr.translate(une(parts[3]),parts[1],parts[2])))
    except Exception as e:
        say('ERR\t'+parts[0]+'\t'+esc(str(e)))
    last[0]=time.time()
)PY";

constexpr DWORD kStartMs = 150000, kTextMs = 120000, kInstallMs = 900000;

std::atomic<bool> g_google{false}, g_fake{false};
std::mutex g_mu;  // переводим по одному: помощник один, и он однопоточный

struct Helper {
    HANDLE proc = nullptr, job = nullptr, in = nullptr, out = nullptr;
    std::string buf;
    std::wstring pairs;
    int nextId = 0;
} g_h;

std::map<std::wstring, std::wstring> g_cache;  // только в памяти: на диск переводы не пишем

void CloseHelper(Helper& h) {
    if (h.in) CloseHandle(h.in);
    if (h.out) CloseHandle(h.out);
    if (h.job) CloseHandle(h.job);  // KILL_ON_JOB_CLOSE: процесс уходит вместе с заданием
    if (h.proc) CloseHandle(h.proc);
    h = Helper{};
}

bool Exists(const std::wstring& f) {
    DWORD a = GetFileAttributesW(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Python из реестра, тот, в котором могут стоять модели. Не из PATH и не из текущей папки
std::vector<std::wstring> PythonCandidates() {
    std::vector<std::wstring> out;
    for (const wchar_t* ver : {L"3.11", L"3.12", L"3.13"})
        for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
            std::wstring key = std::wstring(L"Software\\Python\\PythonCore\\") + ver + L"\\InstallPath";
            wchar_t path[MAX_PATH] = {};
            DWORD size = sizeof(path);
            if (RegGetValueW(root, key.c_str(), L"ExecutablePath", RRF_RT_REG_SZ, nullptr, path, &size) ==
                    ERROR_SUCCESS &&
                Exists(path))
                out.push_back(path);
        }
    return out;
}

bool Launch(Helper& h, const std::wstring& python, const std::wstring& args) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
    if (!CreatePipe(&outR, &outW, &sa, 0)) {
        CloseHandle(inR);
        CloseHandle(inW);
        return false;
    }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    // наследуются только эти три ручки, а не все открытые в программе
    HANDLE list[3] = {inR, outW, nul};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto* attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list, sizeof(list), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inR;
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError = nul;
    si.lpAttributeList = attrs;

    // -P: текущая папка не попадает в пути поиска модулей; рабочая папка - системная, чужого модуля там нет
    std::wstring cmd = L"\"" + python + L"\" -P -c \"" + kHelper + L"\" " + args;
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    PROCESS_INFORMATION pi{};
    BOOL ok =
        CreateProcessW(python.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                       nullptr, sys, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(inR);
    CloseHandle(outW);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(inW);
        CloseHandle(outR);
        return false;
    }
    // задание: помощник не переживёт программу, даже если та упадёт
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        AssignProcessToJobObject(job, pi.hProcess);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    h.proc = pi.hProcess;
    h.job = job;
    h.in = inW;
    h.out = outR;
    return true;
}

bool Alive(const Helper& h) { return h.proc && WaitForSingleObject(h.proc, 0) == WAIT_TIMEOUT; }

// Строка от помощника с ограничением по времени. false - он умер или молчит слишком долго
bool ReadLine(Helper& h, DWORD timeoutMs, std::wstring& line) {
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    for (;;) {
        size_t nl = h.buf.find('\n');
        if (nl != std::string::npos) {
            line = Widen(h.buf.substr(0, nl));
            h.buf.erase(0, nl + 1);
            return true;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(h.out, nullptr, 0, nullptr, &avail, nullptr)) return false;  // процесс вышел, канал закрыт
        if (avail) {
            std::string chunk(std::min<DWORD>(avail, 65536), '\0');
            DWORD got = 0;
            if (!ReadFile(h.out, chunk.data(), (DWORD)chunk.size(), &got, nullptr)) return false;
            h.buf.append(chunk, 0, got);
            continue;
        }
        if (bg::Quitting() || GetTickCount64() >= until) return false;
        Sleep(15);
    }
}

bool WriteLine(Helper& h, const std::wstring& line) {
    std::string bytes = Narrow(line) + "\n";
    DWORD put = 0;
    return WriteFile(h.in, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size();
}

// Помощник запущен и знает свои пары. Не вышло - почему
bool EnsureHelper(const Job& j, Problem& problem, std::wstring& error) {
    if (Alive(g_h)) return true;
    CloseHelper(g_h);
    if (j.msgStep) PostMessageW(j.wnd, j.msgStep, kStarting, (LPARAM)j.texts.size());
    std::vector<std::wstring> pys = PythonCandidates();
    for (const std::wstring& py : pys) {
        Helper h;
        if (!Launch(h, py, L"serve")) continue;
        std::wstring line;
        for (;;) {
            if (!ReadLine(h, kStartMs, line)) {
                line.clear();
                break;
            }
            HelperLine parsed = ParseHelperLine(line);
            if (parsed.kind == HelperLine::Ready) {
                h.pairs = parsed.text;
                g_h = std::move(h);
                return true;
            }
            if (parsed.kind == HelperLine::Fail) break;
        }
        CloseHelper(h);  // в этом Python нет Argos: смотрим следующий
    }
    problem = Problem::NoLocal;
    error = pys.empty() ? L"Не найден Python 3.11-3.13 для локального перевода"
                        : L"В установленном Python нет Argos Translate";
    return false;
}

bool LocalOne(const std::wstring& text, const std::wstring& src, const std::wstring& tgt, std::wstring& out,
              std::wstring& error) {
    const std::wstring key = src + L">" + tgt + L"\t" + text;
    auto cached = g_cache.find(key);
    if (cached != g_cache.end()) {
        out = cached->second;
        return true;
    }
    const std::wstring id = std::to_wstring(++g_h.nextId);
    if (!WriteLine(g_h, id + L"\t" + src + L"\t" + tgt + L"\t" + Escape(text))) {
        error = L"Локальный переводчик не отвечает";
        return false;
    }
    std::wstring line;
    for (;;) {
        if (!ReadLine(g_h, kTextMs, line)) {
            CloseHelper(g_h);
            error = L"Локальный переводчик не отвечает";
            return false;
        }
        HelperLine r = ParseHelperLine(line);
        if ((r.kind == HelperLine::Ok || r.kind == HelperLine::Err) && r.id == id) {
            if (r.kind == HelperLine::Err) {
                error = L"Локальный переводчик: " + r.text.substr(0, 160);
                return false;
            }
            out = r.text;
            if (g_cache.size() >= 500) g_cache.clear();
            g_cache[key] = out;
            return true;
        }
    }
}

// Абзацы переводятся по отдельности: перевод сохраняет разбивку исходного текста
bool LocalText(const std::wstring& text, const std::wstring& src, const std::wstring& tgt, std::wstring& out,
               std::wstring& error) {
    out.clear();
    for (size_t at = 0;;) {
        size_t nl = text.find(L'\n', at);
        std::wstring para = text.substr(at, nl == std::wstring::npos ? nl : nl - at);
        while (!para.empty() && para.back() == L'\r') para.pop_back();
        if (!Trim(para).empty()) {
            std::wstring piece;
            if (!LocalOne(Trim(para), src, tgt, piece, error)) return false;
            out += piece;
        }
        if (nl == std::wstring::npos) return true;
        out += L"\r\n";
        at = nl + 1;
    }
}

// ---- Google ----
bool GooglePost(const std::wstring& text, const std::wstring& tgt, std::wstring& out, std::wstring& error,
                int& status) {
    status = 0;
    HINTERNET s = WinHttpOpen(L"RadialMenu/1.0 (desktop utility)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"translate.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
    std::wstring path = L"/translate_a/single?client=gtx&sl=auto&dt=t&tl=" + tgt;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : nullptr;
    std::string body = "q=" + Narrow(selring::UrlEncode(text)), reply;
    if (r) WinHttpSetTimeouts(r, 8000, 8000, 15000, 25000);
    static const wchar_t kHead[] = L"Content-Type: application/x-www-form-urlencoded\r\n";
    bool sent = r && WinHttpSendRequest(r, kHead, (DWORD)-1, body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(r, nullptr);
    if (sent) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &size, nullptr);
        status = (int)code;
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || !got) break;
            reply.append(chunk, 0, got);
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (status != 200) {
        error = HttpError(status);
        return false;
    }
    if (!ParseGtx(Widen(reply), out)) {
        error = L"Google ответил непонятно";
        return false;
    }
    return true;
}

bool GoogleText(const std::wstring& text, const std::wstring& tgt, std::wstring& out, std::wstring& error) {
    out.clear();
    bool first = true;
    for (const std::wstring& chunk : SplitChunks(text)) {
        std::wstring part;
        int status = 0;
        bool ok = GooglePost(chunk, tgt, part, error, status);
        // сеть моргнула - один повтор; при лимите 429 повтор только продлит его
        if (!ok && status != 429) {
            Sleep(500);
            ok = GooglePost(chunk, tgt, part, error, status);
        }
        if (!ok) return false;
        out += first ? part : L"\n" + part;
        first = false;
    }
    return true;
}

void RunGoogle(const Job& j, Result& r) {
    r.via = Via::Google;
    usage::Hit(L"translate_google");
    r.texts.assign(j.texts.size(), L"");
    std::vector<size_t> todo;
    for (size_t i = 0; i < j.texts.size(); ++i) {
        if (ContainsSecret(j.texts[i]))
            ++r.blocked;  // наружу не уходит ни при каких настройках
        else if (!Trim(j.texts[i]).empty())
            todo.push_back(i);
    }
    if (todo.empty()) {
        r.error = r.blocked ? L"Текст похож на секрет: в Google он не отправлен" : L"Нечего переводить";
        r.problem = Problem::Failed;
        return;
    }
    // снимок даёт десятки коротких кусков: одним запросом быстрее и меньше упираемся в лимит
    if (todo.size() > 1) {
        std::wstring joined;
        for (size_t i : todo) {
            std::wstring one;
            for (wchar_t c : j.texts[i]) one += c == L'\n' || c == L'\r' ? L' ' : c;
            joined += joined.empty() ? one : L"\n" + one;
        }
        std::wstring reply, error;
        std::vector<std::wstring> parts;
        if (GoogleText(joined, j.target, reply, error) && SplitBatch(reply, todo.size(), parts)) {
            for (size_t k = 0; k < todo.size(); ++k) r.texts[todo[k]] = Trim(parts[k]);
            r.ok = true;
            return;
        }
    }
    for (size_t k = 0; k < todo.size(); ++k) {
        if (j.msgStep) PostMessageW(j.wnd, j.msgStep, k, (LPARAM)todo.size());
        if (!GoogleText(j.texts[todo[k]], j.target, r.texts[todo[k]], r.error)) {
            r.problem = Problem::Failed;
            return;
        }
    }
    r.error.clear();
    r.ok = true;
}

void RunLocal(const Job& j, Result& r) {
    r.via = Via::Local;
    if (!EnsureHelper(j, r.problem, r.error)) return;
    if (!HasPair(g_h.pairs, j.source, j.target)) {
        r.problem = Problem::NoModel;
        r.error = L"Нет локальной модели " + j.source + L" - " + j.target;
        return;
    }
    r.texts.assign(j.texts.size(), L"");
    for (size_t i = 0; i < j.texts.size(); ++i) {
        if (j.msgStep) PostMessageW(j.wnd, j.msgStep, i, (LPARAM)j.texts.size());
        if (bg::Quitting() || !LocalText(j.texts[i], j.source, j.target, r.texts[i], r.error)) {
            r.problem = Problem::Failed;
            return;
        }
    }
    r.ok = true;
}

void Post(const Job& j, Result&& r) {
    r.tag = j.tag;
    r.source = j.source;
    r.target = j.target;
    auto* out = new Result(std::move(r));
    if (!PostMessageW(j.wnd, j.msgDone, 0, (LPARAM)out)) delete out;  // окно уже закрыли: итог некому забрать
}

}  // namespace

void Start(Job job) {
    auto j = std::make_shared<Job>(std::move(job));
    bool started = bg::Run([j] {
        std::lock_guard<std::mutex> lock(g_mu);
        Result r;
        if (g_fake) {
            for (const std::wstring& t : j->texts) r.texts.push_back(L"[" + j->target + L"] " + t);
            r.ok = true;
        } else if (j->forceGoogle) {
            if (g_google) {
                RunGoogle(*j, r);
            } else {
                r.problem = Problem::Failed;
                r.error = L"Google выключен в настройках";
            }
        } else {
            RunLocal(*j, r);
            // локально нельзя вовсе (нет Python или модели), а Google разрешён настройкой
            if (!r.ok && g_google && (r.problem == Problem::NoLocal || r.problem == Problem::NoModel)) {
                r = Result{};
                RunGoogle(*j, r);
            }
        }
        Post(*j, std::move(r));
    });
    if (!started) {
        Result r;
        r.problem = Problem::Failed;
        r.error = L"Не удалось запустить перевод";
        Post(*j, std::move(r));
    }
}

void InstallModel(const std::wstring& source, const std::wstring& target, uint64_t tag, HWND wnd, UINT msgDone) {
    Job job;
    job.source = source;
    job.target = target;
    job.tag = tag;
    job.wnd = wnd;
    job.msgDone = msgDone;
    auto j = std::make_shared<Job>(std::move(job));
    bg::Run([j] {
        std::lock_guard<std::mutex> lock(g_mu);
        Result r;
        r.problem = Problem::Failed;
        r.error = L"Не найден Python с Argos Translate";
        for (const std::wstring& py : PythonCandidates()) {
            Helper h;
            if (!Launch(h, py, L"install " + j->source + L" " + j->target)) continue;
            std::wstring line;
            HelperLine parsed;
            while (ReadLine(h, kInstallMs, line)) {
                parsed = ParseHelperLine(line);
                if (parsed.kind == HelperLine::Done || parsed.kind == HelperLine::Fail) break;
            }
            CloseHelper(h);
            if (parsed.kind == HelperLine::Done) {
                r.ok = true;
                r.problem = Problem::None;
                r.error.clear();
                break;
            }
            if (parsed.kind == HelperLine::Fail && parsed.text != L"nomodel")
                r.error = L"Модель не скачалась: " + parsed.text;
            if (parsed.kind == HelperLine::Fail && parsed.text == L"nomodel")
                r.error = L"На серверах Argos нет такой модели";
        }
        CloseHelper(g_h);  // следующий перевод перечитает список моделей
        Post(*j, std::move(r));
    });
}

void SetGoogleAllowed(bool on) { g_google = on; }
bool GoogleAllowed() { return g_google; }
void SetFake(bool on) { g_fake = on; }

void Stop() {
    std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
    if (lock.owns_lock()) CloseHelper(g_h);
}

}  // namespace translate
