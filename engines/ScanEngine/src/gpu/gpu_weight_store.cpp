#include "scanengine/gpu/gpu_weight_store.hpp"

#include <QSet>

#include <algorithm>
#include <limits>

namespace scanengine {
namespace gpu {

namespace {

bool checkedMultiply(qint64 left, qint64 right, qint64* out) {
    if (!out || left < 0 || right < 0
        || (right != 0 && left > std::numeric_limits<qint64>::max() / right)) {
        return false;
    }
    *out = left * right;
    return true;
}

bool checkedAdd(qint64 left, qint64 right, qint64* out) {
    if (!out || left < 0 || right < 0
        || left > std::numeric_limits<qint64>::max() - right) {
        return false;
    }
    *out = left + right;
    return true;
}

bool bf16TensorBytes(const hybrid::SafetensorInfo& tensor, qint64* out) {
    qint64 elements = 1;
    for (qint64 dimension : tensor.shape) {
        if (!checkedMultiply(elements, dimension, &elements))
            return false;
    }
    return checkedMultiply(elements, 2, out);
}

QString sinkError(const QString& operation, const QString& detail) {
    return detail.isEmpty() ? operation : detail;
}

}  // namespace

bool buildGpuBf16WeightPlan(const hybrid::SafetensorsFile& file,
                            GpuBf16WeightPlan* plan,
                            QString* error) {
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!plan)
        return fail(QStringLiteral("GPU weight plan output is null"));
    if (file.tensors.isEmpty())
        return fail(QStringLiteral("GPU weight plan has no tensors"));

    GpuBf16WeightPlan candidate;
    candidate.tensors.reserve(file.tensors.size());
    QSet<QString> names;
    for (const hybrid::SafetensorInfo& tensor : file.tensors) {
        if (tensor.name.isEmpty() || names.contains(tensor.name))
            return fail(QStringLiteral("GPU weight plan has duplicate or empty tensor name"));
        names.insert(tensor.name);
        if (tensor.dtype != QLatin1String("BF16"))
            return fail(QStringLiteral("GPU weight plan requires BF16 tensor ") + tensor.name);
        qint64 bytes = 0;
        if (!bf16TensorBytes(tensor, &bytes))
            return fail(QStringLiteral("GPU weight tensor shape overflow ") + tensor.name);
        if (tensor.dataBegin < 0 || tensor.dataEnd < tensor.dataBegin
            || tensor.dataEnd - tensor.dataBegin != bytes) {
            return fail(QStringLiteral("GPU weight tensor byte size mismatch ") + tensor.name);
        }
        qint64 total = 0;
        if (!checkedAdd(candidate.totalBytes, bytes, &total))
            return fail(QStringLiteral("GPU weight plan total byte overflow"));
        candidate.totalBytes = total;
        candidate.tensors.push_back({tensor.name, tensor.shape, bytes});
    }
    std::sort(candidate.tensors.begin(), candidate.tensors.end(),
              [](const GpuBf16TensorPlan& left, const GpuBf16TensorPlan& right) {
                  return left.name < right.name;
              });
    *plan = std::move(candidate);
    if (error)
        error->clear();
    return true;
}

bool uploadGpuBf16WeightPlan(const hybrid::SafetensorsFile& file,
                             const GpuBf16WeightPlan& plan,
                             qint64 maxChunkBytes,
                             GpuBf16WeightUploadSink* sink,
                             GpuWeightUploadStats* stats,
                             QString* error) {
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!sink)
        return fail(QStringLiteral("GPU weight upload sink is null"));
    if (plan.tensors.isEmpty() || plan.totalBytes <= 0)
        return fail(QStringLiteral("GPU weight upload plan is empty"));
    if (maxChunkBytes <= 0 || (maxChunkBytes % 2) != 0)
        return fail(QStringLiteral("GPU weight upload chunk size must be positive BF16-aligned"));

    GpuBf16WeightPlan verified;
    QString planError;
    if (!buildGpuBf16WeightPlan(file, &verified, &planError))
        return fail(planError);
    if (verified.totalBytes != plan.totalBytes
        || verified.tensors.size() != plan.tensors.size()) {
        return fail(QStringLiteral("GPU weight upload plan identity drifted"));
    }
    for (int index = 0; index < plan.tensors.size(); ++index) {
        const GpuBf16TensorPlan& expected = plan.tensors.at(index);
        const GpuBf16TensorPlan& actual = verified.tensors.at(index);
        if (expected.name != actual.name || expected.shape != actual.shape
            || expected.byteCount != actual.byteCount) {
            return fail(QStringLiteral("GPU weight upload tensor identity drifted ")
                        + expected.name);
        }
    }

    GpuWeightUploadStats candidate;
    candidate.tensorCount = plan.tensors.size();
    QString sinkFailure;
    if (!sink->beginStore(plan, &sinkFailure))
        return fail(sinkError(QStringLiteral("GPU weight store begin failed"), sinkFailure));
    bool beganStore = true;
    auto abort = [&]() {
        if (beganStore) {
            sink->abortStore();
            beganStore = false;
        }
    };

    for (const GpuBf16TensorPlan& tensor : plan.tensors) {
        sinkFailure.clear();
        if (!sink->beginTensor(tensor, &sinkFailure)) {
            abort();
            return fail(sinkError(QStringLiteral("GPU weight tensor begin failed: ")
                                  + tensor.name, sinkFailure));
        }
        QString visitorError;
        const bool visited = hybrid::visitTensorBF16Chunks(
            file, tensor.name, tensor.shape, 0, tensor.byteCount, maxChunkBytes,
            [&](qint64 offset, const QByteArray& bytes, QString* callbackError) {
                sinkFailure.clear();
                if (!sink->uploadChunk(tensor, offset, bytes, &sinkFailure)) {
                    if (callbackError)
                        *callbackError = sinkError(
                            QStringLiteral("GPU weight upload failed: ") + tensor.name,
                            sinkFailure);
                    return false;
                }
                ++candidate.chunkCount;
                candidate.uploadedBytes += bytes.size();
                return true;
            },
            &visitorError);
        if (!visited) {
            abort();
            return fail(visitorError);
        }
        sinkFailure.clear();
        if (!sink->endTensor(tensor, &sinkFailure)) {
            abort();
            return fail(sinkError(QStringLiteral("GPU weight tensor end failed: ")
                                  + tensor.name, sinkFailure));
        }
        ++candidate.completedTensorCount;
    }
    if (candidate.uploadedBytes != plan.totalBytes) {
        abort();
        return fail(QStringLiteral("GPU weight upload byte accounting mismatch"));
    }
    sinkFailure.clear();
    if (!sink->commitStore(&sinkFailure)) {
        abort();
        return fail(sinkError(QStringLiteral("GPU weight store commit failed"), sinkFailure));
    }
    beganStore = false;
    if (stats)
        *stats = candidate;
    if (error)
        error->clear();
    return true;
}

}  // namespace gpu
}  // namespace scanengine
