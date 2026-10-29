// T6_PROF=1: a crude host sampling profiler. Every ~1 ms it samples the instruction pointer of
// every other thread of the process; every 10 s it prints the 40 hottest RVAs (and the share per
// thread), to be resolved against build/t6.map with tools/sym.py.
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <string>
#include <map>

static std::map<std::string, unsigned> s_mod;
static DWORD WINAPI profThread(LPVOID)
{
    const uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    std::unordered_map<uint64_t, unsigned> hist;
    std::unordered_map<DWORD, unsigned> perThread;
    unsigned total = 0;
    ULONGLONG last = GetTickCount64();
    const DWORD self = GetCurrentThreadId();
    for (;;) {
        Sleep(1);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self) continue;
            HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
            if (!t) continue;
            if (SuspendThread(t) != (DWORD)-1) {
                CONTEXT c{}; c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(t, &c)) {
                    const uintptr_t rip = (uintptr_t)c.Rip;
                    // Only count threads that are running our code (not parked in the kernel).
                    if (rip >= base && rip < base + 0x10000000) { hist[rip - base]++; perThread[te.th32ThreadID]++; total++; }
                    else {
                        HMODULE m = nullptr; char name[MAX_PATH] = "?";
                        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)rip, &m) && m)
                            GetModuleFileNameA(m, name, MAX_PATH);
                        const char* b = strrchr(name, '\\'); std::string key = std::to_string(te.th32ThreadID) + ":" + (b ? b + 1 : name);
                        s_mod[key]++;
                    }
                }
                ResumeThread(t);
            }
            CloseHandle(t);
        }
        CloseHandle(snap);
        if (GetTickCount64() - last > 10000 && total) {
            last = GetTickCount64();
            std::vector<std::pair<uint64_t, unsigned>> v(hist.begin(), hist.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            fprintf(stderr, "[prof] %u samples in our code; threads:", total);
            for (auto& kv : perThread) fprintf(stderr, " %lu=%u%%", kv.first, kv.second * 100 / total);
            fputc('\n', stderr);
            for (size_t i = 0; i < v.size() && i < 40; i++) fprintf(stderr, "[prof] rva=0x%llX %u\n", (unsigned long long)v[i].first, v[i].second);
            std::vector<std::pair<std::string, unsigned>> mv(s_mod.begin(), s_mod.end());
            std::sort(mv.begin(), mv.end(), [](auto& a, auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < mv.size() && i < 25; i++) fprintf(stderr, "[prof] outside %s %u\n", mv[i].first.c_str(), mv[i].second);
            s_mod.clear();
            hist.clear(); perThread.clear(); total = 0;
        }
    }
}

extern "C" void t6_prof_start(void)
{
    if (getenv("T6_PROF")) CreateThread(nullptr, 0, profThread, nullptr, 0, nullptr);
}
