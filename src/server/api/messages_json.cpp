#include "taxbroker/api/messages_json.hpp"

#include "taxbroker/diagnostic_messages.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace taxbroker::api {

namespace {

/// The API word of a parameter kind.
std::string_view kindWord(MessageParamKind aKind) {
    switch (aKind)
    {
    case MessageParamKind::Number:
        return "number";
    case MessageParamKind::Text:
        return "text";
    case MessageParamKind::Term:
        return "term";
    }

    throw std::logic_error{"Unknown message parameter kind"};
}

/// A `{"en": ..., "sl": ...}` object for one id of the message table.
nlohmann::json textsOf(std::string_view aId) {
    return {{"en", messageText(aId, MessageLanguage::English)},
            {"sl", messageText(aId, MessageLanguage::Slovenian)}};
}

std::string buildMessagesJson() {
    nlohmann::json messages = nlohmann::json::object();

    for (const MessageDefinition& definition : messageDefinitions())
    {
        nlohmann::json entry = textsOf(definition.mId);
        entry["params"] = nlohmann::json::array();

        for (const MessageParamDefinition& param : definition.mParams)
        {
            entry["params"].push_back(
                {{"name", std::string{param.mName}}, {"kind", std::string{kindWord(param.mKind)}}});
        }

        messages[std::string{definition.mId}] = std::move(entry);
    }

    nlohmann::json terms = nlohmann::json::object();

    for (const std::string_view termId : termKeys())
    {
        terms[std::string{termId}] = textsOf(termId);
    }

    return nlohmann::json{{"messages", std::move(messages)}, {"terms", std::move(terms)}}.dump();
}

} // namespace

const std::string& messagesJson() {
    static const std::string json = buildMessagesJson();

    return json;
}

} // namespace taxbroker::api
