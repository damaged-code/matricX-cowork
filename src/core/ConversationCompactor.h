#pragma once

#include <QList>

#include "ai/AiClient.h"

namespace mcw {

// Fits a conversation into the model's context window before a request is sent.
//
// The house rules (agents.md §14):
//   * operate on a copy — the caller's history and the session store stay
//     untouched;
//   * keep the newest messages whenever the estimate exceeds the budget,
//     dropping the oldest instead of silent truncation mid-conversation;
//   * prepend a short summary of what was dropped so the model keeps its
//     bearings;
//   * images are the most expensive content: only the newest few survive;
//   * consecutive same-role text messages are merged so every provider
//     (notably Gemini and Anthropic) sees valid role alternation.
class ConversationCompactor {
public:
    static QList<ChatMessage> compact(const QList<ChatMessage> &history);

    // Rough token estimate: ~4 chars per token of text, images as a constant.
    static int estimateTokens(const QList<ChatMessage> &history);
};

}  // namespace mcw
