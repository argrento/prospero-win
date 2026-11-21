/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Frame generation in libvulkan.prx (PW_FRAMEGEN=2..4, from a profile's
 * [display] framegen). Every frame a program presents is copied aside, and
 * MihawkGen (third_party/mihawkgen) makes the pictures between it and the
 * one before; they are drawn into extra images of the program's swapchain
 * and presented first, so the screen gets FACTOR pictures for each frame
 * the program draws. FIFO presentation, forced on the swapchain, paces
 * them: a 30 fps program fills a 60 Hz screen at FACTOR 2. Nothing changes for the program or Wine; a
 * swapchain the generator cannot serve (memory, format, the network) is
 * left as it was. Once a second it logs the program's frames and the
 * pictures presented.
 *
 * The performance overlay (pw_hud.h) is drawn in the top left of every
 * picture, after generation, so it is never blended: PW_HUD=fps|frametimes|
 * full ([display] hud), or the frame rate alone for PW_FRAMEGEN_HUD=1 with
 * a factor (show_fps) and PW_FRAMEGEN_COUNTER=1 without (fps_counter). The
 * overlay is drawn into a host buffer, copied into a small image and blitted,
 * scaled, onto the picture: one copy and one blit a picture.
 *
 * With an overlay and no PW_FRAMEGEN it only counts: the program's frames
 * are drawn on as they are presented, for a backend that has no overlay of
 * its own or whose overlay is replaced. The swapchain keeps its images and
 * present mode, and no generator or network is loaded.
 *
 * GPL-3.0-or-later because it links MihawkGen; libvulkan.prx, the only
 * module it goes into, is distributed under the GPL as a whole
 * (THIRD_PARTY.md). Linked by tools/link_radv_prx.sh. */
#include "pw_framegen_vk.h"
#include <pthread.h>
#include <stdint.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <new>
#include "framegen.h"
#include "pw_framegen.h"
#include "pw_hud.h"

extern "C" PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" PFN_vkVoidFunction VKAPI_CALL vk_common_GetDeviceProcAddr(VkDevice device, const char *name);
/* RADV's WSI (pw_vulkan_radv.c): where the swapchain's images show in VideoOut's framebuffer, and its size. */
extern "C" bool wsi_videoout_present_rect(VkRect2D *rect, VkExtent2D *frame);
/* The trained network, embedded by tools/link_radv_prx.sh (pw_framegen_network.S). */
extern "C" const unsigned char pw_framegen_network[], pw_framegen_network_end[];

#define PW_FRAMEGEN_VK_DEFINE(name) PFN_vk##name pw_fgvk_##name;
extern "C" {
PW_FRAMEGEN_VK_DEVICE(PW_FRAMEGEN_VK_DEFINE)
PW_FRAMEGEN_VK_INSTANCE(PW_FRAMEGEN_VK_DEFINE)
}
#undef PW_FRAMEGEN_VK_DEFINE

namespace {

constexpr uint32_t kSlots = 2;      /* frames in flight */
constexpr uint32_t kMaxFactor = 4;
constexpr uint32_t kMaxImages = 16;
constexpr uint32_t kMaxChains = 8;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

constexpr uint32_t kMaxPresentWaits = 16;

struct Chain {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    VkExtent2D extent{};
    uint32_t factor = 0;
    VkImage images[kMaxImages]{};
    uint32_t image_count = 0;
    fg_context *fg = nullptr;
    fg_method method = FG_METHOD_BLEND;
    Image history[2], output[kMaxFactor - 1];
    uint32_t cur = 0;
    bool have_prev = false;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t family = UINT32_MAX;
    VkCommandBuffer cmd[kSlots]{};
    VkFence fence[kSlots]{};
    VkSemaphore real_done[kSlots]{}, acquired[kSlots][kMaxFactor - 1]{}, generated[kSlots][kMaxFactor - 1]{};
    /* counting only: one per image, free again once the image is reacquired */
    VkSemaphore image_done[kMaxImages]{};
    uint32_t slot = 0;
    /* what the overlay counts */
    PwHudStats stats{};
    /* the overlay: its image, and a mapped host buffer a slot it is drawn into */
    Image hud;
    VkBuffer hud_buffer[kSlots]{};
    VkDeviceMemory hud_memory[kSlots]{};
    uint32_t *hud_pixels[kSlots]{};
};

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int factor_setting = -1, hud_setting = -1, counter_setting = -1;
long cpus_online = -1;
/* the game's direct memory, from the title (pw_hud_memory) */
/* used and peak in MiB, in one word so a reader never mixes two samples; 0 none yet */
uint64_t memory_mib;
VkInstance instance_seen = VK_NULL_HANDLE;
bool device_loaded = false;
struct { VkDevice device; VkPhysicalDevice physical; } devices[8];
struct { VkQueue queue; uint32_t family; } queues[32];
Chain *chains[kMaxChains];

PFN_vkCreateDevice real_CreateDevice;
PFN_vkGetDeviceQueue real_GetDeviceQueue;
PFN_vkGetDeviceQueue2 real_GetDeviceQueue2;
PFN_vkCreateSwapchainKHR real_CreateSwapchainKHR;
PFN_vkDestroySwapchainKHR real_DestroySwapchainKHR;
PFN_vkQueuePresentKHR real_QueuePresentKHR;

uint32_t factor(void)
{
    if (factor_setting < 0) {
        const char *value = getenv("PW_FRAMEGEN");
        long n = value ? strtol(value, nullptr, 10) : 0;
        factor_setting = n >= 2 && n <= (long)kMaxFactor ? (int)n : 0;
        if (factor_setting) fprintf(stderr, "pw_framegen: factor %d\n", factor_setting);
    }
    return (uint32_t)factor_setting;
}

/* The overlay's level: PW_HUD, else the frame rate for PW_FRAMEGEN_HUD=1
 * with a factor or PW_FRAMEGEN_COUNTER=1 without. */
PwHudLevel hud(void)
{
    if (hud_setting < 0) {
        PwHudLevel level = pw_hud_level_parse(getenv("PW_HUD"));
        const char *value = getenv(factor() ? "PW_FRAMEGEN_HUD" : "PW_FRAMEGEN_COUNTER");
        if (level == PW_HUD_OFF && value && value[0] == '1') level = PW_HUD_FPS;
        hud_setting = (int)level;
        if (level) fprintf(stderr, "pw_framegen: overlay level %d\n", (int)level);
    }
    return (PwHudLevel)hud_setting;
}

/* Counting without generating: an overlay, and no factor. */
bool counter_only(void)
{
    if (counter_setting < 0) {
        counter_setting = !factor() && hud();
        if (counter_setting) fprintf(stderr, "pw_framegen: counter only\n");
    }
    return counter_setting;
}

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

bool load_device(VkDevice device)
{
    if (device_loaded) return true;
#define PW_FRAMEGEN_VK_LOAD(name) \
    if (!(pw_fgvk_##name = (PFN_vk##name)vk_common_GetDeviceProcAddr(device, "vk" #name))) return false;
    PW_FRAMEGEN_VK_DEVICE(PW_FRAMEGEN_VK_LOAD)
#undef PW_FRAMEGEN_VK_LOAD
    if (!(pw_fgvk_GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
              vk_icdGetInstanceProcAddr(instance_seen, "vkGetPhysicalDeviceMemoryProperties")))
        return false;
    return device_loaded = true;
}

VkPhysicalDevice physical_of(VkDevice device)
{
    for (auto &d : devices)
        if (d.device == device) return d.physical;
    return VK_NULL_HANDLE;
}

uint32_t family_of(VkQueue queue)
{
    for (auto &q : queues)
        if (q.queue == queue) return q.family;
    return 0;  /* RADV's first family does graphics, compute and transfer */
}

void remember_queue(VkQueue queue, uint32_t family)
{
    for (auto &q : queues)
        if (!q.queue || q.queue == queue) { q.queue = queue; q.family = family; return; }
}

uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

void free_image(VkDevice device, Image *image);

/* An image of the chain's size, or extent; with a view unless view is false
 * (an image only copied and blitted, whose usage takes no view). */
bool make_image(Chain *c, VkPhysicalDevice physical, VkImageUsageFlags usage, Image *out,
                VkExtent2D extent = {0, 0}, bool view_too = true)
{
    if (!extent.width) extent = c->extent;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    if (vkCreateImage(c->device, &info, nullptr, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, out->image, &req);
    uint32_t type = memory_type(physical, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(c->device, &alloc, nullptr, &out->memory) != VK_SUCCESS ||
        vkBindImageMemory(c->device, out->image, out->memory, 0) != VK_SUCCESS)
        return false;
    if (!view_too) return true;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(c->device, &view, nullptr, &out->view) == VK_SUCCESS;
}

/* The overlay's image, and a host buffer a slot that each present draws the
 * overlay into (record_hud) while that slot's fence keeps the GPU off it. */
bool make_hud(Chain *c, VkPhysicalDevice physical)
{
    if (!make_image(c, physical, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->hud,
                    {PW_HUD_MAX_W, PW_HUD_MAX_H}, false))
        return false;
    for (uint32_t s = 0; s < kSlots; s++) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = (VkDeviceSize)PW_HUD_MAX_W * PW_HUD_MAX_H * 4;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(c->device, &info, nullptr, &c->hud_buffer[s]) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(c->device, c->hud_buffer[s], &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(physical, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void *mapped = nullptr;
        if (alloc.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(c->device, &alloc, nullptr, &c->hud_memory[s]) != VK_SUCCESS ||
            vkBindBufferMemory(c->device, c->hud_buffer[s], c->hud_memory[s], 0) != VK_SUCCESS ||
            vkMapMemory(c->device, c->hud_memory[s], 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return false;
        c->hud_pixels[s] = (uint32_t *)mapped;
    }
    return true;
}

void free_hud(Chain *c)
{
    free_image(c->device, &c->hud);
    for (uint32_t s = 0; s < kSlots; s++) {
        if (c->hud_buffer[s]) vkDestroyBuffer(c->device, c->hud_buffer[s], nullptr);
        if (c->hud_memory[s]) vkFreeMemory(c->device, c->hud_memory[s], nullptr);  /* unmaps it */
        c->hud_buffer[s] = VK_NULL_HANDLE;
        c->hud_memory[s] = VK_NULL_HANDLE;
        c->hud_pixels[s] = nullptr;
    }
}

void free_image(VkDevice device, Image *image)
{
    if (image->view) vkDestroyImageView(device, image->view, nullptr);
    if (image->image) vkDestroyImage(device, image->image, nullptr);
    if (image->memory) vkFreeMemory(device, image->memory, nullptr);
    *image = Image{};
}

void free_chain(Chain *c)
{
    if (!c) return;
    if (device_loaded) {
        vkDeviceWaitIdle(c->device);
        if (c->fg) fg_context_destroy(c->fg);
        free_image(c->device, &c->history[0]);
        free_image(c->device, &c->history[1]);
        for (auto &image : c->output) free_image(c->device, &image);
        free_hud(c);
        for (uint32_t s = 0; s < kSlots; s++) {
            if (c->fence[s]) vkDestroyFence(c->device, c->fence[s], nullptr);
            if (c->real_done[s]) vkDestroySemaphore(c->device, c->real_done[s], nullptr);
            for (uint32_t k = 0; k < kMaxFactor - 1; k++) {
                if (c->acquired[s][k]) vkDestroySemaphore(c->device, c->acquired[s][k], nullptr);
                if (c->generated[s][k]) vkDestroySemaphore(c->device, c->generated[s][k], nullptr);
            }
        }
        for (VkSemaphore semaphore : c->image_done)
            if (semaphore) vkDestroySemaphore(c->device, semaphore, nullptr);
        if (c->pool) vkDestroyCommandPool(c->device, c->pool, nullptr);
    }
    delete c;
}

/* Counting without generating: the images, the overlay and synchronisation.
 * A swapchain with more images than the table is left as it was. */
bool setup_counter_chain(Chain *c, VkPhysicalDevice physical)
{
    if (!make_hud(c, physical)) {
        fprintf(stderr, "pw_framegen: no overlay image: no overlay\n");
        return false;
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t s = 0; s < kSlots; s++)
        if (vkCreateFence(c->device, &fence, nullptr, &c->fence[s]) != VK_SUCCESS) return false;
    for (uint32_t i = 0; i < c->image_count; i++)
        if (vkCreateSemaphore(c->device, &semaphore, nullptr, &c->image_done[i]) != VK_SUCCESS) return false;
    fprintf(stderr, "pw_framegen: swapchain %ux%u, %u images, counter only\n", c->extent.width, c->extent.height,
            c->image_count);
    return true;
}

/* The generator's images, network and synchronisation for one swapchain. */
bool setup_chain(Chain *c)
{
    VkPhysicalDevice physical = physical_of(c->device);
    if (!physical || !load_device(c->device)) return false;
    uint32_t count = kMaxImages;
    VkResult images = vkGetSwapchainImagesKHR(c->device, c->handle, &count, c->images);
    if (images != VK_SUCCESS && (images != VK_INCOMPLETE || !c->factor)) return false;
    c->image_count = count;
    if (!c->factor) return setup_counter_chain(c, physical);
    fg_create_info info{};
    info.device = c->device;
    info.physical_device = physical;
    info.width = c->extent.width;
    info.height = c->extent.height;
    info.slots = kSlots * (c->factor - 1);
    info.output_format = VK_FORMAT_R8G8B8A8_UNORM;
    char error[256];
    if (fg_context_create(&info, &c->fg, error, sizeof(error)) != VK_SUCCESS) {
        fprintf(stderr, "pw_framegen: no generator: %s\n", error);
        return false;
    }
    if (fg_context_load_network(c->fg, pw_framegen_network, (size_t)(pw_framegen_network_end - pw_framegen_network),
                                error, sizeof(error)) == VK_SUCCESS)
        c->method = FG_METHOD_NEURAL;
    else
        fprintf(stderr, "pw_framegen: no network (%s): blending instead\n", error);
    if (!make_image(c, physical, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->history[0]) ||
        !make_image(c, physical, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &c->history[1]))
        return false;
    for (uint32_t k = 0; k < c->factor - 1; k++)
        if (!make_image(c, physical, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &c->output[k]))
            return false;
    if (hud() && !make_hud(c, physical)) {
        fprintf(stderr, "pw_framegen: no overlay image: counting in the log only\n");
        free_hud(c);
    }
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t s = 0; s < kSlots; s++) {
        if (vkCreateFence(c->device, &fence, nullptr, &c->fence[s]) != VK_SUCCESS ||
            vkCreateSemaphore(c->device, &semaphore, nullptr, &c->real_done[s]) != VK_SUCCESS)
            return false;
        for (uint32_t k = 0; k < c->factor - 1; k++)
            if (vkCreateSemaphore(c->device, &semaphore, nullptr, &c->acquired[s][k]) != VK_SUCCESS ||
                vkCreateSemaphore(c->device, &semaphore, nullptr, &c->generated[s][k]) != VK_SUCCESS)
                return false;
    }
    fprintf(stderr, "pw_framegen: swapchain %ux%u, %u images, factor %u, %s%s\n", c->extent.width,
            c->extent.height, c->image_count, c->factor, c->method == FG_METHOD_NEURAL ? "neural" : "blend",
            c->hud_pixels[0] ? ", overlay on screen" : "");
    return true;
}

/* The command pool and buffers, on the presenting queue's family. */
bool setup_commands(Chain *c, uint32_t family)
{
    if (c->pool && c->family == family) return true;
    if (c->pool) {
        vkDeviceWaitIdle(c->device);
        vkDestroyCommandPool(c->device, c->pool, nullptr);
        c->pool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = family;
    if (vkCreateCommandPool(c->device, &pool, nullptr, &c->pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = c->pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kSlots;
    c->family = family;
    return vkAllocateCommandBuffers(c->device, &alloc, c->cmd) == VK_SUCCESS;
}

void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
             VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void blit(VkCommandBuffer cmd, VkImage from, VkImage to, VkExtent2D extent)
{
    VkImageBlit region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = region.dstOffsets[1] = {(int32_t)extent.width, (int32_t)extent.height, 1};
    vkCmdBlitImage(cmd, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &region, VK_FILTER_NEAREST);
}

/* The overlay, drawn into slot s's buffer and copied into its image, left
 * ready to blit (TRANSFER_SRC): its size in *width and *height, or false
 * when there is nothing to show yet. */
bool record_hud(Chain *c, VkCommandBuffer cmd, uint32_t s, uint32_t *width, uint32_t *height)
{
    if (!c->hud.image || !c->hud_pixels[s]) return false;
    pw_hud_draw(&c->stats, hud(), c->factor != 0, c->hud_pixels[s], PW_HUD_MAX_W, width, height);
    if (!*width || !*height) return false;
    /* What the previous present blitted from it is overwritten: its read comes first. */
    barrier(cmd, c->hud.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.bufferRowLength = PW_HUD_MAX_W;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {*width, *height, 1};
    vkCmdCopyBufferToImage(cmd, c->hud_buffer[s], c->hud.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cmd, c->hud.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    return true;
}

/* The overlay recorded by record_hud onto an image in TRANSFER_DST layout,
 * in its top left, scaled with the picture (2x at 1080p). */
void blit_hud(Chain *c, VkCommandBuffer cmd, VkImage target, uint32_t width, uint32_t height)
{
    uint32_t scale = pw_hud_scale(c->extent.height), margin = 4 * scale;
    if (margin + width * scale > c->extent.width || margin + height * scale > c->extent.height) return;
    VkImageBlit r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffsets[1] = {(int32_t)width, (int32_t)height, 1};
    r.dstOffsets[0] = {(int32_t)margin, (int32_t)margin, 0};
    r.dstOffsets[1] = {(int32_t)(margin + width * scale), (int32_t)(margin + height * scale), 1};
    vkCmdBlitImage(cmd, c->hud.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
}

/* What the full overlay shows besides frames, once a second: the size the
 * program draws at and VideoOut's framebuffer, the process's CPU time and
 * the direct memory the title last gave. */
void sample_full(Chain *c, uint64_t now)
{
    c->stats.render_w = c->extent.width;
    c->stats.render_h = c->extent.height;
    VkRect2D rect;
    VkExtent2D frame;
    if (wsi_videoout_present_rect(&rect, &frame) && frame.width && frame.height) {
        c->stats.output_w = frame.width;
        c->stats.output_h = frame.height;
    }
    if (cpus_online < 0) cpus_online = sysconf(_SC_NPROCESSORS_ONLN);
    c->stats.cpus = cpus_online > 0 ? (uint32_t)cpus_online : 0;
    struct rusage usage;
    if (!getrusage(RUSAGE_SELF, &usage))
        pw_hud_cpu(&c->stats,
                   (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000u +
                       (uint64_t)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec),
                   now);
    if (uint64_t mib = __atomic_load_n(&memory_mib, __ATOMIC_RELAXED)) {
        c->stats.memory_bytes = (mib >> 32) << 20;
        c->stats.memory_peak_bytes = (mib & 0xFFFFFFFFu) << 20;
        c->stats.memory_known = 1;
    }
}

/* Count a program frame and its pictures; once a second, the rates. */
void count_frame(Chain *c, uint32_t generated)
{
    uint64_t now = now_ns();
    if (!pw_hud_frame(&c->stats, now, 1 + generated)) return;
    uint32_t real = (c->stats.fps10 + 5) / 10, shown = (c->stats.shown10 + 5) / 10;
    if (c->factor)
        fprintf(stderr, "pw_framegen: %u frames/s from the program, %u pictures/s presented (%u generated each)\n",
                real, shown, generated);
    else
        fprintf(stderr, "pw_framegen: %u frames/s from the program\n", real);
    if (hud() == PW_HUD_FULL) sample_full(c, now);
}

Chain *find_chain(VkSwapchainKHR handle)
{
    for (auto *c : chains)
        if (c && c->handle == handle) return c;
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL fg_CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *info,
                                               const VkAllocationCallbacks *allocator, VkDevice *device)
{
    VkResult result = real_CreateDevice(physical, info, allocator, device);
    if (result == VK_SUCCESS) {
        pthread_mutex_lock(&lock);
        for (auto &d : devices)
            if (!d.device) { d.device = *device; d.physical = physical; break; }
        pthread_mutex_unlock(&lock);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL fg_GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue *queue)
{
    real_GetDeviceQueue(device, family, index, queue);
    pthread_mutex_lock(&lock);
    remember_queue(*queue, family);
    pthread_mutex_unlock(&lock);
}

VKAPI_ATTR void VKAPI_CALL fg_GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2 *info, VkQueue *queue)
{
    real_GetDeviceQueue2(device, info, queue);
    pthread_mutex_lock(&lock);
    remember_queue(*queue, info->queueFamilyIndex);
    pthread_mutex_unlock(&lock);
}

VKAPI_ATTR VkResult VKAPI_CALL fg_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                     const VkAllocationCallbacks *allocator, VkSwapchainKHR *handle)
{
    VkSwapchainCreateInfoKHR ours = *info;
    ours.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (factor()) {
        ours.minImageCount += factor() - 1;
        ours.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        /* FIFO paces the pictures; mailbox or immediate would drop or tear them. */
        ours.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    VkResult result = real_CreateSwapchainKHR(device, &ours, allocator, handle);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "pw_framegen: swapchain refused with the changes (%d): left as it was\n", (int)result);
        return real_CreateSwapchainKHR(device, info, allocator, handle);
    }
    Chain *c = new (std::nothrow) Chain;
    if (!c) return result;
    c->device = device;
    c->handle = *handle;
    c->extent = info->imageExtent;
    c->factor = factor();
    pthread_mutex_lock(&lock);
    bool ok = setup_chain(c);
    if (ok) {
        ok = false;
        for (auto *&slot : chains)
            if (!slot) { slot = c; ok = true; break; }
    }
    pthread_mutex_unlock(&lock);
    if (!ok) {
        fprintf(stderr, "pw_framegen: swapchain %ux%u left as it was\n", c->extent.width, c->extent.height);
        free_chain(c);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL fg_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR handle,
                                                  const VkAllocationCallbacks *allocator)
{
    pthread_mutex_lock(&lock);
    for (auto *&slot : chains)
        if (slot && slot->handle == handle) { free_chain(slot); slot = nullptr; }
    pthread_mutex_unlock(&lock);
    real_DestroySwapchainKHR(device, handle, allocator);
}

/* Stop serving a chain that can no longer draw: its presents go straight to the driver. */
void drop_chain(Chain *c)
{
    for (auto *&slot : chains)
        if (slot == c) slot = nullptr;
    free_chain(c);
}

/* Counting without generating: draw the counter on the program's frame and
 * present it, after what the program asked to wait for. Called with the lock,
 * and lets it go. */
VkResult present_counted(Chain *c, VkQueue queue, const VkPresentInfoKHR *info)
{
    /* More than the table takes: this frame goes without a counter. */
    if (info->waitSemaphoreCount > kMaxPresentWaits) {
        pthread_mutex_unlock(&lock);
        return real_QueuePresentKHR(queue, info);
    }
    uint32_t s = c->slot;
    c->slot = (c->slot + 1) % kSlots;
    /* The slot's buffer is drawn into below: only once the GPU is done with it. */
    if (vkWaitForFences(c->device, 1, &c->fence[s], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        pthread_mutex_unlock(&lock);
        return real_QueuePresentKHR(queue, info);
    }

    VkCommandBuffer cmd = c->cmd[s];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    uint32_t hud_w = 0, hud_h = 0;
    const bool drawn = record_hud(c, cmd, s, &hud_w, &hud_h);
    VkImage frame = c->images[info->pImageIndices[0]];
    barrier(cmd, frame, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (drawn) blit_hud(c, cmd, frame, hud_w, hud_h);
    barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags stages[kMaxPresentWaits];
    for (uint32_t i = 0; i < info->waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = info->waitSemaphoreCount;
    submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &c->image_done[info->pImageIndices[0]];
    /* The fence stays signalled unless this is sure to reach the queue. */
    vkResetFences(c->device, 1, &c->fence[s]);
    VkResult result = vkQueueSubmit(queue, 1, &submit, c->fence[s]);
    if (result != VK_SUCCESS) {
        /* A failed submit signals nothing: this slot needs a fence that a later frame can wait for. */
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkDestroyFence(c->device, c->fence[s], nullptr);
        c->fence[s] = VK_NULL_HANDLE;
        if (vkCreateFence(c->device, &fence, nullptr, &c->fence[s]) != VK_SUCCESS) drop_chain(c);
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "pw_framegen: submit failed (%d)\n", (int)result);
        return result;
    }
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &c->image_done[info->pImageIndices[0]];
    count_frame(c, 0);
    pthread_mutex_unlock(&lock);
    return real_QueuePresentKHR(queue, &real);
}

/* One submission for the program's frame: copy it aside, then make and draw
 * each picture between it and the previous one into an acquired image. The
 * pictures are presented first, then the program's frame. */
VKAPI_ATTR VkResult VKAPI_CALL fg_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info)
{
    if (info->swapchainCount != 1) return real_QueuePresentKHR(queue, info);
    pthread_mutex_lock(&lock);
    Chain *c = find_chain(info->pSwapchains[0]);
    if (!c || !setup_commands(c, family_of(queue))) {
        pthread_mutex_unlock(&lock);
        return real_QueuePresentKHR(queue, info);
    }
    if (!c->factor) return present_counted(c, queue, info);
    uint32_t s = c->slot;
    c->slot = (c->slot + 1) % kSlots;
    /* The slot's buffers are reused below: only once the GPU is done with them. */
    if (vkWaitForFences(c->device, 1, &c->fence[s], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        pthread_mutex_unlock(&lock);
        return real_QueuePresentKHR(queue, info);
    }
    vkResetFences(c->device, 1, &c->fence[s]);

    uint32_t generate = c->have_prev ? c->factor - 1 : 0, targets[kMaxFactor - 1];
    for (uint32_t k = 0; k < generate; k++) {
        VkResult acquired = vkAcquireNextImageKHR(c->device, c->handle, UINT64_MAX, c->acquired[s][k],
                                                  VK_NULL_HANDLE, &targets[k]);
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) { generate = k; break; }
    }

    VkCommandBuffer cmd = c->cmd[s];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    uint32_t hud_w = 0, hud_h = 0;
    const bool counter = record_hud(c, cmd, s, &hud_w, &hud_h);
    VkImage frame = c->images[info->pImageIndices[0]];
    Image &now = c->history[c->cur], &before = c->history[c->cur ^ 1];
    barrier(cmd, frame, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cmd, now.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit(cmd, frame, now.image, c->extent);
    if (counter) {
        /* the counter onto the program's frame, after its copy aside */
        barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit_hud(c, cmd, frame, hud_w, hud_h);
        barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    } else {
        barrier(cmd, frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    barrier(cmd, now.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    for (uint32_t k = 0; k < generate; k++) {
        Image &out = c->output[k];
        /* after the previous frame's copy out of it */
        barrier(cmd, out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        fg_frame_pair pair{};
        pair.prev_color = before.view;
        pair.next_color = now.view;
        pair.t = (float)(k + 1) / (float)c->factor;
        fg_record_generate(c->fg, cmd, s * (c->factor - 1) + k, c->method, &pair, out.view);
        barrier(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImage target = c->images[targets[k]];
        barrier(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit(cmd, out.image, target, c->extent);
        if (counter) {
            barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            blit_hud(c, cmd, target, hud_w, hud_h);
        }
        barrier(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(cmd);

    VkSemaphore waits[16 + kMaxFactor], signals[kMaxFactor];
    VkPipelineStageFlags stages[16 + kMaxFactor];
    uint32_t wait_count = 0, signal_count = 0;
    for (uint32_t i = 0; i < info->waitSemaphoreCount && wait_count < 16; i++, wait_count++) {
        waits[wait_count] = info->pWaitSemaphores[i];
        stages[wait_count] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    for (uint32_t k = 0; k < generate; k++, wait_count++) {
        waits[wait_count] = c->acquired[s][k];
        stages[wait_count] = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    for (uint32_t k = 0; k < generate; k++) signals[signal_count++] = c->generated[s][k];
    signals[signal_count++] = c->real_done[s];
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = waits;
    submit.pWaitDstStageMask = stages;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = signal_count;
    submit.pSignalSemaphores = signals;
    VkResult result = vkQueueSubmit(queue, 1, &submit, c->fence[s]);
    if (result != VK_SUCCESS) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr, "pw_framegen: submit failed (%d)\n", (int)result);
        return result;
    }
    uint32_t presented = 0;
    for (uint32_t k = 0; k < generate; k++) {
        VkPresentInfoKHR picture{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        picture.waitSemaphoreCount = 1;
        picture.pWaitSemaphores = &c->generated[s][k];
        picture.swapchainCount = 1;
        picture.pSwapchains = &c->handle;
        picture.pImageIndices = &targets[k];
        VkResult shown = real_QueuePresentKHR(queue, &picture);
        presented += shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR;
    }
    VkPresentInfoKHR real = *info;
    real.waitSemaphoreCount = 1;
    real.pWaitSemaphores = &c->real_done[s];
    c->cur ^= 1;
    c->have_prev = true;
    count_frame(c, presented);
    pthread_mutex_unlock(&lock);
    return real_QueuePresentKHR(queue, &real);
}

/* The wrapper for name, remembering the driver's own; NULL when there is none. */
PFN_vkVoidFunction wrap(const char *name, PFN_vkVoidFunction real)
{
#define PW_FRAMEGEN_WRAP(fn) \
    if (!strcmp(name, "vk" #fn)) { \
        if (!real_##fn) real_##fn = (PFN_vk##fn)real; \
        return (PFN_vkVoidFunction)fg_##fn; \
    }
    PW_FRAMEGEN_WRAP(CreateDevice)
    PW_FRAMEGEN_WRAP(GetDeviceQueue)
    PW_FRAMEGEN_WRAP(GetDeviceQueue2)
    PW_FRAMEGEN_WRAP(CreateSwapchainKHR)
    PW_FRAMEGEN_WRAP(DestroySwapchainKHR)
    PW_FRAMEGEN_WRAP(QueuePresentKHR)
#undef PW_FRAMEGEN_WRAP
    return nullptr;
}

}  // namespace

/* The title's direct memory for the full overlay's memory line, used and at
 * most, once a second (native/wine64_main.c, hud_memory_tick). */
extern "C" void pw_hud_memory(uint64_t used, uint64_t peak)
{
    uint64_t used_mib = used >> 20, peak_mib = peak >> 20;
    if (used_mib > 0xFFFFFFFFu) used_mib = 0xFFFFFFFFu;
    if (peak_mib > 0xFFFFFFFFu) peak_mib = 0xFFFFFFFFu;
    /* never 0, so "none yet" stays apart from an empty pool */
    __atomic_store_n(&memory_mib, (used_mib << 32 | peak_mib) | (used_mib || peak_mib ? 0 : 1), __ATOMIC_RELAXED);
}

extern "C" PFN_vkVoidFunction pw_framegen_resolve(VkInstance instance, const char *name, PFN_vkVoidFunction real)
{
    if (!name || !real || (!factor() && !counter_only())) return real;
    if (instance) instance_seen = instance;
    PFN_vkVoidFunction ours = wrap(name, real);
    return ours ? ours : real;
}
