/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Capture lab: lists the audio endpoints, records five seconds from the
 * default capture device and plays them back on the default render device,
 * through WASAPI (winmm's waveIn and DirectSound capture sit on the same
 * mmdevapi). It writes what it saw to audio_capture_lab.txt beside itself
 * and to stdout and stderr (a title's log keeps stderr), so a console run
 * shows whether a game would find a microphone, at what format, and
 * whether its samples carry sound.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o audio_capture_lab.exe \
 *       tests/lab/audio_capture_lab.c -lole32 -luuid
 *
 * Arguments: [seconds] (1-30, default 5).
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

static FILE *out;

static void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (out) {
        va_start(args, format);
        vfprintf(out, format, args);
        va_end(args);
        fflush(out);
    }
    fflush(stdout);
}

static void list(IMMDeviceEnumerator *enumerator, EDataFlow flow, const char *label)
{
    IMMDeviceCollection *devices;
    UINT count = 0;

    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(enumerator, flow, DEVICE_STATE_ACTIVE, &devices))) {
        say("%s: cannot enumerate\n", label);
        return;
    }
    IMMDeviceCollection_GetCount(devices, &count);
    say("%s endpoints: %u\n", label, count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice *device;
        IPropertyStore *store;
        PROPVARIANT name;

        if (FAILED(IMMDeviceCollection_Item(devices, i, &device))) continue;
        PropVariantInit(&name);
        if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) {
            if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
                say("  %u: %ls\n", i, name.pwszVal);
            PropVariantClear(&name);
            IPropertyStore_Release(store);
        }
        IMMDevice_Release(device);
    }
    IMMDeviceCollection_Release(devices);
}

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, which mingw's headers only declare. */
static const GUID subtype_float = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

/* A sample of the mix format as -1..1. */
static double sample(const BYTE *at, const WAVEFORMATEX *format)
{
    const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)format;
    BOOL is_float = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                    (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                     IsEqualGUID(&ex->SubFormat, &subtype_float));
    if (is_float && format->wBitsPerSample == 32) return *(const float *)at;
    switch (format->wBitsPerSample) {
    case 8: return (at[0] - 128) / 128.0;
    case 16: return *(const short *)at / 32768.0;
    case 32: return *(const int *)at / 2147483648.0;
    default: return 0;
    }
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    IMMDeviceEnumerator *enumerator;
    IMMDevice *mic, *speaker;
    IAudioClient *client;
    IAudioCaptureClient *capture;
    IAudioRenderClient *render;
    WAVEFORMATEX *format;
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    HRESULT hr;

    if (seconds < 1 || seconds > 30) seconds = 5;
    GetModuleFileNameA(NULL, path, sizeof(path));
    strcpy(strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path, "audio_capture_lab.txt");
    out = fopen(path, "w");

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator,
                          (void **)&enumerator);
    if (FAILED(hr)) { say("no device enumerator: %#lx\n", hr); return 1; }
    list(enumerator, eRender, "render");
    list(enumerator, eCapture, "capture");

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eConsole, &mic);
    if (FAILED(hr)) { say("no default capture device: %#lx\n", hr); return 0; }
    IMMDevice_Activate(mic, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    IAudioClient_GetMixFormat(client, &format);
    say("capture mix format: tag %#x, %lu Hz, %u channels, %u bits\n", format->wFormatTag,
        format->nSamplesPerSec, format->nChannels, format->wBitsPerSample);
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, NULL);
    if (FAILED(hr)) { say("capture Initialize: %#lx\n", hr); return 1; }
    IAudioClient_GetService(client, &IID_IAudioCaptureClient, (void **)&capture);

    UINT32 total = format->nSamplesPerSec * seconds, got = 0;
    BYTE *recording = calloc(total, format->nBlockAlign);
    double peak = 0, energy = 0;
    DWORD start = GetTickCount();
    hr = IAudioClient_Start(client);
    say("recording %d s: Start %#lx\n", seconds, hr);
    while (got < total && GetTickCount() - start < (DWORD)(seconds + 3) * 1000) {
        UINT32 packet = 0, frames;
        BYTE *data;
        DWORD flags;

        Sleep(10);
        while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &packet)) && packet) {
            if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) break;
            for (UINT32 i = 0; i < frames && got < total; i++, got++) {
                const BYTE *frame = data + i * format->nBlockAlign;
                double value = flags & AUDCLNT_BUFFERFLAGS_SILENT ? 0 : sample(frame, format);
                memcpy(recording + (size_t)got * format->nBlockAlign, frame, format->nBlockAlign);
                if (fabs(value) > peak) peak = fabs(value);
                energy += value * value;
            }
            IAudioCaptureClient_ReleaseBuffer(capture, frames);
        }
    }
    IAudioClient_Stop(client);
    say("captured %u of %u frames in %lu ms; peak %.4f, rms %.4f%s\n", got, total, GetTickCount() - start, peak,
        got ? sqrt(energy / got) : 0.0, peak == 0 ? " (silence: muted, or no sound reached the microphone)" : "");
    IAudioCaptureClient_Release(capture);
    IAudioClient_Release(client);

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &speaker);
    if (FAILED(hr)) { say("no default render device: %#lx\n", hr); return 0; }
    IMMDevice_Activate(speaker, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 10000000, 0, format, NULL);
    if (FAILED(hr)) { say("playback Initialize: %#lx\n", hr); return 1; }
    IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render);
    UINT32 size, played = 0;
    IAudioClient_GetBufferSize(client, &size);
    IAudioClient_Start(client);
    while (played < got) {
        UINT32 padding = 0, room;
        BYTE *data;

        IAudioClient_GetCurrentPadding(client, &padding);
        room = size - padding;
        if (room > got - played) room = got - played;
        if (room && SUCCEEDED(IAudioRenderClient_GetBuffer(render, room, &data))) {
            memcpy(data, recording + (size_t)played * format->nBlockAlign, (size_t)room * format->nBlockAlign);
            IAudioRenderClient_ReleaseBuffer(render, room, 0);
            played += room;
        }
        Sleep(10);
    }
    Sleep(500);
    IAudioClient_Stop(client);
    say("played back %u frames\n", played);
    if (out) fclose(out);
    return 0;
}
