#include "monitor.hpp"
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

static int run(const std::vector<std::string>& args) {
    const auto result = monitor::execute(args, monitor::native_backend);
    std::cout << result.out;
    std::cerr << result.err;
    return result.status;
}
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> args;
    if (argc > 3) return run({});
    for (int i = 1; i < argc; ++i) {
        if (wcslen(argv[i]) > monitor::max_argument) return run({});
        const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!n) return run({});
        std::string s(n, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[i], -1, s.data(), n, nullptr, nullptr))
            return run({});
        s.pop_back();
        args.push_back(std::move(s));
    }
    return run(args);
}
#else
int main(int argc, char* argv[]) {
    if (argc > 3) return run({});
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::char_traits<char>::length(argv[i]) > monitor::max_argument) return run({});
        args.emplace_back(argv[i]);
    }
    return run(args);
}
#endif
