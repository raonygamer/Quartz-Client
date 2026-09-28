#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

struct Fixture
{
    volatile uint32_t Value, Run, Quit;
    unsigned char Signature[16];
};
static struct Fixture* fixture;
__attribute__((noinline)) void fixture_tick(void) { fixture->Value = fixture->Value + 1; }
static DWORD WINAPI worker(void* unused)
{
    (void)unused;
    for (int i = 0; i < 20 && !fixture->Quit; ++i) { if (fixture->Run) fixture_tick(); Sleep(5); }
    return 0;
}
int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    fixture = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!fixture) return 2;
    fixture->Value = 0x13579bdf;
    *(uintptr_t*)((unsigned char*)fixture + 32) = (uintptr_t)fixture;
    *(uint32_t*)((unsigned char*)fixture + 32 + sizeof(uintptr_t)) = 0xdeadbeef;
    for (int i = 0; i < 16; ++i) fixture->Signature[i] = (unsigned char)(0xa0 + i);
    FILE* output = fopen(argv[1], "w");
    if (!output) return 3;
    fprintf(output, "%lu %llu %llu\n", GetCurrentProcessId(), (unsigned long long)(uintptr_t)fixture, (unsigned long long)(uintptr_t)&fixture_tick);
    fclose(output);
    while (!fixture->Quit)
    {
        HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!thread) return 4;
        WaitForSingleObject(thread, INFINITE); CloseHandle(thread);
    }
    VirtualFree(fixture, 0, MEM_RELEASE);
    return 0;
}
