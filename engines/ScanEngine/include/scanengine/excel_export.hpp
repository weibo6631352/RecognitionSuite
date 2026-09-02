#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace scanengine {

struct TableSheet {
    QString title;
    std::vector<QStringList> rows;
    // inclusive 0-based (r1,c1,r2,c2)
    std::vector<std::tuple<int, int, int, int>> merges;
};

struct ExcelExportResult {
    bool success = false;
    QString path;
    int tableCount = 0;
    QString message;
    QStringList sheetNames;
};

std::pair<std::vector<QStringList>, std::vector<std::tuple<int, int, int, int>>>
htmlTableToGrid(const QString& html);

std::vector<TableSheet> extractTablesFromContentList(const QString& path);
std::vector<TableSheet> extractTablesFromMarkdown(const QString& path);
std::optional<TableSheet> extractTextSheetFromMarkdown(const QString& path);

QStringList writeWorkbook(const std::vector<TableSheet>& sheets, const QString& outPath);

ExcelExportResult exportExcelFromParseDir(const QString& parseDir,
                                          const QString& outXlsx = QString(),
                                          const QString& sourceName = QString());

}  // namespace scanengine
