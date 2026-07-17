#include "platform/macos/coreml_model.hpp"

#include "model/coreml_contract.hpp"

#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace saccade::platform::macos {
namespace {

constexpr size_t maximum_bundle_files = 4096;
constexpr size_t maximum_bundle_relative_bytes = 1024;
constexpr uint64_t maximum_bundle_bytes = UINT64_C(4) * 1024U * 1024U * 1024U;

bool zero_reserved(const CoreMlModelConfig& config) noexcept {
    return config.reserved[0] == 0 && config.reserved[1] == 0 && config.reserved[2] == 0;
}

bool compute_policy_valid(CoreMlComputePolicy policy) noexcept {
    return policy == CoreMlComputePolicy::all || policy == CoreMlComputePolicy::cpu_and_gpu ||
           policy == CoreMlComputePolicy::cpu_only || policy == CoreMlComputePolicy::cpu_and_neural_engine;
}

MLComputeUnits compute_units(CoreMlComputePolicy policy) noexcept {
    switch (policy) {
    case CoreMlComputePolicy::all:
        return MLComputeUnitsAll;
    case CoreMlComputePolicy::cpu_and_gpu:
        return MLComputeUnitsCPUAndGPU;
    case CoreMlComputePolicy::cpu_only:
        return MLComputeUnitsCPUOnly;
    case CoreMlComputePolicy::cpu_and_neural_engine:
        return MLComputeUnitsCPUAndNeuralEngine;
    }
    return MLComputeUnitsCPUOnly;
}

bool dimensions_equal(NSArray<NSNumber*>* shape, NSUInteger first, NSUInteger second) noexcept {
    return shape != nil && shape.count == 2 && shape[0].unsignedIntegerValue == first &&
           shape[1].unsignedIntegerValue == second;
}

bool array_is_float32(MLFeatureDescription* description, NSArray<NSNumber*>* expected_shape) noexcept {
    return description != nil && description.type == MLFeatureTypeMultiArray &&
           description.multiArrayConstraint != nil &&
           description.multiArrayConstraint.dataType == MLMultiArrayDataTypeFloat32 &&
           [description.multiArrayConstraint.shape isEqualToArray:expected_shape];
}

bool string_from_span(SaccadeSpanU8 bytes, NSString** output) noexcept {
    if (output == nullptr || bytes.data == nullptr || bytes.size == 0 || bytes.size > NSUIntegerMax) {
        return false;
    }
    NSString* value = [[NSString alloc] initWithBytes:bytes.data
                                               length:static_cast<NSUInteger>(bytes.size)
                                             encoding:NSUTF8StringEncoding];
    if (value == nil) return false;
    *output = value;
    return true;
}

bool model_url(const char* root_text, SaccadeSpanU8 locator, NSURL** output) noexcept {
    if (root_text == nullptr || root_text[0] == '\0' || output == nullptr) return false;
    NSString* locator_text = nil;
    if (!string_from_span(locator, &locator_text)) return false;
    NSString* root_path = [NSString stringWithUTF8String:root_text];
    if (root_path == nil) return false;
    NSURL* root = [[NSURL fileURLWithPath:root_path isDirectory:YES] URLByResolvingSymlinksInPath];
    NSURL* candidate = [[root URLByAppendingPathComponent:locator_text] URLByResolvingSymlinksInPath];
    NSString* root_value = root.path;
    NSString* candidate_value = candidate.path;
    NSString* prefix = [root_value stringByAppendingString:@"/"];
    if (![candidate_value hasPrefix:prefix]) return false;
    *output = candidate;
    return true;
}

struct BundleFile {
    std::filesystem::path path_{};
    std::u8string relative_{};
    uint64_t size_ = 0;
};

bool digest_update(CC_SHA256_CTX* digest, const void* bytes, size_t size) noexcept {
    if (size > std::numeric_limits<CC_LONG>::max()) return false;
    return CC_SHA256_Update(digest, bytes, static_cast<CC_LONG>(size)) == 1;
}

bool digest_u32(CC_SHA256_CTX* digest, uint32_t value) noexcept {
    std::array<uint8_t, sizeof(value)> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
    return digest_update(digest, bytes.data(), bytes.size());
}

bool digest_u64(CC_SHA256_CTX* digest, uint64_t value) noexcept {
    std::array<uint8_t, sizeof(value)> bytes{};
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
    return digest_update(digest, bytes.data(), bytes.size());
}

} // namespace

SaccadeResult coreml_bundle_digest(const char* bundle_path,
                                   std::array<uint8_t, CC_SHA256_DIGEST_LENGTH>* output) noexcept {
    if (bundle_path == nullptr || bundle_path[0] == '\0' || output == nullptr) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }

    try {
        const std::filesystem::path root(bundle_path);
        std::error_code error;
        const std::filesystem::file_status root_status = std::filesystem::symlink_status(root, error);
        if (error) return SACCADE_ERROR_PERMISSION;
        if (std::filesystem::is_symlink(root_status)) return SACCADE_ERROR_PERMISSION;
        if (!std::filesystem::is_directory(root_status)) return SACCADE_ERROR_NOT_FOUND;

        std::vector<BundleFile> files;
        files.reserve(64);
        uint64_t total_bytes = 0;
        std::filesystem::recursive_directory_iterator iterator(root, std::filesystem::directory_options::none, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            const std::filesystem::directory_entry& entry = *iterator;
            const std::filesystem::file_status status = entry.symlink_status(error);
            if (error) break;
            if (std::filesystem::is_symlink(status) ||
                (!std::filesystem::is_directory(status) && !std::filesystem::is_regular_file(status))) {
                return SACCADE_ERROR_PERMISSION;
            }
            if (std::filesystem::is_regular_file(status)) {
                const std::filesystem::path relative = entry.path().lexically_relative(root);
                const uintmax_t size = entry.file_size(error);
                if (error) break;
                std::u8string relative_text = relative.generic_u8string();
                if (relative.empty() || relative_text.empty() || relative_text.size() > maximum_bundle_relative_bytes ||
                    files.size() == maximum_bundle_files || size > maximum_bundle_bytes - total_bytes) {
                    return SACCADE_ERROR_UNSUPPORTED;
                }
                total_bytes += static_cast<uint64_t>(size);
                files.push_back({entry.path(), std::move(relative_text), static_cast<uint64_t>(size)});
            }
            iterator.increment(error);
        }
        if (error) return SACCADE_ERROR_PERMISSION;
        if (files.empty()) return SACCADE_ERROR_INVALID_ARGUMENT;

        std::sort(files.begin(), files.end(), [](const BundleFile& left, const BundleFile& right) noexcept {
            return left.relative_ < right.relative_;
        });

        CC_SHA256_CTX digest{};
        if (CC_SHA256_Init(&digest) != 1) return SACCADE_ERROR_BACKEND;
        std::array<uint8_t, 16 * 1024> buffer{};
        for (const BundleFile& file : files) {
            const std::filesystem::file_status status = std::filesystem::symlink_status(file.path_, error);
            if (error || std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status) ||
                std::filesystem::file_size(file.path_, error) != file.size_ || error) {
                return SACCADE_ERROR_PERMISSION;
            }
            if (file.relative_.size() > UINT32_MAX ||
                !digest_u32(&digest, static_cast<uint32_t>(file.relative_.size())) ||
                !digest_update(&digest, file.relative_.data(), file.relative_.size()) ||
                !digest_u64(&digest, file.size_)) {
                return SACCADE_ERROR_BACKEND;
            }

            std::ifstream stream(file.path_, std::ios::binary);
            if (!stream.is_open()) return SACCADE_ERROR_PERMISSION;
            uint64_t remaining = file.size_;
            while (remaining != 0) {
                const size_t requested = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
                stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(requested));
                if (stream.gcount() != static_cast<std::streamsize>(requested) ||
                    !digest_update(&digest, buffer.data(), requested)) {
                    return SACCADE_ERROR_PERMISSION;
                }
                remaining -= requested;
            }
            if (stream.peek() != std::char_traits<char>::eof()) return SACCADE_ERROR_PERMISSION;
        }
        return CC_SHA256_Final(output->data(), &digest) == 1 ? SACCADE_OK : SACCADE_ERROR_BACKEND;
    } catch (...) {
        return SACCADE_ERROR_BACKEND;
    }
}

namespace {

SaccadeResult verify_model_description(MLModel* model, const model::ArtifactView& artifact,
                                       const model::coreml::Contract& contract, NSString* input_name,
                                       NSString* target_rows_name, NSString* target_count_name) noexcept {
    if (model == nil || input_name == nil || target_rows_name == nil || target_count_name == nil) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    MLFeatureDescription* input = model.modelDescription.inputDescriptionsByName[input_name];
    if (input == nil || input.type != MLFeatureTypeImage || input.imageConstraint == nil ||
        input.imageConstraint.pixelFormatType != kCVPixelFormatType_32BGRA ||
        input.imageConstraint.pixelsWide != artifact.input_width ||
        input.imageConstraint.pixelsHigh != artifact.input_height) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    NSArray<NSNumber*>* rows_shape = @[ @(contract.candidate_capacity), @(model::coreml::target_row_components) ];
    NSArray<NSNumber*>* count_shape = @[ @1 ];
    if (!array_is_float32(model.modelDescription.outputDescriptionsByName[target_rows_name], rows_shape) ||
        !array_is_float32(model.modelDescription.outputDescriptionsByName[target_count_name], count_shape)) {
        return SACCADE_ERROR_UNSUPPORTED;
    }
    return SACCADE_OK;
}

bool count_from_array(MLMultiArray* array, uint32_t maximum, uint32_t* output) noexcept {
    if (array == nil || output == nullptr || array.dataType != MLMultiArrayDataTypeFloat32 || array.count != 1) {
        return false;
    }
    __block float value = 0.0F;
    __block bool read = false;
    [array getBytesWithHandler:^(const void* bytes, NSInteger size) {
      if (bytes == nullptr || size < static_cast<NSInteger>(sizeof(value))) return;
      std::memcpy(&value, bytes, sizeof(value));
      read = true;
    }];
    if (!read || !std::isfinite(value)) return false;
    const long rounded = std::lround(value);
    if (rounded < 0 || rounded > maximum) return false;
    *output = static_cast<uint32_t>(rounded);
    return true;
}

} // namespace

struct CoreMlModel::Impl {
    __strong MLModel* model_ = nil;
    __strong NSString* input_name_ = nil;
    __strong NSString* target_rows_name_ = nil;
    __strong NSString* target_count_name_ = nil;
    model::ArtifactView artifact_{};
    model::coreml::Contract contract_{};
    std::array<kernels::targets::DenseCandidate, kernels::targets::maximum_candidates> candidates_{};
    kernels::targets::PostprocessWorkspace workspace_{};
    CoreMlModelStats stats_{};
    bool initialized_ = false;
};

CoreMlModel::CoreMlModel() noexcept {
    static_assert(sizeof(Impl) <= storage_size);
    static_assert(alignof(Impl) <= 64);
    ::new (static_cast<void*>(storage_.data())) Impl{};
}

CoreMlModel::~CoreMlModel() {
    impl().~Impl();
}

CoreMlModel::Impl& CoreMlModel::impl() noexcept {
    return *std::launder(reinterpret_cast<Impl*>(storage_.data()));
}

const CoreMlModel::Impl& CoreMlModel::impl() const noexcept {
    return *std::launder(reinterpret_cast<const Impl*>(storage_.data()));
}

SaccadeResult CoreMlModel::initialize(const model::ArtifactView& artifact, CoreMlModelConfig config) noexcept {
    Impl& state = impl();
    if (state.initialized_ || config.model_root == nullptr || !zero_reserved(config) ||
        !compute_policy_valid(config.compute_policy)) {
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    if (config.compute_policy == CoreMlComputePolicy::cpu_and_neural_engine) {
        if (!@available(macOS 13.0, *)) {
            return SACCADE_ERROR_UNSUPPORTED;
        }
    }
    model::coreml::Contract contract{};
    const SaccadeResult parsed = model::coreml::parse_contract(artifact, &contract);
    if (parsed != SACCADE_OK) return parsed;
    @autoreleasepool {
        NSURL* url = nil;
        if (!model_url(config.model_root, contract.locator, &url)) {
            return SACCADE_ERROR_PERMISSION;
        }
        if (![[NSFileManager defaultManager] fileExistsAtPath:url.path]) {
            return SACCADE_ERROR_NOT_FOUND;
        }
        std::array<uint8_t, CC_SHA256_DIGEST_LENGTH> actual_digest{};
        const SaccadeResult digested = coreml_bundle_digest(url.fileSystemRepresentation, &actual_digest);
        if (digested != SACCADE_OK) return digested;
        if (actual_digest != contract.bundle_sha256) return SACCADE_ERROR_PERMISSION;

        NSString* input_name = nil;
        NSString* target_rows_name = nil;
        NSString* target_count_name = nil;
        if (!string_from_span(contract.input_name, &input_name) ||
            !string_from_span(contract.target_rows_name, &target_rows_name) ||
            !string_from_span(contract.target_count_name, &target_count_name)) {
            return SACCADE_ERROR_INVALID_ARGUMENT;
        }
        MLModelConfiguration* configuration = [[MLModelConfiguration alloc] init];
        configuration.computeUnits = compute_units(config.compute_policy);
        configuration.allowLowPrecisionAccumulationOnGPU = config.allow_low_precision_gpu ? YES : NO;
        if (@available(macOS 14.4, *)) {
            MLOptimizationHints* hints = [[MLOptimizationHints alloc] init];
            hints.reshapeFrequency = MLReshapeFrequencyHintInfrequent;
            if (@available(macOS 15.0, *)) {
                hints.specializationStrategy = MLSpecializationStrategyFastPrediction;
            }
            configuration.optimizationHints = hints;
        }
        NSError* error = nil;
        MLModel* model = [MLModel modelWithContentsOfURL:url configuration:configuration error:&error];
        if (model == nil || error != nil) return SACCADE_ERROR_BACKEND;
        const SaccadeResult valid =
            verify_model_description(model, artifact, contract, input_name, target_rows_name, target_count_name);
        if (valid != SACCADE_OK) return valid;
        state.model_ = model;
        state.input_name_ = input_name;
        state.target_rows_name_ = target_rows_name;
        state.target_count_name_ = target_count_name;
        state.artifact_ = artifact;
        state.contract_ = contract;
        state.initialized_ = true;
        ++state.stats_.model_loads;
        return SACCADE_OK;
    }
}

SaccadeResult CoreMlModel::predict(const CoreMlPrediction& request, SaccadeMutableSpanU8 output,
                                   CoreMlPredictionResult* result) noexcept {
    Impl& state = impl();
    if (!state.initialized_ || result == nullptr || request.pixel_buffer == nullptr ||
        request.pixel_format != SACCADE_FORMAT_BGRA8 || request.width != state.artifact_.input_width ||
        request.height != state.artifact_.input_height || request.scope.x != 0 || request.scope.y != 0 ||
        request.scope.width != static_cast<int32_t>(request.width) ||
        request.scope.height != static_cast<int32_t>(request.height)) {
        ++state.stats_.unsupported_inputs;
        return SACCADE_ERROR_INVALID_ARGUMENT;
    }
    *result = {};
    @autoreleasepool {
        CVPixelBufferRef pixel_buffer = static_cast<CVPixelBufferRef>(request.pixel_buffer);
        MLFeatureValue* image = [MLFeatureValue featureValueWithPixelBuffer:pixel_buffer];
        NSError* error = nil;
        MLDictionaryFeatureProvider* input =
            [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{state.input_name_ : image} error:&error];
        if (input == nil || error != nil) {
            ++state.stats_.failures;
            return SACCADE_ERROR_BACKEND;
        }
        id<MLFeatureProvider> prediction = [state.model_ predictionFromFeatures:input error:&error];
        if (prediction == nil || error != nil) {
            ++state.stats_.failures;
            return SACCADE_ERROR_BACKEND;
        }
        MLMultiArray* rows = [prediction featureValueForName:state.target_rows_name_].multiArrayValue;
        MLMultiArray* count = [prediction featureValueForName:state.target_count_name_].multiArrayValue;
        uint32_t candidate_count = 0;
        if (!count_from_array(count, state.contract_.candidate_capacity, &candidate_count) || rows == nil ||
            rows.dataType != MLMultiArrayDataTypeFloat32 ||
            !dimensions_equal(rows.shape, state.contract_.candidate_capacity, model::coreml::target_row_components) ||
            rows.strides.count != 2) {
            ++state.stats_.output_contract_failures;
            return SACCADE_ERROR_BACKEND;
        }
        __block SaccadeResult decoded = SACCADE_ERROR_BACKEND;
        __block uint32_t decoded_count = 0;
        [rows getBytesWithHandler:^(const void* bytes, NSInteger size) {
          const uint64_t required_bytes = static_cast<uint64_t>(state.contract_.candidate_capacity) *
                                          rows.strides[0].unsignedIntegerValue * sizeof(float);
          if (required_bytes > static_cast<uint64_t>(std::numeric_limits<NSInteger>::max())) {
              return;
          }
          const NSInteger required = static_cast<NSInteger>(required_bytes);
          if (bytes == nullptr || size < required) return;
          const model::coreml::TargetRows view{
              bytes, model::coreml::ScalarType::float32, state.contract_.candidate_capacity,
              static_cast<uint32_t>(rows.strides[0].unsignedIntegerValue), model::coreml::target_row_components};
          decoded = model::coreml::decode_target_rows(state.contract_, view, candidate_count, request.scope,
                                                      state.candidates_.data(), state.contract_.candidate_capacity,
                                                      &decoded_count);
        }];
        if (decoded != SACCADE_OK) {
            ++state.stats_.output_contract_failures;
            return decoded;
        }
        kernels::targets::PostprocessConfig config{};
        config.maximum_targets = state.artifact_.max_targets;
        config.minimum_confidence_q16 = state.contract_.minimum_confidence_q16;
        config.band_minimum_confidence_q16 = state.contract_.band_minimum_confidence_q16;
        config.band_min_short_side_q3 = state.contract_.band_min_short_side_q3;
        config.band_max_short_side_q3 = state.contract_.band_max_short_side_q3;
        config.iou_threshold_q16 = state.contract_.iou_threshold_q16;
        size_t required = 0;
        kernels::targets::PostprocessStats stats{};
        const SaccadeResult published =
            kernels::targets::postprocess(state.candidates_.data(), decoded_count, config, request.epochs,
                                          &state.workspace_, output, &required, &stats);
        if (published != SACCADE_OK) {
            ++state.stats_.failures;
            return published;
        }
        result->byte_size = required;
        result->target_count = stats.targets_written;
        result->candidate_count = decoded_count;
        ++state.stats_.predictions;
        state.stats_.candidates_decoded += decoded_count;
        state.stats_.targets_published += stats.targets_written;
        return SACCADE_OK;
    }
}

SaccadeResult CoreMlModel::shutdown() noexcept {
    Impl& state = impl();
    if (!state.initialized_) return SACCADE_ERROR_STATE;
    state.model_ = nil;
    state.input_name_ = nil;
    state.target_rows_name_ = nil;
    state.target_count_name_ = nil;
    state.artifact_ = {};
    state.contract_ = {};
    state.initialized_ = false;
    return SACCADE_OK;
}

uint64_t CoreMlModel::stable_id() const noexcept {
    return impl().initialized_ ? impl().artifact_.stable_id : 0;
}

uint32_t CoreMlModel::maximum_output_bytes() const noexcept {
    return impl().initialized_ ? impl().artifact_.max_output_bytes : 0;
}

CoreMlModelStats CoreMlModel::stats() const noexcept {
    return impl().stats_;
}

} // namespace saccade::platform::macos
