#include "monitor.hpp"
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {
class Fixture final : public monitor::Backend {
    std::string scenario_;
    bool paused_ = false, mutated_ = false;
public:
    explicit Fixture(std::string scenario) : scenario_(std::move(scenario)) {}
    const char* name() const override { return monitor::native_backend_name(); }
    monitor::Queue value() {
        monitor::Queue q;
        q.id = "世界 queue"; q.display_name = "Mock \"printer\""; q.backend = name();
        q.state = paused_ ? "stopped" : "idle"; q.accepting = true; q.jobs = 2;
        q.reasons = std::vector<std::string>{paused_ ? "paused" : "none"};
        q.message = "fixture only";
        q.raw = monitor::Json::object({{"fixture", true}});
        return q;
    }
    std::vector<monitor::Queue> list() override {
        if (scenario_ == "unavailable")
            throw monitor::Error(5, "backend_unavailable", "mock unavailable", name(), 503);
        if (scenario_ == "empty") return {};
        return {value()};
    }
    monitor::Queue get(const std::string&) override {
        if ((scenario_ == "observation-failed" && mutated_) || scenario_ == "get-failed")
            throw monitor::Error(3, "unknown_queue", "mock queue disappeared", name(), 404);
        if (scenario_ == "unexpected" && mutated_) throw std::runtime_error("injected");
        return value();
    }
    void mutate(const std::string&, bool pause) override {
        if (scenario_ == "denied") throw monitor::Error(4, "access_denied", "mock denied", name(), 403, "rejected");
        if (scenario_ == "unsupported") throw monitor::Error(6, "unsupported_operation", "mock unsupported", name(), 501, "rejected");
        if (scenario_ == "native-error") throw monitor::Error(8, "native_error", "mock native error", name(), 500, "rejected");
        if (scenario_ == "transport") throw monitor::Error(5, "backend_unavailable", "mock transport lost", name(), 503, "uncertain");
        mutated_ = true; paused_ = pause;
    }
};
}
// Test-only executable; the installed CLI has no mock switches or environment override.
static int run(const std::vector<std::string>& arguments) {
    if (arguments.empty()) return 1;
    const auto& scenario = arguments[0];
    std::vector<std::string> args(arguments.begin() + 1, arguments.end());
    auto response = monitor::execute(args, [scenario]() { return std::make_unique<Fixture>(scenario); });
    std::cout << response.out;
    std::cerr << response.err;
    return response.status;
}
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!n) return 1;
        std::string s(n, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, s.data(), n, nullptr, nullptr))
            return 1;
        s.pop_back();
        args.push_back(std::move(s));
    }
    return run(args);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    return run(args);
}
#endif
