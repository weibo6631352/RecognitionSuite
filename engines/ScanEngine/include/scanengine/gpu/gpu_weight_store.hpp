#pragma once

#include "scanengine/hybrid/safetensors.hpp"

#include <QString>
#include <QVector>

namespace scanengine {
namespace gpu {

// Immutable, source-side description of the BF16 tensors that form one GPU
// model store.  This type deliberately contains no MLX/CUDA object: canonical
// C++ validates the safetensors identity and drives the transaction, while the
// narrow device bridge owns allocation and materialization.
struct GpuBf16TensorPlan {
    QString name;
    QVector<qint64> shape;
    qint64 byteCount = 0;
};

struct GpuBf16WeightPlan {
    QVector<GpuBf16TensorPlan> tensors;
    qint64 totalBytes = 0;
};

struct GpuWeightUploadStats {
    int tensorCount = 0;
    int completedTensorCount = 0;
    int chunkCount = 0;
    qint64 uploadedBytes = 0;
};

// Transactional device-store sink.  beginStore/commitStore delimit the only
// publication boundary: if any metadata check, read, allocation, or upload
// fails, abortStore is called and a partially loaded store must remain hidden
// from inference.  bytes is valid only during uploadChunk; implementations
// must enqueue/copy it before returning.  Callbacks return false with a
// non-empty error to preserve the precise device failure.
class GpuBf16WeightUploadSink {
public:
    virtual ~GpuBf16WeightUploadSink() = default;

    virtual bool beginStore(const GpuBf16WeightPlan& plan, QString* error) = 0;
    virtual bool beginTensor(const GpuBf16TensorPlan& tensor, QString* error) = 0;
    virtual bool uploadChunk(const GpuBf16TensorPlan& tensor,
                             qint64 tensorByteOffset,
                             const QByteArray& bytes,
                             QString* error) = 0;
    virtual bool endTensor(const GpuBf16TensorPlan& tensor, QString* error) = 0;
    virtual bool commitStore(QString* error) = 0;
    virtual void abortStore() = 0;
};

// Build a deterministic all-BF16 plan from a parsed safetensors stream.  The
// plan rejects non-BF16 payloads, duplicate names, invalid/overflowing shapes,
// byte-count drift, and an empty tensor set before a device callback is ever
// made. Tensor order is lexical by name so upload telemetry is reproducible.
bool buildGpuBf16WeightPlan(const hybrid::SafetensorsFile& file,
                            GpuBf16WeightPlan* plan,
                            QString* error);

// Stream a prevalidated plan using raw BF16 bytes.  No tensor is converted to
// float32 and no whole-model host buffer is allocated. maxChunkBytes must be a
// positive BF16-aligned size. A failed invocation aborts the store exactly once
// after beginStore succeeds; only a successful commit publishes it.
bool uploadGpuBf16WeightPlan(const hybrid::SafetensorsFile& file,
                             const GpuBf16WeightPlan& plan,
                             qint64 maxChunkBytes,
                             GpuBf16WeightUploadSink* sink,
                             GpuWeightUploadStats* stats,
                             QString* error);

}  // namespace gpu
}  // namespace scanengine
