#include "mint/infrastructure/model_provider_client.hpp"

#include <string>
#include <utility>

namespace mint {
namespace {

void truncate_utf8_at_boundary(std::string& value, std::size_t byte_limit) {
    if (value.size() <= byte_limit) {
        return;
    }

    auto boundary = byte_limit;
    while (boundary > 0 && (static_cast<unsigned char>(value[boundary]) & 0xC0U) == 0x80U) {
        --boundary;
    }
    value.resize(boundary);
    value += "...";
}

ModelReply demo_tool_call(std::string id, std::string name, Json arguments) {
    Json raw_call = {{"id", id},
                     {"type", "function"},
                     {"function", {{"name", name}, {"arguments", arguments.dump()}}}};

    ModelReply reply;
    reply.assistant_message = {
        {"role", "assistant"}, {"content", nullptr}, {"tool_calls", Json::array({raw_call})}};
    reply.tool_calls.push_back(ToolCall{std::move(id), std::move(name), std::move(arguments)});
    return reply;
}

ModelCallMetadata demo_metadata() {
    ModelCallMetadata metadata;
    metadata.adapter = "demo";
    metadata.model = "deterministic-demo";
    return metadata;
}

} // namespace

ModelReply DemoModelClient::complete(const Json& messages, const Json& tools) {
    (void)tools;

    switch (step_++) {
    case 0: {
        auto reply = demo_tool_call("demo-list", "list_files", {{"path", "."}, {"max_depth", 2}});
        reply.metadata = demo_metadata();
        return reply;
    }
    case 1: {
        auto reply = demo_tool_call("demo-search", "search_text",
                                    {{"path", "."}, {"query", "Agent"}, {"case_sensitive", false}});
        reply.metadata = demo_metadata();
        return reply;
    }
    case 2: {
        auto reply = demo_tool_call("demo-read", "read_file", {{"path", "README.md"}});
        reply.metadata = demo_metadata();
        return reply;
    }
    default: {
        std::string last_result;
        if (messages.is_array() && !messages.empty()) {
            const auto& last = messages.back();
            if (last.is_object() && last.contains("content") && last.at("content").is_string()) {
                last_result = last.at("content").get<std::string>();
            }
        }
        constexpr std::size_t evidence_limit = 700;
        truncate_utf8_at_boundary(last_result, evidence_limit);

        ModelReply reply;
        reply.text = "离线演示完成：Agent 已依次列出文件、搜索文本并读取 README.md。"
                     "\n\n最后一次工具结果：\n" +
                     last_result;
        reply.assistant_message = {{"role", "assistant"}, {"content", reply.text}};
        reply.metadata = demo_metadata();
        return reply;
    }
    }
}

} // namespace mint
