/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Direct3D 12 shared resource: a committed texture on a shared heap
 * (D3D12_HEAP_FLAG_SHARED) and an NT handle for it, as Unreal Engine 5
 * makes them while it starts (tests/test_d3d12_shared_resource.py runs it
 * under the host Wine; on the console it is the same program, run as a
 * game).
 *
 * vkd3d-proton asks winevulkan for the memory's Win32 handle right after it
 * allocates a shared resource, without checking that the device has
 * VK_KHR_external_memory_win32. A device without it, as on a PS5, whose
 * driver has no external memory fds, got a NULL vkGetMemoryWin32HandleKHR
 * and the program jumped to address 0 (wine/patches/1027).
 *
 * One line on stdout says it all; the exit code is 0 when the resource and
 * its shared handle were created. Opening the handle again is reported but
 * not required: it needs the host's external memory fds.
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>

int main(void)
{
    D3D12_HEAP_PROPERTIES heap = { 0 };
    D3D12_RESOURCE_DESC desc = { 0 };
    ID3D12Resource *resource = NULL, *opened = NULL;
    ID3D12Device *device = NULL;
    HRESULT create = E_FAIL, share = E_FAIL, open = E_FAIL;
    HANDLE handle = NULL;
    HRESULT hr;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (FAILED(hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&device))) {
        printf("D3D12_SHARED_RESOURCE device=%#lx verdict=fail\n", (unsigned long)hr);
        return 1;
    }

    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 256;
    desc.Height = 256;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    /* Where the program died before 1027. */
    printf("D3D12_SHARED_RESOURCE step=create\n");
    create = ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                  D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource,
                                                  (void **)&resource);
    if (SUCCEEDED(create)) {
        printf("D3D12_SHARED_RESOURCE step=share\n");
        share = ID3D12Device_CreateSharedHandle(device, (ID3D12DeviceChild *)resource, NULL, GENERIC_ALL, NULL,
                                                &handle);
    }
    if (SUCCEEDED(share)) {
        printf("D3D12_SHARED_RESOURCE step=open\n");
        open = ID3D12Device_OpenSharedHandle(device, handle, &IID_ID3D12Resource, (void **)&opened);
    }

    printf("D3D12_SHARED_RESOURCE create=%#lx share=%#lx open=%#lx verdict=%s\n", (unsigned long)create,
           (unsigned long)share, (unsigned long)open, SUCCEEDED(create) && SUCCEEDED(share) ? "pass" : "fail");
    if (opened) ID3D12Resource_Release(opened);
    if (handle) CloseHandle(handle);
    if (resource) ID3D12Resource_Release(resource);
    ID3D12Device_Release(device);
    return SUCCEEDED(create) && SUCCEEDED(share) ? 0 : 1;
}
