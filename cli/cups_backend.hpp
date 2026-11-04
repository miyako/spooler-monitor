#pragma once
#include "monitor.hpp"
#include <cups/cups.h>
#include <cups/ipp.h>
#include <chrono>

namespace monitor {
struct IppDelete { void operator()(ipp_t* p) const { if (p) ippDelete(p); } };
using Ipp = std::unique_ptr<ipp_t, IppDelete>;
// The transport owns its request even on failure. Tests use actual IPP objects,
// never a scheduler or live printer, to exercise request and response semantics.
using CupsTransport = std::function<Ipp(Ipp, const std::string&, bool)>;
using CupsConnect = std::function<bool(const char*, int)>;
using CupsClock = std::function<std::chrono::steady_clock::time_point()>;
std::string connect_local_cups(const CupsConnect& attempt,
                              const CupsClock& clock = std::chrono::steady_clock::now);
Ipp submit_local_cups(const CupsConnect& connect, const std::function<Ipp()>& submit,
                     const CupsClock& clock = std::chrono::steady_clock::now);
class CupsBackend final : public Backend {
    CupsTransport transport_;
    Ipp request(ipp_op_t operation, const std::string& id, bool mutation);
public:
    explicit CupsBackend(CupsTransport transport);
    const char* name() const override { return "cups"; }
    std::vector<Queue> list() override;
    Queue get(const std::string& id) override;
    void mutate(const std::string& id, bool pause) override;
};
std::string cups_queue_uri(const std::string& id);
}
