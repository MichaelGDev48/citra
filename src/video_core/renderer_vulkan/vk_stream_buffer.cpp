// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/microprofile.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_stream_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

[[nodiscard]] vk::AccessFlags MakeAccessFlags(vk::BufferUsageFlagBits usage) {
    switch (usage) {
    case vk::BufferUsageFlagBits::eVertexBuffer:
        return vk::AccessFlagBits::eVertexAttributeRead;
    case vk::BufferUsageFlagBits::eIndexBuffer:
        return vk::AccessFlagBits::eIndexRead;
    case vk::BufferUsageFlagBits::eUniformBuffer:
        return vk::AccessFlagBits::eUniformRead;
    case vk::BufferUsageFlagBits::eUniformTexelBuffer:
        return vk::AccessFlagBits::eShaderRead;
    default:
        LOG_CRITICAL(Render_Vulkan, "Unknown usage flag {}", usage);
        UNREACHABLE();
    }
    return vk::AccessFlagBits::eNone;
}

[[nodiscard]] vk::PipelineStageFlags MakePipelineStage(vk::BufferUsageFlagBits usage) {
    switch (usage) {
    case vk::BufferUsageFlagBits::eVertexBuffer:
        return vk::PipelineStageFlagBits::eVertexInput;
    case vk::BufferUsageFlagBits::eIndexBuffer:
        return vk::PipelineStageFlagBits::eVertexInput;
    case vk::BufferUsageFlagBits::eUniformBuffer:
        return vk::PipelineStageFlagBits::eVertexShader |
               vk::PipelineStageFlagBits::eGeometryShader |
               vk::PipelineStageFlagBits::eFragmentShader;
    case vk::BufferUsageFlagBits::eUniformTexelBuffer:
        return vk::PipelineStageFlagBits::eFragmentShader;
    default:
        LOG_CRITICAL(Render_Vulkan, "Unknown usage flag {}", usage);
        UNREACHABLE();
    }
    return vk::PipelineStageFlagBits::eNone;
}

StagingBuffer::StagingBuffer(const Instance& instance, u32 size, bool readback)
    : instance{instance} {
    const vk::BufferUsageFlags usage =
        readback ? vk::BufferUsageFlagBits::eTransferDst : vk::BufferUsageFlagBits::eTransferSrc;
    const vk::BufferCreateInfo buffer_info = {.size = size, .usage = usage};

    const VmaAllocationCreateFlags flags =
        readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
                 : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    const VmaAllocationCreateInfo alloc_create_info = {.flags =
                                                           flags | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                                       .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};

    VkBuffer unsafe_buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(buffer_info);
    VmaAllocationInfo alloc_info;
    VmaAllocator allocator = instance.GetAllocator();

    vmaCreateBuffer(allocator, &unsafe_buffer_info, &alloc_create_info, &unsafe_buffer, &allocation,
                    &alloc_info);

    buffer = vk::Buffer{unsafe_buffer};
    mapped = std::span{reinterpret_cast<std::byte*>(alloc_info.pMappedData), size};
}

StagingBuffer::~StagingBuffer() {
    vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(buffer), allocation);
}

StreamBuffer::StreamBuffer(const Instance& instance, Scheduler& scheduler, u32 size,
                           bool readback)
    : instance{instance}, scheduler{scheduler}, staging{instance, size, readback},
      total_size{size}, bucket_size{size / BUCKET_COUNT}, readback{readback} {}

StreamBuffer::StreamBuffer(const Instance& instance, Scheduler& scheduler, u32 size,
                           vk::BufferUsageFlagBits usage, std::span<const vk::Format> view_formats,
                           bool readback)
    : instance{instance}, scheduler{scheduler}, staging{instance, size, readback},
      usage{usage}, total_size{size}, bucket_size{size / BUCKET_COUNT}, readback{readback} {
    const vk::BufferCreateInfo buffer_info = {
        .size = total_size, .usage = usage | vk::BufferUsageFlagBits::eTransferDst};

    const VmaAllocationCreateInfo alloc_create_info = {.usage =
                                                           VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

    VkBuffer unsafe_buffer = VK_NULL_HANDLE;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(buffer_info);
    VmaAllocationInfo alloc_info;
    VmaAllocator allocator = instance.GetAllocator();

    vmaCreateBuffer(allocator, &unsafe_buffer_info, &alloc_create_info, &unsafe_buffer, &allocation,
                    &alloc_info);

    gpu_buffer = vk::Buffer{unsafe_buffer};

    ASSERT(view_formats.size() < MAX_BUFFER_VIEWS);

    vk::Device device = instance.GetDevice();
    for (std::size_t i = 0; i < view_formats.size(); i++) {
        const vk::BufferViewCreateInfo view_info = {
            .buffer = gpu_buffer, .format = view_formats[i], .offset = 0, .range = total_size};

        views[i] = device.createBufferView(view_info);
    }

    view_count = view_formats.size();
}

StreamBuffer::~StreamBuffer() {
    if (gpu_buffer) {
        vk::Device device = instance.GetDevice();
        vmaDestroyBuffer(instance.GetAllocator(), static_cast<VkBuffer>(gpu_buffer), allocation);
        for (std::size_t i = 0; i < view_count; i++) {
            device.destroyBufferView(views[i]);
        }
    }
}

std::tuple<u8*, u32, bool> StreamBuffer::Map(u32 size, u32 alignment) {
    ASSERT(size <= total_size && alignment <= total_size);

    if (alignment > 0) {
        buffer_offset = Common::AlignUp(buffer_offset, alignment);
    }

    bool invalidate = false;
    const u32 new_offset = buffer_offset + size;
    if (u32 new_index = new_offset / bucket_size; new_index != bucket_index) {
        if (new_index >= BUCKET_COUNT) {
            if (readback) {
                Invalidate();
            } else {
                Flush();
            }
            buffer_offset = 0;
            flush_offset = 0;
            new_index = 0;
            invalidate = true;
        }
        ticks[bucket_index] = scheduler.CurrentTick();
        scheduler.Wait(ticks[new_index]);
        bucket_index = new_index;
    }

    u8* mapped = reinterpret_cast<u8*>(staging.mapped.data() + buffer_offset);
    return std::make_tuple(mapped, buffer_offset, invalidate);
}

void StreamBuffer::Commit(u32 size) {
    buffer_offset += size;
}

void StreamBuffer::Flush() {
    if (readback) {
        return;
    }

    const u32 flush_size = buffer_offset - flush_offset;
    ASSERT(flush_size <= total_size);
    ASSERT(flush_offset + flush_size <= total_size);

    if (flush_size > 0) [[likely]] {
        VmaAllocator allocator = instance.GetAllocator();
        vmaFlushAllocation(allocator, staging.allocation, flush_offset, flush_size);
        if (gpu_buffer) {
            scheduler.Record([this, flush_offset = flush_offset, flush_size](vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
                const vk::BufferCopy copy_region = {
                    .srcOffset = flush_offset, .dstOffset = flush_offset, .size = flush_size};

                upload_cmdbuf.copyBuffer(staging.buffer, gpu_buffer, copy_region);

                const vk::BufferMemoryBarrier buffer_barrier = {
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = MakeAccessFlags(usage),
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = gpu_buffer,
                    .offset = flush_offset,
                    .size = flush_size};

                upload_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                              MakePipelineStage(usage),
                                              vk::DependencyFlagBits::eByRegion, {}, buffer_barrier,
                                              {});
            });
        }
        flush_offset = buffer_offset;
    }
}

void StreamBuffer::Invalidate() {
    if (!readback) {
        return;
    }

    const u32 flush_size = buffer_offset - flush_offset;
    ASSERT(flush_size <= total_size);
    ASSERT(flush_offset + flush_size <= total_size);

    if (flush_size > 0) [[likely]] {
        VmaAllocator allocator = instance.GetAllocator();
        vmaInvalidateAllocation(allocator, staging.allocation, flush_offset, flush_size);
        flush_offset = buffer_offset;
    }
}

} // namespace Vulkan
