#include "monitor.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace monitor {
bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        unsigned c = static_cast<unsigned char>(s[i++]), n = 0, cp = 0, min = 0;
        if (c < 0x80) continue;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; cp = c & 31; min = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; cp = c & 15; min = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; cp = c & 7; min = 0x10000; }
        else return false;
        if (n > s.size() - i) return false;
        while (n--) {
            unsigned next = static_cast<unsigned char>(s[i++]);
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}
bool valid_id(std::string_view id) {
    if (id.empty() || id.size() > max_id || !valid_utf8(id)) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const auto c = static_cast<unsigned char>(id[i]);
        if (c < 32 || c == 127) return false;
        if (c == 0xc2 && i + 1 < id.size()) {
            const auto next = static_cast<unsigned char>(id[i + 1]);
            if (next >= 0x80 && next <= 0x9f) return false;
        }
    }
    return true;
}
Json::Json(const std::string& value) {
    if (!valid_utf8(value)) throw Error(8, "native_error", "backend returned invalid UTF-8");
    static constexpr char hex[] = "0123456789abcdef";
    text_ = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': text_ += "\\\""; break;
        case '\\': text_ += "\\\\"; break;
        case '\b': text_ += "\\b"; break;
        case '\f': text_ += "\\f"; break;
        case '\n': text_ += "\\n"; break;
        case '\r': text_ += "\\r"; break;
        case '\t': text_ += "\\t"; break;
        default:
            if (c < 32) { text_ += "\\u00"; text_ += hex[c >> 4]; text_ += hex[c & 15]; }
            else text_ += static_cast<char>(c);
        }
    }
    text_ += '"';
}
Json Json::object(std::initializer_list<std::pair<std::string, Json>> fields) {
    return object(std::vector<std::pair<std::string, Json>>(fields));
}
Json Json::object(const std::vector<std::pair<std::string, Json>>& fields) {
    std::string s = "{";
    for (const auto& f : fields) {
        if (s.size() > 1) s += ',';
        s += Json(f.first).text() + ":" + f.second.text();
        if (s.size() + 2 > max_output) throw Error(8, "native_error", "output limit exceeded");
    }
    return Json(s + "}", 0);
}
Json Json::array(const std::vector<Json>& values) {
    std::string s = "[";
    for (const auto& v : values) {
        if (s.size() > 1) s += ',';
        s += v.text();
        if (s.size() + 2 > max_output) throw Error(8, "native_error", "output limit exceeded");
    }
    return Json(s + "]", 0);
}
Error::Error(int s, std::string c, std::string m, std::string b,
             std::optional<std::int64_t> n, std::string o)
    : std::runtime_error(std::move(m)), status(s), code(std::move(c)),
      backend(std::move(b)), outcome(std::move(o)), native(n) {}
Json Error::json() const {
    return Json::object({{"code", code}, {"message", what()},
        {"backend", backend.empty() ? Json() : Json(backend)},
        {"nativeCode", native ? Json(*native) : Json()}, {"outcome", outcome}});
}
static std::string now() {
    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}
Json Queue::json() const {
    if (!valid_id(id)) throw Error(8, "native_error", "backend returned invalid queue ID", backend);
    std::vector<Json> r;
    if (reasons) for (const auto& reason : *reasons) r.emplace_back(reason);
    return Json::object({{"id", id}, {"displayName", display_name}, {"backend", backend},
        {"state", state}, {"acceptingJobs", accepting ? Json(*accepting) : Json()},
        {"jobCount", jobs ? Json(*jobs) : Json()},
        {"reasons", reasons ? Json::array(r) : Json()},
        {"message", message ? Json(*message) : Json()}, {"observedAt", now()}, {"raw", raw}});
}
static Json mutation(bool accepted, const char* observation, const Json& error) {
    return Json::object({{"accepted", accepted}, {"outcome", accepted ? "accepted" : "rejected"},
                         {"observation", observation}, {"error", error}});
}
Response execute(const std::vector<std::string>& args, const Factory& factory) {
    std::string op = "arguments", backend;
    bool modifying = false, accepted = false, submitted = false;
    try {
        if (args.empty() || args.size() > 2) throw Error(2, "invalid_arguments", usage);
        for (const auto& a : args)
            if (a.size() > max_argument || a.find('\0') != std::string::npos || !valid_utf8(a))
                throw Error(2, "invalid_arguments", usage);
        const auto& cmd = args[0];
        const bool single = cmd == "list" || cmd == "capabilities" || cmd == "--version";
        const bool queue_op = cmd == "get" || cmd == "pause" || cmd == "resume";
        if ((single && args.size() != 1) ||
            (queue_op && (args.size() != 2 || !valid_id(args[1]))) ||
            (cmd == "inspect-argument" && args.size() != 2) ||
            (!single && !queue_op && cmd != "inspect-argument"))
            throw Error(2, "invalid_arguments", usage);
        op = cmd == "--version" ? "version" : cmd;
        std::vector<std::pair<std::string, Json>> fields{
            {"protocol", Json(std::int64_t(1))}, {"ok", true}, {"operation", op}};
        if (op == "version") fields.emplace_back("version", "0.1.0");
        else if (op == "inspect-argument") fields.emplace_back("argument", args[1]);
        else if (op == "capabilities") {
            fields.emplace_back("backend", native_backend_name());
            fields.emplace_back("operations", Json::array({"list", "get", "pause", "resume"}));
            fields.emplace_back("mutationPolicy", "explicit-only");
        } else {
            modifying = op == "pause" || op == "resume";
            backend = native_backend_name();
            auto b = factory();
            backend = b->name();
            auto queues = b->list();
            if (queues.size() > max_queues) throw Error(8, "native_error", "printer count limit exceeded", backend);
            if (op == "list") {
                std::vector<Json> values;
                for (const auto& q : queues) values.push_back(q.json());
                fields.emplace_back("queues", Json::array(values));
            } else {
                if (std::none_of(queues.begin(), queues.end(), [&](const Queue& q) { return q.id == args[1]; }))
                    throw Error(3, "unknown_queue", "queue is not locally configured", backend);
                if (modifying) {
                    submitted = true;
                    b->mutate(args[1], op == "pause");
                    accepted = true;
                }
                auto queue = b->get(args[1]).json();
                if (modifying) fields.emplace_back("mutation", mutation(true, "succeeded", Json()));
                fields.emplace_back("queue", queue);
            }
        }
        return {0, Json::object(fields).text() + "\n", ""};
    } catch (const Error& e) {
        Error error = e;
        if (error.backend.empty() && !backend.empty()) error.backend = backend;
        if (accepted) {
            error = Error(7, "observation_failed", e.what(), backend, e.native, "accepted");
        }
        std::vector<std::pair<std::string, Json>> fields{
            {"protocol", Json(std::int64_t(1))}, {"ok", false}, {"operation", op}, {"error", error.json()}};
        if (modifying) {
            auto m = mutation(accepted, "failed", error.json());
            // A transport failure does not establish rejection.
            if (!accepted && error.outcome != "rejected")
                m = Json::object({{"accepted", false}, {"outcome", error.outcome},
                                  {"observation", "failed"}, {"error", error.json()}});
            fields.emplace_back("mutation", m);
            if (accepted) fields.emplace_back("queue", Json());
        }
        return {error.status, "", Json::object(fields).text() + "\n"};
    } catch (const std::exception&) {
        Error e(accepted ? 7 : 8, accepted ? "observation_failed" : "native_error",
                "native operation could not be completed", backend, {},
                accepted ? "accepted" : submitted ? "uncertain" : "not_submitted");
        std::vector<std::pair<std::string, Json>> fields{
            {"protocol", Json(std::int64_t(1))}, {"ok", false}, {"operation", op}, {"error", e.json()}};
        if (modifying) {
            fields.emplace_back("mutation", Json::object({{"accepted", accepted}, {"outcome", e.outcome},
                {"observation", "failed"}, {"error", e.json()}}));
            if (accepted) fields.emplace_back("queue", Json());
        }
        return {e.status, "", Json::object(fields).text() + "\n"};
    }
}
std::string windows_state(std::uint32_t status) {
    // Error, paused, paper/device and offline states dominate printing flags.
    constexpr std::uint32_t stopped = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x40 |
        0x80 | 0x800 | 0x1000 | 0x40000 | 0x100000 | 0x200000 | 0x400000 | 0x2000000;
    if (status & stopped) return "stopped";
    if (status & (0x400 | 0x4000)) return "processing";
    return status == 0 ? "idle" : "unknown";
}
Json windows_flags(std::uint32_t status) {
    const std::pair<const char*, std::uint32_t> flags[] = {
        {"PAUSED", 0x1}, {"ERROR", 0x2}, {"PENDING_DELETION", 0x4},
        {"PAPER_JAM", 0x8}, {"PAPER_OUT", 0x10}, {"MANUAL_FEED", 0x20},
        {"PAPER_PROBLEM", 0x40}, {"OFFLINE", 0x80}, {"IO_ACTIVE", 0x100},
        {"BUSY", 0x200}, {"PRINTING", 0x400}, {"OUTPUT_BIN_FULL", 0x800},
        {"NOT_AVAILABLE", 0x1000}, {"WAITING", 0x2000}, {"PROCESSING", 0x4000},
        {"INITIALIZING", 0x8000}, {"WARMING_UP", 0x10000}, {"TONER_LOW", 0x20000},
        {"NO_TONER", 0x40000}, {"PAGE_PUNT", 0x80000}, {"USER_INTERVENTION", 0x100000},
        {"OUT_OF_MEMORY", 0x200000}, {"DOOR_OPEN", 0x400000},
        {"SERVER_UNKNOWN", 0x800000}, {"POWER_SAVE", 0x1000000},
        {"SERVER_OFFLINE", 0x2000000}, {"DRIVER_UPDATE_NEEDED", 0x4000000}};
    std::vector<std::pair<std::string, Json>> values;
    for (const auto& f : flags) values.emplace_back(f.first, bool(status & f.second));
    return Json::object(values);
}
}
