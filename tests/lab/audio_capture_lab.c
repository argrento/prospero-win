/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Capture lab: a window with Record and Play buttons and the state of every
 * audio endpoint, through WASAPI (winmm's waveIn and DirectSound capture sit
 * on the same mmdevapi). Record takes up to 30 seconds from the default
 * capture device and shows its level as it comes in; Play plays the
 * recording on the default render device. A pad drives it too: A (Cross)
 * records or stops, B (Circle) plays or stops. Under Playden's launcher
 * mode the pad starts as a mouse: hold the touchpad for 2 seconds to give
 * the program the pad. Close it with the PS button.
 *
 * Every event goes, as one MICLAB line, to audio_capture_lab.txt beside the
 * program and to stdout and stderr (a title's log keeps stderr): the device
 * list when it changes, the capture format, a line a second while recording
 * (peak, rms, packets, silent and discontinuity flags) and a summary.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o audio_capture_lab.exe \
 *       tests/lab/audio_capture_lab.c -lole32 -luuid -lgdi32 -luser32
 *
 * On a console it runs from a game's folder: Game actions > Run launcher or
 * setup... starts it once without changing the game's settings.
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SECONDS 30
#define ID_RECORD 1
#define ID_PLAY 2
#define TIMER_ID 1
#define TIMER_MS 20

static FILE *out;
static HWND window, record_button, play_button;
static HFONT font, small_font;
static IMMDeviceEnumerator *enumerator;

enum { IDLE, RECORDING, PLAYING } state = IDLE;
static char status[512] = "Press Record (Cross), talk, then Stop. Play (Circle) plays it back.";
static char devices[4096], last_devices[4096];
static char format_text[384] = "capture format: not opened yet";
static char result_text[256] = "";

/* recording */
static IAudioClient *capture_client;
static IAudioCaptureClient *capture;
static WAVEFORMATEX *format;
static BYTE *recording;
static UINT32 total, got;
static DWORD started;
static double peak, energy, second_peak, second_energy, meter;
static UINT32 second_frames, second_count, packets, silent_packets, discontinuities;

/* playback */
static IAudioClient *render_client;
static IAudioRenderClient *render;
static UINT32 render_size, played;

/* the pad, through whichever XInput the runtime has */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadState;
typedef struct { DWORD packet; PadState pad; } XState;
static DWORD (WINAPI *xinput_get)(DWORD, XState *);
static WORD last_buttons;

static void say(const char *format_string, ...)
{
    va_list args;
    char line[1024];

    va_start(args, format_string);
    vsnprintf(line, sizeof(line), format_string, args);
    va_end(args);
    printf("MICLAB %s\n", line);
    fflush(stdout);
    fprintf(stderr, "MICLAB %s\n", line);
    fflush(stderr);
    if (out) {
        fprintf(out, "MICLAB %s\n", line);
        fflush(out);
    }
}

static void set_status(const char *text)
{
    snprintf(status, sizeof(status), "%s", text);
    say("%s", text);
    InvalidateRect(window, NULL, FALSE);
}

static const char *state_name(DWORD value)
{
    switch (value) {
    case DEVICE_STATE_ACTIVE: return "active";
    case DEVICE_STATE_DISABLED: return "disabled";
    case DEVICE_STATE_NOTPRESENT: return "not present";
    case DEVICE_STATE_UNPLUGGED: return "unplugged";
    default: return "?";
    }
}

static void device_name(IMMDevice *device, char *name, size_t size)
{
    IPropertyStore *store;
    PROPVARIANT value;

    snprintf(name, size, "(no name)");
    PropVariantInit(&value);
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) {
        if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR)
            snprintf(name, size, "%ls", value.pwszVal);
        PropVariantClear(&value);
        IPropertyStore_Release(store);
    }
}

static size_t append(char *text, size_t used, size_t size, const char *format_string, ...)
{
    va_list args;
    int n;

    if (used >= size) return used;
    va_start(args, format_string);
    n = vsnprintf(text + used, size - used, format_string, args);
    va_end(args);
    return n > 0 ? used + (size_t)n : used;
}

static size_t list(char *text, size_t used, size_t size, EDataFlow flow, const char *label)
{
    IMMDeviceCollection *collection;
    IMMDevice *fallback;
    UINT count = 0;
    char name[256], default_name[256] = "none";

    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow, eConsole, &fallback))) {
        device_name(fallback, default_name, sizeof(default_name));
        IMMDevice_Release(fallback);
    }
    used = append(text, used, size, "%s default: %s\n", label, default_name);
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, flow, DEVICE_STATEMASK_ALL, &collection)))
        return append(text, used, size, "  %s: cannot enumerate\n", label);
    IMMDeviceCollection_GetCount(collection, &count);
    if (!count) used = append(text, used, size, "  no %s endpoints\n", label);
    for (UINT i = 0; i < count; i++) {
        IMMDevice *device;
        DWORD device_state = 0;

        if (FAILED(IMMDeviceCollection_Item(collection, i, &device))) continue;
        device_name(device, name, sizeof(name));
        IMMDevice_GetState(device, &device_state);
        used = append(text, used, size, "  %s %u: %s (%s)\n", label, i, name, state_name(device_state));
        IMMDevice_Release(device);
    }
    IMMDeviceCollection_Release(collection);
    return used;
}

/* Reads the endpoints again; logs the list when it changed (a headset came
 * or went, or the capture device appeared). */
static void refresh_devices(void)
{
    size_t used = list(devices, 0, sizeof(devices), eRender, "render");

    list(devices, used, sizeof(devices), eCapture, "capture");
    if (strcmp(devices, last_devices)) {
        char copy[sizeof(devices)], *line, *next;

        say("devices:");
        snprintf(copy, sizeof(copy), "%s", devices);
        for (line = copy; line && *line; line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            say("%s", line);
        }
        snprintf(last_devices, sizeof(last_devices), "%s", devices);
        InvalidateRect(window, NULL, FALSE);
    }
}

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, which mingw's headers only declare. */
static const GUID subtype_float = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

/* The first channel of a frame as -1..1. */
static double sample(const BYTE *at, const WAVEFORMATEX *wave)
{
    const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)wave;
    BOOL is_float = wave->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (wave->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(&ex->SubFormat, &subtype_float));
    if (is_float && wave->wBitsPerSample == 32) return *(const float *)at;
    switch (wave->wBitsPerSample) {
    case 8: return (at[0] - 128) / 128.0;
    case 16: return *(const short *)at / 32768.0;
    case 32: return *(const int *)at / 2147483648.0;
    default: return 0;
    }
}

static void stop_playing(const char *why)
{
    char line[160];

    if (state != PLAYING) return;
    IAudioClient_Stop(render_client);
    IAudioRenderClient_Release(render);
    IAudioClient_Release(render_client);
    render = NULL;
    render_client = NULL;
    state = IDLE;
    snprintf(line, sizeof(line), "playback %s: %u of %u frames", why, played, got);
    set_status(line);
}

static void stop_recording(void)
{
    char line[256];

    if (state != RECORDING) return;
    IAudioClient_Stop(capture_client);
    IAudioCaptureClient_Release(capture);
    IAudioClient_Release(capture_client);
    capture = NULL;
    capture_client = NULL;
    state = IDLE;
    meter = 0;
    snprintf(result_text, sizeof(result_text),
             "last recording: %.1f s in %.1f s, peak %.4f, rms %.4f, packets %u (silent %u, discontinuity %u)%s",
             format ? (double)got / format->nSamplesPerSec : 0.0, (GetTickCount() - started) / 1000.0, peak,
             got ? sqrt(energy / got) : 0.0, packets, silent_packets, discontinuities, peak == 0 ? " - SILENCE" : "");
    say("%s", result_text);
    snprintf(line, sizeof(line), "Recorded %.1f s. Press Play (Circle) to hear it.",
             format ? (double)got / format->nSamplesPerSec : 0.0);
    set_status(line);
}

static void start_recording(void)
{
    IMMDevice *mic;
    HRESULT hr;
    char name[256], line[512];

    stop_playing("stopped");
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eConsole, &mic);
    if (FAILED(hr)) {
        snprintf(line, sizeof(line), "No microphone: GetDefaultAudioEndpoint(capture) %#lx", hr);
        set_status(line);
        return;
    }
    device_name(mic, name, sizeof(name));
    hr = IMMDevice_Activate(mic, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&capture_client);
    IMMDevice_Release(mic);
    if (FAILED(hr)) {
        snprintf(line, sizeof(line), "capture Activate %#lx", hr);
        set_status(line);
        return;
    }
    if (format) CoTaskMemFree(format);
    IAudioClient_GetMixFormat(capture_client, &format);
    snprintf(format_text, sizeof(format_text), "capture format (%s): tag %#x, %lu Hz, %u channels, %u bits", name,
             format->wFormatTag, format->nSamplesPerSec, format->nChannels, format->wBitsPerSample);
    say("%s", format_text);
    hr = IAudioClient_Initialize(capture_client, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(capture_client, &IID_IAudioCaptureClient, (void **)&capture);
    if (FAILED(hr)) {
        snprintf(line, sizeof(line), "capture Initialize/GetService %#lx", hr);
        IAudioClient_Release(capture_client);
        capture_client = NULL;
        set_status(line);
        return;
    }
    total = format->nSamplesPerSec * MAX_SECONDS;
    free(recording);
    recording = calloc(total, format->nBlockAlign);
    got = second_frames = second_count = packets = silent_packets = discontinuities = 0;
    peak = energy = second_peak = second_energy = meter = 0;
    result_text[0] = 0;
    started = GetTickCount();
    hr = IAudioClient_Start(capture_client);
    state = RECORDING;
    snprintf(line, sizeof(line), "Recording from %s (Start %#lx). Talk now; press Stop (Cross) when done.", name, hr);
    set_status(line);
}

static void pump_recording(void)
{
    UINT32 packet = 0, frames;
    BYTE *data;
    DWORD flags;
    double recent = 0;
    HRESULT hr;

    while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &packet)) && packet) {
        hr = IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL);
        if (FAILED(hr)) {
            say("GetBuffer %#lx", hr);
            break;
        }
        packets++;
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) silent_packets++;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) discontinuities++;
        for (UINT32 i = 0; i < frames && got < total; i++, got++) {
            const BYTE *frame = data + i * format->nBlockAlign;
            double value = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : fabs(sample(frame, format));

            memcpy(recording + (size_t)got * format->nBlockAlign, frame, format->nBlockAlign);
            if (value > peak) peak = value;
            if (value > second_peak) second_peak = value;
            if (value > recent) recent = value;
            energy += value * value;
            second_energy += value * value;
            if (++second_frames == format->nSamplesPerSec) {
                say("second %u: peak %.4f rms %.4f, %lu ms since Start, packets %u (silent %u, discontinuity %u)",
                    ++second_count, second_peak, sqrt(second_energy / second_frames), GetTickCount() - started,
                    packets, silent_packets, discontinuities);
                second_peak = second_energy = 0;
                second_frames = 0;
            }
        }
        IAudioCaptureClient_ReleaseBuffer(capture, frames);
    }
    /* the meter falls back slowly, so a word stays visible */
    meter = recent > meter ? recent : meter * 0.85;
    if (got >= total) stop_recording();
    else if (GetTickCount() - started > (MAX_SECONDS + 3) * 1000u) {
        say("no frames for too long: %u of %u", got, total);
        stop_recording();
    }
    InvalidateRect(window, NULL, FALSE);
}

static void start_playing(void)
{
    IMMDevice *speaker;
    HRESULT hr;
    char name[256], line[512];

    if (state == RECORDING) stop_recording();
    if (!got || !format) {
        set_status("Nothing recorded yet: press Record (Cross) first.");
        return;
    }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &speaker);
    if (FAILED(hr)) {
        snprintf(line, sizeof(line), "No speaker: GetDefaultAudioEndpoint(render) %#lx", hr);
        set_status(line);
        return;
    }
    device_name(speaker, name, sizeof(name));
    hr = IMMDevice_Activate(speaker, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&render_client);
    IMMDevice_Release(speaker);
    if (SUCCEEDED(hr))
        hr = IAudioClient_Initialize(render_client, AUDCLNT_SHAREMODE_SHARED,
                                     AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                     10000000, 0, format, NULL);
    if (SUCCEEDED(hr)) hr = IAudioClient_GetService(render_client, &IID_IAudioRenderClient, (void **)&render);
    if (FAILED(hr)) {
        snprintf(line, sizeof(line), "playback Activate/Initialize/GetService %#lx", hr);
        if (render_client) IAudioClient_Release(render_client);
        render_client = NULL;
        set_status(line);
        return;
    }
    IAudioClient_GetBufferSize(render_client, &render_size);
    played = 0;
    state = PLAYING;
    IAudioClient_Start(render_client);
    snprintf(line, sizeof(line), "Playing %.1f s on %s.", (double)got / format->nSamplesPerSec, name);
    set_status(line);
}

static void pump_playing(void)
{
    UINT32 padding = 0, room;
    BYTE *data;

    if (FAILED(IAudioClient_GetCurrentPadding(render_client, &padding))) {
        stop_playing("failed");
        return;
    }
    if (played >= got) {
        if (!padding) stop_playing("done");
        return;
    }
    room = render_size - padding;
    if (room > got - played) room = got - played;
    if (room && SUCCEEDED(IAudioRenderClient_GetBuffer(render, room, &data))) {
        memcpy(data, recording + (size_t)played * format->nBlockAlign, (size_t)room * format->nBlockAlign);
        IAudioRenderClient_ReleaseBuffer(render, room, 0);
        played += room;
    }
    InvalidateRect(window, NULL, FALSE);
}

static void press_record(void)
{
    if (state == RECORDING) stop_recording();
    else start_recording();
}

static void press_play(void)
{
    if (state == PLAYING) stop_playing("stopped");
    else start_playing();
}

static void poll_pad(void)
{
    XState pad;
    WORD pressed;

    if (!xinput_get) return;
    for (DWORD user = 0; user < 4; user++) {
        if (xinput_get(user, &pad)) continue;
        pressed = pad.pad.buttons & ~last_buttons;
        last_buttons = pad.pad.buttons;
        if (pressed & 0x1000) press_record();      /* A */
        if (pressed & 0x2000) press_play();        /* B */
        return;
    }
}

static void paint(HDC target, RECT *area)
{
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, area->right, area->bottom);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    HBRUSH background = CreateSolidBrush(RGB(24, 26, 32));
    RECT text, bar;
    char line[300];
    int width = area->right, y = 50 + area->bottom / 14;

    FillRect(dc, area, background);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(235, 235, 240));
    SelectObject(dc, font);
    SetRect(&text, 40, y, width - 40, y + 50);
    DrawTextA(dc, status, -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += 60;

    /* the level meter */
    SetRect(&bar, 40, y, width - 40, y + 40);
    FrameRect(dc, &bar, (HBRUSH)GetStockObject(GRAY_BRUSH));
    if (state == RECORDING) {
        HBRUSH fill = CreateSolidBrush(meter > 0.5 ? RGB(230, 90, 70) : RGB(80, 200, 120));
        double db = meter > 0 ? 20 * log10(meter) : -90;
        RECT level = bar;
        InflateRect(&level, -2, -2);
        /* -60 dB..0 dB across the bar */
        level.right = level.left + (LONG)((level.right - level.left) * (db < -60 ? 0 : (db + 60) / 60));
        FillRect(dc, &level, fill);
        DeleteObject(fill);
    }
    y += 50;
    SelectObject(dc, small_font);
    if (state == RECORDING)
        snprintf(line, sizeof(line), "%.1f s recorded in %.1f s, level %.4f, peak %.4f, packets %u (silent %u, "
                 "discontinuity %u)", (double)got / format->nSamplesPerSec, (GetTickCount() - started) / 1000.0, meter,
                 peak, packets, silent_packets, discontinuities);
    else if (state == PLAYING)
        snprintf(line, sizeof(line), "%.1f of %.1f s played", (double)played / format->nSamplesPerSec,
                 (double)got / format->nSamplesPerSec);
    else snprintf(line, sizeof(line), "%s", result_text);
    SetRect(&text, 40, y, width - 40, y + 30);
    DrawTextA(dc, line, -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += 40;
    SetRect(&text, 40, y, width - 40, y + 30);
    DrawTextA(dc, format_text, -1, &text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += 50;
    SetTextColor(dc, RGB(170, 200, 255));
    SetRect(&text, 40, y, width - 40, area->bottom - 40);
    DrawTextA(dc, devices, -1, &text, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SetTextColor(dc, RGB(140, 140, 150));
    SetRect(&text, 40, area->bottom - 40, width - 40, area->bottom - 10);
    DrawTextA(dc, "Cross: Record/Stop   Circle: Play/Stop   Hold touchpad 2 s if Cross moves a pointer   "
              "PS button: close", -1,
              &text, DT_LEFT | DT_TOP | DT_SINGLELINE);

    BitBlt(target, 0, 0, area->right, area->bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteObject(background);
    DeleteDC(dc);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    static DWORD last_refresh;
    static int shown_state = -1;

    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_RECORD) press_record();
        if (LOWORD(wparam) == ID_PLAY) press_play();
        SetFocus(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wparam == 'R' || wparam == VK_RETURN) press_record();
        if (wparam == 'P' || wparam == VK_SPACE) press_play();
        if (wparam == VK_ESCAPE) PostMessageA(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_TIMER:
        poll_pad();
        if (state == RECORDING) pump_recording();
        if (state == PLAYING) pump_playing();
        if (GetTickCount() - last_refresh > 1000) {
            last_refresh = GetTickCount();
            refresh_devices();
        }
        if ((int)state != shown_state) {
            shown_state = state;
            SetWindowTextA(record_button, state == RECORDING ? "Stop (Cross)" : "Record (Cross)");
            SetWindowTextA(play_button, state == PLAYING ? "Stop (Circle)" : "Play (Circle)");
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT area;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &area);
        paint(dc, &area);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, message, wparam, lparam);
}

int main(void)
{
    char path[MAX_PATH];
    WNDCLASSA klass = { 0 };
    HMODULE xinput;
    MSG message;
    HRESULT hr;
    int width = GetSystemMetrics(SM_CXSCREEN), height = GetSystemMetrics(SM_CYSCREEN);

    GetModuleFileNameA(NULL, path, sizeof(path));
    strcpy(strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path, "audio_capture_lab.txt");
    out = fopen(path, "w");
    say("start, screen %dx%d", width, height);

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator,
                          (void **)&enumerator);
    if (FAILED(hr)) {
        say("no device enumerator: %#lx", hr);
        return 1;
    }
    if ((xinput = LoadLibraryA("xinput1_4.dll")) || (xinput = LoadLibraryA("xinput1_3.dll")) ||
        (xinput = LoadLibraryA("xinput9_1_0.dll")))
        xinput_get = (DWORD (WINAPI *)(DWORD, XState *))(void *)GetProcAddress(xinput, "XInputGetState");
    say("pad: %s", xinput_get ? "XInput" : "none");

    if (width < 640 || height < 480) width = 1280, height = 720;
    klass.lpfnWndProc = window_proc;
    klass.hInstance = GetModuleHandleA(NULL);
    klass.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    klass.lpszClassName = "AudioCaptureLab";
    RegisterClassA(&klass);
    window = CreateWindowExA(0, klass.lpszClassName, "Microphone lab", WS_POPUP | WS_VISIBLE, 0, 0, width, height,
                             NULL, NULL, klass.hInstance, NULL);
    if (!window) {
        say("CreateWindow failed: %lu", GetLastError());
        return 1;
    }
    font = CreateFontA(-(height / 24), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                       "Segoe UI");
    small_font = CreateFontA(-(height / 40), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                             0, "Segoe UI");
    record_button = CreateWindowA("BUTTON", "Record (Cross)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 40, 30, width / 4,
                                  height / 14, window, (HMENU)ID_RECORD, klass.hInstance, NULL);
    play_button = CreateWindowA("BUTTON", "Play (Circle)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 60 + width / 4, 30,
                                width / 4, height / 14, window, (HMENU)ID_PLAY, klass.hInstance, NULL);
    SendMessageA(record_button, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageA(play_button, WM_SETFONT, (WPARAM)font, TRUE);
    refresh_devices();
    SetForegroundWindow(window);
    SetFocus(window);
    SetTimer(window, TIMER_ID, TIMER_MS, NULL);
    while (GetMessageA(&message, NULL, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
    if (state == RECORDING) stop_recording();
    if (state == PLAYING) stop_playing("stopped");
    say("quit");
    if (out) fclose(out);
    return 0;
}
