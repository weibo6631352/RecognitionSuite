#include "scanengine/hybrid/prompts.hpp"

namespace scanengine {
namespace hybrid {

QString officialSystemPrompt() {
    return QStringLiteral("You are a helpful assistant.");
}

QString officialPromptForType(const QString& blockType) {
    // mineru_vl_utils.mineru_client.DEFAULT_PROMPTS
    if (blockType == QLatin1String("table"))
        return QStringLiteral("\nTable Recognition:");
    if (blockType == QLatin1String("equation"))
        return QStringLiteral("\nFormula Recognition:");
    if (blockType == QLatin1String("image") || blockType == QLatin1String("chart"))
        return QStringLiteral("\nImage Analysis:");
    if (blockType == QLatin1String("[layout]"))
        return QStringLiteral("\nLayout Detection:");
    if (blockType == QLatin1String("[cross_page_table_merge]"))
        return QString();
    return QStringLiteral("\nText Recognition:");
}

OfficialSampling officialSamplingForType(const QString& blockType) {
    OfficialSampling s;
    if (blockType == QLatin1String("[layout]")) {
        s.presencePenalty = 0.0;
        s.frequencyPenalty = 0.0;
        return s;
    }
    s.presencePenalty = 1.0;
    if (blockType == QLatin1String("table"))
        s.frequencyPenalty = 0.005;
    else
        s.frequencyPenalty = 0.05;
    return s;
}

QStringList officialNotExtractTypes() {
    // mineru.utils.enum_class.NotExtractType
    return QStringList{
        QStringLiteral("text"),
        QStringLiteral("title"),
        QStringLiteral("header"),
        QStringLiteral("footer"),
        QStringLiteral("page_number"),
        QStringLiteral("page_footnote"),
        QStringLiteral("ref_text"),
        QStringLiteral("table_caption"),
        QStringLiteral("image_caption"),
        QStringLiteral("table_footnote"),
        QStringLiteral("image_footnote"),
        QStringLiteral("code_caption"),
        QStringLiteral("phonetic"),
    };
}

QStringList officialAlwaysSkipTypes() {
    return QStringList{
        QStringLiteral("list"),
        QStringLiteral("equation_block"),
        QStringLiteral("image_block"),
    };
}

QStringList officialImageAnalysisTypes() {
    return QStringList{QStringLiteral("image"), QStringLiteral("chart")};
}

bool shouldSkipExtract(const QString& blockType,
                       bool imageAnalysis,
                       const QStringList& notExtractList) {
    if (officialAlwaysSkipTypes().contains(blockType))
        return true;
    if (!imageAnalysis && officialImageAnalysisTypes().contains(blockType))
        return true;
    return notExtractList.contains(blockType);
}

}  // namespace hybrid
}  // namespace scanengine
