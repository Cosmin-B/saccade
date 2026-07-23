#include "platform/windows/runtime_scheduling.hpp"

#include <avrt.h>

namespace saccade::platform::windows {
namespace {

constexpr wchar_t runtime_task[] = L"Games";

bool priority_class_sufficient(DWORD priority_class) noexcept {
    return priority_class == ABOVE_NORMAL_PRIORITY_CLASS || priority_class == HIGH_PRIORITY_CLASS ||
           priority_class == REALTIME_PRIORITY_CLASS;
}

} // namespace

RuntimeScheduling::~RuntimeScheduling() {
    if (initialized() && owner_thread_id_ == GetCurrentThreadId()) (void)shutdown();
}

SaccadeResult RuntimeScheduling::initialize() noexcept {
    if (initialized()) return SACCADE_ERROR_ALREADY_EXISTS;

    const HANDLE process = GetCurrentProcess();
    const DWORD previous_priority_class = GetPriorityClass(process);
    if (previous_priority_class == 0) return SACCADE_ERROR_BACKEND;
    const bool process_priority_elevated = !priority_class_sufficient(previous_priority_class);
    if (process_priority_elevated && SetPriorityClass(process, ABOVE_NORMAL_PRIORITY_CLASS) == 0)
        return SACCADE_ERROR_BACKEND;

    DWORD task_index = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(runtime_task, &task_index);
    if (task == nullptr) {
        if (process_priority_elevated) (void)SetPriorityClass(process, previous_priority_class);
        return SACCADE_ERROR_BACKEND;
    }
    if (AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH) == 0) {
        (void)AvRevertMmThreadCharacteristics(task);
        if (process_priority_elevated) (void)SetPriorityClass(process, previous_priority_class);
        return SACCADE_ERROR_BACKEND;
    }

    task_ = task;
    previous_priority_class_ = previous_priority_class;
    owner_thread_id_ = GetCurrentThreadId();
    process_priority_elevated_ = process_priority_elevated;
    return SACCADE_OK;
}

SaccadeResult RuntimeScheduling::shutdown() noexcept {
    if (!initialized() || owner_thread_id_ != GetCurrentThreadId()) return SACCADE_ERROR_STATE;
    bool reverted = true;
    if (task_ != nullptr) {
        if (AvRevertMmThreadCharacteristics(task_) != 0) {
            task_ = nullptr;
        } else {
            reverted = false;
        }
    }
    if (process_priority_elevated_) {
        if (SetPriorityClass(GetCurrentProcess(), previous_priority_class_) != 0) {
            process_priority_elevated_ = false;
            previous_priority_class_ = 0;
        } else {
            reverted = false;
        }
    }
    if (!initialized()) owner_thread_id_ = 0;
    return reverted ? SACCADE_OK : SACCADE_ERROR_BACKEND;
}

} // namespace saccade::platform::windows
