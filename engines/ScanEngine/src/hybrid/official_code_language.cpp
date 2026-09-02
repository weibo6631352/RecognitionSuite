#include "scanengine/hybrid/official_code_language.hpp"

#include "scanengine/config.hpp"

#include "onnxruntime_c_api.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#ifndef SCANENGINE_ORT_RUNTIME_SHA256
#error "SCANENGINE_ORT_RUNTIME_SHA256 must lock the deployed ONNX Runtime"
#endif
#ifndef SCANENGINE_ORT_RUNTIME_SIZE
#error "SCANENGINE_ORT_RUNTIME_SIZE must lock the deployed ONNX Runtime"
#endif
#ifndef SCANENGINE_ORT_RUNTIME_NAME
#error "SCANENGINE_ORT_RUNTIME_NAME must identify the deployed ONNX Runtime"
#endif

namespace scanengine {
namespace hybrid {
namespace {

constexpr qint64 kMagikaModelSize = 3163737;
constexpr char kMagikaModelSha256[] =
    "fe2d2eb49c5f88a9e0a6c048e15d6ffdf86235519c2afc535044de433169ec8c";
constexpr qint64 kMagikaConfigSize = 2142;
constexpr char kMagikaConfigSha256[] =
    "991a6fb760c0431e5c0350a487ba4bd89dee43b74d53b77cc88106469d99617a";
constexpr char kOrtVersion[] = "1.28.0";
constexpr int kBlockSize = 4096;
constexpr int kBegSize = 1024;
constexpr int kEndSize = 1024;
constexpr int kFeatureSize = kBegSize + kEndSize;
constexpr int kPaddingToken = 256;
constexpr int kMinimumMeaningfulBytes = 8;
constexpr int kTargetLabelCount = 214;

struct LockedMagikaConfig {
    QStringList labels;
    QHash<QString, double> thresholds;
    double mediumConfidenceThreshold = 0.5;
};

using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)() NO_EXCEPTION;
using AppendCpuProviderFn = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);

struct RuntimeState {
    QLibrary library;
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    OrtSession* session = nullptr;
    LockedMagikaConfig config;
    QString modelDir;
    QString runtimePath;

    ~RuntimeState() {
        if (api && session)
            api->ReleaseSession(session);
        if (api && env)
            api->ReleaseEnv(env);
        if (library.isLoaded())
            library.unload();
    }
};

struct RuntimeCache {
    std::mutex mutex;
    QString key;
    std::shared_ptr<RuntimeState> state;
};

RuntimeCache& runtimeCache() {
    static RuntimeCache cache;
    return cache;
}

std::atomic<quint64>& successfulLoadCount() {
    static std::atomic<quint64> count{0};
    return count;
}

bool failWith(QString* err, const QString& message) {
    if (err)
        *err = message;
    return false;
}

bool checkStatus(const OrtApi* api,
                 OrtStatus* status,
                 const QString& operation,
                 QString* err) {
    if (!status)
        return true;
    const char* rawMessage = api ? api->GetErrorMessage(status) : nullptr;
    const QString message = rawMessage ? QString::fromUtf8(rawMessage)
                                       : QStringLiteral("unknown ONNX Runtime error");
    if (api)
        api->ReleaseStatus(status);
    return failWith(err, QStringLiteral("%1: %2").arg(operation, message));
}

bool readLockedFile(const QString& path,
                    qint64 expectedSize,
                    const char* expectedSha256,
                    QByteArray* bytes,
                    QString* err) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return failWith(err, QStringLiteral("cannot open locked file: %1 (%2)")
                                 .arg(path, file.errorString()));
    }
    if (file.size() != expectedSize) {
        return failWith(err,
                        QStringLiteral("locked file size mismatch: %1 (expected %2, actual %3)")
                            .arg(path)
                            .arg(expectedSize)
                            .arg(file.size()));
    }
    const QByteArray payload = file.readAll();
    if (payload.size() != expectedSize || file.error() != QFileDevice::NoError) {
        return failWith(err, QStringLiteral("cannot read locked file: %1 (%2)")
                                 .arg(path, file.errorString()));
    }
    const QByteArray digest =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    if (digest != QByteArray(expectedSha256)) {
        return failWith(err,
                        QStringLiteral("locked file SHA-256 mismatch: %1 (expected %2, actual %3)")
                            .arg(path, QString::fromLatin1(expectedSha256),
                                 QString::fromLatin1(digest)));
    }
    if (bytes)
        *bytes = payload;
    if (err)
        err->clear();
    return true;
}

bool isPythonLineBoundary(ushort code) {
    switch (code) {
    case 0x000A:  // LF
    case 0x000B:  // VT
    case 0x000C:  // FF
    case 0x000D:  // CR
    case 0x001C:  // FS
    case 0x001D:  // GS
    case 0x001E:  // RS
    case 0x0085:  // NEL
    case 0x2028:  // LINE SEPARATOR
    case 0x2029:  // PARAGRAPH SEPARATOR
        return true;
    default:
        return false;
    }
}

QStringList pythonSplitLines(const QString& content) {
    QStringList lines;
    int start = 0;
    for (int index = 0; index < content.size(); ++index) {
        const ushort code = content.at(index).unicode();
        if (!isPythonLineBoundary(code))
            continue;
        lines.append(content.mid(start, index - start));
        if (code == 0x000D && index + 1 < content.size()
            && content.at(index + 1).unicode() == 0x000A) {
            ++index;
        }
        start = index + 1;
    }
    // Python str.splitlines() does not append an empty item for a terminal
    // boundary, but it does retain empty items between adjacent boundaries.
    if (start < content.size())
        lines.append(content.mid(start));
    return lines;
}

bool isPythonWhitespace(ushort code) {
    if ((code >= 0x0009 && code <= 0x000D)
        || (code >= 0x001C && code <= 0x0020)
        || (code >= 0x2000 && code <= 0x200A)) {
        return true;
    }
    switch (code) {
    case 0x0085:
    case 0x00A0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return false;
    }
}

QString pythonStrip(const QString& text) {
    int start = 0;
    int end = text.size();
    while (start < end && isPythonWhitespace(text.at(start).unicode()))
        ++start;
    while (end > start && isPythonWhitespace(text.at(end - 1).unicode()))
        --end;
    return text.mid(start, end - start);
}

bool isAsciiBytesWhitespace(unsigned char byte) {
    return byte == 0x20 || (byte >= 0x09 && byte <= 0x0D);
}

QByteArray bytesLStrip(const QByteArray& bytes) {
    int start = 0;
    while (start < bytes.size()
           && isAsciiBytesWhitespace(static_cast<unsigned char>(bytes.at(start)))) {
        ++start;
    }
    return bytes.mid(start);
}

QByteArray bytesRStrip(const QByteArray& bytes) {
    int end = bytes.size();
    while (end > 0
           && isAsciiBytesWhitespace(static_cast<unsigned char>(bytes.at(end - 1)))) {
        --end;
    }
    return bytes.left(end);
}

QString absoluteOverridePath(const QString& path) {
    const QFileInfo info(path);
    if (info.isAbsolute())
        return QDir::cleanPath(path);
    return QDir::cleanPath(QDir::current().absoluteFilePath(path));
}

QString unicodeEnvironmentVariable(const char* narrowName,
                                   const wchar_t* wideName) {
#ifdef Q_OS_WIN
    Q_UNUSED(narrowName);
    DWORD capacity = GetEnvironmentVariableW(wideName, nullptr, 0);
    while (capacity > 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(capacity));
        const DWORD length =
            GetEnvironmentVariableW(wideName, buffer.data(), capacity);
        if (length == 0)
            return {};
        if (length < capacity)
            return QString::fromWCharArray(buffer.data(), int(length));
        capacity = length + 1;
    }
    return {};
#else
    Q_UNUSED(wideName);
    return qEnvironmentVariable(narrowName);
#endif
}

QString configuredMagikaRoot() {
    QFile manifest(modelManifestPath());
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll());
    if (!document.isObject())
        return {};
    const QString configured = document.object()
                                   .value(QStringLiteral("models-dir"))
                                   .toObject()
                                   .value(QStringLiteral("magika"))
                                   .toString();
    return configured.isEmpty() ? QString() : resolveRepoPath(configured);
}

bool parseLockedConfig(const QByteArray& bytes,
                       LockedMagikaConfig* config,
                       QString* err) {
    if (!config)
        return failWith(err, QStringLiteral("Magika config output is null"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failWith(err, QStringLiteral("locked Magika config is invalid JSON: %1")
                                 .arg(parseError.errorString()));
    }
    const QJsonObject root = document.object();
    auto exactInteger = [&](const char* name, int expected) {
        const QJsonValue value = root.value(QString::fromLatin1(name));
        return value.isDouble() && value.toInt(-1) == expected
               && value.toDouble() == double(expected);
    };
    if (!exactInteger("beg_size", kBegSize) || !exactInteger("mid_size", 0)
        || !exactInteger("end_size", kEndSize)
        || !exactInteger("min_file_size_for_dl", kMinimumMeaningfulBytes)
        || !exactInteger("padding_token", kPaddingToken)
        || !exactInteger("block_size", kBlockSize)
        || root.value(QStringLiteral("use_inputs_at_offsets")).toBool(true)) {
        return failWith(err, QStringLiteral("locked Magika preprocessing config drifted"));
    }
    const double medium = root.value(QStringLiteral("medium_confidence_threshold"))
                              .toDouble(-1.0);
    if (medium != 0.5)
        return failWith(err, QStringLiteral("locked Magika confidence threshold drifted"));

    const QJsonArray labelsJson = root.value(QStringLiteral("target_labels_space")).toArray();
    if (labelsJson.size() != kTargetLabelCount)
        return failWith(err, QStringLiteral("locked Magika label count drifted"));
    QStringList labels;
    labels.reserve(labelsJson.size());
    for (const QJsonValue& value : labelsJson) {
        if (!value.isString() || value.toString().isEmpty())
            return failWith(err, QStringLiteral("locked Magika labels are invalid"));
        labels.append(value.toString());
    }
    if (labels.removeDuplicates() != 0)
        return failWith(err, QStringLiteral("locked Magika labels contain duplicates"));

    const QJsonObject thresholdsJson = root.value(QStringLiteral("thresholds")).toObject();
    QHash<QString, double> thresholds;
    for (auto it = thresholdsJson.constBegin(); it != thresholdsJson.constEnd(); ++it) {
        if (!labels.contains(it.key()) || !it.value().isDouble())
            return failWith(err, QStringLiteral("locked Magika per-label thresholds are invalid"));
        thresholds.insert(it.key(), it.value().toDouble());
    }
    static const QHash<QString, double> expectedThresholds = {
        {QStringLiteral("crt"), 0.9},
        {QStringLiteral("handlebars"), 0.9},
        {QStringLiteral("ignorefile"), 0.95},
        {QStringLiteral("latex"), 0.95},
        {QStringLiteral("markdown"), 0.75},
        {QStringLiteral("ocaml"), 0.9},
        {QStringLiteral("pascal"), 0.95},
        {QStringLiteral("r"), 0.9},
        {QStringLiteral("rst"), 0.9},
        {QStringLiteral("sql"), 0.9},
        {QStringLiteral("tsv"), 0.9},
        {QStringLiteral("zig"), 0.9},
    };
    if (thresholds != expectedThresholds) {
        return failWith(err, QStringLiteral("locked Magika per-label thresholds drifted"));
    }
    const QJsonObject overwrite = root.value(QStringLiteral("overwrite_map")).toObject();
    if (overwrite.size() != 2
        || overwrite.value(QStringLiteral("randombytes")).toString()
               != QLatin1String("unknown")
        || overwrite.value(QStringLiteral("randomtxt")).toString()
               != QLatin1String("txt")) {
        return failWith(err, QStringLiteral("locked Magika overwrite map drifted"));
    }

    config->labels = labels;
    config->thresholds = thresholds;
    config->mediumConfidenceThreshold = medium;
    if (err)
        err->clear();
    return true;
}

bool validateTensorContract(const OrtApi* api,
                            OrtSession* session,
                            bool input,
                            ONNXTensorElementDataType expectedType,
                            int expectedWidth,
                            const char* expectedName,
                            QString* err) {
    size_t count = 0;
    OrtStatus* status = input ? api->SessionGetInputCount(session, &count)
                              : api->SessionGetOutputCount(session, &count);
    if (!checkStatus(api, status,
                     input ? QStringLiteral("read Magika input count")
                           : QStringLiteral("read Magika output count"),
                     err)) {
        return false;
    }
    if (count != 1)
        return failWith(err, QStringLiteral("Magika graph must expose exactly one %1")
                                 .arg(input ? QStringLiteral("input")
                                            : QStringLiteral("output")));

    OrtAllocator* allocator = nullptr;
    if (!checkStatus(api, api->GetAllocatorWithDefaultOptions(&allocator),
                     QStringLiteral("get ONNX Runtime allocator"), err)) {
        return false;
    }
    char* name = nullptr;
    status = input ? api->SessionGetInputName(session, 0, allocator, &name)
                   : api->SessionGetOutputName(session, 0, allocator, &name);
    if (!checkStatus(api, status,
                     input ? QStringLiteral("read Magika input name")
                           : QStringLiteral("read Magika output name"),
                     err)) {
        return false;
    }
    const bool nameMatches = name && std::strcmp(name, expectedName) == 0;
    if (name)
        allocator->Free(allocator, name);
    if (!nameMatches)
        return failWith(err, QStringLiteral("Magika graph %1 name drifted")
                                 .arg(input ? QStringLiteral("input")
                                            : QStringLiteral("output")));

    OrtTypeInfo* typeInfo = nullptr;
    status = input ? api->SessionGetInputTypeInfo(session, 0, &typeInfo)
                   : api->SessionGetOutputTypeInfo(session, 0, &typeInfo);
    if (!checkStatus(api, status,
                     input ? QStringLiteral("read Magika input type")
                           : QStringLiteral("read Magika output type"),
                     err)) {
        return false;
    }
    const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;
    bool ok = checkStatus(api, api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo),
                          QStringLiteral("read Magika tensor contract"), err);
    ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t dimensionCount = 0;
    int64_t dimensions[2] = {0, 0};
    if (ok)
        ok = checkStatus(api, api->GetTensorElementType(tensorInfo, &elementType),
                         QStringLiteral("read Magika tensor element type"), err);
    if (ok)
        ok = checkStatus(api, api->GetDimensionsCount(tensorInfo, &dimensionCount),
                         QStringLiteral("read Magika tensor rank"), err);
    if (ok && dimensionCount == 2)
        ok = checkStatus(api, api->GetDimensions(tensorInfo, dimensions, 2),
                         QStringLiteral("read Magika tensor dimensions"), err);
    api->ReleaseTypeInfo(typeInfo);
    if (!ok)
        return false;
    if (elementType != expectedType || dimensionCount != 2 || dimensions[0] != -1
        || dimensions[1] != expectedWidth) {
        return failWith(err,
                        QStringLiteral("Magika graph %1 tensor contract drifted")
                            .arg(input ? QStringLiteral("input")
                                       : QStringLiteral("output")));
    }
    return true;
}

bool loadRuntimeState(const QString& modelDir,
                      const QString& runtimePath,
                      std::shared_ptr<RuntimeState>* output,
                      QString* err) {
    const QString modelPath = QDir(modelDir).filePath(QStringLiteral("model.onnx"));
    const QString configPath = QDir(modelDir).filePath(QStringLiteral("config.min.json"));
    QByteArray modelBytes;
    QByteArray configBytes;
    if (!readLockedFile(modelPath, kMagikaModelSize, kMagikaModelSha256, &modelBytes, err)
        || !readLockedFile(configPath, kMagikaConfigSize, kMagikaConfigSha256,
                           &configBytes, err)
        || !readLockedFile(runtimePath, qint64(SCANENGINE_ORT_RUNTIME_SIZE),
                           SCANENGINE_ORT_RUNTIME_SHA256, nullptr, err)) {
        return false;
    }

    auto state = std::make_shared<RuntimeState>();
    state->modelDir = modelDir;
    state->runtimePath = runtimePath;
    if (!parseLockedConfig(configBytes, &state->config, err))
        return false;

    state->library.setFileName(runtimePath);
    state->library.setLoadHints(QLibrary::PreventUnloadHint);
    if (!state->library.load()) {
        return failWith(err, QStringLiteral("cannot load locked ONNX Runtime: %1 (%2)")
                                 .arg(runtimePath, state->library.errorString()));
    }
    const QFunctionPointer getApiSymbol = state->library.resolve("OrtGetApiBase");
    if (!getApiSymbol)
        return failWith(err, QStringLiteral("locked ONNX Runtime has no OrtGetApiBase: %1")
                                 .arg(runtimePath));
    const auto getApiBase = reinterpret_cast<GetApiBaseFn>(getApiSymbol);
    const OrtApiBase* apiBase = getApiBase();
    if (!apiBase || !apiBase->GetVersionString
        || QByteArray(apiBase->GetVersionString()) != QByteArray(kOrtVersion)) {
        return failWith(err, QStringLiteral("locked ONNX Runtime version drifted: %1")
                                 .arg(runtimePath));
    }
    state->api = apiBase->GetApi(ORT_API_VERSION);
    if (!state->api)
        return failWith(err, QStringLiteral("ONNX Runtime C API v%1 is unavailable")
                                 .arg(ORT_API_VERSION));
    const QFunctionPointer appendCpuSymbol =
        state->library.resolve("OrtSessionOptionsAppendExecutionProvider_CPU");
    if (!appendCpuSymbol) {
        return failWith(err,
                        QStringLiteral("locked ONNX Runtime has no CPU provider factory: %1")
                            .arg(runtimePath));
    }
    const auto appendCpu = reinterpret_cast<AppendCpuProviderFn>(appendCpuSymbol);

    if (!checkStatus(state->api,
                     state->api->CreateEnv(ORT_LOGGING_LEVEL_WARNING,
                                           "scanengine-magika", &state->env),
                     QStringLiteral("create Magika ONNX Runtime environment"), err)) {
        return false;
    }
    if (!checkStatus(state->api, state->api->DisableTelemetryEvents(state->env),
                     QStringLiteral("disable ONNX Runtime telemetry"), err)) {
        return false;
    }
    OrtSessionOptions* sessionOptions = nullptr;
    if (!checkStatus(state->api, state->api->CreateSessionOptions(&sessionOptions),
                     QStringLiteral("create Magika session options"), err)) {
        return false;
    }
    bool ok = checkStatus(state->api, appendCpu(sessionOptions, 1),
                          QStringLiteral("select CPUExecutionProvider"), err);
    if (ok) {
        // Match Python InferenceSession defaults exactly: no custom thread
        // counts, execution mode, graph optimization level, or allocator flags.
        ok = checkStatus(state->api,
                         state->api->CreateSessionFromArray(
                             state->env, modelBytes.constData(),
                             static_cast<size_t>(modelBytes.size()), sessionOptions,
                             &state->session),
                         QStringLiteral("load locked Magika standard_v3_3 model"), err);
    }
    state->api->ReleaseSessionOptions(sessionOptions);
    if (!ok)
        return false;
    if (!validateTensorContract(state->api, state->session, true,
                                ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32,
                                kFeatureSize, "bytes", err)
        || !validateTensorContract(state->api, state->session, false,
                                   ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                   kTargetLabelCount, "target_label", err)) {
        return false;
    }

    *output = std::move(state);
    successfulLoadCount().fetch_add(1, std::memory_order_relaxed);
    if (err)
        err->clear();
    return true;
}

bool acquireRuntime(std::shared_ptr<RuntimeState>* state, QString* err) {
    if (!state)
        return failWith(err, QStringLiteral("Magika runtime output is null"));
    const QString modelDir = resolveOfficialMagikaModelDir();
    const QString runtimePath = resolveOfficialMagikaRuntimePath();
    const QString key = modelDir + QChar(0x001F) + runtimePath;
    RuntimeCache& cache = runtimeCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.state && cache.key == key) {
        *state = cache.state;
        if (err)
            err->clear();
        return true;
    }
    std::shared_ptr<RuntimeState> loaded;
    if (!loadRuntimeState(modelDir, runtimePath, &loaded, err))
        return false;
    cache.key = key;
    cache.state = loaded;
    *state = std::move(loaded);
    return true;
}

bool runBatchOne(const RuntimeState& state,
                 const QVector<std::int32_t>& features,
                 QVector<float>* scores,
                 QString* err) {
    if (features.size() != kFeatureSize)
        return failWith(err, QStringLiteral("Magika input tensor must contain 2048 values"));
    const OrtApi* api = state.api;
    OrtMemoryInfo* memoryInfo = nullptr;
    if (!checkStatus(api,
                     api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                              &memoryInfo),
                     QStringLiteral("create Magika CPU tensor memory info"), err)) {
        return false;
    }
    const int64_t shape[2] = {1, kFeatureSize};
    OrtValue* input = nullptr;
    bool ok = checkStatus(
        api,
        api->CreateTensorWithDataAsOrtValue(
            memoryInfo, const_cast<std::int32_t*>(features.constData()),
            static_cast<size_t>(features.size()) * sizeof(std::int32_t), shape, 2,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32, &input),
        QStringLiteral("create Magika input tensor"), err);
    api->ReleaseMemoryInfo(memoryInfo);
    if (!ok)
        return false;

    const char* inputNames[] = {"bytes"};
    const char* outputNames[] = {"target_label"};
    const OrtValue* inputs[] = {input};
    OrtValue* output = nullptr;
    ok = checkStatus(api,
                     api->Run(state.session, nullptr, inputNames, inputs, 1,
                              outputNames, 1, &output),
                     QStringLiteral("run Magika batch=1 inference"), err);
    api->ReleaseValue(input);
    if (!ok)
        return false;

    OrtTensorTypeAndShapeInfo* shapeInfo = nullptr;
    ok = checkStatus(api, api->GetTensorTypeAndShape(output, &shapeInfo),
                     QStringLiteral("read Magika output tensor shape"), err);
    ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
    size_t dimensionCount = 0;
    int64_t dimensions[2] = {0, 0};
    size_t elementCount = 0;
    if (ok)
        ok = checkStatus(api, api->GetTensorElementType(shapeInfo, &elementType),
                         QStringLiteral("read Magika output tensor type"), err);
    if (ok)
        ok = checkStatus(api, api->GetDimensionsCount(shapeInfo, &dimensionCount),
                         QStringLiteral("read Magika output tensor rank"), err);
    if (ok && dimensionCount == 2)
        ok = checkStatus(api, api->GetDimensions(shapeInfo, dimensions, 2),
                         QStringLiteral("read Magika output tensor dimensions"), err);
    if (ok)
        ok = checkStatus(api, api->GetTensorShapeElementCount(shapeInfo, &elementCount),
                         QStringLiteral("read Magika output tensor size"), err);
    if (shapeInfo)
        api->ReleaseTensorTypeAndShapeInfo(shapeInfo);
    if (!ok || elementType != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
        || dimensionCount != 2 || dimensions[0] != 1
        || dimensions[1] != kTargetLabelCount
        || elementCount != size_t(kTargetLabelCount)) {
        api->ReleaseValue(output);
        if (ok)
            return failWith(err, QStringLiteral("Magika output tensor contract drifted"));
        return false;
    }
    void* rawScores = nullptr;
    ok = checkStatus(api, api->GetTensorMutableData(output, &rawScores),
                     QStringLiteral("read Magika output tensor data"), err);
    if (ok && !rawScores)
        ok = failWith(err, QStringLiteral("Magika output tensor data is null"));
    if (ok) {
        const float* values = static_cast<const float*>(rawScores);
        scores->resize(kTargetLabelCount);
        std::copy(values, values + kTargetLabelCount, scores->begin());
    }
    api->ReleaseValue(output);
    return ok;
}

}  // namespace

QString cleanOfficialCodeContent(const QString& content) {
    if (content.isEmpty())
        return {};
    const QStringList lines = pythonSplitLines(content);
    int start = 0;
    int end = lines.size();
    if (!lines.isEmpty() && lines.first().startsWith(QStringLiteral("```")))
        start = 1;
    if (end > start && pythonStrip(lines.at(end - 1)) == QLatin1String("```"))
        --end;
    if (start >= end)
        return {};
    return pythonStrip(lines.mid(start, end - start).join(QLatin1Char('\n')));
}

QByteArray normalizeOfficialCodeUtf8(const QString& code) {
    if (code.isEmpty())
        return {};
    QString normalized;
    normalized.reserve(code.size());
    for (int index = 0; index < code.size(); ++index) {
        const QChar current = code.at(index);
        if (current.isHighSurrogate()) {
            if (index + 1 < code.size() && code.at(index + 1).isLowSurrogate()) {
                normalized.append(current);
                normalized.append(code.at(++index));
            }
            continue;
        }
        if (current.isLowSurrogate())
            continue;
        normalized.append(current);
    }
    return normalized.toUtf8();
}

bool prepareOfficialMagikaFeatures(const QByteArray& content,
                                   OfficialMagikaFeatures* features,
                                   QString* err) {
    if (!features)
        return failWith(err, QStringLiteral("Magika feature output is null"));
    *features = {};
    if (content.size() < kMinimumMeaningfulBytes) {
        if (err)
            err->clear();
        return true;
    }

    const int bytesToRead = std::min(kBlockSize, content.size());
    QByteArray beginning = bytesLStrip(content.left(bytesToRead));
    QByteArray ending = bytesRStrip(content.right(bytesToRead));
    beginning = beginning.left(kBegSize);
    if (ending.size() > kEndSize)
        ending = ending.right(kEndSize);

    QVector<std::int32_t> tensor;
    tensor.reserve(kFeatureSize);
    for (const char byte : beginning)
        tensor.append(static_cast<unsigned char>(byte));
    while (tensor.size() < kBegSize)
        tensor.append(kPaddingToken);
    for (int index = ending.size(); index < kEndSize; ++index)
        tensor.append(kPaddingToken);
    for (const char byte : ending)
        tensor.append(static_cast<unsigned char>(byte));
    if (tensor.size() != kFeatureSize)
        return failWith(err, QStringLiteral("Magika feature tensor construction drifted"));

    // Official few-byte branch after whitespace normalization. The narrow
    // MinerU caller originates from valid UTF-8 text and maps unknown to txt.
    if (tensor.at(kMinimumMeaningfulBytes - 1) == kPaddingToken) {
        if (err)
            err->clear();
        return true;
    }
    features->bytes = std::move(tensor);
    features->requiresInference = true;
    if (err)
        err->clear();
    return true;
}

bool finalizeOfficialMagikaPrediction(const QStringList& labels,
                                      const QHash<QString, double>& thresholds,
                                      double mediumConfidenceThreshold,
                                      const QVector<float>& scores,
                                      OfficialCodeLanguageResult* result,
                                      QString* err) {
    if (!result)
        return failWith(err, QStringLiteral("Magika prediction output is null"));
    *result = {};
    if (labels.size() != kTargetLabelCount || scores.size() != labels.size()) {
        return failWith(err, QStringLiteral("Magika labels/scores shape drifted"));
    }
    int bestIndex = 0;
    float bestScore = scores.at(0);
    // Python max(range(...), key=...) keeps the first maximum. Strict > is
    // therefore required; >= would incorrectly select the last tied label.
    for (int index = 1; index < scores.size(); ++index) {
        if (scores.at(index) > bestScore) {
            bestIndex = index;
            bestScore = scores.at(index);
        }
    }
    const QString rawLabel = labels.at(bestIndex);
    QString outputLabel = rawLabel;
    if (rawLabel == QLatin1String("randombytes"))
        outputLabel = QStringLiteral("unknown");
    else if (rawLabel == QLatin1String("randomtxt"))
        outputLabel = QStringLiteral("txt");

    const double threshold = thresholds.value(rawLabel, mediumConfidenceThreshold);
    if (!(double(bestScore) >= threshold))
        outputLabel = QStringLiteral("txt");
    if (outputLabel == QLatin1String("unknown"))
        outputLabel = QStringLiteral("txt");

    result->label = outputLabel;
    result->rawLabel = rawLabel;
    result->score = bestScore;
    result->usedInference = true;
    if (err)
        err->clear();
    return true;
}

QString resolveOfficialMagikaModelDir() {
    const QString overridePath = unicodeEnvironmentVariable(
        "SCANENGINE_MAGIKA_MODEL_DIR", L"SCANENGINE_MAGIKA_MODEL_DIR");
    if (!overridePath.isEmpty())
        return absoluteOverridePath(overridePath);
    static const QString configuredPath = [] {
        QString root = configuredMagikaRoot();
        if (root.isEmpty()) {
            root = QDir(defaultModelsDir())
                       .filePath(QStringLiteral("magika/standard_v3_3"));
        }
        return QDir::cleanPath(root);
    }();
    return configuredPath;
}

QString resolveOfficialMagikaRuntimePath() {
    const QString overridePath = unicodeEnvironmentVariable(
        "SCANENGINE_ORT_RUNTIME", L"SCANENGINE_ORT_RUNTIME");
    if (!overridePath.isEmpty())
        return absoluteOverridePath(overridePath);

    const QDir appDir(repoRoot());
#ifdef Q_OS_WIN
    return appDir.filePath(QStringLiteral(SCANENGINE_ORT_RUNTIME_NAME));
#else
    const QStringList candidates = {
        appDir.filePath(QStringLiteral("Frameworks/")
                        + QStringLiteral(SCANENGINE_ORT_RUNTIME_NAME)),
        QDir(appDir.filePath(QStringLiteral("../Frameworks")))
            .filePath(QStringLiteral(SCANENGINE_ORT_RUNTIME_NAME)),
        appDir.filePath(QStringLiteral(SCANENGINE_ORT_RUNTIME_NAME)),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo::exists(candidate))
            return QDir::cleanPath(candidate);
    }
    return QDir::cleanPath(candidates.first());
#endif
}

bool ensureOfficialMagikaModel(QString* err) {
    std::shared_ptr<RuntimeState> state;
    return acquireRuntime(&state, err);
}

bool inferOfficialMagikaScores(const QString& cleanedCode,
                               QVector<float>* scores,
                               bool* requiresInference,
                               QString* err) {
    if (!scores || !requiresInference)
        return failWith(err, QStringLiteral("Magika raw-score output is null"));
    scores->clear();
    *requiresInference = false;
    const QByteArray content = normalizeOfficialCodeUtf8(cleanedCode);
    OfficialMagikaFeatures features;
    if (!prepareOfficialMagikaFeatures(content, &features, err))
        return false;
    if (!features.requiresInference) {
        if (err)
            err->clear();
        return true;
    }
    std::shared_ptr<RuntimeState> state;
    if (!acquireRuntime(&state, err))
        return false;
    if (!runBatchOne(*state, features.bytes, scores, err))
        return false;
    *requiresInference = true;
    return true;
}

bool guessOfficialCodeLanguage(const QString& cleanedCode,
                               OfficialCodeLanguageResult* result,
                               QString* err,
                               const OfficialCodeLanguageHooks* hooks) {
    if (!result)
        return failWith(err, QStringLiteral("official code language result is null"));
    *result = {};
    const QByteArray content = normalizeOfficialCodeUtf8(cleanedCode);
    if (content.isEmpty()) {
        if (err)
            err->clear();
        return true;
    }
    OfficialMagikaFeatures features;
    if (!prepareOfficialMagikaFeatures(content, &features, err))
        return false;
    if (!features.requiresInference) {
        result->label = features.immediateLabel;
        if (err)
            err->clear();
        return true;
    }

    std::shared_ptr<RuntimeState> state;
    if (!acquireRuntime(&state, err))
        return false;
    QVector<float> scores;
    QString inferenceError;
    const bool inferenceOk = hooks && hooks->infer
                                 ? hooks->infer(features.bytes, &scores, &inferenceError)
                                 : runBatchOne(*state, features.bytes, &scores,
                                               &inferenceError);
    if (!inferenceOk) {
        result->label = QStringLiteral("txt");
        result->usedInference = true;
        result->diagnostic = QStringLiteral("Magika inference failed; official txt fallback: %1")
                                 .arg(inferenceError);
        qWarning().noquote() << result->diagnostic;
        if (err)
            err->clear();
        return true;
    }
    return finalizeOfficialMagikaPrediction(
        state->config.labels, state->config.thresholds,
        state->config.mediumConfidenceThreshold, scores, result, err);
}

quint64 officialMagikaSuccessfulSessionLoadCount() {
    return successfulLoadCount().load(std::memory_order_relaxed);
}

}  // namespace hybrid
}  // namespace scanengine
