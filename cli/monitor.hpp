#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace monitor {
constexpr std::size_t max_id = 1024, max_argument = 16384, max_queues = 4096;
constexpr std::size_t max_output = 16 * 1024 * 1024;
constexpr const char* usage =
    "expected list, get|pause|resume <queueID>, capabilities, inspect-argument <text>, or --version";
bool valid_utf8(std::string_view value);
bool valid_id(std::string_view value);

// Output-only JSON: no parser or untrusted pre-serialized values.
class Json {
    std::string text_;
    explicit Json(std::string text, int) : text_(std::move(text)) {}
public:
    Json() : text_("null") {}
    Json(const std::string& value);
    Json(const char* value) : Json(std::string(value)) {}
    Json(bool value) : text_(value ? "true" : "false") {}
    Json(std::int64_t value) : text_(std::to_string(value)) {}
    static Json object(std::initializer_list<std::pair<std::string, Json>> fields);
    static Json object(const std::vector<std::pair<std::string, Json>>& fields);
    static Json array(const std::vector<Json>& values);
    const std::string& text() const { return text_; }
};
struct Error : std::runtime_error {
    int status;
    std::string code, backend, outcome;
    std::optional<std::int64_t> native;
    Error(int status, std::string code, std::string message, std::string backend = "",
          std::optional<std::int64_t> native = {}, std::string outcome = "not_submitted");
    Json json() const;
};
struct Queue {
    std::string id, display_name, backend, state = "unknown";
    std::optional<bool> accepting;
    std::optional<std::int64_t> jobs;
    std::optional<std::vector<std::string>> reasons;
    std::optional<std::string> message;
    Json raw = Json::object({});
    Json json() const;
};
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char* name() const = 0;
    virtual std::vector<Queue> list() = 0;
    virtual Queue get(const std::string& id) = 0;
    virtual void mutate(const std::string& id, bool pause) = 0;
};
struct Response { int status; std::string out, err; };
using Factory = std::function<std::unique_ptr<Backend>()>;
Response execute(const std::vector<std::string>& arguments, const Factory& factory);
std::unique_ptr<Backend> native_backend();
const char* native_backend_name();
std::string windows_state(std::uint32_t status);
Json windows_flags(std::uint32_t status);
}
