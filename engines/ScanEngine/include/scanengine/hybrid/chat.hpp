#pragma once

#include <QString>

namespace scanengine {
namespace hybrid {

// Official chat_template.jinja for system + one user turn (image then text).
// Matches AutoProcessor.apply_chat_template(..., add_generation_prompt).
QString applyOfficialChatTemplate(const QString& systemPrompt,
                                  const QString& userText,
                                  bool hasImage,
                                  bool addGenerationPrompt);

}  // namespace hybrid
}  // namespace scanengine
