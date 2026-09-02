#include "scanengine/hybrid/safetensors.hpp"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QtEndian>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace scanengine {
namespace hybrid {

namespace {

float bitsToF32(quint32 bits) {
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

float bf16ToF32(quint16 u) {
    return bitsToF32(quint32(u) << 16);
}

float f16ToF32(quint16 h) {
    const int s = (h >> 15) & 1;
    int e = (h >> 10) & 0x1f;
    int m = h & 0x3ff;
    if (e == 0) {
        if (m == 0)
            return s ? -0.0f : 0.0f;
        while ((m & 0x400) == 0) {
            m <<= 1;
            --e;
        }
        m &= 0x3ff;
        ++e;
    } else if (e == 31) {
        const quint32 bits = (quint32(s) << 31) | 0x7f800000u | (quint32(m) << 13);
        return bitsToF32(bits);
    }
    e = e + (127 - 15);
    const quint32 bits = (quint32(s) << 31) | (quint32(e) << 23) | (quint32(m) << 13);
    return bitsToF32(bits);
}

int dtypeWidth(const QString& dtype) {
    if (dtype == QLatin1String("BF16") || dtype == QLatin1String("F16") || dtype == QLatin1String("U16")
        || dtype == QLatin1String("I16"))
        return 2;
    if (dtype == QLatin1String("F32") || dtype == QLatin1String("U32") || dtype == QLatin1String("I32"))
        return 4;
    if (dtype == QLatin1String("F64") || dtype == QLatin1String("U64") || dtype == QLatin1String("I64"))
        return 8;
    if (dtype == QLatin1String("BOOL") || dtype == QLatin1String("U8") || dtype == QLatin1String("I8"))
        return 1;
    return 0;
}

QStringList safetensorsParts(const QString& path) {
    if (QFileInfo::exists(path))
        return QStringList{path};

    QStringList parts;
    for (int index = 1;; ++index) {
        const QString part = path + QStringLiteral(".part%1").arg(index);
        if (!QFileInfo::exists(part))
            break;
        parts.push_back(part);
    }
    return parts;
}

bool readSafetensorsRange(const SafetensorsFile& file,
                          qint64 offset,
                          qint64 length,
                          QByteArray* out) {
    if (!out || offset < 0 || length < 0)
        return false;

    out->clear();
    out->reserve(int(length));
    qint64 remaining = length;
    qint64 partOffset = offset;
    for (const QString& part : file.parts) {
        const qint64 partSize = QFileInfo(part).size();
        if (partOffset >= partSize) {
            partOffset -= partSize;
            continue;
        }

        QFile input(part);
        if (!input.open(QIODevice::ReadOnly) || !input.seek(partOffset))
            return false;
        const qint64 take = std::min(remaining, partSize - partOffset);
        const QByteArray chunk = input.read(take);
        if (qint64(chunk.size()) != take)
            return false;
        out->append(chunk);
        remaining -= take;
        if (remaining == 0)
            return true;
        partOffset = 0;
    }
    return remaining == 0;
}

bool checkedAddNonNegative(qint64 left, qint64 right, qint64* out) {
    if (!out || left < 0 || right < 0
        || left > std::numeric_limits<qint64>::max() - right) {
        return false;
    }
    *out = left + right;
    return true;
}

bool checkedMultiplyNonNegative(qint64 left, qint64 right, qint64* out) {
    if (!out || left < 0 || right < 0
        || (right != 0 && left > std::numeric_limits<qint64>::max() / right)) {
        return false;
    }
    *out = left * right;
    return true;
}

bool checkedShapeNumel(const QVector<qint64>& shape, qint64* out) {
    if (!out)
        return false;
    qint64 numel = 1;
    for (qint64 dimension : shape) {
        if (!checkedMultiplyNonNegative(numel, dimension, &numel))
            return false;
    }
    *out = numel;
    return true;
}

bool shapesEqual(const QVector<qint64>& left, const QVector<qint64>& right) {
    if (left.size() != right.size())
        return false;
    for (int index = 0; index < left.size(); ++index) {
        if (left.at(index) != right.at(index))
            return false;
    }
    return true;
}

}  // namespace

const SafetensorInfo* findTensor(const SafetensorsFile& file, const QString& name) {
    const auto it = file.index.constFind(name);
    if (it == file.index.constEnd())
        return nullptr;
    const int tensorIndex = it.value();
    if (tensorIndex < 0 || tensorIndex >= file.tensors.size())
        return nullptr;
    return &file.tensors.at(tensorIndex);
}

qint64 tensorNumel(const SafetensorInfo& t) {
    qint64 n = 1;
    for (qint64 d : t.shape)
        n *= d;
    return n;
}

bool openSafetensors(const QString& path, SafetensorsFile* out, QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    const QStringList parts = safetensorsParts(path);
    if (parts.isEmpty())
        return fail(QStringLiteral("cannot open ") + path);

    QFile f(parts.first());
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("cannot open ") + path);

    QByteArray lenBuf = f.read(8);
    if (lenBuf.size() != 8)
        return fail(QStringLiteral("truncated safetensors header length"));
    const quint64 headerLen = qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(lenBuf.constData()));
    if (headerLen == 0 || headerLen > 64ull * 1024ull * 1024ull)
        return fail(QStringLiteral("invalid safetensors header length"));

    const QByteArray headerBytes = f.read(int(headerLen));
    if (quint64(headerBytes.size()) != headerLen)
        return fail(QStringLiteral("truncated safetensors header"));

    const QJsonDocument doc = QJsonDocument::fromJson(headerBytes);
    if (!doc.isObject())
        return fail(QStringLiteral("safetensors header is not JSON object"));

    SafetensorsFile file;
    file.path = path;
    file.parts = parts;
    file.headerBytes = qint64(headerLen);
    file.dataStart = 8 + qint64(headerLen);

    const QJsonObject root = doc.object();
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (it.key() == QLatin1String("__metadata__"))
            continue;
        if (!it.value().isObject())
            continue;
        const QJsonObject o = it.value().toObject();
        SafetensorInfo info;
        info.name = it.key();
        info.dtype = o.value(QStringLiteral("dtype")).toString();
        const QJsonArray shape = o.value(QStringLiteral("shape")).toArray();
        info.shape.reserve(shape.size());
        for (const QJsonValue& v : shape)
            info.shape.push_back(qint64(v.toDouble()));
        const QJsonArray offs = o.value(QStringLiteral("data_offsets")).toArray();
        if (offs.size() != 2)
            return fail(QStringLiteral("bad data_offsets for ") + info.name);
        info.dataBegin = file.dataStart + qint64(offs.at(0).toDouble());
        info.dataEnd = file.dataStart + qint64(offs.at(1).toDouble());
        file.index.insert(info.name, file.tensors.size());
        file.tensors.push_back(info);
    }

    *out = file;
    if (err)
        err->clear();
    return true;
}

bool loadTensorRowsF32(const SafetensorsFile& file,
                       const QString& name,
                       qint64 rowStart,
                       qint64 rowCount,
                       QVector<float>* out,
                       QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    const SafetensorInfo* t = findTensor(file, name);
    if (!t)
        return fail(QStringLiteral("missing tensor ") + name);
    if (t->shape.size() != 2)
        return fail(QStringLiteral("expected rank-2 tensor ") + name);
    const qint64 rows = t->shape[0];
    const qint64 cols = t->shape[1];
    if (rowStart < 0 || rowCount < 0 || rowStart + rowCount > rows)
        return fail(QStringLiteral("row range out of bounds for ") + name);
    const int width = dtypeWidth(t->dtype);
    if (width <= 0)
        return fail(QStringLiteral("unsupported dtype ") + t->dtype);

    const qint64 byteOff = t->dataBegin + rowStart * cols * width;
    const qint64 byteLen = rowCount * cols * width;
    QByteArray raw;
    if (!readSafetensorsRange(file, byteOff, byteLen, &raw))
        return fail(QStringLiteral("short read of ") + name);

    out->resize(int(rowCount * cols));
    const uchar* p = reinterpret_cast<const uchar*>(raw.constData());
    float* dst = out->data();
    const qint64 n = rowCount * cols;
    if (t->dtype == QLatin1String("BF16")) {
        for (qint64 i = 0; i < n; ++i)
            dst[i] = bf16ToF32(qFromLittleEndian<quint16>(p + i * 2));
    } else if (t->dtype == QLatin1String("F16")) {
        for (qint64 i = 0; i < n; ++i)
            dst[i] = f16ToF32(qFromLittleEndian<quint16>(p + i * 2));
    } else if (t->dtype == QLatin1String("F32")) {
        for (qint64 i = 0; i < n; ++i) {
            quint32 bits = qFromLittleEndian<quint32>(p + i * 4);
            dst[i] = bitsToF32(bits);
        }
    } else {
        return fail(QStringLiteral("cannot convert dtype ") + t->dtype);
    }
    if (err)
        err->clear();
    return true;
}

bool loadTensorF32(const SafetensorsFile& file,
                   const QString& name,
                   QVector<float>* out,
                   QVector<qint64>* shape,
                   QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    const SafetensorInfo* t = findTensor(file, name);
    if (!t)
        return fail(QStringLiteral("missing tensor ") + name);
    const qint64 n = tensorNumel(*t);
    const int width = dtypeWidth(t->dtype);
    if (width <= 0)
        return fail(QStringLiteral("unsupported dtype ") + t->dtype);
    const qint64 byteLen = t->dataEnd - t->dataBegin;
    if (byteLen != n * width)
        return fail(QStringLiteral("tensor byte size mismatch ") + name);

    QByteArray raw;
    if (!readSafetensorsRange(file, t->dataBegin, byteLen, &raw))
        return fail(QStringLiteral("short read of ") + name);

    out->resize(int(n));
    const uchar* p = reinterpret_cast<const uchar*>(raw.constData());
    float* dst = out->data();
    if (t->dtype == QLatin1String("BF16")) {
        for (qint64 i = 0; i < n; ++i)
            dst[i] = bf16ToF32(qFromLittleEndian<quint16>(p + i * 2));
    } else if (t->dtype == QLatin1String("F16")) {
        for (qint64 i = 0; i < n; ++i)
            dst[i] = f16ToF32(qFromLittleEndian<quint16>(p + i * 2));
    } else if (t->dtype == QLatin1String("F32")) {
        for (qint64 i = 0; i < n; ++i) {
            quint32 bits = qFromLittleEndian<quint32>(p + i * 4);
            dst[i] = bitsToF32(bits);
        }
    } else {
        return fail(QStringLiteral("cannot convert dtype ") + t->dtype);
    }
    if (shape)
        *shape = t->shape;
    if (err)
        err->clear();
    return true;
}

bool visitTensorBF16Chunks(const SafetensorsFile& file,
                           const QString& name,
                           const QVector<qint64>& expectedShape,
                           qint64 tensorByteOffset,
                           qint64 byteCount,
                           qint64 maxChunkBytes,
                           const SafetensorBF16ChunkCallback& callback,
                           QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };

    const SafetensorInfo* tensor = findTensor(file, name);
    if (!tensor)
        return fail(QStringLiteral("missing tensor ") + name);
    if (tensor->dtype != QLatin1String("BF16"))
        return fail(QStringLiteral("expected BF16 tensor ") + name);
    if (!shapesEqual(tensor->shape, expectedShape))
        return fail(QStringLiteral("tensor shape mismatch ") + name);
    if (!callback)
        return fail(QStringLiteral("BF16 chunk callback is empty for ") + name);
    if (tensorByteOffset < 0 || byteCount < 0)
        return fail(QStringLiteral("negative BF16 byte range for ") + name);
    if (maxChunkBytes <= 0
        || maxChunkBytes > qint64(std::numeric_limits<int>::max())) {
        return fail(QStringLiteral("invalid BF16 chunk size for ") + name);
    }
    constexpr qint64 kBf16Bytes = 2;
    if ((tensorByteOffset % kBf16Bytes) != 0 || (byteCount % kBf16Bytes) != 0
        || (maxChunkBytes % kBf16Bytes) != 0) {
        return fail(QStringLiteral("unaligned BF16 byte range or chunk size for ") + name);
    }

    qint64 numel = 0;
    qint64 tensorBytes = 0;
    if (!checkedShapeNumel(tensor->shape, &numel)
        || !checkedMultiplyNonNegative(numel, kBf16Bytes, &tensorBytes)) {
        return fail(QStringLiteral("BF16 tensor byte size overflow ") + name);
    }
    if (tensor->dataBegin < 0 || tensor->dataEnd < tensor->dataBegin
        || file.dataStart < 0 || tensor->dataBegin < file.dataStart) {
        return fail(QStringLiteral("invalid BF16 tensor offsets ") + name);
    }
    const qint64 storedBytes = tensor->dataEnd - tensor->dataBegin;
    if (storedBytes != tensorBytes)
        return fail(QStringLiteral("BF16 tensor byte size mismatch ") + name);
    if (tensorByteOffset > tensorBytes || byteCount > tensorBytes - tensorByteOffset)
        return fail(QStringLiteral("BF16 byte range out of bounds for ") + name);

    struct OpenPart {
        std::unique_ptr<QFile> file;
        qint64 begin = 0;
        qint64 size = 0;
    };
    std::vector<OpenPart> parts;
    parts.reserve(size_t(file.parts.size()));
    qint64 totalBytes = 0;
    for (const QString& partPath : file.parts) {
        auto input = std::make_unique<QFile>(partPath);
        if (!input->open(QIODevice::ReadOnly))
            return fail(QStringLiteral("cannot open safetensors part ") + partPath);
        const qint64 partBytes = input->size();
        qint64 nextTotal = 0;
        if (partBytes < 0 || !checkedAddNonNegative(totalBytes, partBytes, &nextTotal))
            return fail(QStringLiteral("safetensors part size overflow ") + partPath);
        parts.push_back(OpenPart{std::move(input), totalBytes, partBytes});
        totalBytes = nextTotal;
    }
    if (parts.empty())
        return fail(QStringLiteral("safetensors file has no parts for ") + name);
    if (tensor->dataEnd > totalBytes)
        return fail(QStringLiteral("BF16 tensor range exceeds safetensors parts ") + name);

    qint64 absoluteBegin = 0;
    qint64 absoluteEnd = 0;
    if (!checkedAddNonNegative(tensor->dataBegin, tensorByteOffset, &absoluteBegin)
        || !checkedAddNonNegative(absoluteBegin, byteCount, &absoluteEnd)
        || absoluteEnd > tensor->dataEnd) {
        return fail(QStringLiteral("BF16 absolute byte range overflow ") + name);
    }

    // Everything that can be established from metadata and the opened part
    // files has now been checked. No callback is made before this point.
    qint64 absoluteOffset = absoluteBegin;
    qint64 relativeOffset = tensorByteOffset;
    qint64 remaining = byteCount;
    size_t partIndex = 0;
    while (partIndex < parts.size()
           && absoluteOffset >= parts[partIndex].begin + parts[partIndex].size) {
        ++partIndex;
    }

    while (remaining > 0) {
        const qint64 chunkBytes = std::min(remaining, maxChunkBytes);
        QByteArray chunk;
        chunk.resize(int(chunkBytes));
        qint64 filled = 0;
        while (filled < chunkBytes) {
            while (partIndex < parts.size()
                   && absoluteOffset >= parts[partIndex].begin + parts[partIndex].size) {
                ++partIndex;
            }
            if (partIndex >= parts.size())
                return fail(QStringLiteral("short read of BF16 tensor ") + name);

            OpenPart& part = parts[partIndex];
            const qint64 localOffset = absoluteOffset - part.begin;
            const qint64 take = std::min(chunkBytes - filled, part.size - localOffset);
            if (take <= 0 || !part.file->seek(localOffset))
                return fail(QStringLiteral("cannot seek BF16 tensor ") + name);
            const qint64 readBytes = part.file->read(chunk.data() + int(filled), take);
            if (readBytes != take)
                return fail(QStringLiteral("short read of BF16 tensor ") + name);
            filled += take;
            absoluteOffset += take;
        }

        QString callbackError;
        if (!callback(relativeOffset, chunk, &callbackError)) {
            if (!callbackError.isEmpty())
                return fail(callbackError);
            return fail(QStringLiteral("BF16 chunk callback cancelled for ") + name);
        }
        relativeOffset += chunkBytes;
        remaining -= chunkBytes;
    }

    if (err)
        err->clear();
    return true;
}

}  // namespace hybrid
}  // namespace scanengine
