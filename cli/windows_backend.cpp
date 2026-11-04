#include "windows_backend.hpp"
#include <algorithm>
#include <limits>

namespace monitor {
namespace {
constexpr DWORD max_native_buffer = 16 * 1024 * 1024;
Error win_error(DWORD code, bool mutation = false) {
    int status = 8;
    std::string stable = "native_error";
    if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD ||
        code == ERROR_LOGON_FAILURE) { status = 4; stable = "access_denied"; }
    else if (code == ERROR_INVALID_PRINTER_NAME || code == ERROR_PRINTER_NOT_FOUND) {
        status = 3; stable = "unknown_queue";
    } else if (code == RPC_S_SERVER_UNAVAILABLE || code == ERROR_SERVICE_NOT_ACTIVE ||
               code == ERROR_BAD_NETPATH || code == ERROR_PRINT_CANCELLED) {
        status = 5; stable = "backend_unavailable";
    } else if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION) {
        status = 6; stable = "unsupported_operation";
    }
    // RPC/transport errors can occur after a server performed the command.
    const bool transport = code == RPC_S_SERVER_UNAVAILABLE || code == RPC_S_CALL_FAILED ||
        code == RPC_S_CALL_FAILED_DNE || code == ERROR_TIMEOUT || code == ERROR_SEM_TIMEOUT ||
        code == ERROR_OPERATION_ABORTED;
    return Error(status, stable, "Winspool operation failed (Windows error " + std::to_string(code) + ")",
                 "winspool", code, mutation ? (transport ? "uncertain" : "rejected") : "not_submitted");
}
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) throw win_error(GetLastError());
    std::wstring out(n, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n) != n)
        throw win_error(GetLastError());
    return out;
}
std::string utf8(const wchar_t* s) {
    if (!s) return "";
    const auto n = wcslen(s);
    if (n > max_argument) throw Error(8, "native_error", "Winspool string limit exceeded", "winspool");
    if (!n) return "";
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, static_cast<int>(n), nullptr, 0, nullptr, nullptr);
    if (!bytes) throw win_error(GetLastError());
    std::string out(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s, static_cast<int>(n), out.data(), bytes, nullptr, nullptr) != bytes)
        throw win_error(GetLastError());
    return out;
}
class Printer {
    HANDLE handle_ = nullptr;
    const WinspoolApi& api_;
public:
    Printer(const WinspoolApi& api, const std::string& id, bool admin) : api_(api) {
        auto name = wide(id);
        PRINTER_DEFAULTSW defaults{};
        defaults.DesiredAccess = admin ? PRINTER_ACCESS_ADMINISTER : PRINTER_ACCESS_USE;
        if (!api_.open(name.data(), &handle_, &defaults)) throw win_error(GetLastError());
    }
    ~Printer() { if (handle_) api_.close(handle_); }
    Printer(const Printer&) = delete;
    Printer& operator=(const Printer&) = delete;
    HANDLE get() const { return handle_; }
};
Queue queue(const PRINTER_INFO_2W& p) {
    Queue q;
    q.id = utf8(p.pPrinterName);
    if (!valid_id(q.id)) throw Error(8, "native_error", "Winspool returned invalid queue ID", "winspool");
    q.backend = "winspool"; q.display_name = q.id;
    q.state = windows_state(p.Status);
    // Winspool Status does not expose a reliable accepting-jobs property.
    q.jobs = p.cJobs;
    q.raw = Json::object({{"status", Json(static_cast<std::int64_t>(p.Status))},
        {"flags", windows_flags(p.Status)}, {"attributes", Json(static_cast<std::int64_t>(p.Attributes))},
        {"serverName", p.pServerName ? Json(utf8(p.pServerName)) : Json()},
        {"shareName", p.pShareName ? Json(utf8(p.pShareName)) : Json()},
        {"portName", p.pPortName ? Json(utf8(p.pPortName)) : Json()},
        {"driverName", p.pDriverName ? Json(utf8(p.pDriverName)) : Json()},
        {"comment", p.pComment ? Json(utf8(p.pComment)) : Json()},
        {"location", p.pLocation ? Json(utf8(p.pLocation)) : Json()}});
    return q;
}
class WinspoolBackend final : public Backend {
    WinspoolApi api_;
    std::vector<std::string> configured_;
    void matched(const std::string& id) {
        if (std::find(configured_.begin(), configured_.end(), id) == configured_.end())
            throw Error(3, "unknown_queue", "queue is not locally configured", "winspool");
    }
public:
    explicit WinspoolBackend(WinspoolApi api) : api_(std::move(api)) {}
    const char* name() const override { return "winspool"; }
    std::vector<Queue> list() override {
        configured_.clear();
        constexpr DWORD flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;
        DWORD needed = 0, count = 0;
        if (!api_.enumerate(flags, nullptr, 2, nullptr, 0, &needed, &count)) {
            const auto code = GetLastError();
            if (code != ERROR_INSUFFICIENT_BUFFER) throw win_error(code);
        }
        if (!needed) return {};
        std::vector<BYTE> buffer;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (needed > max_native_buffer) throw Error(8, "native_error", "Winspool buffer limit exceeded", "winspool");
            buffer.resize(needed);
            if (api_.enumerate(flags, nullptr, 2, buffer.data(), static_cast<DWORD>(buffer.size()), &needed, &count)) {
                if (count > max_queues || count > buffer.size() / sizeof(PRINTER_INFO_2W))
                    throw Error(8, "native_error", "printer count limit exceeded", "winspool");
                std::vector<Queue> out;
                const auto* printers = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
                for (DWORD i = 0; i < count; ++i) {
                    auto q = queue(printers[i]);
                    if (std::find(configured_.begin(), configured_.end(), q.id) != configured_.end())
                        throw Error(8, "native_error", "duplicate Winspool queue ID", "winspool");
                    configured_.push_back(q.id);
                    out.push_back(std::move(q));
                }
                return out;
            }
            const auto code = GetLastError();
            if (code != ERROR_INSUFFICIENT_BUFFER) throw win_error(code);
        }
        throw Error(8, "native_error", "printer configuration changed repeatedly during enumeration", "winspool");
    }
    Queue get(const std::string& id) override {
        matched(id);
        Printer printer(api_, id, false);
        DWORD needed = 0;
        if (!api_.get(printer.get(), 2, nullptr, 0, &needed)) {
            const auto code = GetLastError();
            if (code != ERROR_INSUFFICIENT_BUFFER) throw win_error(code);
        }
        std::vector<BYTE> buffer;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (needed < sizeof(PRINTER_INFO_2W) || needed > max_native_buffer)
                throw Error(8, "native_error", "invalid Winspool buffer size", "winspool");
            buffer.resize(needed);
            if (api_.get(printer.get(), 2, buffer.data(), static_cast<DWORD>(buffer.size()), &needed)) {
                auto q = queue(*reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data()));
                if (q.id != id) throw Error(3, "unknown_queue", "queue identity changed during observation", "winspool");
                return q;
            }
            const auto code = GetLastError();
            if (code != ERROR_INSUFFICIENT_BUFFER) throw win_error(code);
        }
        throw Error(8, "native_error", "printer configuration changed repeatedly during observation", "winspool");
    }
    void mutate(const std::string& id, bool pause) override {
        matched(id);
        Printer printer(api_, id, true);
        if (!api_.set(printer.get(), 0, nullptr, pause ? PRINTER_CONTROL_PAUSE : PRINTER_CONTROL_RESUME))
            throw win_error(GetLastError(), true);
    }
};
}
const char* native_backend_name() { return "winspool"; }
std::unique_ptr<Backend> winspool_backend(WinspoolApi api) {
    return std::make_unique<WinspoolBackend>(std::move(api));
}
std::unique_ptr<Backend> native_backend() {
    return winspool_backend({EnumPrintersW, OpenPrinterW, GetPrinterW, SetPrinterW, ClosePrinter});
}
}
