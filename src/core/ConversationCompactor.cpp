#include "core/ConversationCompactor.h"

namespace mcw {

namespace {

// Chosen against current large-context models with a wide safety margin: the
// request itself (tool schemas + system prompt) comfortably fits in what
// remains of any mainstream context window.
constexpr int kContextBudgetTokens = 48000;

// The tail that always survives when compaction is required.
constexpr int kTailBudgetTokens = 28000;

// Screenshot PNGs land here (and get base64-inflated for the providers).
constexpr int kImageTokens = 1100;

// Cap on the prepended summary so old conversations cannot crowd out new ones.
constexpr int kMaxSummaryTokens = 600;

int textTokens(const QString &text) {
    return (text.size() + 3) / 4;
}

int messageTokens(const ChatMessage &msg) {
    int t = textTokens(msg.content);
    if (!msg.imagePath.isEmpty()) t += kImageTokens;
    return t;
}

// 0 = user side (plain user message or persisted tool result), 1 = assistant.
int roleBucket(const ChatMessage &msg) {
    return (msg.role == "user" || msg.role == "tool") ? 0 : 1;
}

QList<ChatMessage> dropImagesBeyondNewest3(QList<ChatMessage> msgs) {
    int kept = 0;
    for (int i = msgs.size() - 1; i >= 0; --i) {
        if (msgs[i].imagePath.isEmpty()) continue;
        if (kept >= 3) msgs[i].imagePath.clear();
        ++kept;
    }
    return msgs;
}

QList<ChatMessage> mergeConsecutiveText(QList<ChatMessage> msgs) {
    QList<ChatMessage> out;
    for (ChatMessage msg : msgs) {
        // Consecutive same-role *text* messages merge into one entry; messages
        // with images keep their own slot (their payload must stay attached
        // to the right text).
        if (!out.isEmpty() && msg.imagePath.isEmpty() && out.last().imagePath.isEmpty() &&
            roleBucket(out.last()) == roleBucket(msg)) {
            const QString prev = out.last().content.trimmed();
            if (prev.isEmpty()) {
                out.last().content = msg.content;
                continue;
            }
            out.last().content = prev + "\n\n" + msg.content.trimmed();
        } else {
            out.append(msg);
        }
    }
    return out;
}

QString firstLineOf(const QString &text, int maxChars = 96) {
    QString t = text.simplified();
    if (t.size() > maxChars) t = t.left(maxChars) + "…";
    return t;
}

// Builds the summary message describing the dropped prefix of the conversation.
ChatMessage buildSummary(const QList<ChatMessage> &dropped) {
    QString body;
    for (const ChatMessage &msg : dropped) {
        if (msg.content.trimmed().isEmpty()) continue;
        QString role = msg.role;
        if (role == "tool") role = "tool result";
        body += QString("• %1: %2\n").arg(role, firstLineOf(msg.content));
    }
    if (body.isEmpty()) {
        body = "Earlier conversation omitted to fit the context budget.\n";
    }

    QString text = QString("Summary of earlier conversation (dropped to fit the context window):\n\n%1\n"
                           "[Earlier messages were omitted. Continue the task with this summary in mind.]")
                       .arg(body);

    const int max = kMaxSummaryTokens * 4;
    if (text.size() > max) text = text.left(max) + "…";

    ChatMessage msg;
    msg.role = "user";
    msg.content = text;
    return msg;
}

}  // namespace

int ConversationCompactor::estimateTokens(const QList<ChatMessage> &history) {
    int total = 0;
    for (const ChatMessage &msg : history) total += messageTokens(msg);
    return total;
}

QList<ChatMessage> ConversationCompactor::compact(const QList<ChatMessage> &history) {
    // Small conversations pass through unchanged apart from the merge pass.
    if (estimateTokens(history) <= kContextBudgetTokens) {
        return mergeConsecutiveText(history);
    }

    // Walk backwards greedy-keeping the newest tail that fits the tail budget.
    QList<ChatMessage> tail;
    int tailTokens = 0;
    int cutIndex = history.size();
    while (cutIndex > 0) {
        const int t = messageTokens(history[cutIndex - 1]);
        // Always keep at least the newest message even if it alone is oversized.
        if (!tail.isEmpty() && tailTokens + t > kTailBudgetTokens) break;
        tail.prepend(history[cutIndex - 1]);
        tailTokens += t;
        --cutIndex;
    }

    QList<ChatMessage> dropped;
    for (int i = 0; i < cutIndex; ++i) dropped.append(history[i]);

    QList<ChatMessage> result;
    if (!dropped.isEmpty()) {
        result.append(buildSummary(dropped));
    }
    for (const ChatMessage &msg : tail) result.append(msg);

    // Newly-eligible image drops live only in the kept tail; anything in the
    // dropped prefix is long gone.
    return dropImagesBeyondNewest3(mergeConsecutiveText(result));
}

}  // namespace mcw
