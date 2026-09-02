#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace scanengine {
namespace hybrid {

struct SafetensorInfo {
    QString name;
    QString dtype;
    QVector<qint64> shape;
    qint64 dataBegin = 0;  // absolute file offset
    qint64 dataEnd = 0;
};

struct SafetensorsFile {
    QString path;
    QStringList parts;
    qint64 headerBytes = 0;
    qint64 dataStart = 0;
    QVector<SafetensorInfo> tensors;
    QHash<QString, int> index;
};

const SafetensorInfo* findTensor(const SafetensorsFile& file, const QString& name);

bool openSafetensors(const QString& path, SafetensorsFile* out, QString* err);

// Load a contiguous row range of a 2D tensor as float32 (BF16/F32/F16 source).
bool loadTensorRowsF32(const SafetensorsFile& file,
                       const QString& name,
                       qint64 rowStart,
                       qint64 rowCount,
                       QVector<float>* out,
                       QString* err);

bool loadTensorF32(const SafetensorsFile& file,
                   const QString& name,
                   QVector<float>* out,
                   QVector<qint64>* shape,
                   QString* err);

// Visit an exact, element-aligned byte range of a BF16 tensor without first
// expanding it to float32. tensorByteOffset is relative to the tensor, and the
// QByteArray passed to the callback is valid only for that callback. All
// metadata, logical-range, and physical-part checks complete before the first
// callback. A zero-byte visit succeeds without invoking the callback. A false
// callback return propagates a non-empty callbackError verbatim, or reports
// cancellation when callbackError is empty. Callers must treat any data
// received from a failed visit as uncommitted and discard it.
using SafetensorBF16ChunkCallback =
    std::function<bool(qint64 tensorByteOffset,
                       const QByteArray& bytes,
                       QString* callbackError)>;

bool visitTensorBF16Chunks(const SafetensorsFile& file,
                           const QString& name,
                           const QVector<qint64>& expectedShape,
                           qint64 tensorByteOffset,
                           qint64 byteCount,
                           qint64 maxChunkBytes,
                           const SafetensorBF16ChunkCallback& callback,
                           QString* err);

qint64 tensorNumel(const SafetensorInfo& t);

}  // namespace hybrid
}  // namespace scanengine
