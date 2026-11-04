#pragma once
#include "monitor.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winspool.h>

namespace monitor {
struct WinspoolApi {
    std::function<BOOL(DWORD, LPWSTR, DWORD, LPBYTE, DWORD, LPDWORD, LPDWORD)> enumerate;
    std::function<BOOL(LPWSTR, LPHANDLE, LPPRINTER_DEFAULTSW)> open;
    std::function<BOOL(HANDLE, DWORD, LPBYTE, DWORD, LPDWORD)> get;
    std::function<BOOL(HANDLE, DWORD, LPBYTE, DWORD)> set;
    std::function<BOOL(HANDLE)> close;
};
std::unique_ptr<Backend> winspool_backend(WinspoolApi api);
}
