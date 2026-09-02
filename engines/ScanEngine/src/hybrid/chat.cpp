#include "scanengine/hybrid/chat.hpp"

#include "scanengine/hybrid/prompts.hpp"

namespace scanengine {
namespace hybrid {

QString applyOfficialChatTemplate(const QString& systemPrompt,
                                  const QString& userText,
                                  bool hasImage,
                                  bool addGenerationPrompt) {
    const QString sys = systemPrompt.isEmpty() ? officialSystemPrompt() : systemPrompt;
    QString out;
    out += QStringLiteral("<|im_start|>system\n");
    out += sys;
    out += QStringLiteral("<|im_end|>\n<|im_start|>user\n");
    if (hasImage)
        out += QStringLiteral("<|vision_start|><|image_pad|><|vision_end|>");
    out += userText;
    out += QStringLiteral("<|im_end|>\n");
    if (addGenerationPrompt)
        out += QStringLiteral("<|im_start|>assistant\n");
    return out;
}

}  // namespace hybrid
}  // namespace scanengine
