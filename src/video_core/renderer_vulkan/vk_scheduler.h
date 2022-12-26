// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <utility>
#include <queue>
#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"

namespace Vulkan {

enum class StateFlags {
    AllDirty = 0,
    Renderpass = 1 << 0,
    Pipeline = 1 << 1,
    DescriptorSets = 1 << 2
};

DECLARE_ENUM_FLAG_OPERATORS(StateFlags)

class Instance;
class RenderpassCache;
class RendererVulkan;

/// The scheduler abstracts command buffer and fence management with an interface that's able to do
/// OpenGL-like operations on Vulkan command buffers.
class Scheduler {
public:
    explicit Scheduler(const Instance& instance, RenderpassCache& renderpass_cache,
                       RendererVulkan& renderer);
    ~Scheduler();

    /// Sends the current execution context to the GPU.
    void Flush(vk::Semaphore signal = nullptr, vk::Semaphore wait = nullptr);

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(vk::Semaphore signal = nullptr, vk::Semaphore wait = nullptr);

    /// Waits for the worker thread to finish executing everything. After this function returns it's
    /// safe to touch worker resources.
    void WaitWorker();

    /// Sends currently recorded work to the worker thread.
    void DispatchWork();

    /// Records the command to the current chunk.
    template <typename T>
    void Record(T&& command) {
        if (!use_worker_thread) {
            command(render_cmdbuf, upload_cmdbuf);
            return;
        }

        if (chunk->Record(command)) {
            return;
        }
        DispatchWork();
        (void)chunk->Record(command);
    }

    /// Marks the provided state as non dirty
    void MarkStateNonDirty(StateFlags flag) noexcept {
        state |= flag;
    }

    /// Returns true if the state is dirty
    [[nodiscard]] bool IsStateDirty(StateFlags flag) const noexcept {
        return False(state & flag);
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore.IsFree(tick);
    }

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick) {
        if (tick >= master_semaphore.CurrentTick()) {
            // Make sure we are not waiting for the current tick without signalling
            Flush();
        }
        master_semaphore.Wait(tick);
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept {
        return master_semaphore;
    }

private:
    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer render_cmdbuf,
                             vk::CommandBuffer upload_cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer render_cmdbuf,
                     vk::CommandBuffer upload_cmdbuf) const override {
            command(render_cmdbuf, upload_cmdbuf);
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        void ExecuteAll(vk::CommandBuffer render_cmdbuf, vk::CommandBuffer upload_cmdbuf);

        template <typename T>
        bool Record(T& command) {
            using FuncType = TypedCommand<T>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            recorded_counts++;
            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return false;
            }
            Command* const current_last = last;
            last = new (data.data() + command_offset) FuncType(std::move(command));

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return true;
        }

        void MarkSubmit() {
            submit = true;
        }

        bool Empty() const {
            return recorded_counts == 0;
        }

        bool HasSubmit() const {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        std::size_t recorded_counts = 0;
        std::size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x80000> data{};
    };

private:
    void WorkerThread(std::stop_token stop_token);

    void AllocateWorkerCommandBuffers();

    void SubmitExecution(vk::Semaphore signal_semaphore, vk::Semaphore wait_semaphore);

    void AcquireNewChunk();

private:
    const Instance& instance;
    RenderpassCache& renderpass_cache;
    RendererVulkan& renderer;
    MasterSemaphore master_semaphore;
    CommandPool command_pool;
    std::unique_ptr<CommandChunk> chunk;
    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    vk::CommandBuffer render_cmdbuf;
    vk::CommandBuffer upload_cmdbuf;
    StateFlags state{};
    std::mutex reserve_mutex;
    std::mutex work_mutex;
    std::condition_variable_any work_cv;
    std::condition_variable wait_cv;
    std::jthread worker_thread;
    bool use_worker_thread;
};

} // namespace Vulkan
