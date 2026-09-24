// woke.wtf launcher - local singleplayer / private-testing injector.
//
// This is a plain LoadLibraryW remote-thread loader: it starts nothing in the background,
// hides nothing, and exists so the client can be tested without depending on a
// third-party injector. It refuses cross-architecture targets, since an x64 DLL cannot
// be loaded into a 32-bit process.

#include "injector.h"

#include <windows.h>
#include <tlhelp32.h>

#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <string.h>  // _wcsicmp (Microsoft CRT extension used for case-insensitive matching)

namespace woke::launcher {
namespace {

bool parse_unsigned(const wchar_t* text, DWORD& out) {
    if (text == nullptr || *text == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || (end != nullptr && *end != L'\0')) {
        return false;
    }
    out = static_cast<DWORD>(value);
    return true;
}

DWORD find_process_by_name(const wchar_t* name) {
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (::Process32FirstW(snapshot, &entry) != FALSE) {
        do {
            if (::_wcsicmp(entry.szExeFile, name) == 0) {
                found = entry.th32ProcessID;
                break;
            }
        } while (::Process32NextW(snapshot, &entry) != FALSE);
    }
    (void)::CloseHandle(snapshot);
    return found;
}

bool is_32_bit_process(HANDLE process, bool& wow64) {
    BOOL result = FALSE;
    if (::IsWow64Process(process, &result) == FALSE) {
        return false;
    }
    wow64 = result != FALSE;
    return true;
}

void print_usage() {
    std::fwprintf(stderr,
        L"woke_injector - local testing loader for woke.dll\n"
        L"\n"
        L"usage: woke_injector <path\\to\\woke.dll> [--pid <number> | --process <name>]\n"
        L"       default target process: javaw.exe\n"
        L"\n"
        L"For private server testing and local singleplayer development only.\n");
}

} // namespace

int run(int argc, wchar_t** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    const wchar_t* dll_path = argv[1];
    if (::GetFileAttributesW(dll_path) == INVALID_FILE_ATTRIBUTES) {
        std::fwprintf(stderr, L"error: '%ls' does not exist\n", dll_path);
        return 2;
    }

    DWORD target_pid = 0;
    const wchar_t* process_name = L"javaw.exe";

    for (int index = 2; index + 1 < argc; ++index) {
        if (::_wcsicmp(argv[index], L"--pid") == 0) {
            if (!parse_unsigned(argv[index + 1], target_pid)) {
                std::fwprintf(stderr, L"error: --pid expects a number\n");
                return 2;
            }
            ++index;
        } else if (::_wcsicmp(argv[index], L"--process") == 0) {
            process_name = argv[index + 1];
            ++index;
        } else {
            std::fwprintf(stderr, L"error: unknown argument '%ls'\n", argv[index]);
            print_usage();
            return 2;
        }
    }

    if (target_pid == 0) {
        target_pid = find_process_by_name(process_name);
        if (target_pid == 0) {
            std::fwprintf(stderr, L"error: no running process named '%ls'\n", process_name);
            return 3;
        }
    }

    const HANDLE process = ::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, target_pid);
    if (process == nullptr) {
        std::fwprintf(stderr, L"error: OpenProcess failed for pid %lu (error %lu)\n", target_pid,
            ::GetLastError());
        return 3;
    }

    bool wow64 = false;
    if (is_32_bit_process(process, wow64) && wow64) {
        std::fwprintf(stderr, L"error: pid %lu is a 32-bit process; woke.dll targets x64\n",
            target_pid);
        (void)::CloseHandle(process);
        return 4;
    }

    const std::size_t path_bytes = (std::wcslen(dll_path) + 1) * sizeof(wchar_t);
    void* remote_buffer = ::VirtualAllocEx(process, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (remote_buffer == nullptr) {
        std::fwprintf(stderr, L"error: VirtualAllocEx failed (error %lu)\n", ::GetLastError());
        (void)::CloseHandle(process);
        return 4;
    }

    if (::WriteProcessMemory(process, remote_buffer, dll_path, path_bytes, nullptr) == FALSE) {
        std::fwprintf(stderr, L"error: WriteProcessMemory failed (error %lu)\n", ::GetLastError());
        (void)::VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        (void)::CloseHandle(process);
        return 4;
    }

    const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel32 != nullptr ? ::GetProcAddress(kernel32, "LoadLibraryW") : nullptr);
    if (load_library == nullptr) {
        std::fwprintf(stderr, L"error: could not resolve LoadLibraryW\n");
        (void)::VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        (void)::CloseHandle(process);
        return 4;
    }

    const HANDLE thread =
        ::CreateRemoteThread(process, nullptr, 0, load_library, remote_buffer, 0, nullptr);
    if (thread == nullptr) {
        std::fwprintf(stderr, L"error: CreateRemoteThread failed (error %lu)\n", ::GetLastError());
        (void)::VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
        (void)::CloseHandle(process);
        return 4;
    }

    const DWORD wait = ::WaitForSingleObject(thread, 10000);
    DWORD module_handle = 0;
    (void)::GetExitCodeThread(thread, &module_handle);

    (void)::CloseHandle(thread);
    (void)::VirtualFreeEx(process, remote_buffer, 0, MEM_RELEASE);
    (void)::CloseHandle(process);

    if (wait != WAIT_OBJECT_0) {
        std::fwprintf(stderr, L"warning: LoadLibraryW did not finish within 10s\n");
        return 5;
    }
    if (module_handle == 0) {
        std::fwprintf(stderr, L"error: LoadLibraryW returned NULL - the DLL was rejected\n");
        return 6;
    }

    std::wprintf(L"loaded '%ls' into pid %lu (module 0x%08lX)\n", dll_path, target_pid,
        module_handle);
    std::wprintf(L"check the game's logs/ directory for the session file and latest.log\n");
    return 0;
}

} // namespace woke::launcher

int wmain(int argc, wchar_t** argv) {
    return woke::launcher::run(argc, argv);
}
