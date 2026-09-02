#pragma once

#include "scanengine/hybrid/types.hpp"

#include <QString>
#include <QVector>

namespace scanengine {
namespace hybrid {

// mineru_vl_utils.post_process.otsl2html.convert_otsl_to_html
QString convertOfficialOtslToHtml(const QString& otsl);

// mineru_vl_utils.post_process.simple_process table branch (OTSL → HTML).
void postProcessOfficialTableBlocks(QVector<ContentBlock>* blocks);

// vlm_middle_json_mkcontent._replace_eq_tags_in_table_html
// <eq>latex</eq> → " $latex$ " for markdown / content_list.
QString formatOfficialEmbeddedTableHtml(const QString& html);

}  // namespace hybrid
}  // namespace scanengine
