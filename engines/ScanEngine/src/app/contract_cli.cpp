#include "scanengine/config.hpp"
#include "scanengine/excel_export.hpp"
#include "scanengine/markdown_preview.hpp"
#include "scanengine/models.hpp"
#include "scanengine/output_layout.hpp"
#include "scanengine/hybrid/canonical_model.hpp"
#include "scanengine/hybrid/chat.hpp"
#include "scanengine/hybrid/device_policy.hpp"
#include "scanengine/hybrid/embed.hpp"
#include "scanengine/hybrid/extract.hpp"
#include "scanengine/hybrid/layout_v2.hpp"
#include "scanengine/hybrid/ocr_ppv6.hpp"
#include "scanengine/hybrid/orchestrator.hpp"
#include "scanengine/hybrid/otsl.hpp"
#include "scanengine/hybrid/preprocessor.hpp"
#include "scanengine/hybrid/prompts.hpp"
#include "scanengine/hybrid/qwen2_language.hpp"
#include "scanengine/hybrid/qwen2_vision.hpp"
#include "scanengine/hybrid/qwen2_vl.hpp"
#include "scanengine/hybrid/tokenizer.hpp"
#include "scanengine/hybrid/vlm_config.hpp"
#include "scanengine/hybrid/vlm_paths.hpp"
#include "scanengine/hybrid/vlm_weights.hpp"
#include "scanengine/parser_service.hpp"
#include "scanengine/strict_json.hpp"
#include "scanengine/ui_options.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <algorithm>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <cmath>
#include <cstdio>

using namespace scanengine;

static void printHelp() {
    QTextStream out(stdout);
    out.setCodec("UTF-8");
    out << "ScanEngineTool <command>\n"
           "  allocate --output-root DIR --source PATH [--now YYYYMMDD-HHMMSS]\n"
           "  export-excel --parse-dir DIR [--out FILE] [--source NAME]\n"
           "  preview-html --md FILE\n"
           "  parse --input IMAGE [--output-root DIR] [--effort medium] "
           "[--device gpu-required]\n"
           "  hybrid-plan [--effort medium] [--ocr 0|1]\n"
           "  hybrid-vlm-info\n"
           "  hybrid-vlm-load\n"
           "  hybrid-vlm-encode --text TEXT\n"
           "  hybrid-vlm-embed [--text TEXT]\n"
           "  hybrid-vlm-generate [--text TEXT] [--max-tokens N] [--image FILE]\n"
           "  hybrid-vlm-vision [--image FILE]\n"
           "  hybrid-extract --image FILE --blocks FILE.json [--max-tokens N] [--ocr 0|1] "
           "[--device gpu-required]\n"
           "  hybrid-layout --image FILE --out FILE.json [--device gpu-required]\n"
           "  hybrid-orient --image FILE --layout FILE.json --out FILE.json "
           "[--device gpu-required]\n"
           "  hybrid-ocr --image FILE --blocks FILE.json --layout FILE.json --out FILE.json "
           "[--device gpu-required]\n"
           "  hybrid-hints --layout FILE.json --out FILE.json [--image FILE] "
           "[--page-w N --page-h N]\n"
           "  hybrid-canonical --mode formula|title --blocks FILE.json --out FILE.json "
           "[--layout FILE.json --image FILE]\n"
           "  hybrid-middle --image FILE --blocks FILE.json --out FILE.json "
           "[--preproc FILE.json --image-dir DIR]\n"
           "  hybrid-artifacts --middle FILE.json --output DIR --stem NAME\n"
           "  hybrid-schema-fixture --mode finalize|visual|list|sidecar --input FILE.json --out FILE.json\n"
           "  env\n";
}

static bool flagOf(const QStringList& args, const QString& key, bool fallback) {
    const int i = args.indexOf(key);
    if (i < 0 || i + 1 >= args.size())
        return fallback;
    const QString v = args.at(i + 1);
    return v == QLatin1String("1") || v == QLatin1String("true");
}

static QString optOf(const QStringList& args, const QString& key, const QString& fallback = QString()) {
    const int i = args.indexOf(key);
    if (i < 0 || i + 1 >= args.size())
        return fallback;
    return args.at(i + 1);
}

static int applyDeviceArg(const QStringList& args, QTextStream& out, int* fallbackCount = nullptr) {
    const QString name = optOf(args, QStringLiteral("--device"), QStringLiteral("gpu-required"));
    bool ok = false;
    const hybrid::DeviceMode mode = hybrid::deviceModeFromName(name, &ok);
    if (!ok) {
        out << "unknown --device (want gpu-required)\n";
        return 2;
    }
    QString err;
    int localFallback = 0;
    if (!hybrid::applyHybridDeviceMode(mode, &localFallback, &err)) {
        out << err << '\n';
        return 1;
    }
    if (fallbackCount)
        *fallbackCount = localFallback;
    return 0;
}

static bool readTraceOrModelPage(const QString& path,
                                 QJsonArray* page,
                                 QString* error,
                                 QJsonArray* pageSize = nullptr) {
    if (!page)
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open json: %1").arg(path);
        return false;
    }
    QJsonDocument document;
    QString jsonError;
    if (!parseJsonDocumentStrict(file.readAll(), &document, &jsonError)) {
        if (error)
            *error = QStringLiteral("cannot parse json: %1 (%2)")
                         .arg(path, jsonError);
        return false;
    }
    QJsonValue value;
    if (document.isObject()) {
        const QJsonObject object = document.object();
        if (pageSize)
            *pageSize = object.value(QStringLiteral("page_size")).toArray();
        if (object.contains(QStringLiteral("payload")))
            value = object.value(QStringLiteral("payload"));
        else if (object.contains(QStringLiteral("blocks")))
            value = object.value(QStringLiteral("blocks"));
        else
            // The PP-DocLayoutV2 project envelope is {dets,page_size}.
            value = object.value(QStringLiteral("dets"));
    } else if (document.isArray()) {
        value = document.array();
    }
    QJsonArray array = value.toArray();
    while (array.size() == 1 && array.at(0).isArray())
        array = array.at(0).toArray();
    if (array.isEmpty() && !value.isArray()) {
        if (error)
            *error = QStringLiteral("json has no page array: %1").arg(path);
        return false;
    }
    for (const QJsonValue& item : array) {
        if (!item.isObject()) {
            if (error)
                *error = QStringLiteral("json page contains a non-object block: %1").arg(path);
            return false;
        }
    }
    *page = array;
    if (error)
        error->clear();
    return true;
}

static bool readExternalContentBlocks(const QString& path,
                                      QJsonArray* page,
                                      QVector<hybrid::ContentBlock>* blocks,
                                      QString* error) {
    if (!page || !blocks) {
        if (error)
            *error = QStringLiteral("external content block output is null");
        return false;
    }
    QJsonArray inputPage;
    if (!readTraceOrModelPage(path, &inputPage, error))
        return false;

    static const QStringList blockTypes = {
        QStringLiteral("text"),          QStringLiteral("title"),
        QStringLiteral("doc_title"),     QStringLiteral("paragraph_title"),
        QStringLiteral("table"),         QStringLiteral("equation"),
        QStringLiteral("formula_number"), QStringLiteral("code"),
        QStringLiteral("algorithm"),     QStringLiteral("aside_text"),
        QStringLiteral("ref_text"),      QStringLiteral("index"),
        QStringLiteral("phonetic"),      QStringLiteral("list_item"),
        QStringLiteral("table_caption"), QStringLiteral("image_caption"),
        QStringLiteral("code_caption"),  QStringLiteral("caption"),
        QStringLiteral("table_footnote"), QStringLiteral("image_footnote"),
        QStringLiteral("footnote"),      QStringLiteral("header"),
        QStringLiteral("footer"),        QStringLiteral("page_number"),
        QStringLiteral("page_footnote"), QStringLiteral("image"),
        QStringLiteral("chart"),         QStringLiteral("list"),
        QStringLiteral("image_block"),   QStringLiteral("equation_block"),
        QStringLiteral("unknown"),
    };

    QVector<hybrid::ContentBlock> parsed;
    parsed.reserve(inputPage.size());
    auto failAt = [&](int index, const QString& message) {
        if (error)
            *error = QStringLiteral("invalid external content block %1: %2").arg(index).arg(message);
        return false;
    };
    for (int index = 0; index < inputPage.size(); ++index) {
        const QJsonObject object = inputPage.at(index).toObject();
        const QJsonValue typeValue = object.value(QStringLiteral("type"));
        if (!typeValue.isString() || !blockTypes.contains(typeValue.toString()))
            return failAt(index, QStringLiteral("unknown or missing type"));

        const QJsonValue bboxValue = object.value(QStringLiteral("bbox"));
        if (!bboxValue.isArray() || bboxValue.toArray().size() != 4)
            return failAt(index, QStringLiteral("bbox must be an array of four numbers"));
        const QJsonArray bbox = bboxValue.toArray();
        double coordinates[4] = {};
        for (int coordinateIndex = 0; coordinateIndex < 4; ++coordinateIndex) {
            if (!bbox.at(coordinateIndex).isDouble())
                return failAt(index, QStringLiteral("bbox coordinates must be numbers"));
            coordinates[coordinateIndex] = bbox.at(coordinateIndex).toDouble();
            if (!std::isfinite(coordinates[coordinateIndex])
                || coordinates[coordinateIndex] < 0.0
                || coordinates[coordinateIndex] > 1.0) {
                return failAt(index, QStringLiteral("bbox coordinates must be finite and in [0, 1]"));
            }
        }
        if (coordinates[0] >= coordinates[2] || coordinates[1] >= coordinates[3])
            return failAt(index, QStringLiteral("bbox must have positive width and height"));

        hybrid::ContentBlock block;
        block.type = typeValue.toString();
        block.xmin = coordinates[0];
        block.ymin = coordinates[1];
        block.xmax = coordinates[2];
        block.ymax = coordinates[3];

        const QJsonValue angleValue = object.value(QStringLiteral("angle"));
        if (!angleValue.isUndefined() && !angleValue.isNull()) {
            if (!angleValue.isDouble())
                return failAt(index, QStringLiteral("angle must be null or 0/90/180/270"));
            const double angleNumber = angleValue.toDouble();
            const int angle = int(angleNumber);
            if (!std::isfinite(angleNumber) || angleNumber != double(angle)
                || (angle != 0 && angle != 90 && angle != 180 && angle != 270)) {
                return failAt(index, QStringLiteral("angle must be null or 0/90/180/270"));
            }
            block.angle = angle;
        }

        const QJsonValue contentValue = object.value(QStringLiteral("content"));
        block.contentNull = contentValue.isNull();
        if (!contentValue.isUndefined() && !contentValue.isNull()) {
            if (!contentValue.isString())
                return failAt(index, QStringLiteral("content must be null or a string"));
            block.content = contentValue.toString();
        }

        const QJsonValue mergePrevValue = object.value(QStringLiteral("merge_prev"));
        if (!mergePrevValue.isUndefined()) {
            if (!mergePrevValue.isBool())
                return failAt(index, QStringLiteral("merge_prev must be a boolean"));
            block.mergePrev = mergePrevValue.toBool();
        }
        if (block.mergePrev && block.type != QLatin1String("text"))
            return failAt(index, QStringLiteral("merge_prev is only valid for text"));

        const QJsonValue subTypeValue = object.value(QStringLiteral("sub_type"));
        if (subTypeValue.isString())
            block.subType = subTypeValue.toString();
        parsed.push_back(block);
    }

    *page = inputPage;
    *blocks = parsed;
    if (error)
        error->clear();
    return true;
}

int main(int argc, char* argv[]) {
    // Config and model discovery use applicationDirPath(), which is only valid
    // after the application object exists.
    QGuiApplication app(argc, argv);
    QString runtimeError;
    if (!scanengine::hybrid::initializeHybridRuntime(&runtimeError)) {
        std::fprintf(stderr, "%s\n", runtimeError.toUtf8().constData());
        return 1;
    }
    // The table-image path uses QRawFont, so this process needs a GUI
    // application object. It still creates no windows; leave platform plugin
    // selection to Qt so a deployed Windows bundle can use qwindows.dll.
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        printHelp();
        return 2;
    }
    const QString cmd = args.at(1);
    QTextStream out(stdout);
    out.setCodec("UTF-8");

    if (cmd == QLatin1String("allocate")) {
        const QString root = optOf(args, QStringLiteral("--output-root"));
        const QString source = optOf(args, QStringLiteral("--source"));
        QDateTime now;
        const QString nowText = optOf(args, QStringLiteral("--now"));
        if (!nowText.isEmpty())
            now = QDateTime::fromString(nowText, QStringLiteral("yyyyMMdd-HHmmss"));
        const JobPaths job = allocateJobPaths(root, source, now);
        QJsonObject o;
        o.insert(QStringLiteral("success"), job.success);
        o.insert(QStringLiteral("error"), job.error);
        o.insert(QStringLiteral("job_id"), job.jobId);
        o.insert(QStringLiteral("work_dir"), job.workDir);
        o.insert(QStringLiteral("intermediate_dir"), job.intermediateDir);
        o.insert(QStringLiteral("source_copy"), job.sourceCopy);
        o.insert(QStringLiteral("excel_path"), job.excelPath);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << '\n';
        return job.success ? 0 : 1;
    }

    if (cmd == QLatin1String("export-excel")) {
        const ExcelExportResult r = exportExcelFromParseDir(
            optOf(args, QStringLiteral("--parse-dir")),
            optOf(args, QStringLiteral("--out")),
            optOf(args, QStringLiteral("--source")));
        QJsonObject o;
        o.insert(QStringLiteral("success"), r.success);
        o.insert(QStringLiteral("path"), r.path);
        o.insert(QStringLiteral("table_count"), r.tableCount);
        o.insert(QStringLiteral("message"), r.message);
        o.insert(QStringLiteral("sheet_names"), QJsonArray::fromStringList(r.sheetNames));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)) << '\n';
        return r.success ? 0 : 1;
    }

    if (cmd == QLatin1String("preview-html")) {
        const QString mdPath = optOf(args, QStringLiteral("--md"));
        QFile f(mdPath);
        if (!f.open(QIODevice::ReadOnly))
            return 1;
        out << markdownToPreviewHtml(QString::fromUtf8(f.readAll()), mdPath);
        return 0;
    }

    if (cmd == QLatin1String("parse")) {
        const QString input = optOf(args, QStringLiteral("--input"));
        if (input.isEmpty()) {
            out << "parse requires --input\n";
            return 2;
        }
        ParseOptions o;
        o.inputPath = input;
        o.outputDir = optOf(args, QStringLiteral("--output-root"), defaultOutputDir());
        o.effort = optOf(args, QStringLiteral("--effort"), QStringLiteral("medium"));
        o.device = optOf(args, QStringLiteral("--device"), QStringLiteral("gpu-required"));
        if (o.effort != QLatin1String("medium")) {
            out << "ScanEngine only accepts official hybrid medium\n";
            return 2;
        }

        ParserService svc;

        QTextStream err(stderr);
        err.setCodec("UTF-8");
        const ParseResult r = svc.run(
            o,
            [&err](const QString& line) {
                err << line << '\n';
                err.flush();
            },
            [&err](TaskStatus status, const QString& message) {
                err << "[" << taskStatusValue(status) << "] " << message << '\n';
                err.flush();
            });

        QJsonObject obj;
        obj.insert(QStringLiteral("success"), r.success);
        obj.insert(QStringLiteral("status"), taskStatusValue(r.status));
        obj.insert(QStringLiteral("message"), r.message);
        obj.insert(QStringLiteral("output_dir"), r.outputDir);
        obj.insert(QStringLiteral("markdown_path"), r.markdownPath);
        obj.insert(QStringLiteral("json_path"), r.jsonPath);
        obj.insert(QStringLiteral("excel_path"), r.excelPath);
        obj.insert(QStringLiteral("fallback_count"), r.fallbackCount);
        obj.insert(QStringLiteral("device"), o.device);
        out << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Indented));
        return r.success ? 0 : 1;
    }

    if (cmd == QLatin1String("hybrid-plan")) {
        const QString effortName = optOf(args, QStringLiteral("--effort"), QStringLiteral("medium"));
        if (effortName != QLatin1String("medium")) {
            out << "ScanEngine only accepts official hybrid medium\n";
            return 2;
        }
        const bool ocr = flagOf(args, QStringLiteral("--ocr"), false);
        out << "image_analysis="
            << (hybrid::effectiveImageAnalysis(hybrid::Effort::Medium, true) ? "true" : "false")
            << '\n';
        int i = 1;
        for (const hybrid::Stage& st : hybrid::planStages(hybrid::Effort::Medium, ocr))
            out << QString::number(i++) << " " << st.id << "  " << st.referenceOperation
                << (st.model.isEmpty() ? QString() : (QStringLiteral("  [") + st.model + QLatin1Char(']')))
                << '\n';
        return 0;
    }

    if (cmd == QLatin1String("hybrid-vlm-info")) {
        const hybrid::VlmPaths p = hybrid::resolveOfficialVlmPaths();
        const QString miss = hybrid::vlmPathsMissing(p);
        QJsonObject o;
        o.insert(QStringLiteral("snapshot"), p.snapshotDir);
        o.insert(QStringLiteral("config"), p.configJson);
        o.insert(QStringLiteral("weights"), p.weights);
        o.insert(QStringLiteral("ready"), miss.isEmpty());
        if (!miss.isEmpty())
            o.insert(QStringLiteral("missing"), miss);
        hybrid::VlmModelConfig cfg;
        QString cfgErr;
        if (hybrid::loadOfficialVlmConfig(p.configJson, &cfg, &cfgErr)) {
            o.insert(QStringLiteral("model_type"), cfg.modelType);
            o.insert(QStringLiteral("architecture"), cfg.architecture);
            o.insert(QStringLiteral("hidden_size"), cfg.hiddenSize);
            o.insert(QStringLiteral("num_hidden_layers"), cfg.numHiddenLayers);
            o.insert(QStringLiteral("vocab_size"), cfg.vocabSize);
            o.insert(QStringLiteral("num_attention_heads"), cfg.numAttentionHeads);
            o.insert(QStringLiteral("num_key_value_heads"), cfg.numKeyValueHeads);
            o.insert(QStringLiteral("intermediate_size"), cfg.intermediateSize);
            o.insert(QStringLiteral("vision_depth"), cfg.visionDepth);
            o.insert(QStringLiteral("vision_embed_dim"), cfg.visionEmbedDim);
            o.insert(QStringLiteral("vision_hidden_size"), cfg.visionHiddenSize);
            o.insert(QStringLiteral("vision_patch_size"), cfg.visionPatchSize);
            o.insert(QStringLiteral("vision_spatial_merge_size"), cfg.visionSpatialMergeSize);
            o.insert(QStringLiteral("vision_num_heads"), cfg.visionNumHeads);
            o.insert(QStringLiteral("tie_word_embeddings"), cfg.tieWordEmbeddings);
            o.insert(QStringLiteral("dtype"), cfg.dtype);
            o.insert(QStringLiteral("rms_norm_eps"), cfg.rmsNormEps);
            o.insert(QStringLiteral("rope_theta"), cfg.ropeTheta);
            o.insert(QStringLiteral("image_token_id"), cfg.imageTokenId);
            o.insert(QStringLiteral("vision_start_token_id"), cfg.visionStartTokenId);
        } else {
            o.insert(QStringLiteral("config_error"), cfgErr);
        }
        hybrid::OfficialWeightIndex widx;
        QString werr;
        if (hybrid::indexOfficialWeights(p.weights, &widx, &werr)) {
            o.insert(QStringLiteral("tensor_count"), widx.tensorCount);
            o.insert(QStringLiteral("visual_count"), widx.visualCount);
            o.insert(QStringLiteral("language_count"), widx.languageCount);
            o.insert(QStringLiteral("has_lm_head"), widx.hasLmHead);
            o.insert(QStringLiteral("embed"), widx.embedRemappedName);
        } else if (!miss.contains(p.weights)) {
            o.insert(QStringLiteral("weights_error"), werr);
        }
        o.insert(QStringLiteral("system_prompt"), hybrid::officialSystemPrompt());
        o.insert(QStringLiteral("prompt_text"), hybrid::officialPromptForType(QStringLiteral("text")));
        o.insert(QStringLiteral("prompt_layout"), hybrid::officialPromptForType(QStringLiteral("[layout]")));
        o.insert(QStringLiteral("forward"), QStringLiteral("embed_lookup"));
        o.insert(QStringLiteral("generate"), false);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return (miss.isEmpty() && cfgErr.isEmpty()) ? 0 : 1;
    }

    if (cmd == QLatin1String("hybrid-vlm-load")) {
        const hybrid::VlmPaths p = hybrid::resolveOfficialVlmPaths();
        hybrid::OfficialWeightIndex widx;
        QString werr;
        QJsonObject o;
        o.insert(QStringLiteral("weights"), p.weights);
        if (!hybrid::indexOfficialWeights(p.weights, &widx, &werr)) {
            o.insert(QStringLiteral("ok"), false);
            o.insert(QStringLiteral("error"), werr);
            out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
            return 1;
        }
        QJsonArray embedShape;
        for (qint64 d : widx.embedShape)
            embedShape.append(double(d));
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("tensor_count"), widx.tensorCount);
        o.insert(QStringLiteral("visual_count"), widx.visualCount);
        o.insert(QStringLiteral("language_count"), widx.languageCount);
        o.insert(QStringLiteral("has_lm_head"), widx.hasLmHead);
        o.insert(QStringLiteral("embed_original"), widx.embedOriginalName);
        o.insert(QStringLiteral("embed_remapped"), widx.embedRemappedName);
        o.insert(QStringLiteral("embed_shape"), embedShape);
        o.insert(QStringLiteral("remap_example"),
                 hybrid::remapMlxQwen2VlKey(QStringLiteral("visual.patch_embed.proj.weight")));
        o.insert(QStringLiteral("generate"), false);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-vlm-encode")) {
        const QString text = optOf(args, QStringLiteral("--text"));
        if (text.isEmpty()) {
            out << "hybrid-vlm-encode requires --text\n";
            return 2;
        }
        const hybrid::VlmPaths p = hybrid::resolveOfficialVlmPaths();
        hybrid::OfficialTokenizer tok;
        QString terr;
        if (!hybrid::loadOfficialTokenizer(p.tokenizerJson, &tok, &terr)) {
            out << terr << '\n';
            return 1;
        }
        const QVector<int> ids = hybrid::encodeOfficial(tok, text);
        QJsonArray arr;
        for (int id : ids)
            arr.append(id);
        QJsonObject o;
        o.insert(QStringLiteral("text"), text);
        o.insert(QStringLiteral("ids"), arr);
        o.insert(QStringLiteral("decoded"), hybrid::decodeOfficial(tok, ids, false));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-vlm-embed")) {
        const QString text = optOf(args, QStringLiteral("--text"),
                                   QStringLiteral("You are a helpful assistant."));
        const hybrid::VlmPaths p = hybrid::resolveOfficialVlmPaths();
        hybrid::OfficialTokenizer tok;
        hybrid::OfficialWeightIndex widx;
        QString err;
        if (!hybrid::loadOfficialTokenizer(p.tokenizerJson, &tok, &err)) {
            out << err << '\n';
            return 1;
        }
        if (!hybrid::indexOfficialWeights(p.weights, &widx, &err)) {
            out << err << '\n';
            return 1;
        }
        const QString chat = hybrid::applyOfficialChatTemplate(
            hybrid::officialSystemPrompt(),
            hybrid::officialPromptForType(QStringLiteral("text")),
            true,
            true);
        const QVector<int> ids = hybrid::encodeOfficial(tok, text);
        hybrid::EmbedLookup look;
        if (!hybrid::lookupOfficialEmbeds(widx, ids, &look, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonArray idArr;
        for (int id : ids)
            idArr.append(id);
        QJsonArray l2;
        for (double v : look.l2)
            l2.append(v);
        QJsonArray first4;
        for (int i = 0; i < 4 && i < look.rows.size(); ++i)
            first4.append(double(look.rows.at(i)));
        QJsonObject o;
        o.insert(QStringLiteral("text"), text);
        o.insert(QStringLiteral("ids"), idArr);
        o.insert(QStringLiteral("hidden"), look.hidden);
        o.insert(QStringLiteral("lookup_sum"), look.sum);
        o.insert(QStringLiteral("lookup_mean"), look.mean);
        o.insert(QStringLiteral("lookup_l2"), l2);
        o.insert(QStringLiteral("first4"), first4);
        o.insert(QStringLiteral("chat_template"), chat);
        o.insert(QStringLiteral("generate"), false);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-vlm-generate")) {
        const QString text = optOf(args, QStringLiteral("--text"), QStringLiteral("Hello"));
        const int maxTokens = optOf(args, QStringLiteral("--max-tokens"), QStringLiteral("8")).toInt();
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const hybrid::VlmPaths p = hybrid::resolveOfficialVlmPaths();
        hybrid::OfficialTokenizer tok;
        hybrid::Qwen2LanguageModel* model = nullptr;
        QString err;
        if (!hybrid::loadOfficialTokenizer(p.tokenizerJson, &tok, &err)) {
            out << err << '\n';
            return 1;
        }
        if (!hybrid::officialLanguageModel(&model, &err)) {
            out << err << '\n';
            return 1;
        }
        const bool hasImage = !imagePath.isEmpty();
        const QString userText = hasImage ? hybrid::officialPromptForType(QStringLiteral("text"))
                                          : text;
        const QString chat = hybrid::applyOfficialChatTemplate(
            hybrid::officialSystemPrompt(), hasImage ? userText : text, hasImage, true);
        const QVector<int> prompt = hybrid::encodeOfficial(tok, chat);
        QJsonArray newIds;
        QJsonObject o;
        o.insert(QStringLiteral("text"), hasImage ? userText : text);
        o.insert(QStringLiteral("chat"), chat);
        if (hasImage) {
            QImage img(imagePath);
            if (img.isNull())
                img = QImage(56, 56, QImage::Format_RGB888);
            hybrid::VlmPreprocessorConfig pre;
            if (!hybrid::loadOfficialPreprocessorConfig(p.preprocessorJson, &pre, &err)) {
                out << err << '\n';
                return 1;
            }
            hybrid::OfficialImagePatches patches;
            if (!hybrid::processOfficialImage(img, pre, &patches, &err)) {
                out << err << '\n';
                return 1;
            }
            hybrid::Qwen2VisionModel* vision = nullptr;
            if (!hybrid::officialVisionModel(&vision, &err)) {
                out << err << '\n';
                return 1;
            }
            hybrid::VlmGenerateResult r;
            const hybrid::OfficialSampling sampling =
                hybrid::officialSamplingForType(QStringLiteral("text"));
            const hybrid::VlmGenerationContract generationContract =
                hybrid::VlmGenerationContract::explicitMaxNewTokens(maxTokens, sampling);
            if (!hybrid::generateOfficialVlmGreedy(
                    *model, *vision, tok, prompt, patches, generationContract, &r, &err)) {
                out << err << '\n';
                return 1;
            }
            for (int id : r.newIds)
                newIds.append(id);
            o.insert(QStringLiteral("decoded"), r.decoded);
            o.insert(QStringLiteral("last_argmax"), r.lastArgmax);
            o.insert(QStringLiteral("last_max"), double(r.lastMax));
            o.insert(QStringLiteral("vision_tokens"), r.numVisionTokens);
            o.insert(QStringLiteral("vision"), true);
        } else {
            hybrid::LanguageGenerateResult r;
            if (!hybrid::generateOfficialLanguageGreedy(*model, tok, prompt, maxTokens, &r, &err)) {
                out << err << '\n';
                return 1;
            }
            for (int id : r.newIds)
                newIds.append(id);
            o.insert(QStringLiteral("decoded"), r.decoded);
            o.insert(QStringLiteral("last_argmax"), r.lastArgmax);
            o.insert(QStringLiteral("last_max"), double(r.lastMax));
            o.insert(QStringLiteral("vision"), false);
        }
        o.insert(QStringLiteral("new_ids"), newIds);
        o.insert(QStringLiteral("language_forward"), true);
        o.insert(QStringLiteral("generate"), true);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-vlm-vision")) {
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const hybrid::VlmPaths p = hybrid::resolveOfficialVlmPaths();
        hybrid::VlmPreprocessorConfig pre;
        QString err;
        if (!hybrid::loadOfficialPreprocessorConfig(p.preprocessorJson, &pre, &err)) {
            out << err << '\n';
            return 1;
        }
        QImage img;
        if (!imagePath.isEmpty())
            img = QImage(imagePath);
        if (img.isNull()) {
            img = QImage(56, 56, QImage::Format_RGB888);
            img.fill(QColor(255, 0, 0));
        }
        hybrid::OfficialImagePatches patches;
        if (!hybrid::processOfficialImage(img, pre, &patches, &err)) {
            out << err << '\n';
            return 1;
        }
        hybrid::Qwen2VisionModel* vision = nullptr;
        if (!hybrid::officialVisionModel(&vision, &err)) {
            out << err << '\n';
            return 1;
        }
        QVector<float> feat;
        if (!hybrid::forwardOfficialVision(*vision, patches, &feat, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonArray first4;
        for (int i = 0; i < 4 && i < feat.size(); ++i)
            first4.append(double(feat.at(i)));
        QJsonArray patchHead;
        for (int i = 0; i < 8 && i < patches.patches.size(); ++i)
            patchHead.append(double(patches.patches.at(i)));
        QJsonObject o;
        o.insert(QStringLiteral("patch0"),
                 patches.patches.isEmpty() ? 0 : double(patches.patches.at(0)));
        o.insert(QStringLiteral("patch392"),
                 patches.patches.size() > 392 ? double(patches.patches.at(392)) : 0);
        o.insert(QStringLiteral("patch_head"), patchHead);
        o.insert(QStringLiteral("grid_t"), patches.gridT);
        o.insert(QStringLiteral("grid_h"), patches.gridH);
        o.insert(QStringLiteral("grid_w"), patches.gridW);
        o.insert(QStringLiteral("num_patches"), patches.numPatches());
        o.insert(QStringLiteral("num_vision_tokens"), patches.numVisionTokens());
        o.insert(QStringLiteral("feat_dim"), feat.size() / std::max(1, patches.numVisionTokens()));
        o.insert(QStringLiteral("first4"), first4);
        o.insert(QStringLiteral("vision"), true);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-extract")) {
        const int deviceStatus = applyDeviceArg(args, out);
        if (deviceStatus != 0)
            return deviceStatus;
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const QString blocksPath = optOf(args, QStringLiteral("--blocks"));
        const QString maxTokensArg = optOf(args, QStringLiteral("--max-tokens"));
        const bool ocr = flagOf(args, QStringLiteral("--ocr"), true);
        if (imagePath.isEmpty() || blocksPath.isEmpty()) {
            out << "hybrid-extract requires --image and --blocks\n";
            return 2;
        }
        bool maxTokensOk = true;
        int maxTokens = maxTokensArg.toInt(&maxTokensOk);
        if (maxTokensArg.isEmpty()) {
            maxTokens = hybrid::officialVlmTokenLimit();
            maxTokensOk = maxTokens > 0;
            if (!maxTokensOk) {
                out << "missing text_config.max_position_embeddings in official VLM config\n";
                return 1;
            }
        }
        if (!maxTokensOk || maxTokens <= 0) {
            out << "--max-tokens must be greater than zero\n";
            return 2;
        }
        const QImage img = hybrid::loadOfficialHybridPage(imagePath);
        if (img.isNull()) {
            out << "cannot load image\n";
            return 1;
        }
        QString err;
        QJsonArray inputBlocks;
        QVector<hybrid::ContentBlock> blocks;
        if (!readExternalContentBlocks(blocksPath, &inputBlocks, &blocks, &err)) {
            out << err << '\n';
            return 1;
        }

        const QStringList notExtract = ocr ? QStringList() : hybrid::officialNotExtractTypes();
        if (!hybrid::extractOfficialPageWithLayout(img,
                                                   &inputBlocks,
                                                   false,
                                                   notExtract,
                                                   maxTokens,
                                                   nullptr,
                                                   &err)) {
            out << err << '\n';
            return 1;
        }

        QJsonObject result;
        result.insert(QStringLiteral("blocks"), inputBlocks);
        result.insert(QStringLiteral("vision"), true);
        result.insert(QStringLiteral("extract"), true);
        out << QString::fromUtf8(
            hybrid::officialHybridJsonBytes(QJsonDocument(result), true));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-layout")) {
        const int deviceStatus = applyDeviceArg(args, out);
        if (deviceStatus != 0)
            return deviceStatus;
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        if (imagePath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-layout requires --image and --out\n";
            return 2;
        }
        QString err;
        if (!hybrid::runOfficialLayoutSidecar(imagePath, outPath, &err)) {
            out << err << '\n';
            return 1;
        }
        QVector<hybrid::LayoutDet> dets;
        int w = 0, h = 0;
        if (!hybrid::loadOfficialLayoutJson(outPath, &dets, &w, &h, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("count"), dets.size());
        o.insert(QStringLiteral("out"), outPath);
        o.insert(QStringLiteral("page_w"), w);
        o.insert(QStringLiteral("page_h"), h);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-orient")) {
        const int deviceStatus = applyDeviceArg(args, out);
        if (deviceStatus != 0)
            return deviceStatus;
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const QString layoutPath = optOf(args, QStringLiteral("--layout"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        if (imagePath.isEmpty() || layoutPath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-orient requires --image --layout --out\n";
            return 2;
        }
        QString err;
        if (!hybrid::runOfficialTableOrientSidecar(imagePath, layoutPath, outPath, &err)) {
            out << err << '\n';
            return 1;
        }
        QVector<hybrid::LayoutDet> dets;
        int w = 0, h = 0;
        if (!hybrid::loadOfficialLayoutJson(outPath, &dets, &w, &h, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonArray angles;
        for (const hybrid::LayoutDet& d : dets) {
            if (d.label == QLatin1String("table"))
                angles.append(d.angle);
        }
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("out"), outPath);
        o.insert(QStringLiteral("table_angles"), angles);
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-ocr")) {
        const int deviceStatus = applyDeviceArg(args, out);
        if (deviceStatus != 0)
            return deviceStatus;
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const QString blocksPath = optOf(args, QStringLiteral("--blocks"));
        const QString layoutPath = optOf(args, QStringLiteral("--layout"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        if (imagePath.isEmpty() || blocksPath.isEmpty() || layoutPath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-ocr requires --image --blocks --layout --out\n";
            return 2;
        }
        QString err;
        if (!hybrid::runOfficialOcrSidecar(imagePath, blocksPath, layoutPath, outPath, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("out"), outPath);
        QFile f(outPath);
        if (!f.open(QIODevice::ReadOnly)) {
            out << "cannot open OCR sidecar output\n";
            return 1;
        }
        QJsonDocument doc;
        if (!parseJsonDocumentStrict(f.readAll(), &doc, &err)) {
            out << "cannot parse OCR sidecar output: " << err << '\n';
            return 1;
        }
        if (!doc.isObject()) {
            out << "OCR sidecar output is not an object\n";
            return 1;
        }
        o.insert(QStringLiteral("count"),
                 doc.object().value(QStringLiteral("items")).toArray().size());
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-hints")) {
        const QString layoutPath = optOf(args, QStringLiteral("--layout"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const QString pageW = optOf(args, QStringLiteral("--page-w"));
        const QString pageH = optOf(args, QStringLiteral("--page-h"));
        if (layoutPath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-hints requires --layout and --out\n";
            return 2;
        }
        bool widthOk = true;
        bool heightOk = true;
        const int width = pageW.isEmpty() ? 0 : pageW.toInt(&widthOk);
        const int height = pageH.isEmpty() ? 0 : pageH.toInt(&heightOk);
        if (!widthOk || !heightOk || width < 0 || height < 0) {
            out << "--page-w and --page-h must be non-negative integers\n";
            return 2;
        }
        QString err;
        if (!hybrid::runOfficialMediumHintsSidecar(layoutPath, outPath, &err, imagePath, width, height)) {
            out << err << '\n';
            return 1;
        }
        QJsonArray page;
        if (!hybrid::readOfficialModelJson(outPath, &page, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("out"), outPath);
        o.insert(QStringLiteral("count"), page.size());
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-canonical")) {
        const QString mode = optOf(args, QStringLiteral("--mode"));
        const QString blocksPath = optOf(args, QStringLiteral("--blocks"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        if ((mode != QLatin1String("formula") && mode != QLatin1String("title"))
            || blocksPath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-canonical requires --mode formula|title --blocks --out\n";
            return 2;
        }
        QString err;
        QJsonArray page;
        if (!readTraceOrModelPage(blocksPath, &page, &err)) {
            out << err << '\n';
            return 1;
        }
        if (mode == QLatin1String("formula")) {
            hybrid::optimizeHybridFormulaNumberBlocks(page);
        } else {
            const QString layoutPath = optOf(args, QStringLiteral("--layout"));
            const QString imagePath = optOf(args, QStringLiteral("--image"));
            if (layoutPath.isEmpty() || imagePath.isEmpty()) {
                out << "title mode requires --layout --image\n";
                return 2;
            }
            QJsonArray layout;
            QJsonArray layoutPageSize;
            if (!readTraceOrModelPage(layoutPath, &layout, &err, &layoutPageSize)) {
                out << err << '\n';
                return 1;
            }
            const QImage image = hybrid::loadOfficialHybridPage(imagePath);
            if (image.isNull()) {
                out << "cannot load image\n";
                return 1;
            }
            const double layoutWidth = layoutPageSize.size() == 2
                                               && layoutPageSize.at(0).toDouble() > 0.0
                                           ? layoutPageSize.at(0).toDouble()
                                           : image.width();
            const double layoutHeight = layoutPageSize.size() == 2
                                                && layoutPageSize.at(1).toDouble() > 0.0
                                            ? layoutPageSize.at(1).toDouble()
                                            : image.height();
            hybrid::applyLayoutTitleSplit(page, layout, layoutWidth, layoutHeight);
        }
        if (!hybrid::writeOfficialModelJson(outPath, page, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("mode"), mode);
        result.insert(QStringLiteral("out"), outPath);
        result.insert(QStringLiteral("count"), page.size());
        out << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-schema-fixture")) {
        const QString mode = optOf(args, QStringLiteral("--mode"));
        const QString inputPath = optOf(args, QStringLiteral("--input"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        if ((mode != QLatin1String("finalize") && mode != QLatin1String("visual")
             && mode != QLatin1String("list") && mode != QLatin1String("sidecar"))
            || inputPath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-schema-fixture requires --mode finalize|visual|list|sidecar --input --out\n";
            return 2;
        }
        QFile inputFile(inputPath);
        if (!inputFile.open(QIODevice::ReadOnly)) {
            out << "cannot open schema fixture input\n";
            return 1;
        }
        QJsonDocument inputDocument;
        QString inputError;
        if (!parseJsonDocumentStrict(inputFile.readAll(), &inputDocument, &inputError)) {
            out << "cannot parse schema fixture input: " << inputError << '\n';
            return 1;
        }
        if (!inputDocument.isObject()) {
            out << "schema fixture input is not an object\n";
            return 1;
        }
        QString err;
        QJsonObject result;
        if (!hybrid::runHybridSchemaFixture(mode, inputDocument.object(), &result, &err)) {
            out << err << '\n';
            return 1;
        }
        if (!QDir().mkpath(QFileInfo(outPath).absolutePath())) {
            out << "cannot create schema fixture output directory\n";
            return 1;
        }
        QFile outputFile(outPath);
        if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out << "cannot write schema fixture output\n";
            return 1;
        }
        const QByteArray bytes = hybrid::officialHybridJsonBytes(QJsonDocument(result));
        if (outputFile.write(bytes) != bytes.size() || !outputFile.flush()) {
            out << "cannot write schema fixture output bytes\n";
            return 1;
        }
        QJsonObject summary;
        summary.insert(QStringLiteral("ok"), true);
        summary.insert(QStringLiteral("mode"), mode);
        summary.insert(QStringLiteral("out"), outPath);
        out << QString::fromUtf8(QJsonDocument(summary).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-middle")) {
        const QString imagePath = optOf(args, QStringLiteral("--image"));
        const QString blocksPath = optOf(args, QStringLiteral("--blocks"));
        const QString outPath = optOf(args, QStringLiteral("--out"));
        const QString preprocPath = optOf(args, QStringLiteral("--preproc"));
        const QString imageDir = optOf(args, QStringLiteral("--image-dir"));
        if (imagePath.isEmpty() || blocksPath.isEmpty() || outPath.isEmpty()) {
            out << "hybrid-middle requires --image --blocks --out\n";
            return 2;
        }
        int pdfWidth = 0;
        int pdfHeight = 0;
        QImage img = hybrid::loadOfficialHybridPage(imagePath, &pdfWidth, &pdfHeight);
        if (img.isNull()) {
            out << "cannot load image\n";
            return 1;
        }
        QJsonArray arr;
        QString err;
        if (!readTraceOrModelPage(blocksPath, &arr, &err)) {
            out << err << '\n';
            return 1;
        }
        if (!hybrid::writeOfficialMiddleJson(arr, pdfWidth > 0 ? pdfWidth : img.width(),
                                             pdfHeight > 0 ? pdfHeight : img.height(), outPath,
                                             &err, QStringLiteral("medium"), preprocPath, &img,
                                             imageDir)) {
            out << err << '\n';
            return 1;
        }
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        o.insert(QStringLiteral("out"), outPath);
        o.insert(QStringLiteral("engine"), QStringLiteral("cpp-middle"));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("hybrid-artifacts")) {
        const QString middlePath = optOf(args, QStringLiteral("--middle"));
        const QString outputDir = optOf(args, QStringLiteral("--output"));
        const QString stem = optOf(args, QStringLiteral("--stem"));
        if (middlePath.isEmpty() || outputDir.isEmpty() || stem.isEmpty()) {
            out << "hybrid-artifacts requires --middle --output --stem\n";
            return 2;
        }
        QString markdownPath;
        QString listPath;
        QString err;
        if (!hybrid::writeHybridPageArtifactsFromMiddle(outputDir, stem, middlePath,
                                                        &markdownPath, &listPath, &err)) {
            out << err << '\n';
            return 1;
        }
        QJsonObject result;
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("markdown"), markdownPath);
        result.insert(QStringLiteral("content_list"), listPath);
        out << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Indented));
        return 0;
    }

    if (cmd == QLatin1String("env")) {
        const QStringList missingModels = hybrid::officialHybridMissingFiles();
        QJsonObject o;
        o.insert(QStringLiteral("repo_root"), repoRoot());
        o.insert(QStringLiteral("models_dir"), defaultModelsDir());
        o.insert(QStringLiteral("models_ready"), missingModels.isEmpty());
        o.insert(QStringLiteral("missing_models"), QJsonArray::fromStringList(missingModels));
        o.insert(QStringLiteral("model_manifest"), modelManifestPath());
        o.insert(QStringLiteral("output_dir"), defaultOutputDir());
        o.insert(QStringLiteral("qt"), QString::fromLatin1(qVersion()));
        o.insert(QStringLiteral("gpu_backend"), hybrid::certifiedGpuBackendName());
        o.insert(QStringLiteral("gpu_compiled"), hybrid::certifiedGpuBackendCompiled());
        QString gpuReason;
        const bool gpuReady = hybrid::certifiedGpuBackendReady(&gpuReason);
        o.insert(QStringLiteral("gpu_ready"), gpuReady);
        o.insert(QStringLiteral("gpu_reason"), gpuReason);
        o.insert(QStringLiteral("gpu_cache_limit"),
                 QJsonValue(qint64(hybrid::recommendedHybridGpuCacheLimit())));
        o.insert(QStringLiteral("gpu_cache_limit_applied"),
                 QJsonValue(qint64(hybrid::appliedHybridGpuCacheLimit())));
        out << QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented));
        return 0;
    }

    printHelp();
    return 2;
}
