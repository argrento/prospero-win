/* SPDX-License-Identifier: LGPL-2.1-or-later
 * A Windows page made read-only or inaccessible faults on access even when
 * its host page is larger and holds pages that stay writable (the PS5's are
 * 16 KiB), and those neighbours keep working without faults the program sees.
 * Emulators that recompile code protect the 4 KiB pages holding it and expect
 * the fault that tells them the code changed (DuckStation, for one). */
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define PAGE 4096
#define PAGES 40

static unsigned char *base;
static volatile LONG faults, fault_page, fault_write;

/* Records the fault and makes the page writable again, as an emulator does
 * after it drops the code it compiled from that page. */
static LONG CALLBACK handler(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *rec = info->ExceptionRecord;
    unsigned char *addr = (unsigned char *)rec->ExceptionInformation[1];
    DWORD old;

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || addr < base || addr >= base + PAGES * PAGE)
        return EXCEPTION_CONTINUE_SEARCH;
    faults++;
    fault_page = (LONG)((addr - base) / PAGE);
    fault_write = rec->ExceptionInformation[0] == 1;
    VirtualProtect(base + fault_page * PAGE, PAGE, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static int protect_at(unsigned char *page, DWORD prot)
{
    DWORD old;

    if (VirtualProtect(page, PAGE, prot, &old)) return 1;
    printf("subpage protect %p %#lx failed %lu\n", page, prot, GetLastError());
    return 0;
}

static int protect(int page, DWORD prot)
{
    return protect_at(base + page * PAGE, prot);
}

/* Reads PAGE bytes of a file into a page, as a game loads its data: the kernel
 * writes the buffer, so no fault is raised for Wine to step over. Returns whether
 * ReadFile succeeded. */
static int read_file_into(HANDLE file, unsigned char *buffer)
{
    DWORD got = 0;

    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    return ReadFile(file, buffer, PAGE, &got, NULL) && got == PAGE;
}

/* Writes one byte to each page in turn; returns how many faults the program saw. */
static LONG write_pages(int first, int count, unsigned char value)
{
    LONG before = faults;
    int page;

    for (page = first; page < first + count; page++) ((volatile unsigned char *)base)[page * PAGE + 123] = value;
    return faults - before;
}

/* Decommits pages that are `own`, beside a page that is `neighbour` in the same host
 * page, and commits them again: they come back zeroed and the neighbour keeps its
 * protection. Chromium's allocator decommits this way, and stops if it fails. */
static int decommit_beside(const char *what, DWORD neighbour, DWORD own, int count)
{
    unsigned char *memory = VirtualAlloc(NULL, 16 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    MEMORY_BASIC_INFORMATION info;
    DWORD old;
    int i, ok = memory != NULL;

    for (i = 1; ok && i <= count; i++) memory[i * PAGE] = 0x55;
    ok = ok && VirtualProtect(memory, PAGE, neighbour, &old);
    ok = ok && (own == PAGE_READWRITE || VirtualProtect(memory + PAGE, count * PAGE, own, &old));
    ok = ok && VirtualFree(memory + PAGE, count * PAGE, MEM_DECOMMIT);
    printf("subpage decommit %s: %s (error %lu)\n", what, ok ? "done" : "failed", ok ? 0ul : GetLastError());
    ok = ok && VirtualAlloc(memory + PAGE, count * PAGE, MEM_COMMIT, PAGE_READWRITE);
    for (i = 1; ok && i <= count; i++) ok = memory[i * PAGE] == 0;
    ok = ok && VirtualQuery(memory, &info, sizeof(info)) && info.Protect == neighbour;
    printf("subpage decommit %s: %s\n", what, ok ? "zeroed, neighbour kept" : "wrong after commit");
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

static int expect(const char *what, LONG got, LONG want)
{
    printf("subpage %s: %ld fault(s), expected %ld\n", what, got, want);
    return got == want;
}

static volatile unsigned char *watch_byte;

static DWORD WINAPI write_watch_byte(void *arg)
{
    *watch_byte = (unsigned char)(UINT_PTR)arg;
    return 0;
}

/* Writes one byte in another thread and waits for it: a write Wine keeps
 * faulting on, without the program seeing it, never ends. */
static int write_in_time(volatile unsigned char *where, unsigned char value)
{
    HANDLE thread;
    DWORD wait;

    watch_byte = where;
    thread = CreateThread(NULL, 0, write_watch_byte, (void *)(UINT_PTR)value, 0, NULL);
    if (!thread) return 0;
    wait = WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    if (wait == WAIT_OBJECT_0) return *where == value;
    printf("subpage write-watch write did not end\nsubpage verdict=fail\n");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1);
    return 0;
}

static unsigned char *copy_from, *copy_to;

static DWORD WINAPI copy_bytes(void *arg)
{
    void *to = copy_to, *from = copy_from;
    SIZE_T count = 8;

    (void)arg;
    __asm__ __volatile__("rep movsb" : "+D"(to), "+S"(from), "+c"(count) : : "memory");
    return 0;
}

/* Copies with one instruction (movs, as memcpy's tail) from a page beside an
 * inaccessible one to a page beside a read-only one, in two host pages that both
 * need opening for it: the copy ends and the program sees no fault. */
static int copy_between_host_pages(void)
{
    unsigned char *memory = VirtualAlloc(NULL, 16 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned char *saved = base;
    HANDLE thread;
    DWORD wait;
    LONG got;
    int ok = memory != NULL;

    if (ok) memset(memory + PAGE, 'm', 8);
    ok = ok && protect_at(memory, PAGE_NOACCESS) && protect_at(memory + 4 * PAGE, PAGE_READONLY);
    if (!ok) return 0;
    base = memory;  /* the handler counts faults in this allocation */
    copy_from = memory + PAGE;
    copy_to = memory + 5 * PAGE;
    got = faults;
    thread = CreateThread(NULL, 0, copy_bytes, NULL, 0, NULL);
    wait = thread ? WaitForSingleObject(thread, 5000) : WAIT_FAILED;
    if (thread) CloseHandle(thread);
    if (wait != WAIT_OBJECT_0)
    {
        printf("subpage copy between host pages did not end\nsubpage verdict=fail\n");
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 1);
    }
    ok = expect("copy between two host pages beside protected pages", faults - got, 0) &&
         !memcmp(memory + 5 * PAGE, "mmmmmmmm", 8);
    base = saved;
    VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

/* Whether GetWriteWatch lists the page, and only pages of its host page. */
static int watched(unsigned char *memory, int page)
{
    void *pages[16];
    ULONG_PTR count = sizeof(pages) / sizeof(pages[0]), i;
    ULONG granularity;
    int found = 0;

    if (GetWriteWatch(0, memory, 16 * PAGE, pages, &count, &granularity)) return 0;
    for (i = 0; i < count; i++)
    {
        if ((unsigned char *)pages[i] == memory + page * PAGE) found = 1;
        else if (((unsigned char *)pages[i] - memory) / (4 * PAGE) != page / 4) return 0;
    }
    return found;
}

/* Writes to a writable page of a write-watched allocation beside a read-only page,
 * as a game that compiles another console's code into its memory and makes the
 * code pages read-only: the write ends and is listed, before and after a reset,
 * and a write to the read-only page still faults. */
static int write_watch_beside(void)
{
    unsigned char *memory = VirtualAlloc(NULL, 16 * PAGE, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    unsigned char *saved = base;
    LONG got;
    int ok = memory != NULL;

    ok = ok && protect_at(memory + PAGE, PAGE_READONLY);
    got = faults;
    ok = ok && write_in_time(memory + 123, 1) && watched(memory, 0);
    ok = ok && !ResetWriteWatch(memory, 16 * PAGE);
    ok = ok && write_in_time(memory + 2 * PAGE + 123, 2) && watched(memory, 2);
    ok = ok && expect("write-watched writes beside a read-only page", faults - got, 0);
    base = memory;  /* the handler takes faults in this allocation */
    got = faults;
    ok = ok && write_in_time(memory + PAGE + 123, 3);
    ok = ok && expect("write-watched write to the read-only page", faults - got, 1) && fault_page == 1 && fault_write;
    base = saved;
    if (memory) VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

static volatile LONG toggling;
static unsigned char *toggle_memory;

static DWORD WINAPI toggle_neighbour(void *arg)
{
    DWORD old;

    (void)arg;
    while (toggling)
    {
        VirtualProtect(toggle_memory, PAGE, PAGE_NOACCESS, &old);
        VirtualProtect(toggle_memory, PAGE, PAGE_READWRITE, &old);
    }
    return 0;
}

/* Reads and writes of a page while another thread keeps protecting and unprotecting the
 * page beside it, as a program that guards its own memory: a fault that finds the host
 * page opened by the other thread meanwhile is retried, never raised. */
static int accesses_while_neighbour_changes(void)
{
    unsigned char *memory = VirtualAlloc(NULL, 16 * PAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    unsigned char *saved = base;
    HANDLE thread;
    LONG got;
    int i, ok = memory != NULL;

    if (!ok) return 0;
    base = memory;
    toggle_memory = memory;
    toggling = 1;
    got = faults;
    thread = CreateThread(NULL, 0, toggle_neighbour, NULL, 0, NULL);
    for (i = 0; i < 200000; i++)
    {
        ((volatile unsigned char *)memory)[PAGE + (i & 4095)] = (unsigned char)i;
        (void)((volatile unsigned char *)memory)[2 * PAGE + (i & 4095)];
    }
    toggling = 0;
    if (thread)
    {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
    ok = expect("accesses while the neighbour's protection changes", faults - got, 0) && thread;
    base = saved;
    VirtualFree(memory, 0, MEM_RELEASE);
    return ok;
}

int main(void)
{
    LARGE_INTEGER freq, start, end;
    char folder[MAX_PATH], path[MAX_PATH], data[PAGE], message[100];
    HANDLE file;
    DWORD written;
    WSADATA wsa;
    SOCKET udp;
    struct sockaddr_in where;
    int where_size = sizeof(where), timeout = 3000;
    volatile unsigned char sink;
    LONG got;
    const char *report_env = getenv("PW_SUBPAGE_REPORT");
    int report = report_env && !strcmp(report_env, "1");
    int i, ok;

    base = VirtualAlloc(NULL, PAGES * PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ok = base && AddVectoredExceptionHandler(1, handler);

    /* A read-only page inside a host page whose other pages stay writable. */
    ok = ok && protect(1, PAGE_READONLY);
    ok = ok && expect("writes beside a read-only page", write_pages(0, 1, 1) + write_pages(2, 2, 2), 0);
    got = write_pages(1, 1, 3);
    ok = ok && expect("write to the read-only page", got, 1) && fault_page == 1 && fault_write;
    ok = ok && base[PAGE + 123] == 3;
    ok = ok && expect("second write after the handler made it writable", write_pages(1, 1, 4), 0);

    /* Read-only pages on both sides of a host page boundary, writable ones between. */
    ok = ok && protect(3, PAGE_READONLY) && protect(4, PAGE_READONLY);
    ok = ok && expect("writes between read-only pages", write_pages(0, 3, 5) + write_pages(5, 3, 6), 0);
    ok = ok && expect("writes to both read-only pages", write_pages(3, 2, 7), 2);

    /* An inaccessible page faults on a read too. */
    ok = ok && protect(9, PAGE_NOACCESS);
    got = faults;
    sink = base[8 * PAGE] + base[10 * PAGE];
    ok = ok && expect("reads beside an inaccessible page", faults - got, 0);
    got = faults;
    sink = base[9 * PAGE + 7];
    ok = ok && expect("read of the inaccessible page", faults - got, 1) && fault_page == 9 && !fault_write;
    (void)sink;

    /* Many writes beside a protected page: the first ones are stepped over unseen.
     * When they keep coming, Wine keeps the host page writable and none is seen;
     * with PW_SUBPAGE_REPORT=1 it reports one write to the protected page itself
     * instead (the handler then makes it writable, as an emulator does), and none
     * after (patch 0989). */
    ok = ok && protect(13, PAGE_READONLY);
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    got = faults;
    for (i = 0; i < 20000; i++) ((volatile unsigned char *)base)[12 * PAGE + (i & 4095)] = (unsigned char)i;
    QueryPerformanceCounter(&end);
    got = faults - got;
    printf("subpage 20000 writes beside a read-only page: %ld fault(s), expected %s\n", got,
           report ? "at most 1 on that page" : "0");
    ok = ok && (got == 0 || (report && got == 1 && fault_page == 13 && fault_write));
    printf("subpage neighbour writes: %.2f us each\n",
           (double)(end.QuadPart - start.QuadPart) * 1e6 / freq.QuadPart / 20000);
    ok = ok && base[12 * PAGE + 19999 % 4096] == (unsigned char)19999;

    /* Many reads and writes beside an inaccessible page, as a garbage collector's
     * heap: none is seen, and they stop being stepped over (the host page keeps the
     * union of its pages' protections). Protecting the page again splits the host
     * page, and touching it faults again. */
    ok = ok && protect(17, PAGE_NOACCESS);
    QueryPerformanceCounter(&start);
    got = faults;
    for (i = 0; i < 20000; i++)
    {
        ((volatile unsigned char *)base)[16 * PAGE + (i & 4095)] = (unsigned char)i;
        sink = base[18 * PAGE + (i & 4095)];
    }
    QueryPerformanceCounter(&end);
    ok = ok && expect("20000 reads and writes beside an inaccessible page", faults - got, 0);
    printf("subpage accesses beside an inaccessible page: %.2f us each\n",
           (double)(end.QuadPart - start.QuadPart) * 1e6 / freq.QuadPart / 40000);
    ok = ok && protect(17, PAGE_READWRITE) && protect(17, PAGE_NOACCESS);
    got = faults;
    sink = base[17 * PAGE + 9];
    ok = ok && expect("read of the inaccessible page protected again", faults - got, 1) && fault_page == 17;

    /* Many writes beside a code page, as .NET's stubs: a code page and the data
     * page its code reads, side by side. The program never asked to learn of
     * writes there and has no handler for them (an access violation ends the
     * process), so none is reported; the host page keeps the union of its pages'
     * protections and the code still runs. */
    base[21 * PAGE] = 0xc3;  /* ret */
    ok = ok && protect(21, PAGE_EXECUTE_READ);
    QueryPerformanceCounter(&start);
    got = faults;
    for (i = 0; i < 20000; i++) ((volatile unsigned char *)base)[20 * PAGE + (i & 4095)] = (unsigned char)i;
    QueryPerformanceCounter(&end);
    ok = ok && expect("20000 writes beside a code page", faults - got, 0);
    printf("subpage writes beside a code page: %.2f us each\n",
           (double)(end.QuadPart - start.QuadPart) * 1e6 / freq.QuadPart / 20000);
    ok = ok && base[20 * PAGE + 19999 % 4096] == (unsigned char)19999;
    /* A reported write left the code page writable, not executable: call it only when none was. */
    if (ok) ((void (*)(void))(base + 21 * PAGE))();
    /* Protecting the code page again splits the host page: a write to it faults again. */
    ok = ok && protect(21, PAGE_READWRITE) && protect(21, PAGE_EXECUTE_READ);
    got = write_pages(21, 1, 9);
    ok = ok && expect("write to the code page protected again", got, 1) && fault_page == 21 && fault_write;

    /* A file read into a writable page beside a read-only one succeeds, as on
     * Windows; a read into the read-only page itself fails, and that page still
     * faults afterwards (patch 0988). */
    memset(data, 'f', sizeof(data));
    ok = ok && GetTempPathA(MAX_PATH, folder) && GetTempFileNameA(folder, "sub", 0, path);
    file = ok ? CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL)
              : INVALID_HANDLE_VALUE;
    for (i = 0; i < 4; i++)  /* four pages of file */
        ok = ok && file != INVALID_HANDLE_VALUE && WriteFile(file, data, sizeof(data), &written, NULL) && written == sizeof(data);
    ok = ok && protect(25, PAGE_READONLY);
    got = read_file_into(file, base + 24 * PAGE) && base[24 * PAGE + 7] == 'f';
    printf("subpage file read beside a read-only page: %s\n", got ? "read" : "failed");
    ok = ok && got;
    got = read_file_into(file, base + 25 * PAGE);
    printf("subpage file read into the read-only page: %s\n", got ? "read" : "refused");
    ok = ok && !got && base[25 * PAGE + 7] != 'f';
    ok = ok && expect("write to the read-only page after the reads", write_pages(25, 1, 10), 1) && fault_page == 25;
    /* A read from a host page nobody protected into a writable page of one beside a
     * read-only page: the kernel stops short at that host page, and the read is
     * finished, not returned short as if the file ended. */
    ok = ok && protect(33, PAGE_READONLY);
    SetFilePointer(file, 0, NULL, FILE_BEGIN);
    written = 0;
    got = ReadFile(file, base + 29 * PAGE, 4 * PAGE, &written, NULL) && written == 4 * PAGE && base[32 * PAGE + 4095] == 'f';
    printf("subpage file read into a protected host page: %lu of %u bytes\n", written, 4 * PAGE);
    ok = ok && got;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);

    /* A datagram received into a writable page beside a read-only one arrives whole:
     * a receive whose copy fails loses the datagram, so the buffer is opened first. */
    ok = ok && protect(37, PAGE_READONLY) && !WSAStartup(MAKEWORD(2, 2), &wsa);
    udp = ok ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : INVALID_SOCKET;
    memset(&where, 0, sizeof(where));
    where.sin_family = AF_INET;
    where.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ok = ok && udp != INVALID_SOCKET && !bind(udp, (struct sockaddr *)&where, sizeof(where)) &&
         !getsockname(udp, (struct sockaddr *)&where, &where_size) &&
         !setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    memset(message, 'u', sizeof(message));
    ok = ok && sendto(udp, message, sizeof(message), 0, (struct sockaddr *)&where, sizeof(where)) == sizeof(message);
    got = ok ? recv(udp, (char *)base + 36 * PAGE, sizeof(message), 0) : -1;
    printf("subpage datagram beside a read-only page: %ld of %u bytes\n", got, (unsigned)sizeof(message));
    ok = ok && got == sizeof(message) && !memcmp(base + 36 * PAGE, message, sizeof(message));
    if (udp != INVALID_SOCKET) closesocket(udp);

    /* Pages decommitted beside protected ones, also when they are protected too. */
    ok = decommit_beside("writable beside read-only", PAGE_READONLY, PAGE_READWRITE, 1) && ok;
    ok = decommit_beside("three writable beside inaccessible", PAGE_NOACCESS, PAGE_READWRITE, 3) && ok;
    ok = decommit_beside("read-only beside read-only", PAGE_READONLY, PAGE_READONLY, 1) && ok;
    ok = decommit_beside("read-only beside inaccessible", PAGE_NOACCESS, PAGE_READONLY, 1) && ok;
    ok = decommit_beside("inaccessible beside inaccessible", PAGE_NOACCESS, PAGE_NOACCESS, 1) && ok;
    ok = decommit_beside("inaccessible beside writable", PAGE_READWRITE, PAGE_NOACCESS, 1) && ok;

    ok = write_watch_beside() && ok;
    ok = copy_between_host_pages() && ok;
    ok = accesses_while_neighbour_changes() && ok;

    printf("subpage verdict=%s\n", ok ? "pass" : "fail");
    fflush(stdout);
    return ok ? 0 : 1;
}
