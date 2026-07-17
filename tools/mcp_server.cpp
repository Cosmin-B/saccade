#include "agent_client.hpp"
#include "core/stack_string_builder.hpp"

#include <saccade/saccade_agent.h>

#define JSMN_PARENT_LINKS
#define JSMN_STRICT
#include "jsmn.h"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

constexpr size_t input_capacity = 65536;
constexpr size_t token_capacity = 512;
constexpr uint32_t default_maximum_results = 256;
constexpr uint64_t maximum_exact_json_integer = UINT64_C(9007199254740991);

enum class ExitCode : int { success, connection_failed, input_failed, output_failed };

struct ServerStorage {
    saccade::tools::AgentClientStorage agent{};
    std::array<char, input_capacity + 1U> input{};
    std::array<jsmntok_t, token_capacity> tokens{};
};

struct JsonDocument {
    const char* text = nullptr;
    size_t size = 0;
    jsmntok_t* tokens = nullptr;
    int count = 0;
};

bool write_bytes(const char* bytes, size_t size) noexcept {
    return std::fwrite(bytes, 1, size, stdout) == size;
}

bool write_text(std::string_view text) noexcept {
    return write_bytes(text.data(), text.size());
}

std::string_view token_text(const JsonDocument& document, int index) noexcept {
    const jsmntok_t& token = document.tokens[index];
    return {document.text + token.start, static_cast<size_t>(token.end - token.start)};
}

bool token_equals(const JsonDocument& document, int index, std::string_view value) noexcept {
    return index >= 0 && token_text(document, index) == value;
}

int field(const JsonDocument& document, int object, std::string_view key) noexcept {
    if (object < 0 || document.tokens[object].type != JSMN_OBJECT) return -1;
    for (int index = object + 1; index + 1 < document.count; ++index) {
        if (document.tokens[index].parent == object && document.tokens[index].type == JSMN_STRING &&
            token_equals(document, index, key))
            return index + 1;
    }
    return -1;
}

bool parse_u64(const JsonDocument& document, int token_index, uint64_t* output) noexcept {
    if (token_index < 0 || output == nullptr ||
        (document.tokens[token_index].type != JSMN_PRIMITIVE && document.tokens[token_index].type != JSMN_STRING))
        return false;
    const std::string_view text = token_text(document, token_index);
    if (text.empty()) return false;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, *output, 10);
    return result.ec == std::errc{} && result.ptr == end &&
           (document.tokens[token_index].type == JSMN_STRING || *output <= maximum_exact_json_integer);
}

bool parse_i32(const JsonDocument& document, int token_index, int32_t* output) noexcept {
    if (token_index < 0 || document.tokens[token_index].type != JSMN_PRIMITIVE || output == nullptr) return false;
    const std::string_view text = token_text(document, token_index);
    if (text.empty()) return false;
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, *output, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_bool(const JsonDocument& document, int token_index, bool* output) noexcept {
    if (token_equals(document, token_index, "true")) {
        *output = true;
        return true;
    }
    if (token_equals(document, token_index, "false")) {
        *output = false;
        return true;
    }
    return false;
}

bool append_utf8(uint32_t value, uint8_t* output, size_t capacity, size_t* size) noexcept {
    if (value <= 0x7FU) {
        if (*size == capacity || value == 0) return false;
        output[(*size)++] = static_cast<uint8_t>(value);
    } else if (value <= 0x7FFU) {
        if (capacity - *size < 2) return false;
        output[(*size)++] = static_cast<uint8_t>(0xC0U | value >> 6U);
        output[(*size)++] = static_cast<uint8_t>(0x80U | (value & 0x3FU));
    } else if (value <= 0xFFFFU && (value < 0xD800U || value > 0xDFFFU)) {
        if (capacity - *size < 3) return false;
        output[(*size)++] = static_cast<uint8_t>(0xE0U | value >> 12U);
        output[(*size)++] = static_cast<uint8_t>(0x80U | (value >> 6U & 0x3FU));
        output[(*size)++] = static_cast<uint8_t>(0x80U | (value & 0x3FU));
    } else if (value <= 0x10FFFFU) {
        if (capacity - *size < 4) return false;
        output[(*size)++] = static_cast<uint8_t>(0xF0U | value >> 18U);
        output[(*size)++] = static_cast<uint8_t>(0x80U | (value >> 12U & 0x3FU));
        output[(*size)++] = static_cast<uint8_t>(0x80U | (value >> 6U & 0x3FU));
        output[(*size)++] = static_cast<uint8_t>(0x80U | (value & 0x3FU));
    } else {
        return false;
    }
    return true;
}

int hexadecimal(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool decode_string(const JsonDocument& document, int token_index, uint8_t* output, size_t capacity,
                   size_t* output_size) noexcept {
    if (token_index < 0 || document.tokens[token_index].type != JSMN_STRING || output == nullptr ||
        output_size == nullptr)
        return false;
    const std::string_view input = token_text(document, token_index);
    size_t written = 0;
    for (size_t index = 0; index < input.size(); ++index) {
        const uint8_t value = static_cast<uint8_t>(input[index]);
        if (value != '\\') {
            if (written == capacity || value < 0x20U) return false;
            output[written++] = value;
            continue;
        }
        if (++index == input.size()) return false;
        const char escaped = input[index];
        if (escaped == 'u') {
            if (input.size() - index <= 4) return false;
            uint32_t codepoint = 0;
            for (uint32_t digit = 0; digit < 4; ++digit) {
                const int nibble = hexadecimal(input[++index]);
                if (nibble < 0) return false;
                codepoint = codepoint << 4U | static_cast<uint32_t>(nibble);
            }
            if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                if (input.size() - index <= 6 || input[index + 1U] != '\\' || input[index + 2U] != 'u') return false;
                index += 2U;
                uint32_t low = 0;
                for (uint32_t digit = 0; digit < 4; ++digit) {
                    const int nibble = hexadecimal(input[++index]);
                    if (nibble < 0) return false;
                    low = low << 4U | static_cast<uint32_t>(nibble);
                }
                if (low < 0xDC00U || low > 0xDFFFU) return false;
                codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + low - 0xDC00U;
            }
            if (!append_utf8(codepoint, output, capacity, &written)) return false;
            continue;
        }
        uint8_t decoded = 0;
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            decoded = static_cast<uint8_t>(escaped);
            break;
        case 'b':
            decoded = '\b';
            break;
        case 'f':
            decoded = '\f';
            break;
        case 'n':
            decoded = '\n';
            break;
        case 'r':
            decoded = '\r';
            break;
        case 't':
            decoded = '\t';
            break;
        default:
            return false;
        }
        if (written == capacity) return false;
        output[written++] = decoded;
    }
    *output_size = written;
    return true;
}

bool write_json_string(const uint8_t* bytes, size_t size) noexcept {
    if (!write_text("\"")) return false;
    saccade::core::StackStringBuilder<256> chunk;
    constexpr char hexadecimal_digits[] = "0123456789abcdef";
    for (size_t index = 0; index < size; ++index) {
        if (chunk.size() > chunk.capacity() - 6U) {
            if (!write_text(chunk.view())) return false;
            chunk.reset();
        }
        const uint8_t value = bytes[index];
        const char* escape = value == '"'    ? "\\\""
                             : value == '\\' ? "\\\\"
                             : value == '\b' ? "\\b"
                             : value == '\f' ? "\\f"
                             : value == '\n' ? "\\n"
                             : value == '\r' ? "\\r"
                             : value == '\t' ? "\\t"
                                             : nullptr;
        if (escape != nullptr) {
            if (!chunk.append(escape)) return false;
        } else if (value < 0x20U) {
            if (!chunk.append("\\u00") || !chunk.append(hexadecimal_digits[value >> 4U]) ||
                !chunk.append(hexadecimal_digits[value & 0x0FU]))
                return false;
        } else if (!chunk.append(static_cast<char>(value))) {
            return false;
        }
    }
    return write_text(chunk.view()) && write_text("\"");
}

std::string_view scope_name(SaccadeAgentScopeKind kind) noexcept {
    switch (kind) {
    case SACCADE_AGENT_SCOPE_ACTIVE_WINDOW:
        return "active-window";
    case SACCADE_AGENT_SCOPE_DISPLAY:
        return "display";
    case SACCADE_AGENT_SCOPE_DESKTOP:
        return "desktop";
    case SACCADE_AGENT_SCOPE_RECT:
        return "rect";
    default:
        return "unknown";
    }
}

std::string_view source_mode_name(SaccadeAgentSourceMode mode) noexcept {
    switch (mode) {
    case SACCADE_AGENT_SOURCE_PIXEL:
        return "pixel";
    case SACCADE_AGENT_SOURCE_SEMANTIC:
        return "semantic";
    case SACCADE_AGENT_SOURCE_GRID:
        return "grid";
    case SACCADE_AGENT_SOURCE_FUSED:
        return "fused";
    default:
        return "unknown";
    }
}

std::string_view role_name(SaccadeAgentTargetRole role) noexcept {
    switch (role) {
    case SACCADE_AGENT_ROLE_BUTTON:
        return "button";
    case SACCADE_AGENT_ROLE_LINK:
        return "link";
    case SACCADE_AGENT_ROLE_TEXT:
        return "text";
    case SACCADE_AGENT_ROLE_TEXT_FIELD:
        return "text-field";
    case SACCADE_AGENT_ROLE_CHECKBOX:
        return "checkbox";
    case SACCADE_AGENT_ROLE_RADIO:
        return "radio";
    case SACCADE_AGENT_ROLE_MENU_ITEM:
        return "menu-item";
    case SACCADE_AGENT_ROLE_SLIDER:
        return "slider";
    case SACCADE_AGENT_ROLE_IMAGE:
        return "image";
    case SACCADE_AGENT_ROLE_WINDOW:
        return "window";
    default:
        return "unknown";
    }
}

bool write_id(const JsonDocument& document, int id) noexcept {
    if (id < 0) return write_text("null");
    if (document.tokens[id].type == JSMN_STRING) {
        return write_text("\"") && write_text(token_text(document, id)) && write_text("\"");
    }
    return document.tokens[id].type == JSMN_PRIMITIVE && write_text(token_text(document, id));
}

bool response_prefix(const JsonDocument& document, int id) noexcept {
    return write_text("{\"jsonrpc\":\"2.0\",\"id\":") && write_id(document, id);
}

bool write_error(const JsonDocument& document, int id, int code, std::string_view message) noexcept {
    saccade::core::StackStringBuilder<256> tail;
    return response_prefix(document, id) && tail.append(",\"error\":{\"code\":") && tail.append_signed(code) &&
           tail.append(",\"message\":") && write_text(tail.view()) &&
           write_json_string(reinterpret_cast<const uint8_t*>(message.data()), message.size()) && write_text("}}\n");
}

bool write_initialize(const JsonDocument& document, int id) noexcept {
    return response_prefix(document, id) &&
           write_text(",\"result\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{\"tools\":{}},"
                      "\"serverInfo\":{\"name\":\"saccade\",\"version\":\"0.1.0\"}}}\n");
}

bool write_tools(const JsonDocument& document, int id) noexcept {
    return response_prefix(document, id) &&
           write_text(",\"result\":{\"tools\":["
                      "{\"name\":\"saccade_observe\",\"description\":\"Observe the latest immutable desktop "
                      "target generation. 64-bit identifiers are decimal strings.\",\"inputSchema\":{"
                      "\"type\":\"object\",\"properties\":{\"maximumTargets\":{\"type\":\"integer\","
                      "\"minimum\":1,\"maximum\":10000},\"scope\":{\"enum\":[\"active-window\",\"display\","
                      "\"desktop\",\"rect\"]},\"scopeId\":{\"oneOf\":[{\"type\":\"string\",\"pattern\":"
                      "\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991}]},"
                      "\"sourceMode\":{\"enum\":["
                      "\"pixel\",\"semantic\",\"grid\",\"fused\"]},\"xQ8\":{\"type\":\"integer\"},"
                      "\"yQ8\":{\"type\":\"integer\"},\"widthQ8\":{\"type\":\"integer\",\"minimum\":1},"
                      "\"heightQ8\":{\"type\":\"integer\",\"minimum\":1},\"afterGeneration\":{"
                      "\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\","
                      "\"minimum\":0,\"maximum\":9007199254740991}]}}}},"
                      "{\"name\":\"saccade_query\",\"description\":\"Query stable desktop targets by "
                      "identity, role, capability, source, confidence, or text. 64-bit identifiers are decimal "
                      "strings.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"generation\":{"
                      "\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\","
                      "\"minimum\":0,\"maximum\":9007199254740991}]},\"targetId\":{\"oneOf\":[{\"type\":\"string\","
                      "\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991}]},"
                      "\"scope\":{\"enum\":["
                      "\"active-window\",\"display\",\"desktop\",\"rect\"]},\"scopeId\":{\"oneOf\":[{"
                      "\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,"
                      "\"maximum\":9007199254740991}]},"
                      "\"sourceMode\":{\"enum\":[\"pixel\",\"semantic\",\"grid\",\"fused\"]},\"xQ8\":{"
                      "\"type\":\"integer\"},\"yQ8\":{\"type\":\"integer\"},\"widthQ8\":{\"type\":"
                      "\"integer\",\"minimum\":1},\"heightQ8\":{\"type\":\"integer\",\"minimum\":1},"
                      "\"role\":{\"type\":\"integer\"},\"capability\":{\"type\":\"integer\"},\"source\":{"
                      "\"type\":\"integer\"},\"minimumConfidenceQ16\":{\"type\":\"integer\"},\"text\":{"
                      "\"type\":\"string\"},\"textMatch\":{\"enum\":[\"exact\",\"prefix\",\"substring\"]},"
                      "\"maximumResults\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":10000},"
                      "\"afterGeneration\":{\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{"
                      "\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991}]}}}},"
                      "{\"name\":\"saccade_act\",\"description\":\"Execute one generation-validated "
                      "immediate desktop action. 64-bit identifiers are decimal strings.\",\"inputSchema\":{"
                      "\"type\":\"object\",\"properties\":{"
                      "\"kind\":{\"enum\":[\"move\",\"hover\",\"click\",\"hold\",\"drag\",\"scroll\","
                      "\"key\",\"key-chord\",\"text\",\"window\",\"release\",\"text-select\",\"invoke\","
                      "\"cycle\",\"abort\",\"physical\"]},"
                      "\"generation\":{\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":"
                      "\"integer\",\"minimum\":0,\"maximum\":9007199254740991}]},\"targetId\":{\"oneOf\":[{"
                      "\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,"
                      "\"maximum\":9007199254740991}]},\"secondaryTargetId\":{"
                      "\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\","
                      "\"minimum\":0,\"maximum\":9007199254740991}]},\"processId\":{\"oneOf\":[{\"type\":\"string\","
                      "\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,\"maximum\":9007199254740991}]},"
                      "\"windowId\":{\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\","
                      "\"minimum\":0,\"maximum\":9007199254740991}]},"
                      "\"displayId\":{\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":"
                      "\"integer\",\"minimum\":0,\"maximum\":9007199254740991}]},\"transformEpoch\":{\"oneOf\":[{"
                      "\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,"
                      "\"maximum\":9007199254740991}]},\"permissionEpoch\":{"
                      "\"oneOf\":[{\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\","
                      "\"minimum\":0,\"maximum\":9007199254740991}]},\"physicalSequence\":{\"oneOf\":[{"
                      "\"type\":\"string\",\"pattern\":\"^[0-9]+$\"},{\"type\":\"integer\",\"minimum\":0,"
                      "\"maximum\":9007199254740991}]},\"expectedButtons\":{\"type\":"
                      "\"integer\"},\"expectedModifiers\":{\"type\":\"integer\"},\"dryRun\":{\"type\":"
                      "\"boolean\"},\"verifyNextGeneration\":{\"type\":\"boolean\"},\"button\":{\"type\":\"integer\"},"
                      "\"modifiers\":{\"type\":\"integer\"},\"keyUsage\":{\"type\":\"integer\"},"
                      "\"repeat\":{\"type\":\"integer\"},\"deltaXQ8\":{\"type\":\"integer\"},"
                      "\"deltaYQ8\":{\"type\":\"integer\"},\"xQ8\":{\"type\":\"integer\"},"
                      "\"yQ8\":{\"type\":\"integer\"},\"secondaryXQ8\":{\"type\":\"integer\"},"
                      "\"secondaryYQ8\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"},"
                      "\"backward\":{\"type\":\"boolean\"}},\"required\":[\"kind\"]}}]}}\n");
}

class Server final {
  public:
    explicit Server(ServerStorage* storage) noexcept : storage_(storage) {}

    bool process(const JsonDocument& document) noexcept {
        if (document.count == 0 || document.tokens[0].type != JSMN_OBJECT)
            return write_error(document, -1, -32600, "Invalid Request");
        const int method = field(document, 0, "method");
        const int id = field(document, 0, "id");
        if (method < 0 || document.tokens[method].type != JSMN_STRING)
            return write_error(document, id, -32600, "Invalid Request");
        if (token_equals(document, method, "notifications/initialized") ||
            token_equals(document, method, "notifications/cancelled"))
            return true;
        if (id < 0) return true;
        if (token_equals(document, method, "initialize")) {
            initialized_ = true;
            return write_initialize(document, id);
        }
        if (!initialized_) return write_error(document, id, -32002, "Server is not initialized");
        if (token_equals(document, method, "ping"))
            return response_prefix(document, id) && write_text(",\"result\":{}}\n");
        if (token_equals(document, method, "tools/list")) return write_tools(document, id);
        if (!token_equals(document, method, "tools/call")) return write_error(document, id, -32601, "Method not found");
        const int params = field(document, 0, "params");
        const int name = field(document, params, "name");
        const int arguments = field(document, params, "arguments");
        if (name < 0 || arguments < 0 || document.tokens[arguments].type != JSMN_OBJECT)
            return write_error(document, id, -32602, "Invalid tool arguments");
        if (token_equals(document, name, "saccade_observe")) return observe(document, id, arguments);
        if (token_equals(document, name, "saccade_query")) return query(document, id, arguments);
        if (token_equals(document, name, "saccade_act")) return act(document, id, arguments);
        return write_error(document, id, -32602, "Unknown tool");
    }

  private:
    bool ensure_client() noexcept {
        if (connected_) return true;
        if (!client_.connect()) {
            client_.close();
            return false;
        }
        constexpr SaccadeAgentCapabilityBits requested =
            SACCADE_AGENT_CAPABILITY_OBSERVE | SACCADE_AGENT_CAPABILITY_POINTER | SACCADE_AGENT_CAPABILITY_KEYBOARD |
            SACCADE_AGENT_CAPABILITY_WINDOW;
        if (!client_.hello(requested, &storage_->agent, &granted_)) {
            client_.close();
            return false;
        }
        connected_ = true;
        return true;
    }

    void disconnect_client() noexcept {
        client_.close();
        connected_ = false;
        granted_ = 0;
    }

    bool transact(const void* request, size_t request_size, size_t* response_size, bool retry_safe) noexcept {
        if (client_.transact(request, request_size, &storage_->agent, response_size)) return true;
        disconnect_client();
        if (!ensure_client() || !retry_safe) return false;
        if (client_.transact(request, request_size, &storage_->agent, response_size)) return true;
        disconnect_client();
        return false;
    }

    bool write_tool_error(const JsonDocument& document, int id, std::string_view message) noexcept {
        return response_prefix(document, id) && write_text(",\"result\":{\"content\":[{\"type\":\"text\",\"text\":") &&
               write_json_string(reinterpret_cast<const uint8_t*>(message.data()), message.size()) &&
               write_text("}],\"isError\":true}}\n");
    }

    bool write_targets(const JsonDocument& document, int id, const uint8_t* response, size_t response_size,
                       uint32_t targets_offset, uint32_t target_stride, uint32_t target_count,
                       const SaccadeAgentGeneration& generation, const SaccadeAgentScope& scope,
                       uint32_t message_flags) noexcept {
        if (target_stride != sizeof(SaccadeAgentTarget) || targets_offset > response_size ||
            static_cast<size_t>(target_count) * target_stride > response_size - targets_offset)
            return write_tool_error(document, id, "Invalid service response");
        saccade::core::StackStringBuilder<256> summary;
        summary.append("Observed ");
        summary.append_unsigned(target_count);
        summary.append(" targets at generation ");
        summary.append_unsigned(generation.generation);
        summary.append('.');
        if (!response_prefix(document, id) || !write_text(",\"result\":{\"content\":[{\"type\":\"text\",\"text\":") ||
            !write_json_string(reinterpret_cast<const uint8_t*>(summary.view().data()), summary.view().size()) ||
            !write_text("}],\"structuredContent\":{\"generation\":\""))
            return false;
        saccade::core::StackStringBuilder<1024> prefix;
        if (!prefix.append_unsigned(generation.generation) || !prefix.append("\",\"epochs\":{\"scene\":\"") ||
            !prefix.append_unsigned(generation.scene_epoch) || !prefix.append("\",\"damage\":\"") ||
            !prefix.append_unsigned(generation.damage_epoch) || !prefix.append("\",\"transform\":\"") ||
            !prefix.append_unsigned(generation.transform_epoch) || !prefix.append("\",\"permission\":\"") ||
            !prefix.append_unsigned(generation.permission_epoch) || !prefix.append("\",\"topology\":\"") ||
            !prefix.append_unsigned(generation.topology_epoch) || !prefix.append("\"},\"context\":{\"processId\":\"") ||
            !prefix.append_unsigned(generation.focus_id) || !prefix.append("\",\"windowId\":\"") ||
            !prefix.append_unsigned(generation.window_id) || !prefix.append("\",\"displayId\":\"") ||
            !prefix.append_unsigned(generation.display_id) || !prefix.append("\"},\"scope\":{\"kind\":\"") ||
            !prefix.append(scope_name(scope.kind)) || !prefix.append("\",\"sourceMode\":\"") ||
            !prefix.append(source_mode_name(scope.source_mode)) || !prefix.append("\",\"stableId\":\"") ||
            !prefix.append_unsigned(scope.stable_id) || !prefix.append("\",\"xQ8\":") ||
            !prefix.append_signed(scope.rect.x_q8) || !prefix.append(",\"yQ8\":") ||
            !prefix.append_signed(scope.rect.y_q8) || !prefix.append(",\"widthQ8\":") ||
            !prefix.append_signed(scope.rect.width_q8) || !prefix.append(",\"heightQ8\":") ||
            !prefix.append_signed(scope.rect.height_q8) || !prefix.append("},\"truncated\":") ||
            !prefix.append((message_flags & SACCADE_AGENT_MESSAGE_TRUNCATED) != 0 ? "true" : "false") ||
            !prefix.append(",\"sourceIncomplete\":") ||
            !prefix.append((message_flags & SACCADE_AGENT_MESSAGE_SOURCE_INCOMPLETE) != 0 ? "true" : "false") ||
            !prefix.append(",\"targets\":[") || !write_text(prefix.view()))
            return false;
        for (uint32_t index = 0; index < target_count; ++index) {
            SaccadeAgentTarget target{};
            std::memcpy(&target, response + targets_offset + static_cast<size_t>(index) * target_stride,
                        sizeof(target));
            saccade::core::StackStringBuilder<768> row;
            if (!row.append(index == 0 ? "{\"index\":" : ",{\"index\":") || !row.append_unsigned(index) ||
                !row.append(",\"id\":\"") || !row.append_unsigned(target.target_id) ||
                !row.append("\",\"parentId\":\"") || !row.append_unsigned(target.parent_id) ||
                !row.append("\",\"windowId\":\"") || !row.append_unsigned(target.window_id) ||
                !row.append("\",\"displayId\":\"") || !row.append_unsigned(target.display_id) ||
                !row.append("\",\"role\":\"") || !row.append(role_name(target.role)) ||
                !row.append("\",\"roleCode\":") || !row.append_unsigned(target.role) ||
                !row.append(",\"capabilities\":") || !row.append_unsigned(target.capability_bits) ||
                !row.append(",\"flags\":") || !row.append_unsigned(target.flags) || !row.append(",\"sourceBits\":") ||
                !row.append_unsigned(target.source_bits) || !row.append(",\"confidenceQ16\":") ||
                !row.append_unsigned(target.confidence_q16) || !row.append(",\"order\":") ||
                !row.append_unsigned(target.order) || !row.append(",\"xQ8\":") ||
                !row.append_signed(target.bounds.x_q8) || !row.append(",\"yQ8\":") ||
                !row.append_signed(target.bounds.y_q8) || !row.append(",\"widthQ8\":") ||
                !row.append_signed(target.bounds.width_q8) || !row.append(",\"heightQ8\":") ||
                !row.append_signed(target.bounds.height_q8) || !row.append(",\"safeXQ8\":") ||
                !row.append_signed(target.safe_point.x_q8) || !row.append(",\"safeYQ8\":") ||
                !row.append_signed(target.safe_point.y_q8))
                return false;
            if (target.text_size != 0) {
                if (target.text_offset > response_size || target.text_size > response_size - target.text_offset ||
                    !row.append(",\"text\":") || !write_text(row.view()) ||
                    !write_json_string(response + target.text_offset, target.text_size) || !write_text("}"))
                    return false;
            } else if (!row.append("}") || !write_text(row.view())) {
                return false;
            }
        }
        return write_text("]}}}\n");
    }

    bool observe(const JsonDocument& document, int id, int arguments) noexcept {
        uint64_t maximum = default_maximum_results;
        const int maximum_token = field(document, arguments, "maximumTargets");
        if (maximum_token >= 0 && !parse_u64(document, maximum_token, &maximum))
            return write_error(document, id, -32602, "Invalid maximumTargets");
        if (maximum == 0 || maximum > SACCADE_AGENT_MAX_TARGETS)
            return write_error(document, id, -32602, "maximumTargets is out of range");
        SaccadeAgentObserveRequest request{};
        request.header = {sizeof(request), SACCADE_AGENT_API_VERSION, SACCADE_AGENT_MESSAGE_OBSERVE_REQUEST, 0};
        request.request_id = 2;
        request.scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
        request.scope.source_mode = SACCADE_AGENT_SOURCE_FUSED;
        request.freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
        request.freshness.timeout_ns = UINT64_C(2'000'000'000);
        request.requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
        request.maximum_targets = static_cast<uint32_t>(maximum);
        request.target_stride = sizeof(SaccadeAgentTarget);
        request.total_capacity = SACCADE_AGENT_MAX_MESSAGE_BYTES;
        if (!parse_scope(document, arguments, &request.scope))
            return write_error(document, id, -32602, "Invalid observation scope");
        const int after_generation = field(document, arguments, "afterGeneration");
        if (after_generation >= 0) {
            if (!parse_u64(document, after_generation, &request.freshness.after_generation))
                return write_error(document, id, -32602, "Invalid afterGeneration");
            request.freshness.policy = SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
        }
        if (!ensure_client()) return write_tool_error(document, id, "Saccade service is unavailable");
        size_t response_size = 0;
        if (!transact(&request, sizeof(request), &response_size, true) ||
            response_size < sizeof(SaccadeAgentObserveCompletion))
            return write_tool_error(document, id, "Observe transport failed");
        SaccadeAgentObserveCompletion completion{};
        std::memcpy(&completion, storage_->agent.response.data(), sizeof(completion));
        if (completion.result != SACCADE_AGENT_OK) return write_tool_error(document, id, "Observe was rejected");
        return write_targets(document, id, storage_->agent.response.data(), response_size, completion.targets_offset,
                             completion.target_stride, completion.target_count, completion.generation, completion.scope,
                             completion.header.flags);
    }

    bool query(const JsonDocument& document, int id, int arguments) noexcept {
        constexpr size_t prefix_size = sizeof(SaccadeAgentQueryRequest) + sizeof(SaccadeAgentQueryFilter);
        std::memset(storage_->agent.request.data(), 0, prefix_size);
        auto* request = reinterpret_cast<SaccadeAgentQueryRequest*>(storage_->agent.request.data());
        auto* filter = reinterpret_cast<SaccadeAgentQueryFilter*>(storage_->agent.request.data() + sizeof(*request));
        request->header = {sizeof(*request), SACCADE_AGENT_API_VERSION, SACCADE_AGENT_MESSAGE_QUERY_REQUEST, 0};
        request->request_id = 3;
        request->scope.kind = SACCADE_AGENT_SCOPE_DESKTOP;
        request->scope.source_mode = SACCADE_AGENT_SOURCE_FUSED;
        request->requested_capability_bits = SACCADE_AGENT_CAPABILITY_OBSERVE;
        request->maximum_results = default_maximum_results;
        request->filter_count = 1;
        request->filter_stride = sizeof(*filter);
        request->filters_offset = sizeof(*request);
        request->freshness.policy = SACCADE_AGENT_FRESHNESS_LATEST_VALID;
        request->freshness.timeout_ns = UINT64_C(2'000'000'000);
        if (!parse_scope(document, arguments, &request->scope))
            return write_error(document, id, -32602, "Invalid query scope");
        uint64_t value = 0;
        if (!optional_u64(document, arguments, "generation", &request->generation) ||
            !optional_u64(document, arguments, "targetId", &filter->target_id))
            return write_error(document, id, -32602, "Invalid query integer");
        const int after_generation = field(document, arguments, "afterGeneration");
        if (after_generation >= 0) {
            if (field(document, arguments, "generation") >= 0 ||
                !parse_u64(document, after_generation, &request->freshness.after_generation))
                return write_error(document, id, -32602, "Invalid afterGeneration");
            request->freshness.policy = SACCADE_AGENT_FRESHNESS_AFTER_GENERATION;
        }
        if (field(document, arguments, "targetId") >= 0) filter->flags |= SACCADE_AGENT_QUERY_STABLE_ID;
        if (!query_u32(document, arguments, "role", SACCADE_AGENT_QUERY_ROLE, &filter->role, &filter->flags) ||
            !query_u32(document, arguments, "capability", SACCADE_AGENT_QUERY_CAPABILITY,
                       &filter->required_capability_bits, &filter->flags) ||
            !query_u32(document, arguments, "source", SACCADE_AGENT_QUERY_SOURCE, &filter->source_bits,
                       &filter->flags) ||
            !query_u32(document, arguments, "minimumConfidenceQ16", SACCADE_AGENT_QUERY_CONFIDENCE,
                       &filter->minimum_confidence_q16, &filter->flags))
            return write_error(document, id, -32602, "Invalid query integer");
        const int maximum = field(document, arguments, "maximumResults");
        if (maximum >= 0 && (!parse_u64(document, maximum, &value) || value == 0 || value > SACCADE_AGENT_MAX_TARGETS))
            return write_error(document, id, -32602, "maximumResults is out of range");
        if (maximum >= 0) request->maximum_results = static_cast<uint32_t>(value);
        size_t request_size = prefix_size;
        const int text = field(document, arguments, "text");
        if (text >= 0) {
            size_t text_size = 0;
            if (!decode_string(document, text, storage_->agent.request.data() + request_size,
                               SACCADE_AGENT_MAX_TEXT_BYTES, &text_size) ||
                text_size == 0)
                return write_error(document, id, -32602, "Invalid text");
            filter->flags |= SACCADE_AGENT_QUERY_TEXT;
            filter->text_offset = static_cast<uint32_t>(request_size);
            filter->text_size = static_cast<uint32_t>(text_size);
            filter->text_match = SACCADE_AGENT_TEXT_EXACT;
            request_size += text_size;
            const int match = field(document, arguments, "textMatch");
            if (match >= 0) {
                filter->text_match = token_equals(document, match, "exact")       ? SACCADE_AGENT_TEXT_EXACT
                                     : token_equals(document, match, "prefix")    ? SACCADE_AGENT_TEXT_PREFIX
                                     : token_equals(document, match, "substring") ? SACCADE_AGENT_TEXT_SUBSTRING
                                                                                  : 0;
                if (filter->text_match == 0) return write_error(document, id, -32602, "Invalid textMatch");
            }
        }
        request->total_size = static_cast<uint32_t>(request_size);
        if (!ensure_client()) return write_tool_error(document, id, "Saccade service is unavailable");
        size_t response_size = 0;
        if (!transact(storage_->agent.request.data(), request_size, &response_size, true) ||
            response_size < sizeof(SaccadeAgentQueryCompletion))
            return write_tool_error(document, id, "Query transport failed");
        SaccadeAgentQueryCompletion completion{};
        std::memcpy(&completion, storage_->agent.response.data(), sizeof(completion));
        if (completion.result != SACCADE_AGENT_OK) return write_tool_error(document, id, "Query was rejected");
        return write_targets(document, id, storage_->agent.response.data(), response_size, completion.targets_offset,
                             completion.target_stride, completion.target_count, completion.generation, completion.scope,
                             completion.header.flags);
    }

    bool act(const JsonDocument& document, int id, int arguments) noexcept {
        auto* batch = reinterpret_cast<SaccadeAgentActionBatch*>(storage_->agent.request.data());
        auto* action = reinterpret_cast<SaccadeAgentAction*>(storage_->agent.request.data() + sizeof(*batch));
        *batch = {};
        *action = {};
        batch->header = {sizeof(*batch), SACCADE_AGENT_API_VERSION, SACCADE_AGENT_MESSAGE_ACTION_BATCH, 0};
        batch->request_id = 4;
        batch->policy = SACCADE_AGENT_BATCH_STOP_ON_FAILURE;
        batch->deadline_ns = saccade::tools::monotonic_time_ns() + UINT64_C(2'000'000'000);
        batch->action_count = 1;
        batch->action_stride = sizeof(*action);
        batch->actions_offset = sizeof(*batch);
        batch->payload_offset = sizeof(*batch) + sizeof(*action);
        batch->total_size = batch->payload_offset;
        action->button_bits = SACCADE_AGENT_BUTTON_LEFT;
        action->repeat_count = 1;
        const int kind = field(document, arguments, "kind");
        if (!parse_action_kind(document, kind, action, &batch->requested_capability_bits))
            return write_error(document, id, -32602, "Invalid action kind");
        if (!optional_u64(document, arguments, "generation", &batch->preconditions.generation) ||
            !optional_u64(document, arguments, "targetId", &action->target_id) ||
            !optional_u64(document, arguments, "secondaryTargetId", &action->secondary_target_id))
            return write_error(document, id, -32602, "Invalid action integer");
        if (field(document, arguments, "generation") >= 0)
            batch->preconditions.flags |= SACCADE_AGENT_PRECONDITION_GENERATION;
        if (!precondition_u64(document, arguments, "processId", SACCADE_AGENT_PRECONDITION_FOCUS,
                              &batch->preconditions.focus_id, &batch->preconditions.flags) ||
            !precondition_u64(document, arguments, "windowId", SACCADE_AGENT_PRECONDITION_WINDOW,
                              &batch->preconditions.window_id, &batch->preconditions.flags) ||
            !precondition_u64(document, arguments, "displayId", SACCADE_AGENT_PRECONDITION_DISPLAY,
                              &batch->preconditions.display_id, &batch->preconditions.flags) ||
            !precondition_u64(document, arguments, "transformEpoch", SACCADE_AGENT_PRECONDITION_TRANSFORM,
                              &batch->preconditions.transform_epoch, &batch->preconditions.flags) ||
            !precondition_u64(document, arguments, "permissionEpoch", SACCADE_AGENT_PRECONDITION_PERMISSION,
                              &batch->preconditions.permission_epoch, &batch->preconditions.flags))
            return write_error(document, id, -32602, "Invalid action precondition");
        if (!action_u32(document, arguments, "button", &action->button_bits) ||
            !action_u32(document, arguments, "modifiers", &action->modifiers) ||
            !action_u32(document, arguments, "keyUsage", &action->key_usage) ||
            !action_u32(document, arguments, "repeat", &action->repeat_count) ||
            !action_i32(document, arguments, "deltaXQ8", &action->delta_x_q8) ||
            !action_i32(document, arguments, "deltaYQ8", &action->delta_y_q8))
            return write_error(document, id, -32602, "Invalid action value");

        const int physical_sequence = field(document, arguments, "physicalSequence");
        const int expected_buttons = field(document, arguments, "expectedButtons");
        const int expected_modifiers = field(document, arguments, "expectedModifiers");
        if ((expected_buttons >= 0 || expected_modifiers >= 0) && physical_sequence < 0)
            return write_error(document, id, -32602, "Physical expectations require physicalSequence");
        if (physical_sequence >= 0) {
            if (!parse_u64(document, physical_sequence, &batch->preconditions.physical_sequence) ||
                !action_u32(document, arguments, "expectedButtons", &batch->preconditions.expected_buttons) ||
                !action_u32(document, arguments, "expectedModifiers", &batch->preconditions.expected_modifiers))
                return write_error(document, id, -32602, "Invalid physical precondition");
            batch->preconditions.flags |= SACCADE_AGENT_PRECONDITION_PHYSICAL_STATE;
        }

        const int dry_run = field(document, arguments, "dryRun");
        bool dry_run_value = false;
        if (dry_run >= 0 && !parse_bool(document, dry_run, &dry_run_value))
            return write_error(document, id, -32602, "Invalid dryRun value");
        if (dry_run_value) batch->header.flags |= SACCADE_AGENT_BATCH_DRY_RUN;
        const int verify = field(document, arguments, "verifyNextGeneration");
        bool verify_value = false;
        if (verify >= 0 && !parse_bool(document, verify, &verify_value))
            return write_error(document, id, -32602, "Invalid verifyNextGeneration value");
        if (verify_value) batch->header.flags |= SACCADE_AGENT_BATCH_VERIFY_NEXT_GENERATION;

        bool point_present = false;
        bool secondary_point_present = false;
        if (!action_point(document, arguments, "xQ8", "yQ8", &action->point, &point_present) ||
            !action_point(document, arguments, "secondaryXQ8", "secondaryYQ8", &action->secondary_point,
                          &secondary_point_present))
            return write_error(document, id, -32602, "Action points require coordinate pairs");
        const bool dual_target =
            action->kind == SACCADE_AGENT_ACTION_DRAG_DROP || action->kind == SACCADE_AGENT_ACTION_TEXT_SELECT;
        const bool point_action =
            action->kind == SACCADE_AGENT_ACTION_POINTER_MOVE || action->kind == SACCADE_AGENT_ACTION_POINTER_HOVER ||
            action->kind == SACCADE_AGENT_ACTION_CLICK || action->kind == SACCADE_AGENT_ACTION_HOLD ||
            action->kind == SACCADE_AGENT_ACTION_DRAG_DROP || action->kind == SACCADE_AGENT_ACTION_SCROLL ||
            action->kind == SACCADE_AGENT_ACTION_TEXT || action->kind == SACCADE_AGENT_ACTION_TEXT_SELECT ||
            action->kind == SACCADE_AGENT_ACTION_INVOKE;
        if (secondary_point_present != (point_present && dual_target) || (point_present && !point_action))
            return write_error(document, id, -32602, "Invalid points for action kind");
        if (point_present) action->flags |= SACCADE_AGENT_ACTION_EXPLICIT_POINTS;

        const int backward = field(document, arguments, "backward");
        bool backward_value = false;
        if (backward >= 0 && (!parse_bool(document, backward, &backward_value)))
            return write_error(document, id, -32602, "Invalid backward value");
        if (backward_value) action->flags |= SACCADE_AGENT_ACTION_CYCLE_BACKWARD;
        const int text = field(document, arguments, "text");
        if (text >= 0) {
            size_t text_size = 0;
            if (!decode_string(document, text, storage_->agent.request.data() + batch->payload_offset,
                               SACCADE_AGENT_MAX_TEXT_BYTES, &text_size) ||
                text_size == 0)
                return write_error(document, id, -32602, "Invalid action text");
            action->payload_size = static_cast<uint32_t>(text_size);
            batch->payload_size = static_cast<uint32_t>(text_size);
            batch->total_size += static_cast<uint32_t>(text_size);
        }
        if (!ensure_client()) return write_tool_error(document, id, "Saccade service is unavailable");
        size_t response_size = 0;
        if (!transact(storage_->agent.request.data(), batch->total_size, &response_size, false) ||
            response_size < sizeof(SaccadeAgentActionCompletion))
            return write_tool_error(document, id, "Action transport failed");
        SaccadeAgentActionCompletion completion{};
        std::memcpy(&completion, storage_->agent.response.data(), sizeof(completion));
        if (!response_prefix(document, id) ||
            !write_text(completion.result == SACCADE_AGENT_OK
                            ? ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Action completed.\"}],"
                              "\"structuredContent\":{\"result\":"
                            : ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"Action rejected.\"}],"
                              "\"structuredContent\":{\"result\":"))
            return false;
        saccade::core::StackStringBuilder<1024> result;
        return result.append_signed(completion.result) && result.append(",\"completedActions\":") &&
               result.append_unsigned(completion.completed_action_count) && result.append(",\"failedAction\":") &&
               result.append_unsigned(completion.failed_action_index) && result.append(",\"generation\":\"") &&
               result.append_unsigned(completion.validated_generation) && result.append("\",\"physical\":{\"xQ8\":") &&
               result.append_signed(completion.physical_state.pointer.x_q8) && result.append(",\"yQ8\":") &&
               result.append_signed(completion.physical_state.pointer.y_q8) && result.append(",\"buttons\":") &&
               result.append_unsigned(completion.physical_state.buttons) && result.append(",\"modifiers\":") &&
               result.append_unsigned(completion.physical_state.modifiers) && result.append(",\"activeLeaseId\":\"") &&
               result.append_unsigned(completion.physical_state.active_lease_id) &&
               result.append("\",\"permissionEpoch\":\"") &&
               result.append_unsigned(completion.physical_state.permission_epoch) &&
               result.append("\",\"sequence\":\"") &&
               result.append_unsigned(completion.physical_state.physical_sequence) && result.append("\",\"flags\":") &&
               result.append_unsigned(completion.physical_state.flags) && result.append("}") &&
               ((completion.header.flags & SACCADE_AGENT_MESSAGE_NEXT_GENERATION_AVAILABLE) == 0 ||
                (result.append(",\"nextGeneration\":{\"generation\":\"") &&
                 result.append_unsigned(completion.next_generation.generation) &&
                 result.append("\",\"sceneEpoch\":\"") &&
                 result.append_unsigned(completion.next_generation.scene_epoch) &&
                 result.append("\",\"damageEpoch\":\"") &&
                 result.append_unsigned(completion.next_generation.damage_epoch) &&
                 result.append("\",\"processId\":\"") && result.append_unsigned(completion.next_generation.focus_id) &&
                 result.append("\",\"windowId\":\"") && result.append_unsigned(completion.next_generation.window_id) &&
                 result.append("\",\"displayId\":\"") &&
                 result.append_unsigned(completion.next_generation.display_id) && result.append("\"}"))) &&
               result.append("},\"isError\":") &&
               result.append(completion.result == SACCADE_AGENT_OK ? "false" : "true") && result.append("}}\n") &&
               write_text(result.view());
    }

    bool parse_scope(const JsonDocument& document, int arguments, SaccadeAgentScope* scope) noexcept {
        const int kind = field(document, arguments, "scope");
        if (kind >= 0) {
            scope->kind = token_equals(document, kind, "active-window") ? SACCADE_AGENT_SCOPE_ACTIVE_WINDOW
                          : token_equals(document, kind, "display")     ? SACCADE_AGENT_SCOPE_DISPLAY
                          : token_equals(document, kind, "desktop")     ? SACCADE_AGENT_SCOPE_DESKTOP
                          : token_equals(document, kind, "rect")        ? SACCADE_AGENT_SCOPE_RECT
                                                                        : 0;
            if (scope->kind == 0) return false;
        }

        const int source_mode = field(document, arguments, "sourceMode");
        if (source_mode >= 0) {
            scope->source_mode = token_equals(document, source_mode, "pixel")      ? SACCADE_AGENT_SOURCE_PIXEL
                                 : token_equals(document, source_mode, "semantic") ? SACCADE_AGENT_SOURCE_SEMANTIC
                                 : token_equals(document, source_mode, "grid")     ? SACCADE_AGENT_SOURCE_GRID
                                 : token_equals(document, source_mode, "fused")    ? SACCADE_AGENT_SOURCE_FUSED
                                                                                   : 0;
            if (scope->source_mode == 0) return false;
        }

        const int stable_id = field(document, arguments, "scopeId");
        if (stable_id >= 0 && !parse_u64(document, stable_id, &scope->stable_id)) return false;

        const int x = field(document, arguments, "xQ8");
        const int y = field(document, arguments, "yQ8");
        const int width = field(document, arguments, "widthQ8");
        const int height = field(document, arguments, "heightQ8");
        const bool any_rect_component = x >= 0 || y >= 0 || width >= 0 || height >= 0;
        const bool complete_rect = x >= 0 && y >= 0 && width >= 0 && height >= 0;
        if (any_rect_component && !complete_rect) return false;
        if (complete_rect &&
            (!parse_i32(document, x, &scope->rect.x_q8) || !parse_i32(document, y, &scope->rect.y_q8) ||
             !parse_i32(document, width, &scope->rect.width_q8) ||
             !parse_i32(document, height, &scope->rect.height_q8)))
            return false;

        if (scope->kind == SACCADE_AGENT_SCOPE_RECT)
            return complete_rect && scope->stable_id == 0 && scope->rect.width_q8 > 0 && scope->rect.height_q8 > 0;
        if (any_rect_component) return false;
        if (scope->kind == SACCADE_AGENT_SCOPE_DISPLAY) return scope->stable_id != 0;
        return scope->kind == SACCADE_AGENT_SCOPE_ACTIVE_WINDOW ||
               (scope->kind == SACCADE_AGENT_SCOPE_DESKTOP && scope->stable_id == 0);
    }

    bool optional_u64(const JsonDocument& document, int arguments, std::string_view name, uint64_t* output) noexcept {
        const int token = field(document, arguments, name);
        return token < 0 || parse_u64(document, token, output);
    }

    bool precondition_u64(const JsonDocument& document, int arguments, std::string_view name, uint32_t flag,
                          uint64_t* output, uint32_t* flags) noexcept {
        const int token = field(document, arguments, name);
        if (token < 0) return true;
        if (!parse_u64(document, token, output)) return false;
        *flags |= flag;
        return true;
    }

    template <typename Value>
    bool query_u32(const JsonDocument& document, int arguments, std::string_view name, uint32_t flag, Value* output,
                   uint32_t* flags) noexcept {
        const int token = field(document, arguments, name);
        if (token < 0) return true;
        uint64_t value = 0;
        if (!parse_u64(document, token, &value) || value > UINT32_MAX) return false;
        *output = static_cast<Value>(value);
        *flags |= flag;
        return true;
    }

    template <typename Value>
    bool action_u32(const JsonDocument& document, int arguments, std::string_view name, Value* output) noexcept {
        const int token = field(document, arguments, name);
        if (token < 0) return true;
        uint64_t value = 0;
        if (!parse_u64(document, token, &value) || value > UINT32_MAX) return false;
        *output = static_cast<Value>(value);
        return true;
    }

    bool action_i32(const JsonDocument& document, int arguments, std::string_view name, int32_t* output) noexcept {
        const int token = field(document, arguments, name);
        return token < 0 || parse_i32(document, token, output);
    }

    bool action_point(const JsonDocument& document, int arguments, std::string_view x_name, std::string_view y_name,
                      SaccadeAgentPointQ8* output, bool* present) noexcept {
        const int x = field(document, arguments, x_name);
        const int y = field(document, arguments, y_name);
        if ((x < 0) != (y < 0)) return false;
        *present = x >= 0;
        return !*present || (parse_i32(document, x, &output->x_q8) && parse_i32(document, y, &output->y_q8));
    }

    bool parse_action_kind(const JsonDocument& document, int kind, SaccadeAgentAction* action,
                           SaccadeAgentCapabilityBits* capabilities) noexcept {
        if (kind < 0 || document.tokens[kind].type != JSMN_STRING) return false;
        if (token_equals(document, kind, "move"))
            action->kind = SACCADE_AGENT_ACTION_POINTER_MOVE;
        else if (token_equals(document, kind, "hover"))
            action->kind = SACCADE_AGENT_ACTION_POINTER_HOVER;
        else if (token_equals(document, kind, "click"))
            action->kind = SACCADE_AGENT_ACTION_CLICK;
        else if (token_equals(document, kind, "hold"))
            action->kind = SACCADE_AGENT_ACTION_HOLD;
        else if (token_equals(document, kind, "drag"))
            action->kind = SACCADE_AGENT_ACTION_DRAG_DROP;
        else if (token_equals(document, kind, "scroll"))
            action->kind = SACCADE_AGENT_ACTION_SCROLL;
        else if (token_equals(document, kind, "key"))
            action->kind = SACCADE_AGENT_ACTION_KEY;
        else if (token_equals(document, kind, "key-chord"))
            action->kind = SACCADE_AGENT_ACTION_KEY_CHORD;
        else if (token_equals(document, kind, "text"))
            action->kind = SACCADE_AGENT_ACTION_TEXT;
        else if (token_equals(document, kind, "window"))
            action->kind = SACCADE_AGENT_ACTION_WINDOW_ACTIVATE;
        else if (token_equals(document, kind, "release"))
            action->kind = SACCADE_AGENT_ACTION_RELEASE;
        else if (token_equals(document, kind, "text-select"))
            action->kind = SACCADE_AGENT_ACTION_TEXT_SELECT;
        else if (token_equals(document, kind, "invoke"))
            action->kind = SACCADE_AGENT_ACTION_INVOKE;
        else if (token_equals(document, kind, "cycle"))
            action->kind = SACCADE_AGENT_ACTION_WINDOW_CYCLE;
        else if (token_equals(document, kind, "abort"))
            action->kind = SACCADE_AGENT_ACTION_ABORT;
        else if (token_equals(document, kind, "physical"))
            action->kind = SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE;
        else
            return false;

        switch (action->kind) {
        case SACCADE_AGENT_ACTION_KEY:
        case SACCADE_AGENT_ACTION_KEY_CHORD:
            *capabilities = SACCADE_AGENT_CAPABILITY_KEYBOARD;
            break;
        case SACCADE_AGENT_ACTION_TEXT:
            *capabilities = SACCADE_AGENT_CAPABILITY_KEYBOARD | SACCADE_AGENT_CAPABILITY_POINTER;
            break;
        case SACCADE_AGENT_ACTION_WINDOW_ACTIVATE:
        case SACCADE_AGENT_ACTION_WINDOW_CYCLE:
            *capabilities = SACCADE_AGENT_CAPABILITY_WINDOW;
            break;
        case SACCADE_AGENT_ACTION_QUERY_PHYSICAL_STATE:
            *capabilities = SACCADE_AGENT_CAPABILITY_OBSERVE;
            break;
        case SACCADE_AGENT_ACTION_ABORT:
            *capabilities = SACCADE_AGENT_CAPABILITY_POINTER;
            break;
        default:
            *capabilities = SACCADE_AGENT_CAPABILITY_POINTER;
            break;
        }
        return true;
    }

    ServerStorage* storage_ = nullptr;
    saccade::tools::AgentClient client_{};
    SaccadeAgentCapabilityBits granted_ = 0;
    bool connected_ = false;
    bool initialized_ = false;
};

} // namespace

int main() {
    static ServerStorage storage;
    Server server(&storage);
    while (std::fgets(storage.input.data(), static_cast<int>(storage.input.size()), stdin) != nullptr) {
        const size_t size = std::strlen(storage.input.data());
        if (size == 0) continue;
        if (storage.input[size - 1U] != '\n' && !std::feof(stdin)) return static_cast<int>(ExitCode::input_failed);
        const size_t json_size = storage.input[size - 1U] == '\n' ? size - 1U : size;
        jsmn_parser parser{};
        jsmn_init(&parser);
        const int count = jsmn_parse(&parser, storage.input.data(), json_size, storage.tokens.data(),
                                     static_cast<unsigned int>(storage.tokens.size()));
        JsonDocument document{storage.input.data(), json_size, storage.tokens.data(), count > 0 ? count : 0};
        const bool written = count > 0 ? server.process(document) : write_error(document, -1, -32700, "Parse error");
        if (!written || std::fflush(stdout) != 0) return static_cast<int>(ExitCode::output_failed);
    }
    return std::ferror(stdin) == 0 ? static_cast<int>(ExitCode::success) : static_cast<int>(ExitCode::input_failed);
}
