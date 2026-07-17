#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <onnxruntime_c_api.h>
#include <onnxruntime_session_options_config_keys.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>

namespace {

constexpr uint32_t input_width = 1280;
constexpr uint32_t input_height = 768;
constexpr uint32_t input_channels = 3;
constexpr uint32_t candidate_capacity = 1024;
constexpr uint32_t candidate_components = 6;
constexpr uint32_t default_warmup_runs = 3;
constexpr uint32_t default_measured_runs = 30;
constexpr uint32_t maximum_runs = 1000;
constexpr uint32_t maximum_threads = 256;
constexpr uint32_t runtime_api_version = 17;
constexpr size_t runtime_path_capacity = 32 * 1024;
constexpr size_t input_elements = static_cast<size_t>(input_width) * input_height * input_channels;
constexpr size_t output_elements = static_cast<size_t>(candidate_capacity) * candidate_components;

enum class ExitCode : int {
    success,
    usage,
    runtime,
    environment,
    options,
    session,
    memory,
    tensor,
    warmup,
    run,
    timing
};

enum class Provider : uint8_t { cpu, xnnpack, xnnpack_strict };

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

HMODULE load_adjacent_runtime() noexcept {
    std::array<wchar_t, runtime_path_capacity> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return nullptr;
    size_t separator = length;
    while (separator != 0 && path[separator - 1U] != L'\\')
        --separator;
    constexpr wchar_t name[] = L"onnxruntime.dll";
    if (separator == 0 || separator + std::size(name) > path.size()) return nullptr;
    std::memcpy(path.data() + separator, name, sizeof(name));
    return LoadLibraryExW(path.data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

bool parse_count(const wchar_t* text, uint32_t maximum, uint32_t* output) noexcept {
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(text, &end, 10);
    if (end == text || *end != L'\0' || value == 0 || value > maximum) return false;
    *output = static_cast<uint32_t>(value);
    return true;
}

bool parse_provider(const wchar_t* text, Provider* output) noexcept {
    if (std::wcscmp(text, L"cpu") == 0) {
        *output = Provider::cpu;
    } else if (std::wcscmp(text, L"xnnpack") == 0) {
        *output = Provider::xnnpack;
    } else if (std::wcscmp(text, L"xnnpack-strict") == 0) {
        *output = Provider::xnnpack_strict;
    } else {
        return false;
    }
    return true;
}

uint64_t private_bytes() noexcept {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) != 0
               ? counters.PrivateUsage
               : 0;
}

double milliseconds(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency) noexcept {
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

double percentile(const std::array<double, maximum_runs>& values, uint32_t count, uint32_t numerator,
                  uint32_t denominator) noexcept {
    const uint32_t index = std::min(count - 1U, ((count - 1U) * numerator + denominator / 2U) / denominator);
    return values[index];
}

struct OrtState {
    const OrtApiBase* api_base_ = nullptr;
    const OrtApi* api_ = nullptr;
    HMODULE runtime_ = nullptr;
    OrtEnv* environment_ = nullptr;
    OrtSessionOptions* options_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    OrtValue* input_ = nullptr;
    OrtValue* output_ = nullptr;

    ~OrtState() {
        if (api_ != nullptr) {
            if (output_ != nullptr) api_->ReleaseValue(output_);
            if (input_ != nullptr) api_->ReleaseValue(input_);
            if (memory_ != nullptr) api_->ReleaseMemoryInfo(memory_);
            if (session_ != nullptr) api_->ReleaseSession(session_);
            if (options_ != nullptr) api_->ReleaseSessionOptions(options_);
            if (environment_ != nullptr) api_->ReleaseEnv(environment_);
        }
        if (runtime_ != nullptr) (void)FreeLibrary(runtime_);
    }

    bool succeeded(OrtStatus* status) noexcept {
        if (status == nullptr) return true;
        std::fprintf(stderr, "%s\n", api_->GetErrorMessage(status));
        api_->ReleaseStatus(status);
        return false;
    }
};

struct TensorStorage {
    std::array<float, input_elements> input_{};
    std::array<float, output_elements> output_{};
};

thread_local TensorStorage tensors;

void initialize_input() noexcept {
    uint32_t state = 0x6d2b79f5U;
    for (float& value : tensors.input_) {
        state = state * 1664525U + 1013904223U;
        value = static_cast<float>(state >> 8U) * (1.0F / 16777215.0F);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 6) return exit_code(ExitCode::usage);

    uint32_t measured_runs = default_measured_runs;
    uint32_t warmup_runs = default_warmup_runs;
    uint32_t thread_count = 1;
    Provider provider = Provider::cpu;
    if ((argc >= 3 && !parse_count(argv[2], maximum_runs, &measured_runs)) ||
        (argc >= 4 && !parse_count(argv[3], maximum_runs, &warmup_runs)) ||
        (argc >= 5 && !parse_count(argv[4], maximum_threads, &thread_count)) ||
        (argc == 6 && !parse_provider(argv[5], &provider))) {
        return exit_code(ExitCode::usage);
    }

    LARGE_INTEGER frequency{};
    LARGE_INTEGER session_start{};
    LARGE_INTEGER session_end{};
    if (QueryPerformanceFrequency(&frequency) == 0 || QueryPerformanceCounter(&session_start) == 0) {
        return exit_code(ExitCode::timing);
    }

    OrtState ort;
    ort.runtime_ = load_adjacent_runtime();
    if (ort.runtime_ == nullptr) return exit_code(ExitCode::runtime);
    using GetApiBaseFn = const OrtApiBase*(__cdecl*)() noexcept;
    const auto get_api_base = reinterpret_cast<GetApiBaseFn>(GetProcAddress(ort.runtime_, "OrtGetApiBase"));
    ort.api_base_ = get_api_base == nullptr ? nullptr : get_api_base();
    ort.api_ = ort.api_base_ == nullptr ? nullptr : ort.api_base_->GetApi(runtime_api_version);
    if (ort.api_ == nullptr) return exit_code(ExitCode::runtime);
    if (!ort.succeeded(ort.api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "saccade-cpu", &ort.environment_))) {
        return exit_code(ExitCode::environment);
    }
    if (!ort.succeeded(ort.api_->DisableTelemetryEvents(ort.environment_))) {
        return exit_code(ExitCode::environment);
    }
    if (!ort.succeeded(ort.api_->CreateSessionOptions(&ort.options_)) ||
        !ort.succeeded(ort.api_->SetSessionExecutionMode(ort.options_, ORT_SEQUENTIAL)) ||
        !ort.succeeded(ort.api_->SetIntraOpNumThreads(
            ort.options_, provider == Provider::cpu ? static_cast<int>(thread_count) : 1)) ||
        !ort.succeeded(ort.api_->SetInterOpNumThreads(ort.options_, 1)) ||
        !ort.succeeded(ort.api_->SetSessionGraphOptimizationLevel(ort.options_, ORT_ENABLE_ALL))) {
        return exit_code(ExitCode::options);
    }
    std::array<char, 4> thread_count_text{};
    const auto converted =
        std::to_chars(thread_count_text.data(), thread_count_text.data() + thread_count_text.size(), thread_count);
    if (converted.ec != std::errc{}) return exit_code(ExitCode::options);
    *converted.ptr = '\0';
    if (provider != Provider::cpu) {
        constexpr std::array<const char*, 1> keys{"intra_op_num_threads"};
        const std::array<const char*, 1> values{thread_count_text.data()};
        if (!ort.succeeded(ort.api_->SessionOptionsAppendExecutionProvider(ort.options_, "XNNPACK", keys.data(),
                                                                           values.data(), keys.size()))) {
            return exit_code(ExitCode::options);
        }
        if (provider == Provider::xnnpack_strict && !ort.succeeded(ort.api_->AddSessionConfigEntry(
                                                        ort.options_, kOrtSessionOptionsDisableCPUEPFallback, "1"))) {
            return exit_code(ExitCode::options);
        }
    }
    if (!ort.succeeded(ort.api_->CreateSession(ort.environment_, argv[1], ort.options_, &ort.session_))) {
        return exit_code(ExitCode::session);
    }
    if (!ort.succeeded(ort.api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &ort.memory_))) {
        return exit_code(ExitCode::memory);
    }

    initialize_input();
    constexpr std::array<int64_t, 4> input_shape{1, input_channels, input_height, input_width};
    constexpr std::array<int64_t, 2> output_shape{candidate_capacity, candidate_components};
    if (!ort.succeeded(ort.api_->CreateTensorWithDataAsOrtValue(
            ort.memory_, tensors.input_.data(), sizeof(tensors.input_), input_shape.data(), input_shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ort.input_)) ||
        !ort.succeeded(ort.api_->CreateTensorWithDataAsOrtValue(
            ort.memory_, tensors.output_.data(), sizeof(tensors.output_), output_shape.data(), output_shape.size(),
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ort.output_))) {
        return exit_code(ExitCode::tensor);
    }
    if (QueryPerformanceCounter(&session_end) == 0) return exit_code(ExitCode::timing);

    constexpr std::array<const char*, 1> input_names{"input"};
    constexpr std::array<const char*, 1> output_names{"candidates"};
    const std::array<const OrtValue*, 1> inputs{ort.input_};
    std::array<OrtValue*, 1> outputs{ort.output_};
    const auto run = [&]() noexcept {
        return ort.succeeded(ort.api_->Run(ort.session_, nullptr, input_names.data(), inputs.data(), inputs.size(),
                                           output_names.data(), output_names.size(), outputs.data()));
    };

    for (uint32_t index = 0; index < warmup_runs; ++index) {
        if (!run()) return exit_code(ExitCode::warmup);
    }

    const uint64_t memory_before = private_bytes();
    std::array<double, maximum_runs> samples{};
    for (uint32_t index = 0; index < measured_runs; ++index) {
        LARGE_INTEGER start{};
        LARGE_INTEGER end{};
        if (QueryPerformanceCounter(&start) == 0 || !run() || QueryPerformanceCounter(&end) == 0) {
            return exit_code(ExitCode::run);
        }
        samples[index] = milliseconds(start, end, frequency);
    }
    const uint64_t memory_after = private_bytes();

    std::sort(samples.begin(), samples.begin() + measured_runs);
    std::printf("{\n"
                "  \"provider\": \"%s\",\n"
                "  \"runtime\": \"%s\",\n"
                "  \"input\": \"fp32-nchw-1280x768\",\n"
                "  \"threads\": %u,\n"
                "  \"session_ms\": %.3f,\n"
                "  \"warmup_runs\": %u,\n"
                "  \"measured_runs\": %u,\n"
                "  \"minimum_ms\": %.3f,\n"
                "  \"median_ms\": %.3f,\n"
                "  \"p95_ms\": %.3f,\n"
                "  \"p99_ms\": %.3f,\n"
                "  \"maximum_ms\": %.3f,\n"
                "  \"private_bytes_before\": %llu,\n"
                "  \"private_bytes_after\": %llu\n"
                "}\n",
                provider == Provider::cpu
                    ? "ort-cpu"
                    : (provider == Provider::xnnpack ? "ort-xnnpack-hybrid" : "ort-xnnpack-strict"),
                ort.api_base_->GetVersionString(), thread_count, milliseconds(session_start, session_end, frequency),
                warmup_runs, measured_runs, samples[0], percentile(samples, measured_runs, 50, 100),
                percentile(samples, measured_runs, 95, 100), percentile(samples, measured_runs, 99, 100),
                samples[measured_runs - 1U], static_cast<unsigned long long>(memory_before),
                static_cast<unsigned long long>(memory_after));
    return exit_code(ExitCode::success);
}
