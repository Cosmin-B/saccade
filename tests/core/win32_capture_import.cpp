#include <saccade/saccade.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>

#include <cstdint>

namespace {

enum class TestResult : int {
    success,
    create_failed,
    import_failed,
    texture_retain_mismatch,
    fence_retain_mismatch,
    release_failed,
    texture_release_mismatch,
    fence_release_mismatch,
    second_import_failed,
    second_texture_retain_mismatch,
    second_fence_retain_mismatch,
    destroy_failed,
    destroy_texture_release_mismatch,
    destroy_fence_release_mismatch
};

int result(TestResult value) noexcept {
    return static_cast<int>(value);
}

class RefProbe final : public IUnknown {
  public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** output) override {
        if (output == nullptr) return E_POINTER;
        *output = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        ++add_ref_calls_;
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ++release_calls_;
        return --references_;
    }

    [[nodiscard]] ULONG references() const noexcept { return references_; }

    [[nodiscard]] uint32_t add_ref_calls() const noexcept { return add_ref_calls_; }

    [[nodiscard]] uint32_t release_calls() const noexcept { return release_calls_; }

  private:
    ULONG references_ = 1;
    uint32_t add_ref_calls_ = 0;
    uint32_t release_calls_ = 0;
};

SaccadeWin32CaptureFrameDesc frame_desc(RefProbe* texture, RefProbe* ready_fence, uint64_t frame_id) noexcept {
    SaccadeWin32CaptureFrameDesc value{};
    value.struct_size = sizeof(value);
    value.api_version = SACCADE_API_VERSION;
    value.texture = texture;
    value.pixel_format = 1;
    value.width = 1920;
    value.height = 1080;
    value.frame_id = frame_id;
    value.transform_epoch = 7;
    value.ready_fence = ready_fence;
    value.ready_value = frame_id;
    return value;
}

} // namespace

int main() {
    SaccadeRuntimeDesc runtime_desc{};
    runtime_desc.struct_size = sizeof(runtime_desc);
    runtime_desc.api_version = SACCADE_API_VERSION;
    SaccadeRuntimeHandle runtime = 0;
    if (saccade_runtime_create(&runtime_desc, &runtime) != SACCADE_OK) {
        return result(TestResult::create_failed);
    }

    RefProbe first_texture;
    RefProbe first_fence;
    const SaccadeWin32CaptureFrameDesc first_desc = frame_desc(&first_texture, &first_fence, 1);
    SaccadeFrameHandle frame = 0;
    if (saccade_frame_import_win32_capture(runtime, &first_desc, &frame) != SACCADE_OK || frame == 0) {
        return result(TestResult::import_failed);
    }
    if (first_texture.references() != 2 || first_texture.add_ref_calls() != 1 || first_texture.release_calls() != 0) {
        return result(TestResult::texture_retain_mismatch);
    }
    if (first_fence.references() != 2 || first_fence.add_ref_calls() != 1 || first_fence.release_calls() != 0) {
        return result(TestResult::fence_retain_mismatch);
    }
    if (saccade_frame_release(runtime, frame) != SACCADE_OK) {
        return result(TestResult::release_failed);
    }
    if (first_texture.references() != 1 || first_texture.release_calls() != 1) {
        return result(TestResult::texture_release_mismatch);
    }
    if (first_fence.references() != 1 || first_fence.release_calls() != 1) {
        return result(TestResult::fence_release_mismatch);
    }

    RefProbe second_texture;
    RefProbe second_fence;
    const SaccadeWin32CaptureFrameDesc second_desc = frame_desc(&second_texture, &second_fence, 2);
    frame = 0;
    if (saccade_frame_import_win32_capture(runtime, &second_desc, &frame) != SACCADE_OK || frame == 0) {
        return result(TestResult::second_import_failed);
    }
    if (second_texture.references() != 2 || second_texture.add_ref_calls() != 1) {
        return result(TestResult::second_texture_retain_mismatch);
    }
    if (second_fence.references() != 2 || second_fence.add_ref_calls() != 1) {
        return result(TestResult::second_fence_retain_mismatch);
    }
    if (saccade_runtime_destroy(runtime) != SACCADE_OK) {
        return result(TestResult::destroy_failed);
    }
    if (second_texture.references() != 1 || second_texture.release_calls() != 1) {
        return result(TestResult::destroy_texture_release_mismatch);
    }
    if (second_fence.references() != 1 || second_fence.release_calls() != 1) {
        return result(TestResult::destroy_fence_release_mismatch);
    }
    return result(TestResult::success);
}
