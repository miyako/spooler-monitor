#include "cups_backend.hpp"
#include <cups/http.h>
#include <algorithm>
#include <cerrno>
#include <sys/socket.h>

namespace monitor {
static Error cups_error(ipp_status_t code, bool mutation) {
    int status = 8;
    std::string stable = "native_error";
    if (code == IPP_STATUS_ERROR_NOT_FOUND || code == IPP_STATUS_ERROR_GONE) {
        status = 3; stable = "unknown_queue";
    } else if (code == IPP_STATUS_ERROR_FORBIDDEN || code == IPP_STATUS_ERROR_NOT_AUTHENTICATED ||
               code == IPP_STATUS_ERROR_NOT_AUTHORIZED) {
        status = 4; stable = "access_denied";
    } else if (code == IPP_STATUS_ERROR_SERVICE_UNAVAILABLE) {
        status = 5; stable = "backend_unavailable";
    } else if (code == IPP_STATUS_ERROR_OPERATION_NOT_SUPPORTED) {
        status = 6; stable = "unsupported_operation";
    }
    return Error(status, stable, ippErrorString(code), "cups", static_cast<std::int64_t>(code),
                 mutation ? "rejected" : "not_submitted");
}
std::string cups_queue_uri(const std::string& id) {
    if (!valid_id(id) || id == "." || id == ".." || id.find_first_of("/\\") != std::string::npos)
        throw Error(2, "invalid_arguments", usage);
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string uri = "ipp://localhost:631/printers/";
    for (unsigned char c : id) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            uri += static_cast<char>(c);
        else { uri += '%'; uri += hex[c >> 4]; uri += hex[c & 15]; }
    }
    return uri;
}
CupsBackend::CupsBackend(CupsTransport transport) : transport_(std::move(transport)) {}
Ipp CupsBackend::request(ipp_op_t operation, const std::string& id, bool mutation) {
    Ipp req(ippNewRequest(operation));
    if (!req) throw Error(8, "native_error", "cannot allocate IPP request", "cups");
    if (!id.empty()) {
        const auto uri = cups_queue_uri(id);
        ippAddString(req.get(), IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", nullptr, uri.c_str());
    }
    ippAddString(req.get(), IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", nullptr, cupsUser());
    if (!mutation) {
        const char* attributes[] = {"printer-name", "printer-info", "printer-state",
            "printer-state-reasons", "printer-state-message", "printer-is-accepting-jobs",
            "queued-job-count", "printer-uri-supported", "operations-supported",
            "printer-type", "printer-is-temporary"};
        ippAddStrings(req.get(), IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
                      sizeof(attributes) / sizeof(*attributes), nullptr, attributes);
    }
    if (operation == IPP_OP_CUPS_GET_PRINTERS) {
        const int excluded = CUPS_PRINTER_CLASS | CUPS_PRINTER_DISCOVERED | CUPS_PRINTER_IMPLICIT;
        ippAddInteger(req.get(), IPP_TAG_OPERATION, IPP_TAG_ENUM, "printer-type", 0);
        ippAddInteger(req.get(), IPP_TAG_OPERATION, IPP_TAG_ENUM, "printer-type-mask", excluded);
        ippAddInteger(req.get(), IPP_TAG_OPERATION, IPP_TAG_INTEGER, "limit", static_cast<int>(max_queues + 1));
    }
    auto response = transport_(std::move(req), mutation ? "/admin/" : "/", mutation);
    if (!response) throw Error(5, "backend_unavailable", "local CUPS scheduler did not respond",
                               "cups", {}, mutation ? "uncertain" : "not_submitted");
    const auto status = ippGetStatusCode(response.get());
    // CUPS uses not-found for an empty configured printer set.
    if (operation == IPP_OP_CUPS_GET_PRINTERS && status == IPP_STATUS_ERROR_NOT_FOUND)
        return response;
    if (status >= IPP_STATUS_REDIRECTION_OTHER_SITE) throw cups_error(status, mutation);
    return response;
}
namespace {
struct Attributes {
    ipp_attribute_t *name = nullptr, *info = nullptr, *state = nullptr, *reasons = nullptr,
        *message = nullptr, *accepting = nullptr, *jobs = nullptr, *uri = nullptr,
        *operations = nullptr, *type = nullptr, *temporary = nullptr;
    bool any = false;
};
[[noreturn]] void malformed() { throw Error(8, "native_error", "malformed CUPS printer attributes", "cups"); }
std::optional<std::string> text(ipp_attribute_t* attr, ipp_tag_t tag) {
    if (!attr) return {};
    if (ippGetValueTag(attr) != tag || ippGetCount(attr) != 1) malformed();
    const char* value = ippGetString(attr, 0, nullptr);
    if (!value) malformed();
    std::string s(value);
    if (s.size() > max_argument || !valid_utf8(s)) malformed();
    return s;
}
std::optional<std::int64_t> integer(ipp_attribute_t* attr, ipp_tag_t tag) {
    if (!attr) return {};
    if (ippGetValueTag(attr) != tag || ippGetCount(attr) != 1) malformed();
    return ippGetInteger(attr, 0);
}
std::optional<bool> boolean(ipp_attribute_t* attr) {
    if (!attr) return {};
    if (ippGetValueTag(attr) != IPP_TAG_BOOLEAN || ippGetCount(attr) != 1) malformed();
    return ippGetBoolean(attr, 0) != 0;
}
std::optional<std::vector<std::string>> texts(ipp_attribute_t* attr, ipp_tag_t tag) {
    if (!attr) return {};
    if (ippGetValueTag(attr) != tag || ippGetCount(attr) < 1 || ippGetCount(attr) > 1024) malformed();
    std::vector<std::string> values;
    for (int i = 0; i < ippGetCount(attr); ++i) {
        const char* s = ippGetString(attr, i, nullptr);
        if (!s || std::char_traits<char>::length(s) > max_argument || !valid_utf8(s)) malformed();
        values.emplace_back(s);
    }
    return values;
}
Json strings_json(const std::optional<std::vector<std::string>>& values) {
    if (!values) return {};
    std::vector<Json> out;
    for (const auto& v : *values) out.emplace_back(v);
    return Json::array(out);
}
std::optional<Queue> decode(const Attributes& a) {
    auto id = text(a.name, IPP_TAG_NAME);
    if (!id || !valid_id(*id)) malformed();
    // Validate the name before deriving any URI. Returned printer-uri is data only.
    try { cups_queue_uri(*id); } catch (const Error&) { malformed(); }
    auto type = integer(a.type, IPP_TAG_ENUM);
    auto temporary = boolean(a.temporary);
    if (!type) malformed();
    if ((*type & (CUPS_PRINTER_CLASS | CUPS_PRINTER_DISCOVERED | CUPS_PRINTER_IMPLICIT)) ||
        temporary.value_or(false)) return {};
    Queue q;
    q.id = *id; q.backend = "cups";
    q.display_name = text(a.info, IPP_TAG_TEXT).value_or(*id);
    auto state = integer(a.state, IPP_TAG_ENUM);
    if (state == IPP_PSTATE_IDLE) q.state = "idle";
    else if (state == IPP_PSTATE_PROCESSING) q.state = "processing";
    else if (state == IPP_PSTATE_STOPPED) q.state = "stopped";
    q.accepting = boolean(a.accepting);
    q.jobs = integer(a.jobs, IPP_TAG_INTEGER);
    if (q.jobs && *q.jobs < 0) malformed();
    q.reasons = texts(a.reasons, IPP_TAG_KEYWORD);
    q.message = text(a.message, IPP_TAG_TEXT);
    auto uris = texts(a.uri, IPP_TAG_URI);
    Json ops;
    if (a.operations) {
        if (ippGetValueTag(a.operations) != IPP_TAG_ENUM || ippGetCount(a.operations) > 1024) malformed();
        std::vector<Json> out;
        for (int i = 0; i < ippGetCount(a.operations); ++i)
            out.emplace_back(static_cast<std::int64_t>(ippGetInteger(a.operations, i)));
        ops = Json::array(out);
    }
    q.raw = Json::object({{"printer-state", state ? Json(*state) : Json()},
        {"printer-state-reasons", strings_json(q.reasons)},
        {"printer-state-message", q.message ? Json(*q.message) : Json()},
        {"printer-is-accepting-jobs", q.accepting ? Json(*q.accepting) : Json()},
        {"queued-job-count", q.jobs ? Json(*q.jobs) : Json()},
        {"printer-uri-supported", strings_json(uris)}, {"operations-supported", ops},
        {"printer-type", Json(*type)},
        {"printer-is-temporary", temporary ? Json(*temporary) : Json()}});
    return q;
}
std::vector<Queue> decode_response(ipp_t* response) {
    Attributes a;
    std::vector<Queue> queues;
    auto finish = [&]() {
        if (a.any) {
            auto q = decode(a);
            if (q) {
                if (queues.size() >= max_queues) throw Error(8, "native_error", "printer count limit exceeded", "cups");
                if (std::any_of(queues.begin(), queues.end(), [&](const Queue& old) { return old.id == q->id; }))
                    malformed();
                queues.push_back(std::move(*q));
            }
        }
        a = Attributes{};
    };
    std::size_t attribute_count = 0;
    for (auto attr = ippFirstAttribute(response); attr; attr = ippNextAttribute(response)) {
        if (++attribute_count > max_queues * 32) throw Error(8, "native_error", "attribute count limit exceeded", "cups");
        const char* name = ippGetName(attr);
        if (!name || ippGetGroupTag(attr) != IPP_TAG_PRINTER) { finish(); continue; }
        a.any = true;
        ipp_attribute_t** destination = nullptr;
        const std::pair<const char*, ipp_attribute_t**> fields[] = {
            {"printer-name", &a.name}, {"printer-info", &a.info}, {"printer-state", &a.state},
            {"printer-state-reasons", &a.reasons}, {"printer-state-message", &a.message},
            {"printer-is-accepting-jobs", &a.accepting}, {"queued-job-count", &a.jobs},
            {"printer-uri-supported", &a.uri}, {"operations-supported", &a.operations},
            {"printer-type", &a.type}, {"printer-is-temporary", &a.temporary}};
        for (const auto& field : fields) if (name == std::string(field.first)) destination = field.second;
        if (destination) {
            if (*destination) malformed();
            *destination = attr;
        }
    }
    finish();
    return queues;
}
const char* no_password(const char*, http_t*, const char*, const char*, void*) { return nullptr; }
struct HttpDelete { void operator()(http_t* p) const { if (p) httpClose(p); } };
}
std::vector<Queue> CupsBackend::list() {
    auto response = request(IPP_OP_CUPS_GET_PRINTERS, "", false);
    return decode_response(response.get());
}
Queue CupsBackend::get(const std::string& id) {
    auto response = request(IPP_OP_GET_PRINTER_ATTRIBUTES, id, false);
    auto queues = decode_response(response.get());
    if (queues.size() != 1 || queues[0].id != id)
        throw Error(3, "unknown_queue", "queue disappeared or is no longer locally configured", "cups");
    return queues[0];
}
void CupsBackend::mutate(const std::string& id, bool pause) {
    request(pause ? IPP_OP_PAUSE_PRINTER : IPP_OP_RESUME_PRINTER, id, true);
}
const char* native_backend_name() { return "cups"; }
std::string connect_local_cups(const CupsConnect& attempt, const CupsClock& clock) {
    constexpr const char* socket = "/private/var/run/cupsd";
    const auto deadline = clock() + std::chrono::milliseconds(3000);
    auto remaining = [&]() {
        return static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(3000,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock()).count())));
    };
    int budget = remaining();
    if (budget <= 0) throw Error(5, "backend_unavailable",
        "local CUPS connection deadline exceeded before /private/var/run/cupsd or localhost:631 attempt",
        "cups", ETIMEDOUT, "not_submitted");
    if (attempt(socket, budget)) return socket;
    const int socket_error = errno;
    budget = remaining();
    if (budget > 0 && attempt("localhost", budget)) return "localhost";
    const int tcp_error = budget > 0 ? errno : ETIMEDOUT;
    throw Error(5, "backend_unavailable",
        "cannot connect to local CUPS scheduler; attempted /private/var/run/cupsd (errno " +
        std::to_string(socket_error) + "), then localhost:631 (" +
        (budget > 0 ? "errno " + std::to_string(tcp_error) : "not attempted: connection deadline exceeded") + ")",
        "cups", tcp_error, "not_submitted");
}
Ipp submit_local_cups(const CupsConnect& connect, const std::function<Ipp()>& submit, const CupsClock& clock) {
    connect_local_cups(connect, clock);
    // Transport selection ends before submission. Never catch/retry a submitted request.
    return submit();
}
std::unique_ptr<Backend> native_backend() {
    cupsSetPasswordCB2(no_password, nullptr);
    return std::make_unique<CupsBackend>([](Ipp request, const std::string& resource, bool mutation) {
        // Never consult cupsServer(), ippPort(), CUPS_SERVER or printer-supplied URIs.
        std::unique_ptr<http_t, HttpDelete> http;
        return submit_local_cups([&](const char* endpoint, int remaining_ms) {
            http.reset(httpConnect2(endpoint, 631, nullptr, AF_UNSPEC,
                                   HTTP_ENCRYPTION_IF_REQUESTED, 1, remaining_ms, nullptr));
            return bool(http);
        }, [&]() {
            httpSetTimeout(http.get(), 5.0, [](http_t*, void*) { return 0; }, nullptr);
            Ipp response(cupsDoRequest(http.get(), request.release(), resource.c_str()));
            if (!response) {
                // No IPP rejection response means submission/acceptance cannot be inferred.
                const auto code = cupsLastError();
                if (code == IPP_STATUS_ERROR_NOT_AUTHENTICATED || code == IPP_STATUS_ERROR_NOT_AUTHORIZED ||
                    code == IPP_STATUS_ERROR_FORBIDDEN) throw cups_error(code, mutation);
                throw Error(5, "backend_unavailable", "local CUPS request failed without an IPP response",
                            "cups", static_cast<std::int64_t>(code), mutation ? "uncertain" : "not_submitted");
            }
            return response;
        });
    });
}
}
