#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <DbgHelp.h>
#include <string>
#pragma comment(lib,"DbgHelp.lib")

// ============================================================================
//  CrashHandler
//  Registers an unhandled exception filter that:
//    1. Writes a .dmp minidump next to the .exe
//    2. Flushes the log file
//    3. Shows a MessageBox with the crash address and log path
// ============================================================================
namespace CrashHandler {

inline std::wstring g_logPath;
inline std::wstring g_dumpPath;

inline LONG WINAPI filter(EXCEPTION_POINTERS* ep) {
    // ── Write minidump ─────────────────────────────────────────────────────
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dumpPath = exePath;
    auto dot = dumpPath.rfind(L'.');
    if (dot != std::wstring::npos) dumpPath = dumpPath.substr(0, dot);
    dumpPath += L"_crash.dmp";
    g_dumpPath = dumpPath;

    HANDLE hFile = CreateFileW(dumpPath.c_str(),
                               GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei {};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers    = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          hFile,
                          (MINIDUMP_TYPE)(MiniDumpWithDataSegs |
                                          MiniDumpWithHandleData |
                                          MiniDumpWithThreadInfo),
                          &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }

    // ── Build error message ────────────────────────────────────────────────
    DWORD code   = ep->ExceptionRecord->ExceptionCode;
    void* addr   = ep->ExceptionRecord->ExceptionAddress;

    wchar_t msg[1024];
    swprintf_s(msg, sizeof(msg)/sizeof(wchar_t),
        L"AudioRouter crashed!\n\n"
        L"Exception: 0x%08X\n"
        L"Address:   0x%p\n\n"
        L"Minidump:  %s\n"
        L"Log file:  %s\n\n"
        L"Please send both files to the developer.",
        code, addr,
        dumpPath.c_str(),
        g_logPath.c_str());

    MessageBoxW(nullptr, msg, L"AudioRouter — Fatal Error",
                MB_OK | MB_ICONERROR | MB_TOPMOST);

    return EXCEPTION_EXECUTE_HANDLER;
}

inline void install(const std::wstring& logPath) {
    g_logPath = logPath;
    SetUnhandledExceptionFilter(filter);
}

} // namespace CrashHandler
