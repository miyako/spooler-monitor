#include "monitor.hpp"
#include <iostream>
#include <limits>
#include <cerrno>
#ifdef __APPLE__
#include "cups_backend.hpp"
#elif defined(_WIN32)
#include "windows_backend.hpp"
#endif

using namespace monitor;
namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
bool contains(const std::string& value, const std::string& part) { return value.find(part) != std::string::npos; }
struct MockState {
    int lists = 0, gets = 0, mutations = 0;
    bool pause = false, missing = false, fail_list = false, fail_get = false;
    int mutation_error = 0;
    bool uncertain = false, throw_unexpected = false, fail_before_submission = false;
    std::size_t count = 1;
};
Queue sample() {
    Queue q;
    q.id = "世界 queue"; q.display_name = "Printer \"one\"";
    q.backend = native_backend_name(); q.state = "idle";
    return q;
}
class Mock final : public Backend {
    std::shared_ptr<MockState> s;
public:
    explicit Mock(std::shared_ptr<MockState> state) : s(std::move(state)) {}
    const char* name() const override { return native_backend_name(); }
    std::vector<Queue> list() override {
        ++s->lists;
        if (s->fail_list) throw Error(5, "backend_unavailable", "offline", name());
        return std::vector<Queue>(s->missing ? 0 : s->count, sample());
    }
    Queue get(const std::string& id) override {
        ++s->gets;
        check(id == sample().id, "get exact configured ID");
        if (s->throw_unexpected) throw std::runtime_error("injected observation failure");
        if (s->fail_get) throw Error(3, "unknown_queue", "removed", name(), 77);
        auto q = sample();
        if (s->mutations) q.state = s->pause ? "stopped" : "idle";
        return q;
    }
    void mutate(const std::string& id, bool pause) override {
        ++s->mutations; s->pause = pause;
        check(id == sample().id, "mutation exact configured ID");
        if (s->fail_before_submission)
            throw Error(4, "access_denied", "cannot open administrative handle", name());
        if (s->mutation_error) {
            const char* codes[] = {"", "", "", "unknown_queue", "access_denied",
                                   "backend_unavailable", "unsupported_operation", "", "native_error"};
            throw Error(s->mutation_error, codes[s->mutation_error], "rejected command", name(), 99,
                        s->uncertain ? "uncertain" : "rejected");
        }
    }
};
Factory fake(std::shared_ptr<MockState> state) {
    return [state]() { return std::make_unique<Mock>(state); };
}
void protocol_tests() {
    auto state = std::make_shared<MockState>();
    auto invoke = [&](std::vector<std::string> args) { return execute(args, fake(state)); };
    auto r = invoke({"list"});
    check(r.status == 0 && r.err.empty() && contains(r.out, "\"queues\":[{"), "mock list");
    check(contains(r.out, "\"acceptingJobs\":null") && contains(r.out, "\"reasons\":null") &&
          contains(r.out, "\"observedAt\":") && contains(r.out, "\"raw\":{}"), "complete queue schema");
    r = invoke({"get", sample().id});
    check(r.status == 0 && state->gets == 1 && state->mutations == 0, "get read-only");
    for (const auto& operation : {"pause", "resume"}) {
        r = invoke({operation, sample().id});
        check(r.status == 0 && contains(r.out, "\"accepted\":true") &&
              contains(r.out, "\"observation\":\"succeeded\""), "explicit mutation");
        check(state->pause == (std::string(operation) == "pause"), "pause/resume dispatch");
    }
    state->fail_get = true;
    r = invoke({"pause", sample().id});
    check(r.status == 7 && r.out.empty() && contains(r.err, "\"code\":\"observation_failed\"") &&
          contains(r.err, "\"accepted\":true") && contains(r.err, "\"queue\":null") &&
          contains(r.err, "\"outcome\":\"accepted\"") && contains(r.err, "\"nativeCode\":77"),
          "accepted but observation failed is exit 7 retaining acceptance");
    r = invoke({"get", sample().id});
    check(r.status == 3 && !contains(r.err, "\"mutation\""), "get failure distinct from post-mutation");
    state->fail_get = false; state->throw_unexpected = true;
    r = invoke({"resume", sample().id});
    check(r.status == 7 && contains(r.err, "\"queue\":null"), "unexpected failure retains accepted mutation");
    state->throw_unexpected = false;
    state->fail_before_submission = true;
    r = invoke({"pause", sample().id});
    check(r.status == 4 && contains(r.err, "\"outcome\":\"not_submitted\""),
          "backend pre-submission failure preserves definite absence of submission");
    state->fail_before_submission = false;
    for (int status : {3, 4, 5, 6, 8}) {
        state->mutation_error = status;
        r = invoke({"pause", sample().id});
        check(r.status == status && contains(r.err, "\"accepted\":false") &&
              contains(r.err, "\"outcome\":\"rejected\"") && !contains(r.err, "\"queue\":"),
              "backend rejection mapping");
    }
    state->uncertain = true; state->mutation_error = 5;
    r = invoke({"pause", sample().id});
    check(r.status == 5 && contains(r.err, "\"outcome\":\"uncertain\""), "uncertainty is not rejection");
    state->mutation_error = 0; state->uncertain = false; state->missing = true;
    const auto mutations_before = state->mutations;
    r = invoke({"pause", "\\\\unconfigured\\remote"});
    check(r.status == 3 && state->mutations == mutations_before && contains(r.err, "not_submitted"),
          "arbitrary remote ID never opened or submitted");
    r = invoke({"list"});
    check(contains(r.out, "\"queues\":[]"), "empty list success");
    state->missing = false; state->fail_list = true;
    r = invoke({"resume", sample().id});
    check(r.status == 5 && state->mutations == mutations_before && contains(r.err, "not_submitted"),
          "failed preflight never submits mutation");
    state->fail_list = false; state->count = max_queues + 1;
    r = invoke({"list"});
    check(r.status == 8 && r.out.empty(), "printer count bound");
    state = std::make_shared<MockState>();
    const std::vector<std::vector<std::string>> invalid = {
        {}, {""}, {"List"}, {"list", "extra"}, {"get"}, {"pause", ""}, {"get", "line\nbreak"},
        {"get", std::string("a\0b", 3)}, {"get", std::string(max_id + 1, 'a')},
        {"inspect-argument"}, {"--version", "extra"}, {"get", "a", "extra"},
        {"inspect-argument", std::string(max_argument + 1, 'x')},
        {"inspect-argument", std::string("\xc0\xaf", 2)}};
    for (const auto& args : invalid) {
        r = invoke(args);
        check(r.status == 2 && r.out.empty() && contains(r.err, usage) &&
              contains(r.err, "\"operation\":\"arguments\""), "invalid arguments exact diagnostic");
    }
    check(state->lists == 0 && state->mutations == 0, "invalid args never initialize backend");
    for (const std::string literal : {"", " ", "世界 😀", "quote\" slash\\\n\t", ";&`$(echo nope)"}) {
        r = invoke({"inspect-argument", literal});
        check(r.status == 0 && contains(r.out, "\"argument\":" + Json(literal).text()), "literal roundtrip");
    }
    for (const auto& op : {"--version", "capabilities"}) {
        r = invoke({op});
        check(r.status == 0 && r.err.empty(), "offline diagnostic success");
    }
    check(state->lists == 0, "diagnostics never access spooler");
    auto unavailable = []() -> std::unique_ptr<Backend> { throw Error(5, "backend_unavailable", "offline"); };
    r = execute({"pause", sample().id}, unavailable);
    check(r.status == 5 && contains(r.err, "\"mutation\":") && contains(r.err, "not_submitted"),
          "factory failure preserves mutation preflight context");
}
void encoding_tests() {
    for (const std::string s : {"a", "世界 😀", "\x7f", "\xc2\x80", "\xf4\x8f\xbf\xbf"})
        check(valid_utf8(s), "valid UTF-8");
    for (const std::string s : {"\x80", "\xff", "\xc0\xaf", "\xc2", "\xe2\x82", "\xe0\x80\x80",
                               "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80"})
        check(!valid_utf8(s), "malformed UTF-8 rejected");
    check(Json(std::string("\0\1\n\"\\", 5)).text() == "\"\\u0000\\u0001\\n\\\"\\\\\"", "JSON escaping");
    check(!valid_id("a\tb") && !valid_id(std::string("a\0b", 3)) &&
          valid_id("\\\\server\\configured queue") && valid_id("世界 café"), "ID guards");
    check(windows_state(0) == "idle" && windows_state(0x400) == "processing" &&
          windows_state(0x401) == "stopped" && windows_state(0x402) == "stopped" &&
          windows_state(0x2000400) == "stopped" &&
          windows_state(0x80000000) == "unknown", "conservative Windows status precedence");
    check(windows_state(0x4000) == "processing", "Windows PROCESSING flag is processing");
    check(windows_state(0x4001) == "stopped" && windows_state(0x4002) == "stopped",
          "Windows paused/error dominates PROCESSING");
    check(windows_state(0x10000) == "unknown", "Windows warm-up alone is not stopped");
    check(windows_state(0x10400) == "processing" && windows_state(0x14000) == "processing",
          "Windows warm-up with PRINTING or PROCESSING remains processing");
    check(windows_state(0x14001) == "stopped" && windows_state(0x10402) == "stopped",
          "Windows paused/error dominates warm-up plus normal processing");
    check(contains(windows_flags(0x14000).text(), "\"WARMING_UP\":true") &&
          contains(windows_flags(0x14000).text(), "\"PROCESSING\":true") &&
          contains(windows_flags(0x14000).text(), "\"PRINTING\":false"),
          "Windows raw warm-up and PROCESSING flags remain intact");
    check(contains(windows_flags(0x10402).text(), "\"WARMING_UP\":true") &&
          contains(windows_flags(0x10402).text(), "\"PRINTING\":true") &&
          contains(windows_flags(0x10402).text(), "\"ERROR\":true"),
          "Windows raw conflicting warm-up/printing/error flags remain intact");
    auto flags = windows_flags(0x401).text();
    check(contains(flags, "\"PAUSED\":true") && contains(flags, "\"PRINTING\":true") &&
          contains(flags, "\"ERROR\":false") && contains(flags, "\"DRIVER_UPDATE_NEEDED\":false"),
          "Windows all named flags preserved including conflicts");
    check(contains(windows_flags(0x2000000).text(), "\"SERVER_OFFLINE\":true") &&
          contains(windows_flags(0x2000000).text(), "\"DRIVER_UPDATE_NEEDED\":false") &&
          contains(windows_flags(0x4000000).text(), "\"DRIVER_UPDATE_NEEDED\":true"),
          "Windows server-offline and driver-update bits are distinct");
}
#ifdef __APPLE__
Ipp reply(ipp_status_t status = IPP_STATUS_OK) {
    Ipp r(ippNew());
    ippSetStatusCode(r.get(), status);
    return r;
}
void printer(ipp_t* r, const std::string& id, int state = 3, int type = 0, bool temporary = false) {
    ippAddString(r, IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", nullptr, id.c_str());
    ippAddString(r, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-info", nullptr, "世界 \"printer\"");
    ippAddInteger(r, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-type", type);
    ippAddBoolean(r, IPP_TAG_PRINTER, "printer-is-temporary", temporary);
    if (state >= 0) ippAddInteger(r, IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", state);
    ippAddBoolean(r, IPP_TAG_PRINTER, "printer-is-accepting-jobs", 1);
    ippAddInteger(r, IPP_TAG_PRINTER, IPP_TAG_INTEGER, "queued-job-count", 4);
    const char* reasons[] = {"paused", "media-needed"};
    ippAddStrings(r, IPP_TAG_PRINTER, IPP_TAG_KEYWORD, "printer-state-reasons", 2, nullptr, reasons);
    ippAddString(r, IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-state-message", nullptr, "waiting");
    ippAddString(r, IPP_TAG_PRINTER, IPP_TAG_URI, "printer-uri-supported", nullptr,
                 "ipp://untrusted.example/printers/not-authoritative");
    const int operations[] = {IPP_OP_GET_PRINTER_ATTRIBUTES, IPP_OP_PAUSE_PRINTER, IPP_OP_RESUME_PRINTER};
    ippAddIntegers(r, IPP_TAG_PRINTER, IPP_TAG_ENUM, "operations-supported", 3, operations);
}
template<class F> void expect_error(F action, int status, const char* outcome = nullptr) {
    try { action(); } catch (const Error& e) {
        check(e.status == status, "CUPS error mapping");
        if (outcome) check(e.outcome == outcome, "CUPS error outcome");
        return;
    }
    check(false, "CUPS failure expected");
}
void cups_tests() {
    std::vector<std::string> attempts;
    auto endpoint = connect_local_cups([&](const char* host, int budget) {
        check(budget > 0 && budget <= 3000, "bounded initial connection budget");
        attempts.emplace_back(host); return true;
    });
    check(endpoint == "/private/var/run/cupsd" &&
          attempts == std::vector<std::string>{"/private/var/run/cupsd"},
          "fixed system Unix socket is primary; no TCP attempt on success");
    attempts.clear();
    endpoint = connect_local_cups([&](const char* host, int) {
        attempts.emplace_back(host); errno = ECONNREFUSED; return attempts.size() == 2;
    });
    check(endpoint == "localhost" &&
          attempts == std::vector<std::string>{"/private/var/run/cupsd", "localhost"},
          "only explicit localhost fallback after socket failure");
    attempts.clear();
    try {
        connect_local_cups([&](const char* host, int) {
            attempts.emplace_back(host); errno = attempts.size() == 1 ? ENOENT : ECONNREFUSED;
            return false;
        });
        check(false, "both connection failures must fail");
    } catch (const Error& e) {
        check(e.status == 5 && e.code == "backend_unavailable" && e.backend == "cups" &&
              e.outcome == "not_submitted" && e.native == ECONNREFUSED,
              "failed local endpoint attempts preserve not-submitted status");
        check(contains(e.what(), "/private/var/run/cupsd") && contains(e.what(), "localhost:631") &&
              attempts.size() == 2, "connection diagnostic includes both attempted endpoints");
    }
    auto simulated_time = std::chrono::steady_clock::time_point{};
    auto clock = [&]() { return simulated_time; };
    std::vector<int> budgets;
    endpoint = connect_local_cups([&](const char*, int budget) {
        budgets.push_back(budget);
        simulated_time += std::chrono::milliseconds(1000);
        errno = ECONNREFUSED;
        return budgets.size() == 2;
    }, clock);
    check(endpoint == "localhost" && budgets == std::vector<int>{3000,2000},
              "socket and TCP share one three-second connection deadline");
    simulated_time = {}; budgets.clear();
    try {
        connect_local_cups([&](const char*, int budget) {
                budgets.push_back(budget); simulated_time += std::chrono::milliseconds(3000);
                errno = ECONNREFUSED; return false;
        }, clock);
        check(false, "exhausted deadline must fail");
    } catch (const Error& e) {
        check(budgets == std::vector<int>{3000} && e.native == ETIMEDOUT &&
                  e.outcome == "not_submitted" && contains(e.what(), "not attempted"),
                  "no TCP attempt after primary consumes connection budget");
    }
    for (const auto& error : std::vector<Error>{
        Error(5, "backend_unavailable", "transport lost after send", "cups", {}, "uncertain"),
        Error(4, "access_denied", "auth rejected", "cups", {}, "rejected"),
        Error(8, "native_error", "protocol rejected", "cups", {}, "rejected")}) {
        int connections = 0, submissions = 0;
        try {
                submit_local_cups([&](const char*, int) { ++connections; return true; }, [&]() -> Ipp {
                    ++submissions; throw error;
                });
                check(false, "submitted failure must propagate");
        } catch (const Error& e) {
                check(connections == 1 && submissions == 1 && e.status == error.status &&
                      e.outcome == error.outcome, "no transport fallback or resubmission after request failure");
        }
    }
    check(cups_queue_uri("世界 %?#") ==
          "ipp://localhost:631/printers/%E4%B8%96%E7%95%8C%20%25%3F%23", "CUPS URI percent encoding");
    for (const auto& id : {"../bad", "x/y", "x\\y", ".", ".."})
        expect_error([&]() { cups_queue_uri(id); }, 2);
    int requests = 0;
    CupsBackend backend([&](Ipp request, const std::string& resource, bool mutation) {
        ++requests;
        check(!mutation && resource == "/", "list endpoint only local root");
        check(ippGetOperation(request.get()) == IPP_OP_CUPS_GET_PRINTERS, "CUPS-Get-Printers only, not discovery");
        auto mask = ippFindAttribute(request.get(), "printer-type-mask", IPP_TAG_ENUM);
        check(mask && (ippGetInteger(mask, 0) & CUPS_PRINTER_CLASS) &&
              (ippGetInteger(mask, 0) & CUPS_PRINTER_DISCOVERED), "classes/discovery excluded at request");
        auto r = reply();
        printer(r.get(), "local", -1);
        ippAddSeparator(r.get()); printer(r.get(), "class", 3, CUPS_PRINTER_CLASS);
        ippAddSeparator(r.get()); printer(r.get(), "bonjour", 3, CUPS_PRINTER_DISCOVERED);
        ippAddSeparator(r.get()); printer(r.get(), "implicit", 3, CUPS_PRINTER_IMPLICIT);
        ippAddSeparator(r.get()); printer(r.get(), "temporary", 3, 0, true);
        return r;
    });
    auto queues = backend.list();
    check(requests == 1 && queues.size() == 1 && queues[0].id == "local", "configured queues only");
    check(queues[0].state == "unknown" && queues[0].accepting == true && queues[0].jobs == 4 &&
          queues[0].reasons->size() == 2, "CUPS absence and typed values");
    auto raw = queues[0].raw.text();
    check(contains(raw, "\"printer-state\":null") && contains(raw, "untrusted.example") &&
          contains(raw, "\"operations-supported\":["), "CUPS raw values retained not trusted");
    for (const auto& state : std::vector<std::pair<int, std::string>>{{3,"idle"}, {4,"processing"}, {5,"stopped"}, {99,"unknown"}}) {
        CupsBackend b([&](Ipp request, const std::string& resource, bool mutation) {
            check(!mutation && resource == "/" &&
                  ippGetOperation(request.get()) == IPP_OP_GET_PRINTER_ATTRIBUTES, "get operation");
            auto uri = ippFindAttribute(request.get(), "printer-uri", IPP_TAG_URI);
            check(uri && std::string(ippGetString(uri, 0, nullptr)) == cups_queue_uri("世界 %?#"),
                  "get uses derived local URI");
            auto r = reply(); printer(r.get(), "世界 %?#", state.first); return r;
        });
        check(b.get("世界 %?#").state == state.second, "CUPS state mapping");
    }
    for (bool pause : {false, true}) {
        CupsBackend b([&](Ipp request, const std::string& resource, bool mutation) {
            check(mutation && resource == "/admin/", "mutation admin resource");
            check(ippGetOperation(request.get()) == (pause ? IPP_OP_PAUSE_PRINTER : IPP_OP_RESUME_PRINTER),
                  "standard IPP pause/resume, not reject/accept jobs");
            auto uri = ippFindAttribute(request.get(), "printer-uri", IPP_TAG_URI);
            check(uri && std::string(ippGetString(uri, 0, nullptr)) == cups_queue_uri("local"), "mutation local URI");
            return reply();
        });
        b.mutate("local", pause);
    }
    for (const auto& error : std::vector<std::pair<ipp_status_t, int>>{
        {IPP_STATUS_ERROR_NOT_FOUND,3}, {IPP_STATUS_ERROR_NOT_AUTHENTICATED,4},
        {IPP_STATUS_ERROR_NOT_AUTHORIZED,4}, {IPP_STATUS_ERROR_FORBIDDEN,4},
        {IPP_STATUS_ERROR_SERVICE_UNAVAILABLE,5}, {IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED,6},
        {IPP_STATUS_ERROR_INTERNAL,8}}) {
        CupsBackend b([&](Ipp, const std::string&, bool) { return reply(error.first); });
        expect_error([&]() { b.mutate("local", true); }, error.second, "rejected");
    }
    CupsBackend empty([](Ipp, const std::string&, bool) { return reply(IPP_STATUS_ERROR_NOT_FOUND); });
    check(empty.list().empty(), "CUPS empty printer set not-found is list success");
    CupsBackend disconnected([](Ipp, const std::string&, bool) { return Ipp{}; });
    expect_error([&]() { disconnected.list(); }, 5, "not_submitted");
    expect_error([&]() { disconnected.mutate("local", true); }, 5, "uncertain");
    for (int malformed = 0; malformed < 6; ++malformed) {
        CupsBackend b([&](Ipp, const std::string&, bool) {
            auto r = reply();
            if (malformed == 0) { // Missing printer-type cannot establish configured status.
                ippAddString(r.get(), IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", nullptr, "local");
            } else {
                printer(r.get(), "local");
                if (malformed == 1) ippAddInteger(r.get(), IPP_TAG_PRINTER, IPP_TAG_ENUM, "printer-state", 3);
                else {
                    auto attr = ippFindAttribute(r.get(), malformed == 2 ? "queued-job-count" :
                        malformed == 3 ? "printer-state" : malformed == 4 ? "printer-name" : "printer-is-accepting-jobs",
                        IPP_TAG_ZERO);
                    ippDeleteAttribute(r.get(), attr);
                    if (malformed == 2) ippAddInteger(r.get(), IPP_TAG_PRINTER, IPP_TAG_INTEGER, "queued-job-count", -1);
                    if (malformed == 3) ippAddString(r.get(), IPP_TAG_PRINTER, IPP_TAG_TEXT, "printer-state", nullptr, "idle");
                    if (malformed == 4) ippAddString(r.get(), IPP_TAG_PRINTER, IPP_TAG_NAME, "printer-name", nullptr, "a\nb");
                    if (malformed == 5) ippAddInteger(r.get(), IPP_TAG_PRINTER, IPP_TAG_INTEGER, "printer-is-accepting-jobs", 1);
                }
            }
            return r;
        });
        expect_error([&]() { b.list(); }, 8);
    }
    CupsBackend changed([](Ipp, const std::string&, bool) {
        auto r = reply(); printer(r.get(), "other"); return r;
    });
    expect_error([&]() { changed.get("local"); }, 3);
    CupsBackend duplicate([](Ipp, const std::string&, bool) {
        auto r = reply(); printer(r.get(), "local");
        ippAddSeparator(r.get()); printer(r.get(), "local"); return r;
    });
    expect_error([&]() { duplicate.list(); }, 8);
}
#endif
// Winspool API tests compile and run on Windows without opening real printers.
#ifdef _WIN32
void windows_tests() {
    static_assert(PRINTER_STATUS_DRIVER_UPDATE_NEEDED == 0x4000000, "Windows status constants changed");
    int opens = 0, closes = 0, sets = 0;
    DWORD desired_access = 0, control = 0, set_error = 0;
    bool fail_get = false;
    std::wstring id = L"\\\\configured-server\\\u4e16\u754c queue";
    WinspoolApi api;
    auto fill = [&](LPBYTE buffer, DWORD bytes, LPDWORD needed) -> BOOL {
        *needed = sizeof(PRINTER_INFO_2W);
        if (bytes < *needed) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        PRINTER_INFO_2W p{};
        p.pPrinterName = id.data(); p.Status = PRINTER_STATUS_PAUSED | PRINTER_STATUS_PRINTING;
        p.cJobs = 2;
        *reinterpret_cast<PRINTER_INFO_2W*>(buffer) = p;
        return TRUE;
    };
    api.enumerate = [&](DWORD flags, LPWSTR name, DWORD level, LPBYTE data, DWORD bytes,
                        LPDWORD needed, LPDWORD count) {
        check(flags == (PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS) && !name && level == 2,
              "Windows enumerates only configured local and connected printers");
        *count = 1;
        return fill(data, bytes, needed);
    };
    api.open = [&](LPWSTR name, LPHANDLE handle, LPPRINTER_DEFAULTSW defaults) {
        check(name == id, "Windows opens exact enumerated Unicode ID");
        desired_access = defaults->DesiredAccess;
        ++opens; *handle = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1)); return TRUE;
    };
    api.close = [&](HANDLE) { ++closes; return TRUE; };
    api.get = [&](HANDLE, DWORD level, LPBYTE data, DWORD bytes, LPDWORD needed) {
        check(level == 2, "Windows GetPrinter level 2");
        if (fail_get) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
        return fill(data, bytes, needed);
    };
    api.set = [&](HANDLE, DWORD level, LPBYTE data, DWORD command) {
        check(level == 0 && !data, "Windows SetPrinter command only");
        ++sets; control = command;
        if (set_error) { SetLastError(set_error); return FALSE; }
        return TRUE;
    };
    auto backend = winspool_backend(api);
    auto queues = backend->list();
    check(queues.size() == 1 && queues[0].state == "stopped" && queues[0].jobs == 2 &&
          !queues[0].accepting && !queues[0].reasons && opens == 0, "Winspool list and conservative status");
    const auto queue_id = queues[0].id;
    backend->get(queue_id);
    check(opens == closes && desired_access == PRINTER_ACCESS_USE, "read-only handle RAII");
    backend->mutate(queue_id, true);
    check(opens == closes && control == PRINTER_CONTROL_PAUSE && desired_access == PRINTER_ACCESS_ADMINISTER,
          "explicit pause with admin handle RAII");
    backend->mutate(queue_id, false);
    check(opens == closes && control == PRINTER_CONTROL_RESUME, "explicit resume");
    try { backend->mutate("\\\\arbitrary\\remote", true); check(false, "unconfigured target accepted"); }
    catch (const Error& e) { check(e.status == 3 && opens == closes && sets == 2, "remote ID rejected before open"); }
    for (const auto& error : std::vector<std::pair<DWORD, int>>{
        {ERROR_ACCESS_DENIED,4}, {ERROR_INVALID_PRINTER_NAME,3},
        {ERROR_SERVICE_NOT_ACTIVE,5}, {ERROR_NOT_SUPPORTED,6}, {ERROR_INVALID_PARAMETER,8},
        {RPC_S_SERVER_UNAVAILABLE,5}}) {
        set_error = error.first;
        try { backend->mutate(queue_id, true); check(false, "set failure expected"); }
        catch (const Error& e) {
            check(e.status == error.second && e.native == error.first && opens == closes, "Winspool error and RAII");
            check(e.outcome == (error.first == RPC_S_SERVER_UNAVAILABLE ? "uncertain" : "rejected"),
                  "Winspool transport uncertainty distinct from rejection");
        }
    }
    fail_get = true;
    try { backend->get(queue_id); check(false, "get failure expected"); }
    catch (const Error& e) { check(e.status == 4 && opens == closes, "Winspool query failure closes handle"); }
    api.enumerate = [](DWORD, LPWSTR, DWORD, LPBYTE, DWORD, LPDWORD needed, LPDWORD) {
        *needed = 32 * 1024 * 1024; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE;
    };
    backend = winspool_backend(api);
    try { backend->list(); check(false, "huge allocation expected to fail"); }
    catch (const Error& e) { check(e.status == 8, "Winspool buffer bounded"); }
}
#endif
}
int main() {
    try {
        encoding_tests();
        protocol_tests();
#ifdef __APPLE__
        cups_tests();
#elif defined(_WIN32)
        windows_tests();
#endif
        std::cout << checks << " checks passed (mock backends only; no live queue mutations)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test failed after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
