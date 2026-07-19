#include "agent_client.hpp"
#include "agent_result_text.hpp"
#include "core/stack_string_builder.hpp"
#include "saccade_tool_version.h"

#include <saccade/saccade_agent.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

enum class ExitCode : int {
    success,
    usage,
    connection_failed,
    protocol_failed,
    request_failed,
    output_failed,
    input_failed
};

using Client = saccade::tools::AgentClient;
using ClientStorage = saccade::tools::AgentClientStorage;

int exit_code(ExitCode value) noexcept {
    return static_cast<int>(value);
}

bool parse_u64(const char* text, uint64_t* output) noexcept {
    if (text == nullptr || *text == '\0' || output == nullptr) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return false;
    *output = static_cast<uint64_t>(value);
    return true;
}

bool parse_i32(const char* text, int32_t* output) noexcept {
    if (text == nullptr || *text == '\0' || output == nullptr) return false;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) return false;
    *output = static_cast<int32_t>(value);
    return true;
}

bool write_stdout(std::string_view text) noexcept {
    return std::fwrite(text.data(), 1, text.size(), stdout) == text.size();
}

bool write_stdout(const uint8_t* bytes, size_t size) noexcept {
    return std::fwrite(bytes, 1, size, stdout) == size;
}

FILE* open_file(const char* path, const char* mode) noexcept {
#if defined(_WIN32)
    FILE* file = nullptr;
    return fopen_s(&file, path, mode) == 0 ? file : nullptr;
#else
    return std::fopen(path, mode);
#endif
}

int fail(ExitCode code, const char* reason) noexcept {
    std::fputs("saccade: ", stderr);
    std::fputs(reason, stderr);
    std::fputc('\n', stderr);
    return exit_code(code);
}

void print_usage(FILE* stream) noexcept {
    std::fputs(
        "Usage: saccade <command> [options]\n"
        "\n"
        "Commands:\n"
        "  observe [--json]                    Capture the current scene snapshot.\n"
        "  query [options] [--json]            Filter targets in the current scene.\n"
        "    --generation <id>                 Query an exact scene generation.\n"
        "    --after-generation <id>           Wait for a generation newer than <id>.\n"
        "    --target <id>  --role <n>  --capability <bits>  --source <bits>\n"
        "    --minimum-confidence <q16>  --text <value>  --text-file <path>\n"
        "    --text-match exact|prefix|substring\n"
        "  act [options] [--json]              Execute one validated action.\n"
        "    --kind move|hover|click|hold|drag|scroll|key|key-chord|text|window|\n"
        "           cycle|abort|physical|release|text-select|invoke\n"
        "    --target <id>  --to <id>  --x-q8 <n> --y-q8 <n>  --to-x-q8 <n> --to-y-q8 <n>\n"
        "    --button <bits>  --modifiers <bits>  --key <usage>  --repeat <n>\n"
        "    --backward <0|1>  --text-file <path>\n"
        "    --dry-run                         Validate the batch without executing input.\n"
        "    --verify-next-generation          Confirm the post-action scene generation.\n"
        "  wait <generation> [--json]          Observe once a newer generation exists.\n"
        "  batch <file> [--json]               Send a raw binary action batch.\n"
        "\n"
        "Global options:\n"
        "  --json      Emit structured JSON instead of raw wire bytes.\n"
        "  --help      Show this help.\n"
        "  --version   Show the tool version.\n"
        "\n"
        "The Windows executable is named saccade-cli.\n",
        stream);
}

bool emit_error(SaccadeAgentResult result, int32_t platform_error) noexcept {
    saccade::core::StackStringBuilder<256> line;
    return line.append("{\"result\":") && line.append_signed(result) && line.append(",\"result_text\":\"") &&
           line.append(saccade::tools::agent_result_text(result)) && line.append("\",\"platform_error\":") &&
           line.append_signed(platform_error) && line.append("}\n") && write_stdout(line.view());
}

bool write_json_string(const uint8_t* bytes, size_t size) noexcept {
    if (!write_stdout("\"")) return false;
    saccade::core::StackStringBuilder<256> chunk;
    for (size_t index = 0; index < size; ++index) {
        const uint8_t value = bytes[index];
        const char* escape = nullptr;
        switch (value) {
        case '"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }
        if (chunk.size() > chunk.capacity() - 6U) {
            if (!write_stdout(chunk.view())) return false;
            chunk.reset();
        }
        if (escape != nullptr) {
            if (!chunk.append(escape)) return false;
        } else if (value < 0x20U) {
            constexpr char hexadecimal[] = "0123456789abcdef";
            if (!chunk.append("\\u00") || !chunk.append(hexadecimal[value >> 4U]) ||
                !chunk.append(hexadecimal[value & 0x0FU]))
                return false;
        } else if (!chunk.append(static_cast<char>(value))) {
            return false;
        }
    }
    return write_stdout(chunk.view()) && write_stdout("\"");
}

bool emit_targets(const uint8_t* response, size_t response_size, const SaccadeAgentTarget* targets, uint32_t count,
                  uint64_t generation, uint32_t message_flags) noexcept {
    saccade::core::StackStringBuilder<256> line;
    if (!line.append("{\"generation\":") || !line.append_unsigned(generation) || !line.append(",\"truncated\":") ||
        !line.append((message_flags & SACCADE_AGENT_MESSAGE_TRUNCATED) != 0 ? "true" : "false") ||
        !line.append(",\"source_incomplete\":") ||
        !line.append((message_flags & SACCADE_AGENT_MESSAGE_SOURCE_INCOMPLETE) != 0 ? "true" : "false") ||
        !line.append(",\"targets\":[\n") || !write_stdout(line.view()))
        return false;
    for (uint32_t index = 0; index < count; ++index) {
        const SaccadeAgentTarget& target = targets[index];
        line.reset();
        if (!line.append(index == 0 ? "  {\"id\":" : "  ,{\"id\":") || !line.append_unsigned(target.target_id) ||
            !line.append(",\"parent\":") || !line.append_unsigned(target.parent_id) || !line.append(",\"role\":") ||
            !line.append_unsigned(target.role) || !line.append(",\"capabilities\":") ||
            !line.append_unsigned(target.capability_bits) || !line.append(",\"confidence_q16\":") ||
            !line.append_unsigned(target.confidence_q16) || !line.append(",\"x_q8\":") ||
            !line.append_signed(target.bounds.x_q8) || !line.append(",\"y_q8\":") ||
            !line.append_signed(target.bounds.y_q8) || !line.append(",\"width_q8\":") ||
            !line.append_signed(target.bounds.width_q8) || !line.append(",\"height_q8\":") ||
            !line.append_signed(target.bounds.height_q8))
            return false;
        if (target.text_size != 0) {
            if (target.text_offset > response_size || target.text_size > response_size - target.text_offset ||
                !line.append(",\"text\":") || !write_stdout(line.view()) ||
                !write_json_string(response + target.text_offset, target.text_size) || !write_stdout("}\n"))
                return false;
        } else if (!line.append("}\n") || !write_stdout(line.view())) {
            return false;
        }
    }
    return write_stdout("]}\n");
}

SaccadeAgentScope default_scope() noexcept {
    SaccadeAgentScope scope{};
    scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
    scope.source_mode = SACCADE_AGENT_SOURCE_FUSED;
    return scope;
}

SaccadeAgentCapabilityBits requested_capabilities(int argc, char** argv) noexcept {
    if (std::strcmp(argv[1], "batch") == 0) {
        return SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER |
               SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_WINDOW;
    }
    if (std::strcmp(argv[1], "act") != 0) return SACCADE_AGENT_CAPABILITY_OBSERVE;
    const char* kind = "click";
    SaccadeAgentCapabilityBits extra = 0;
    for (int index = 2; index < argc; ++index) {
        if (std::strcmp(argv[index], "--verify-next-generation") == 0) extra |= SACCADE_AGENT_CAPABILITY_OBSERVE;
        if (index + 1 < argc && std::strcmp(argv[index], "--kind") == 0) kind = argv[index + 1];
    }
    if (std::strcmp(kind, "text") == 0)
        return extra | SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_POINTER;
    if (std::strcmp(kind, "key") == 0 || std::strcmp(kind, "key-chord") == 0)
        return extra | SACCADE_AGENT_CAPABILITY_KEYBOARD;
    if (std::strcmp(kind, "window") == 0 || std::strcmp(kind, "cycle") == 0)
        return extra | SACCADE_AGENT_CAPABILITY_WINDOW;
    if (std::strcmp(kind, "abort") == 0) return extra | SACCADE_AGENT_CAPABILITY_POINTER;
    if (std::strcmp(kind, "physical") == 0) return extra | SACCADE_AGENT_CAPABILITY_OBSERVE;
    return extra | SACCADE_AGENT_CAPABILITY_POINTER;
}

bool emit_action_result(const SaccadeAgentActionResult& entry, bool first) noexcept {
    saccade::core::StackStringBuilder<256> line;
    return line.append(first ? "  {\"index\":" : "  ,{\"index\":") && line.append_unsigned(entry.action_index) &&
           line.append(",\"kind\":") && line.append_unsigned(entry.kind) && line.append(",\"result\":") &&
           line.append_signed(entry.result) && line.append(",\"result_text\":\"") &&
           line.append(saccade::tools::agent_result_text(entry.result)) && line.append("\",\"platform_error\":") &&
           line.append_signed(entry.platform_error) && line.append(",\"target\":") &&
           line.append_unsigned(entry.resolved_target_id) && line.append(",\"secondary_target\":") &&
           line.append_unsigned(entry.resolved_secondary_target_id) && line.append(",\"validated_generation\":") &&
           line.append_unsigned(entry.validated_generation) && line.append(",\"flags\":") &&
           line.append_unsigned(entry.flags) && line.append("}\n") && write_stdout(line.view());
}

bool emit_action_completion(const SaccadeAgentActionCompletion& completion, const uint8_t* response,
                            size_t response_size) noexcept {
    saccade::core::StackStringBuilder<512> line;
    if (!(line.append("{\"request_id\":") && line.append_unsigned(completion.request_id) &&
          line.append(",\"result\":") && line.append_signed(completion.result) && line.append(",\"result_text\":\"") &&
          line.append(saccade::tools::agent_result_text(completion.result)) &&
          line.append("\",\"platform_error\":") && line.append_signed(completion.platform_error) &&
          line.append(",\"completed_actions\":") && line.append_unsigned(completion.completed_action_count) &&
          line.append(",\"failed_action\":") && line.append_unsigned(completion.failed_action_index) &&
          line.append(",\"validated_generation\":") && line.append_unsigned(completion.validated_generation) &&
          write_stdout(line.view())))
        return false;
    line.reset();
    if (!(line.append(",\"physical\":{\"x_q8\":") && line.append_signed(completion.physical_state.pointer.x_q8) &&
          line.append(",\"y_q8\":") && line.append_signed(completion.physical_state.pointer.y_q8) &&
          line.append(",\"buttons\":") && line.append_unsigned(completion.physical_state.buttons) &&
          line.append(",\"modifiers\":") && line.append_unsigned(completion.physical_state.modifiers) &&
          line.append(",\"active_lease_id\":") && line.append_unsigned(completion.physical_state.active_lease_id) &&
          line.append(",\"permission_epoch\":") && line.append_unsigned(completion.physical_state.permission_epoch) &&
          line.append(",\"sequence\":") && line.append_unsigned(completion.physical_state.physical_sequence) &&
          line.append(",\"flags\":") && line.append_unsigned(completion.physical_state.flags) && line.append("}") &&
          write_stdout(line.view())))
        return false;
    if ((completion.header.flags & SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE) != 0) {
        line.reset();
        if (!(line.append(",\"next_generation\":{\"generation\":") &&
              line.append_unsigned(completion.next_generation.generation) && line.append(",\"scene_epoch\":") &&
              line.append_unsigned(completion.next_generation.scene_epoch) && line.append(",\"focus_id\":") &&
              line.append_unsigned(completion.next_generation.focus_id) && line.append(",\"window_id\":") &&
              line.append_unsigned(completion.next_generation.window_id) && line.append(",\"display_id\":") &&
              line.append_unsigned(completion.next_generation.display_id) && line.append("}") &&
              write_stdout(line.view())))
            return false;
    }
    const size_t result_bytes = static_cast<size_t>(completion.action_result_count) * completion.action_result_stride;
    if (completion.action_result_count != 0 && completion.action_result_stride == sizeof(SaccadeAgentActionResult) &&
        completion.action_results_offset <= response_size &&
        result_bytes <= response_size - completion.action_results_offset) {
        if (!write_stdout(",\"actions\":[\n")) return false;
        for (uint32_t index = 0; index < completion.action_result_count; ++index) {
            SaccadeAgentActionResult entry{};
            std::memcpy(&entry,
                        response + completion.action_results_offset +
                            static_cast<size_t>(index) * completion.action_result_stride,
                        sizeof(entry));
            if (!emit_action_result(entry, index == 0)) return false;
        }
        if (!write_stdout("]")) return false;
    }
    return write_stdout("}\n");
}

ExitCode observe(Client* client, ClientStorage* storage, uint64_t after_generation, bool json) noexcept {
    SaccadeAgentObserveRequest request{};
    request.header.struct_size = static_cast<uint32_t>(sizeof(request));
    request.header.api_version = SACCADE_AGENT_API_VERSION;
    request.header.message_kind = SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST;
    request.request_id = 2;
    request.scope = default_scope();
    request.freshness.policy =
        after_generation == 0 ? SACCADE_AGENT_FRESHNESS_LATEST_VALID : SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
    request.freshness.after_generation = after_generation;
    request.freshness.timeout_ns = UINT64_C(2'000'000'000);
    request.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    request.maximum_targets = SACCADE_AGENT_MAX_TARGETS;
    request.target_stride = static_cast<uint32_t>(sizeof(SaccadeAgentTarget));
    request.total_capacity = SACCADE_AGENT_MAX_MESSAGE_BYTES;
    size_t response_size = 0;
    if (!client->transact(&request, sizeof(request), storage, &response_size) ||
        response_size < sizeof(SaccadeAgentObserveCompletion))
        return ExitCode::request_failed;
    SaccadeAgentObserveCompletion completion{};
    std::memcpy(&completion, storage->response.data(), sizeof(completion));
    if (!json) {
        if (!write_stdout(storage->response.data(), response_size)) return ExitCode::output_failed;
        return completion.result == SACCADE_AGENT_OK ? ExitCode::success : ExitCode::request_failed;
    }
    if (completion.result != SACCADE_AGENT_OK) {
        return emit_error(completion.result, completion.platform_error) ? ExitCode::request_failed
                                                                        : ExitCode::output_failed;
    }
    const size_t target_bytes = static_cast<size_t>(completion.target_count) * completion.target_stride;
    if (completion.target_stride != sizeof(SaccadeAgentTarget) || completion.targets_offset > response_size ||
        target_bytes > response_size - completion.targets_offset)
        return ExitCode::protocol_failed;
    return emit_targets(
               storage->response.data(), response_size,
               reinterpret_cast<const SaccadeAgentTarget*>(storage->response.data() + completion.targets_offset),
               completion.target_count, completion.generation.generation, completion.header.flags)
               ? ExitCode::success
               : ExitCode::output_failed;
}

ExitCode query(Client* client, ClientStorage* storage, int argc, char** argv, bool json) noexcept {
    constexpr size_t request_prefix_size = sizeof(SaccadeAgentQueryRequest) + sizeof(SaccadeAgentQueryFilter);
    std::memset(storage->request.data(), 0, request_prefix_size);
    auto* request = reinterpret_cast<SaccadeAgentQueryRequest*>(storage->request.data());
    auto* filter = reinterpret_cast<SaccadeAgentQueryFilter*>(storage->request.data() + sizeof(*request));
    request->header.struct_size = static_cast<uint32_t>(sizeof(*request));
    request->header.api_version = SACCADE_AGENT_API_VERSION;
    request->header.message_kind = SACCADE_AGENT_MESSAGE_QUERY_REQUEST;
    request->request_id = 3;
    request->scope = default_scope();
    request->requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
    request->maximum_results = SACCADE_AGENT_MAX_TARGETS;
    request->filter_count = 1;
    request->filter_stride = static_cast<uint32_t>(sizeof(*filter));
    request->filters_offset = static_cast<uint32_t>(sizeof(*request));
    request->freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
    const char* text_value = nullptr;
    const char* text_file = nullptr;
    for (int index = 2; index < argc;) {
        if (std::strcmp(argv[index], "--json") == 0) {
            ++index;
            continue;
        }
        if (index + 1 >= argc) return ExitCode::usage;
        const char* option = argv[index];
        const char* argument = argv[index + 1];
        if (std::strcmp(option, "--text") == 0) {
            text_value = argument;
            index += 2;
            continue;
        }
        if (std::strcmp(option, "--text-file") == 0) {
            text_file = argument;
            index += 2;
            continue;
        }
        if (std::strcmp(option, "--text-match") == 0) {
            if (std::strcmp(argument, "exact") == 0)
                filter->text_match = SACCADE_AGENT_TEXT_EXACT;
            else if (std::strcmp(argument, "prefix") == 0)
                filter->text_match = SACCADE_AGENT_TEXT_PREFIX;
            else if (std::strcmp(argument, "substring") == 0)
                filter->text_match = SACCADE_AGENT_TEXT_SUBSTRING;
            else
                return ExitCode::usage;
            index += 2;
            continue;
        }
        uint64_t value = 0;
        if (!parse_u64(argument, &value)) return ExitCode::usage;
        if (std::strcmp(option, "--generation") == 0) {
            request->generation = value;
        } else if (std::strcmp(option, "--after-generation") == 0) {
            request->freshness.policy = SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
            request->freshness.after_generation = value;
            request->freshness.timeout_ns = UINT64_C(2'000'000'000);
        } else if (std::strcmp(option, "--target") == 0) {
            filter->flags |= SACCADE_AGENT_QUERY_STABLE_ID;
            filter->target_id = value;
        } else if (std::strcmp(option, "--role") == 0) {
            filter->flags |= SACCADE_AGENT_QUERY_ROLE;
            filter->role = static_cast<SaccadeAgentTargetRole>(value);
        } else if (std::strcmp(option, "--capability") == 0) {
            filter->flags |= SACCADE_AGENT_QUERY_CAPABILITY;
            filter->required_capability_bits = static_cast<SaccadeAgentTargetCapabilityBits>(value);
        } else if (std::strcmp(option, "--source") == 0) {
            filter->flags |= SACCADE_AGENT_QUERY_SOURCE;
            filter->source_bits = static_cast<SaccadeAgentTargetSourceBits>(value);
        } else if (std::strcmp(option, "--minimum-confidence") == 0) {
            filter->flags |= SACCADE_AGENT_QUERY_CONFIDENCE;
            filter->minimum_confidence_q16 = static_cast<uint32_t>(value);
        } else {
            return ExitCode::usage;
        }
        index += 2;
    }
    if (text_value != nullptr && text_file != nullptr) return ExitCode::usage;
    size_t request_size = request_prefix_size;
    size_t text_size = 0;
    if (text_value != nullptr) {
        text_size = std::strlen(text_value);
        if (text_size == 0 || text_size > SACCADE_AGENT_MAX_TEXT_BYTES) return ExitCode::usage;
        std::memcpy(storage->request.data() + request_size, text_value, text_size);
    } else if (text_file != nullptr) {
        FILE* input = open_file(text_file, "rb");
        if (input == nullptr) return ExitCode::input_failed;
        text_size = std::fread(storage->request.data() + request_size, 1, SACCADE_AGENT_MAX_TEXT_BYTES + 1U, input);
        const bool complete = std::feof(input) != 0;
        std::fclose(input);
        if (!complete || text_size == 0 || text_size > SACCADE_AGENT_MAX_TEXT_BYTES) return ExitCode::input_failed;
    }
    if (text_size != 0) {
        filter->flags |= SACCADE_AGENT_QUERY_TEXT;
        filter->text_offset = static_cast<uint32_t>(request_size);
        filter->text_size = static_cast<uint32_t>(text_size);
        if (filter->text_match == 0) filter->text_match = SACCADE_AGENT_TEXT_EXACT;
        request_size += text_size;
    } else if (filter->text_match != 0) {
        return ExitCode::usage;
    }
    request->total_size = static_cast<uint32_t>(request_size);
    size_t response_size = 0;
    if (!client->transact(storage->request.data(), request_size, storage, &response_size) ||
        response_size < sizeof(SaccadeAgentQueryCompletion))
        return ExitCode::request_failed;
    SaccadeAgentQueryCompletion completion{};
    std::memcpy(&completion, storage->response.data(), sizeof(completion));
    if (!json) {
        if (!write_stdout(storage->response.data(), response_size)) return ExitCode::output_failed;
        return completion.result == SACCADE_AGENT_OK ? ExitCode::success : ExitCode::request_failed;
    }
    if (completion.result != SACCADE_AGENT_OK) {
        return emit_error(completion.result, completion.platform_error) ? ExitCode::request_failed
                                                                        : ExitCode::output_failed;
    }
    const size_t target_bytes = static_cast<size_t>(completion.target_count) * completion.target_stride;
    if (completion.target_stride != sizeof(SaccadeAgentTarget) || completion.targets_offset > response_size ||
        target_bytes > response_size - completion.targets_offset)
        return ExitCode::protocol_failed;
    return emit_targets(
               storage->response.data(), response_size,
               reinterpret_cast<const SaccadeAgentTarget*>(storage->response.data() + completion.targets_offset),
               completion.target_count, completion.generation.generation, completion.header.flags)
               ? ExitCode::success
               : ExitCode::output_failed;
}

ExitCode batch(Client* client, ClientStorage* storage, const char* path, bool json) noexcept {
    FILE* input = open_file(path, "rb");
    if (input == nullptr) return ExitCode::input_failed;
    const size_t size = std::fread(storage->request.data(), 1, storage->request.size(), input);
    const bool complete = std::feof(input) != 0;
    std::fclose(input);
    if (!complete || size < sizeof(SaccadeAgentActionBatch)) return ExitCode::input_failed;
    size_t response_size = 0;
    if (!client->transact(storage->request.data(), size, storage, &response_size) ||
        response_size < sizeof(SaccadeAgentActionCompletion))
        return ExitCode::request_failed;
    SaccadeAgentActionCompletion completion{};
    std::memcpy(&completion, storage->response.data(), sizeof(completion));
    if (!json) {
        if (!write_stdout(storage->response.data(), response_size)) return ExitCode::output_failed;
        return completion.result == SACCADE_AGENT_OK ? ExitCode::success : ExitCode::request_failed;
    }
    if (!emit_action_completion(completion, storage->response.data(), response_size)) return ExitCode::output_failed;
    return completion.result == SACCADE_AGENT_OK ? ExitCode::success : ExitCode::request_failed;
}

ExitCode act(Client* client, ClientStorage* storage, int argc, char** argv, bool json) noexcept {
    auto* batch = reinterpret_cast<SaccadeAgentActionBatch*>(storage->request.data());
    auto* action = reinterpret_cast<SaccadeAgentAction*>(storage->request.data() + sizeof(*batch));
    *batch = {};
    *action = {};
    batch->header.struct_size = static_cast<uint32_t>(sizeof(*batch));
    batch->header.api_version = SACCADE_AGENT_API_VERSION;
    batch->header.message_kind = SACCADE_AGENT_MESSAGE_ACTION_BATCH;
    batch->request_id = 4;
    batch->requested_capability_bits = requested_capabilities(argc, argv);
    batch->policy = SACCADE_AGENT_BATCH_STOP_ON_FAILURE;
    batch->deadline_ns = saccade::tools::monotonic_time_ns() + UINT64_C(2'000'000'000);
    batch->action_count = 1;
    batch->action_stride = static_cast<uint32_t>(sizeof(*action));
    batch->actions_offset = static_cast<uint32_t>(sizeof(*batch));
    batch->payload_offset = static_cast<uint32_t>(sizeof(*batch) + sizeof(*action));
    batch->total_size = batch->payload_offset;
    action->kind = SACCADE_AGENT_ACTION_CLICK;
    action->button_bits = SACCADE_AGENT_BUTTON_LEFT;
    action->repeat_count = 1;
    const char* text_file = nullptr;
    bool point_x = false;
    bool point_y = false;
    bool secondary_point_x = false;
    bool secondary_point_y = false;
    for (int index = 2; index < argc;) {
        if (std::strcmp(argv[index], "--json") == 0) {
            ++index;
            continue;
        }
        if (std::strcmp(argv[index], "--dry-run") == 0) {
            batch->header.flags |= SACCADE_AGENT_BATCH_DRY_RUN;
            ++index;
            continue;
        }
        if (std::strcmp(argv[index], "--verify-next-generation") == 0) {
            batch->header.flags |= SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION;
            ++index;
            continue;
        }
        if (index + 1 >= argc) return ExitCode::usage;
        const char* option = argv[index];
        const char* value = argv[index + 1];
        uint64_t unsigned_value = 0;
        if (std::strcmp(option, "--kind") == 0) {
            if (std::strcmp(value, "move") == 0)
                action->kind = SACCADE_AGENT_ACTION_POINTER_MOVE;
            else if (std::strcmp(value, "hover") == 0)
                action->kind = SACCADE_AGENT_ACTION_POINTER_HOVER;
            else if (std::strcmp(value, "click") == 0)
                action->kind = SACCADE_AGENT_ACTION_CLICK;
            else if (std::strcmp(value, "hold") == 0)
                action->kind = SACCADE_AGENT_ACTION_HOLD;
            else if (std::strcmp(value, "drag") == 0)
                action->kind = SACCADE_AGENT_ACTION_DRAG_DROP;
            else if (std::strcmp(value, "scroll") == 0)
                action->kind = SACCADE_AGENT_ACTION_SCROLL;
            else if (std::strcmp(value, "key") == 0)
                action->kind = SACCADE_AGENT_ACTION_KEY;
            else if (std::strcmp(value, "key-chord") == 0)
                action->kind = SACCADE_AGENT_ACTION_KEY_CHORD;
            else if (std::strcmp(value, "text") == 0)
                action->kind = SACCADE_AGENT_ACTION_TEXT;
            else if (std::strcmp(value, "window") == 0)
                action->kind = SACCADE_AGENT_ACTION_WINDOW_ACTIVATE;
            else if (std::strcmp(value, "cycle") == 0)
                action->kind = SACCADE_AGENT_ACTION_WINDOW_CYCLE;
            else if (std::strcmp(value, "abort") == 0)
                action->kind = SACCADE_AGENT_ACTION_ABORT;
            else if (std::strcmp(value, "physical") == 0)
                action->kind = SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE;
            else if (std::strcmp(value, "release") == 0)
                action->kind = SACCADE_AGENT_ACTION_RELEASE;
            else if (std::strcmp(value, "text-select") == 0)
                action->kind = SACCADE_AGENT_ACTION_TEXT_SELECT;
            else if (std::strcmp(value, "invoke") == 0)
                action->kind = SACCADE_AGENT_ACTION_INVOKE;
            else
                return ExitCode::usage;
        } else if (std::strcmp(option, "--dx-q8") == 0) {
            if (!parse_i32(value, &action->delta_x_q8)) return ExitCode::usage;
        } else if (std::strcmp(option, "--dy-q8") == 0) {
            if (!parse_i32(value, &action->delta_y_q8)) return ExitCode::usage;
        } else if (std::strcmp(option, "--x-q8") == 0) {
            if (!parse_i32(value, &action->point.x_q8)) return ExitCode::usage;
            point_x = true;
        } else if (std::strcmp(option, "--y-q8") == 0) {
            if (!parse_i32(value, &action->point.y_q8)) return ExitCode::usage;
            point_y = true;
        } else if (std::strcmp(option, "--to-x-q8") == 0) {
            if (!parse_i32(value, &action->secondary_point.x_q8)) return ExitCode::usage;
            secondary_point_x = true;
        } else if (std::strcmp(option, "--to-y-q8") == 0) {
            if (!parse_i32(value, &action->secondary_point.y_q8)) return ExitCode::usage;
            secondary_point_y = true;
        } else if (std::strcmp(option, "--text-file") == 0) {
            text_file = value;
        } else {
            if (!parse_u64(value, &unsigned_value)) return ExitCode::usage;
            if (std::strcmp(option, "--generation") == 0) {
                batch->preconditions.flags |= SACCADE_AGENT_PRECONDITION_GENERATION;
                batch->preconditions.generation = unsigned_value;
            } else if (std::strcmp(option, "--target") == 0) {
                action->target_id = unsigned_value;
            } else if (std::strcmp(option, "--to") == 0) {
                action->secondary_target_id = unsigned_value;
            } else if (std::strcmp(option, "--button") == 0) {
                action->button_bits = static_cast<SaccadeAgentButtonBits>(unsigned_value);
            } else if (std::strcmp(option, "--modifiers") == 0) {
                action->modifiers = static_cast<SaccadeAgentModifierBits>(unsigned_value);
            } else if (std::strcmp(option, "--key") == 0) {
                action->key_usage = static_cast<uint32_t>(unsigned_value);
            } else if (std::strcmp(option, "--repeat") == 0) {
                action->repeat_count = static_cast<uint32_t>(unsigned_value);
            } else if (std::strcmp(option, "--duration-ns") == 0) {
                action->duration_ns = unsigned_value;
            } else if (std::strcmp(option, "--backward") == 0) {
                if (unsigned_value != 0) action->flags |= SACCADE_AGENT_ACTION_CYCLE_BACKWARD;
            } else {
                return ExitCode::usage;
            }
        }
        index += 2;
    }

    if (point_x != point_y || secondary_point_x != secondary_point_y) return ExitCode::usage;
    const bool dual_target =
        action->kind == SACCADE_AGENT_ACTION_DRAG_DROP || action->kind == SACCADE_AGENT_ACTION_TEXT_SELECT;
    const bool point_action =
        action->kind == SACCADE_AGENT_ACTION_POINTER_MOVE || action->kind == SACCADE_AGENT_ACTION_POINTER_HOVER ||
        action->kind == SACCADE_AGENT_ACTION_CLICK || action->kind == SACCADE_AGENT_ACTION_HOLD ||
        action->kind == SACCADE_AGENT_ACTION_DRAG_DROP || action->kind == SACCADE_AGENT_ACTION_SCROLL ||
        action->kind == SACCADE_AGENT_ACTION_TEXT || action->kind == SACCADE_AGENT_ACTION_TEXT_SELECT ||
        action->kind == SACCADE_AGENT_ACTION_INVOKE;
    if (secondary_point_x != (point_x && dual_target) || (point_x && !point_action)) return ExitCode::usage;
    if (point_x) action->flags |= SACCADE_AGENT_ACTION_EXPLICIT_POINTS;

    if (text_file != nullptr) {
        FILE* input = open_file(text_file, "rb");
        if (input == nullptr) return ExitCode::input_failed;
        const size_t size =
            std::fread(storage->request.data() + batch->payload_offset, 1, SACCADE_AGENT_MAX_TEXT_BYTES, input);
        const bool complete = std::feof(input) != 0;
        std::fclose(input);
        if (!complete || size == 0) return ExitCode::input_failed;
        action->payload_size = static_cast<uint32_t>(size);
        batch->payload_size = static_cast<uint32_t>(size);
        batch->total_size += static_cast<uint32_t>(size);
    }
    size_t response_size = 0;
    if (!client->transact(storage->request.data(), batch->total_size, storage, &response_size) ||
        response_size < sizeof(SaccadeAgentActionCompletion))
        return ExitCode::request_failed;
    SaccadeAgentActionCompletion completion{};
    std::memcpy(&completion, storage->response.data(), sizeof(completion));
    if (!json) {
        if (!write_stdout(storage->response.data(), response_size)) return ExitCode::output_failed;
        return completion.result == SACCADE_AGENT_OK ? ExitCode::success : ExitCode::request_failed;
    }
    if (!emit_action_completion(completion, storage->response.data(), response_size)) return ExitCode::output_failed;
    return completion.result == SACCADE_AGENT_OK ? ExitCode::success : ExitCode::request_failed;
}

int finish(ExitCode code) noexcept {
    switch (code) {
    case ExitCode::success:
        return exit_code(code);
    case ExitCode::usage:
        return fail(code, "invalid arguments (run 'saccade --help')");
    case ExitCode::connection_failed:
        return fail(code, "connection lost");
    case ExitCode::protocol_failed:
        return fail(code, "protocol error in the service response");
    case ExitCode::request_failed:
        return fail(code, "request rejected by the agent service");
    case ExitCode::output_failed:
        return fail(code, "failed to write output");
    case ExitCode::input_failed:
        return fail(code, "failed to read input");
    }
    return exit_code(code);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(stderr);
        return fail(ExitCode::usage, "missing command");
    }
    // Global flags work at any position and never touch the service, so
    // `saccade act --help` cannot fail with a connection error.
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--help") == 0 || std::strcmp(argv[index], "-h") == 0 ||
            std::strcmp(argv[index], "help") == 0) {
            print_usage(stdout);
            return exit_code(ExitCode::success);
        }
        if (std::strcmp(argv[index], "--version") == 0) {
            std::fputs("saccade " SACCADE_TOOL_VERSION "\n", stdout);
            return exit_code(ExitCode::success);
        }
    }
    const bool known_command = std::strcmp(argv[1], "observe") == 0 || std::strcmp(argv[1], "query") == 0 ||
                               std::strcmp(argv[1], "act") == 0 || std::strcmp(argv[1], "wait") == 0 ||
                               std::strcmp(argv[1], "batch") == 0;
    if (!known_command) {
        print_usage(stderr);
        return fail(ExitCode::usage, "unknown command");
    }
    static ClientStorage storage;
    Client client;
    if (!client.connect())
        return fail(ExitCode::connection_failed, "connection failed (is the Saccade application running?)");
    const SaccadeAgentCapabilityBits requested = requested_capabilities(argc, argv);
    SaccadeAgentCapabilityBits granted = 0;
    if (!client.hello(requested, &storage, &granted) || (granted & requested) != requested)
        return fail(ExitCode::protocol_failed, "handshake failed or a required capability was not granted");
    bool json = false;
    for (int index = 2; index < argc; ++index)
        json |= std::strcmp(argv[index], "--json") == 0;
    if (std::strcmp(argv[1], "observe") == 0) {
        if (argc == 2 || (argc == 3 && json)) return finish(observe(&client, &storage, 0, json));
        return finish(ExitCode::usage);
    }
    if (std::strcmp(argv[1], "query") == 0) return finish(query(&client, &storage, argc, argv, json));
    if (std::strcmp(argv[1], "act") == 0) return finish(act(&client, &storage, argc, argv, json));
    if (std::strcmp(argv[1], "wait") == 0) {
        uint64_t generation = 0;
        const char* value = nullptr;
        for (int index = 2; index < argc; ++index) {
            if (std::strcmp(argv[index], "--json") == 0) continue;
            if (std::strcmp(argv[index], "--after-generation") == 0 && index + 1 < argc)
                value = argv[++index];
            else if (value == nullptr)
                value = argv[index];
            else
                return finish(ExitCode::usage);
        }
        return parse_u64(value, &generation) ? finish(observe(&client, &storage, generation, json))
                                             : finish(ExitCode::usage);
    }
    const char* path = nullptr;
    for (int index = 2; index < argc; ++index) {
        if (std::strcmp(argv[index], "--json") == 0) continue;
        if (std::strcmp(argv[index], "--input") == 0 && index + 1 < argc)
            path = argv[++index];
        else if (path == nullptr)
            path = argv[index];
        else
            return finish(ExitCode::usage);
    }
    return path != nullptr ? finish(batch(&client, &storage, path, json)) : finish(ExitCode::usage);
}
