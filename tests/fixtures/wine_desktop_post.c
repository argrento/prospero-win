/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Console probe for Wine patch 1039: where a message posted to the desktop
 * window goes. A message thread asks for the desktop first, so on the PS5 the
 * server-created desktop's handle names it, then makes a message-only window
 * of its own (parent HWND_MESSAGE). The main thread posts to that window, to
 * the thread, to the desktop and to the HWND_MESSAGE root, and the message
 * thread reports what it received.
 *
 * Expected with 1039: the window and thread posts arrive, the desktop and
 * root posts don't. Without it the desktop post arrives in the message
 * thread's queue (what crashed Slay the Spire's OpenAL thread). The run only
 * means something when both handles name the message thread: if another
 * thread of the process asked for the desktop first, it says INCONCLUSIVE.
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -o wine_desktop_post.exe wine_desktop_post.c -luser32
 * Each line goes to stderr with a "PROBE desktop_post" prefix; exit code 0
 * means pass, 1 fail, 2 inconclusive.
 */
#include <windows.h>
#include <stdio.h>

#define MSG_TO_WINDOW (WM_USER + 7)
#define MSG_TO_THREAD (WM_USER + 9)
#define MSG_TO_DESKTOP WM_USER
#define MSG_TO_ROOT (WM_USER + 3)

static HWND own_window, desktop, root;
static HANDLE ready, posted;
static DWORD message_tid;
static int got_window, got_thread, got_desktop, got_root, got_other;

static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == MSG_TO_WINDOW && wparam == 1 && lparam == 2) got_window++;
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI message_thread(void *arg)
{
    WNDCLASSW cls = {0};
    MSG msg;
    DWORD start;

    (void)arg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    desktop = GetDesktopWindow();
    cls.lpfnWndProc = window_proc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"wine_desktop_post";
    RegisterClassW(&cls);
    own_window = CreateWindowExW(0, L"wine_desktop_post", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL,
                                 cls.hInstance, NULL);
    root = own_window ? GetAncestor(own_window, GA_PARENT) : NULL;
    SetEvent(ready);
    WaitForSingleObject(posted, INFINITE);

    start = GetTickCount();
    while (GetTickCount() - start < 1000)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            /* the IDs alone count: a GetMessage loop like OpenAL's acts on them */
            if (msg.hwnd == NULL && msg.message == MSG_TO_THREAD && msg.wParam == 3) got_thread++;
            else if (msg.hwnd != own_window && msg.message == MSG_TO_DESKTOP) got_desktop++;
            else if (msg.hwnd != own_window && msg.message == MSG_TO_ROOT) got_root++;
            else if (msg.hwnd == own_window) DispatchMessageW(&msg);
            else if (msg.message >= WM_USER && msg.message < 0x8000) got_other++;
        }
        Sleep(10);
    }
    return 0;
}

int main(void)
{
    DWORD desktop_tid, desktop_pid = 0, root_tid, root_pid = 0;
    HANDLE thread;
    BOOL post_window, post_thread, post_desktop, post_root;
    int pass, owned;

    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    posted = CreateEventW(NULL, TRUE, FALSE, NULL);
    thread = ready && posted ? CreateThread(NULL, 0, message_thread, NULL, 0, &message_tid) : NULL;
    if (!thread || WaitForSingleObject(ready, 10000) != WAIT_OBJECT_0)
    {
        fprintf(stderr, "PROBE desktop_post FAIL setup error=%lu\n", GetLastError());
        return 1;
    }

    desktop_tid = GetWindowThreadProcessId(desktop, &desktop_pid);
    root_tid = root ? GetWindowThreadProcessId(root, &root_pid) : 0;
    fprintf(stderr, "PROBE desktop_post desktop=%p tid=%lu pid=%lu root=%p tid=%lu own=%p message_thread=%lu process=%lu\n",
            desktop, desktop_tid, desktop_pid, root, root_tid, own_window, message_tid, GetCurrentProcessId());

    post_window = own_window && PostMessageW(own_window, MSG_TO_WINDOW, 1, 2);
    post_thread = PostThreadMessageW(message_tid, MSG_TO_THREAD, 3, 0);
    post_desktop = PostMessageW(desktop, MSG_TO_DESKTOP, 0, 0);
    post_root = root ? PostMessageW(root, MSG_TO_ROOT, 0, 0) : FALSE;
    SetEvent(posted);
    if (WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0)
    {
        fprintf(stderr, "PROBE desktop_post FAIL message thread did not finish\n");
        return 1;
    }

    fprintf(stderr, "PROBE desktop_post posted window=%d thread=%d desktop=%d root=%d\n", post_window, post_thread,
            post_desktop, post_root);
    fprintf(stderr, "PROBE desktop_post received window=%d thread=%d desktop=%d root=%d other=%d\n", got_window,
            got_thread, got_desktop, got_root, got_other);
    pass = own_window && post_window && post_thread && post_desktop && post_root && got_window == 1 &&
           got_thread == 1 && !got_desktop && !got_root;
    owned = desktop_tid == message_tid && root_tid == message_tid;
    if (pass && !owned)
    {
        fprintf(stderr, "PROBE desktop_post INCONCLUSIVE: the desktop or root names another thread\n");
        return 2;
    }
    fprintf(stderr, "PROBE desktop_post %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
