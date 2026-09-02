#include "scanengine/hybrid/ocr_ppv6.hpp"

#include "scanengine/hybrid/ocr.hpp"
#include "scanengine/hybrid/safetensors.hpp"

#include <QByteArray>
#include <QDir>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#if defined(SCANENGINE_MLX)
#include "scanengine/hybrid/mlx_windows_static.hpp"
#include <mlx/mlx.h>
namespace mx = mlx::core;
#endif

namespace scanengine {
namespace hybrid {
#if defined(SCANENGINE_MLX)

namespace {

mx::array relu(const mx::array& x) {
    return mx::maximum(x, mx::array(0.0f));
}
mx::array silu(const mx::array& x) {
    return x * mx::sigmoid(x);
}
mx::array gelu(const mx::array& x) {
    const float inv = 0.7071067811865476f;
    return x * mx::array(0.5f) * (mx::array(1.0f) + mx::erf(x * mx::array(inv)));
}
mx::array hardsigmoid(const mx::array& x) {
    return mx::clip(x + mx::array(3.0f), mx::array(0.0f), mx::array(6.0f)) / mx::array(6.0f);
}

mx::array loadF32(const SafetensorsFile& file, const QString& name) {
    QVector<float> data;
    QVector<qint64> shape;
    QString err;
    if (!loadTensorF32(file, name, &data, &shape, &err))
        throw std::runtime_error(err.toStdString());
    std::vector<int> sh;
    for (qint64 d : shape)
        sh.push_back(int(d));
    return mx::array(data.constData(), mx::Shape(sh.begin(), sh.end()));
}

mx::array convW(const SafetensorsFile& file, const QString& name) {
    // OIHW -> OHWI
    return mx::transpose(loadF32(file, name), {0, 2, 3, 1});
}

struct ConvBN {
    mx::array w = mx::array(0.0f);
    mx::array mean = mx::array(0.0f);
    mx::array var = mx::array(0.0f);
    mx::array gamma = mx::array(0.0f);
    mx::array beta = mx::array(0.0f);
    int strideH = 1;
    int strideW = 1;
    int padH = 0;
    int padW = 0;
    int groups = 1;
    bool hasBias = false;
    mx::array convBias = mx::array(0.0f);
    QString act;

    mx::array operator()(const mx::array& x) const {
        mx::array y = mx::conv2d(x, w, {strideH, strideW}, {padH, padW}, {1, 1}, groups);
        if (hasBias)
            y = y + convBias;
        const mx::array scale = gamma / mx::sqrt(var + mx::array(1e-5f));
        y = (y - mean) * scale + beta;
        if (act == QLatin1String("relu"))
            return relu(y);
        if (act == QLatin1String("silu"))
            return silu(y);
        if (act == QLatin1String("gelu"))
            return gelu(y);
        return y;
    }
};

ConvBN loadConvBN(const SafetensorsFile& file,
                  const QString& prefix,
                  int strideH,
                  int strideW,
                  int padH,
                  int padW,
                  int groups,
                  const QString& act,
                  const QString& convName = QStringLiteral("convolution"),
                  const QString& bnName = QStringLiteral("normalization")) {
    ConvBN c;
    c.w = convW(file, prefix + QLatin1Char('.') + convName + QStringLiteral(".weight"));
    c.mean = loadF32(file, prefix + QLatin1Char('.') + bnName + QStringLiteral(".running_mean"));
    c.var = loadF32(file, prefix + QLatin1Char('.') + bnName + QStringLiteral(".running_var"));
    c.gamma = loadF32(file, prefix + QLatin1Char('.') + bnName + QStringLiteral(".weight"));
    c.beta = loadF32(file, prefix + QLatin1Char('.') + bnName + QStringLiteral(".bias"));
    const auto sh = c.w.shape();
    // broadcast BN over NHWC: reshape to {1,1,1,C}
    c.mean = mx::reshape(c.mean, {1, 1, 1, sh[0]});
    c.var = mx::reshape(c.var, {1, 1, 1, sh[0]});
    c.gamma = mx::reshape(c.gamma, {1, 1, 1, sh[0]});
    c.beta = mx::reshape(c.beta, {1, 1, 1, sh[0]});
    c.strideH = strideH;
    c.strideW = strideW;
    c.padH = padH;
    c.padW = padW;
    c.groups = groups;
    c.act = act;
    const QString biasName = prefix + QLatin1Char('.') + convName + QStringLiteral(".bias");
    if (findTensor(file, biasName)) {
        c.hasBias = true;
        c.convBias = mx::reshape(loadF32(file, biasName), {1, 1, 1, sh[0]});
    }
    return c;
}

mx::array padBR(const mx::array& x) {
    // NHWC pad bottom+right 1
    return mx::pad(x, {{0, 0}, {0, 1}, {0, 1}, {0, 0}});
}

mx::array maxPool2s1(const mx::array& x) {
    const auto sh = x.shape();
    const int h = sh[1];
    const int w = sh[2];
    auto a = mx::slice(x, {0, 0, 0, 0}, {sh[0], h - 1, w - 1, sh[3]});
    auto b = mx::slice(x, {0, 0, 1, 0}, {sh[0], h - 1, w, sh[3]});
    auto c = mx::slice(x, {0, 1, 0, 0}, {sh[0], h, w - 1, sh[3]});
    auto d = mx::slice(x, {0, 1, 1, 0}, {sh[0], h, w, sh[3]});
    return mx::maximum(mx::maximum(a, b), mx::maximum(c, d));
}

mx::array nearestUp(const mx::array& x, int scale) {
    return mx::repeat(mx::repeat(x, scale, 1), scale, 2);
}

mx::array seLcnet(const mx::array& x, const mx::array& w1, const mx::array& b1, const mx::array& w2, const mx::array& b2) {
    auto s = mx::mean(x, {1, 2}, true);
    s = mx::conv2d(s, w1) + b1;
    s = relu(s);
    s = mx::conv2d(s, w2) + b2;
    s = hardsigmoid(s);
    return x * s;
}

mx::array seRep(const mx::array& x, const mx::array& w1, const mx::array& b1, const mx::array& w2, const mx::array& b2) {
    auto s = mx::mean(x, {1, 2}, true);
    s = mx::conv2d(s, w1) + b1;
    s = relu(s);
    s = mx::conv2d(s, w2) + b2;
    s = mx::clip(s * mx::array(0.2f) + mx::array(0.5f), mx::array(0.0f), mx::array(1.0f));
    return x * s;
}

struct DwBlock {
    bool useRepDw = false;
    bool residual = false;
    bool useSe = false;
    mx::array tokenW = mx::array(0.0f);
    mx::array tokenB = mx::array(0.0f);
    ConvBN tokenBn;
    mx::array seW1 = mx::array(0.0f);
    mx::array seB1 = mx::array(0.0f);
    mx::array seW2 = mx::array(0.0f);
    mx::array seB2 = mx::array(0.0f);
    ConvBN ch1;
    ConvBN ch2;
    int k = 3;
    int strideH = 1;
    int strideW = 1;

    mx::array operator()(const mx::array& x) const {
        mx::array h = x;
        if (useRepDw) {
            h = mx::conv2d(h, tokenW, {1, 1}, {k / 2, k / 2}, {1, 1}, tokenW.shape()[0]) + tokenB;
        } else {
            h = tokenBn(h);
        }
        if (useSe)
            h = seLcnet(h, seW1, seB1, seW2, seB2);
        const mx::array residualIn = h;
        h = gelu(ch1(h));
        h = ch2(h);
        if (residual)
            h = residualIn + h;
        return h;
    }
};

DwBlock loadDw(const SafetensorsFile& file,
               const QString& prefix,
               int inC,
               int outC,
               int k,
               int strideH,
               int strideW,
               bool se) {
    DwBlock b;
    b.k = k;
    b.strideH = strideH;
    b.strideW = strideW;
    b.useSe = se;
    b.residual = (inC == outC && strideH == 1 && strideW == 1);
    b.useRepDw = (strideH == 1 && strideW == 1 && inC == outC);
    if (b.useRepDw) {
        b.tokenW = convW(file, prefix + QStringLiteral(".token_conv.weight"));
        b.tokenB = mx::reshape(loadF32(file, prefix + QStringLiteral(".token_conv.bias")), {1, 1, 1, outC});
    } else {
        b.tokenBn = loadConvBN(file, prefix + QStringLiteral(".token_conv"), strideH, strideW, k / 2, k / 2, inC,
                               QString());
    }
    if (se) {
        b.seW1 = convW(file, prefix + QStringLiteral(".token_squeeze_excitation.convolutions.0.weight"));
        b.seB1 = mx::reshape(loadF32(file, prefix + QStringLiteral(".token_squeeze_excitation.convolutions.0.bias")),
                             {1, 1, 1, inC / 4});
        b.seW2 = convW(file, prefix + QStringLiteral(".token_squeeze_excitation.convolutions.2.weight"));
        b.seB2 = mx::reshape(loadF32(file, prefix + QStringLiteral(".token_squeeze_excitation.convolutions.2.bias")),
                             {1, 1, 1, inC});
    }
    b.ch1 = loadConvBN(file, prefix + QStringLiteral(".channel_conv1"), 1, 1, 0, 0, 1, QString());
    b.ch2 = loadConvBN(file, prefix + QStringLiteral(".channel_conv2"), 1, 1, 0, 0, 1, QString());
    return b;
}

struct Stem {
    ConvBN stem1;
    ConvBN stem2a;
    ConvBN stem2b;
    ConvBN stem3;
    ConvBN stem4;
    mx::array operator()(const mx::array& x) const {
        mx::array e = stem1(x);
        e = padBR(e);
        mx::array a = stem2a(e);
        a = padBR(a);
        a = stem2b(a);
        const mx::array p = maxPool2s1(e);
        e = mx::concatenate({p, a}, 3);
        e = stem3(e);
        return stem4(e);
    }
};

Stem loadStem(const SafetensorsFile& file, const QString& p, int c1, int c2) {
    Stem s;
    s.stem1 = loadConvBN(file, p + QStringLiteral(".stem1"), 2, 2, 1, 1, 1, QStringLiteral("relu"));
    s.stem2a = loadConvBN(file, p + QStringLiteral(".stem2a"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    s.stem2b = loadConvBN(file, p + QStringLiteral(".stem2b"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    s.stem3 = loadConvBN(file, p + QStringLiteral(".stem3"), 2, 2, 1, 1, 1, QStringLiteral("relu"));
    s.stem4 = loadConvBN(file, p + QStringLiteral(".stem4"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    (void)c1;
    (void)c2;
    return s;
}

struct InsertConv {
    mx::array inW = mx::array(0.0f);
    mx::array se1w = mx::array(0.0f);
    mx::array se1b = mx::array(0.0f);
    mx::array se2w = mx::array(0.0f);
    mx::array se2b = mx::array(0.0f);
    mx::array operator()(const mx::array& x) const {
        mx::array h = mx::conv2d(x, inW);
        return h + seRep(h, se1w, se1b, se2w, se2b);
    }
};

struct InputConv {
    mx::array dwW = mx::array(0.0f);
    mx::array dwB = mx::array(0.0f);
    mx::array pwW = mx::array(0.0f);
    mx::array se1w = mx::array(0.0f);
    mx::array se1b = mx::array(0.0f);
    mx::array se2w = mx::array(0.0f);
    mx::array se2b = mx::array(0.0f);
    mx::array operator()(const mx::array& x) const {
        const int g = dwW.shape()[0];
        mx::array h = mx::conv2d(x, dwW, {1, 1}, {3, 3}, {1, 1}, g) + dwB;
        h = mx::conv2d(h, pwW);
        return h + seRep(h, se1w, se1b, se2w, se2b);
    }
};

struct DetNet {
    Stem stem;
    std::vector<DwBlock> blocks;
    std::vector<int> stageEnds;
    InsertConv insert[4];
    InputConv inputc[4];
    ConvBN down;
    ConvBN up;
    mx::array finalW = mx::array(0.0f);
    mx::array finalB = mx::array(0.0f);
};

struct RecNet {
    Stem stem;
    std::vector<DwBlock> blocks;
    ConvBN skip;
    ConvBN red;
    ConvBN local;
    mx::array lnW = mx::array(0.0f);
    mx::array lnB = mx::array(0.0f);
    mx::array qkvW[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array qkvB[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array projW[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array projB[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array n1w[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array n1b[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array n2w[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array n2b[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array fc1W[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array fc1B[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array fc2W[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array fc2B[2] = {mx::array(0.0f), mx::array(0.0f)};
    mx::array ctcW = mx::array(0.0f);
    mx::array ctcB = mx::array(0.0f);
};

DetNet loadDet(const SafetensorsFile& file) {
    DetNet n;
    const QString enc = QStringLiteral("model.backbone.encoder");
    n.stem = loadStem(file, enc + QStringLiteral(".convolution"), 24, 48);
    // NET_CONFIG_DET small
    const int cfg[][6] = {
        // stage, k, in, out, s, se
        {0, 3, 48, 48, 1, 1}, {0, 3, 48, 48, 1, 0},
        {1, 3, 48, 96, 2, 0}, {1, 3, 96, 96, 1, 1}, {1, 3, 96, 96, 1, 0},
        {2, 3, 96, 192, 2, 0}, {2, 3, 192, 192, 1, 1}, {2, 3, 192, 192, 1, 0},
        {2, 3, 192, 192, 1, 1}, {2, 3, 192, 192, 1, 0},
        {3, 3, 192, 384, 2, 0}, {3, 3, 384, 384, 1, 1}, {3, 3, 384, 384, 1, 0},
    };
    int idx[4] = {0, 0, 0, 0};
    int lastStage = -1;
    for (const auto& row : cfg) {
        const int st = row[0];
        const QString p = enc + QStringLiteral(".blocks.%1.blocks.%2").arg(st).arg(idx[st]++);
        n.blocks.push_back(loadDw(file, p, row[2], row[3], row[1], row[4], row[4], row[5] != 0));
        if (st != lastStage && lastStage >= 0)
            n.stageEnds.push_back(int(n.blocks.size()) - 1);
        lastStage = st;
    }
    n.stageEnds.push_back(int(n.blocks.size()));
    // stageEnds should be after each stage: 2, 5, 10, 13
    n.stageEnds = {2, 5, 10, 13};
    for (int i = 0; i < 4; ++i) {
        const QString ip = QStringLiteral("model.neck.insert_conv.%1").arg(i);
        n.insert[i].inW = convW(file, ip + QStringLiteral(".in_conv.weight"));
        n.insert[i].se1w = convW(file, ip + QStringLiteral(".squeeze_excitation_block.conv1.weight"));
        n.insert[i].se1b = mx::reshape(loadF32(file, ip + QStringLiteral(".squeeze_excitation_block.conv1.bias")),
                                       {1, 1, 1, 24});
        n.insert[i].se2w = convW(file, ip + QStringLiteral(".squeeze_excitation_block.conv2.weight"));
        n.insert[i].se2b = mx::reshape(loadF32(file, ip + QStringLiteral(".squeeze_excitation_block.conv2.bias")),
                                       {1, 1, 1, 96});
        const QString op = QStringLiteral("model.neck.input_conv.%1").arg(i);
        n.inputc[i].dwW = convW(file, op + QStringLiteral(".depthwise_convolution.weight"));
        n.inputc[i].dwB = mx::reshape(loadF32(file, op + QStringLiteral(".depthwise_convolution.bias")), {1, 1, 1, 96});
        n.inputc[i].pwW = convW(file, op + QStringLiteral(".pointwise_convolution.weight"));
        n.inputc[i].se1w = convW(file, op + QStringLiteral(".squeeze_excitation_module.conv1.weight"));
        n.inputc[i].se1b =
            mx::reshape(loadF32(file, op + QStringLiteral(".squeeze_excitation_module.conv1.bias")), {1, 1, 1, 6});
        n.inputc[i].se2w = convW(file, op + QStringLiteral(".squeeze_excitation_module.conv2.weight"));
        n.inputc[i].se2b =
            mx::reshape(loadF32(file, op + QStringLiteral(".squeeze_excitation_module.conv2.bias")), {1, 1, 1, 24});
    }
    n.down = loadConvBN(file, QStringLiteral("head.conv_down"), 1, 1, 1, 1, 1, QStringLiteral("relu"),
                        QStringLiteral("convolution"), QStringLiteral("norm"));
    n.up = loadConvBN(file, QStringLiteral("head.conv_up"), 2, 2, 0, 0, 1, QStringLiteral("relu"),
                      QStringLiteral("convolution"), QStringLiteral("norm"));
    // ConvTranspose2d weight is (C_in, C_out, k, k) -> MLX (C_out, k, k, C_in)
    n.up.w = mx::transpose(loadF32(file, QStringLiteral("head.conv_up.convolution.weight")), {1, 2, 3, 0});
    n.finalW = mx::transpose(loadF32(file, QStringLiteral("head.conv_final.weight")), {1, 2, 3, 0});
    n.finalB = mx::reshape(loadF32(file, QStringLiteral("head.conv_final.bias")), {1, 1, 1, 1});
    return n;
}

mx::array convT2(const mx::array& x, const mx::array& w, const mx::array* bias, int stride) {
    mx::array y = mx::conv_transpose2d(x, w, {stride, stride}, {0, 0}, {1, 1}, {0, 0}, 1);
    if (bias)
        y = y + *bias;
    return y;
}

mx::array detForward(const DetNet& n, const mx::array& xNchw) {
    // NCHW -> NHWC
    mx::array x = mx::transpose(xNchw, {0, 2, 3, 1});
    mx::array h = n.stem(x);
    std::vector<mx::array> feats;
    int bi = 0;
    for (int s = 0; s < 4; ++s) {
        const int end = n.stageEnds[size_t(s)];
        for (; bi < end; ++bi)
            h = n.blocks[size_t(bi)](h);
        feats.push_back(h);
    }
    std::vector<mx::array> fused = {n.insert[0](feats[0]), n.insert[1](feats[1]), n.insert[2](feats[2]),
                                    n.insert[3](feats[3])};
    for (int i = 2; i >= 0; --i)
        fused[size_t(i)] = fused[size_t(i)] + nearestUp(fused[size_t(i + 1)], 2);
    std::vector<mx::array> proc;
    const int scales[4] = {1, 2, 4, 8};
    for (int i = 0; i < 4; ++i) {
        mx::array f = n.inputc[i](fused[size_t(i)]);
        proc.push_back((scales[i] == 1) ? f : nearestUp(f, scales[i]));
    }
    mx::array cat = mx::concatenate({proc[3], proc[2], proc[1], proc[0]}, 3);
    mx::array m = n.down(cat);
    // conv_up is ConvTranspose2d k=2 s=2, then BN+ReLU
    {
        mx::array y = convT2(m, n.up.w, n.up.hasBias ? &n.up.convBias : nullptr, 2);
        const mx::array scale = n.up.gamma / mx::sqrt(n.up.var + mx::array(1e-5f));
        y = (y - n.up.mean) * scale + n.up.beta;
        m = relu(y);
    }
    m = convT2(m, n.finalW, &n.finalB, 2);
    m = mx::sigmoid(m);
    return mx::transpose(m, {0, 3, 1, 2});  // NCHW maps
}

RecNet loadRec(const SafetensorsFile& file) {
    RecNet n;
    const QString enc = QStringLiteral("model.backbone.encoder");
    n.stem = loadStem(file, enc + QStringLiteral(".convolution"), 48, 96);
    // rec small blocks
    struct Row {
        int st, k, in, out, sh, sw, se;
    };
    const Row cfg[] = {
        {0, 3, 96, 96, 1, 1, 1},
        {1, 3, 96, 96, 1, 1, 0},
        {1, 3, 96, 96, 1, 1, 0},
        {2, 3, 96, 192, 2, 1, 0},
        {2, 3, 192, 192, 1, 1, 1},
        {2, 3, 192, 192, 1, 1, 0},
        {2, 3, 192, 192, 1, 1, 1},
        {2, 3, 192, 192, 1, 1, 0},
        {2, 3, 192, 192, 1, 1, 1},
        {2, 3, 192, 192, 1, 1, 0},
        {3, 3, 192, 384, 2, 1, 0},
        {3, 3, 384, 384, 1, 1, 1},
        {3, 3, 384, 384, 1, 1, 0},
    };
    int idx[4] = {0, 0, 0, 0};
    for (const Row& r : cfg) {
        const QString p = enc + QStringLiteral(".blocks.%1.blocks.%2").arg(r.st).arg(idx[r.st]++);
        n.blocks.push_back(loadDw(file, p, r.in, r.out, r.k, r.sh, r.sw, r.se != 0));
    }
    n.skip = loadConvBN(file, QStringLiteral("head.encoder.conv_block.0"), 1, 1, 0, 0, 1, QStringLiteral("silu"));
    n.red = loadConvBN(file, QStringLiteral("head.encoder.conv_block.1"), 1, 1, 0, 0, 1, QStringLiteral("silu"));
    n.local = loadConvBN(file, QStringLiteral("head.encoder.conv_block.2"), 1, 1, 0, 3, 120, QStringLiteral("silu"));
    n.lnW = loadF32(file, QStringLiteral("head.encoder.norm.weight"));
    n.lnB = loadF32(file, QStringLiteral("head.encoder.norm.bias"));
    for (int i = 0; i < 2; ++i) {
        const QString p = QStringLiteral("head.encoder.svtr_block.%1").arg(i);
        n.qkvW[i] = mx::transpose(loadF32(file, p + QStringLiteral(".self_attn.qkv.weight")));
        n.qkvB[i] = loadF32(file, p + QStringLiteral(".self_attn.qkv.bias"));
        n.projW[i] = mx::transpose(loadF32(file, p + QStringLiteral(".self_attn.projection.weight")));
        n.projB[i] = loadF32(file, p + QStringLiteral(".self_attn.projection.bias"));
        n.n1w[i] = loadF32(file, p + QStringLiteral(".layer_norm1.weight"));
        n.n1b[i] = loadF32(file, p + QStringLiteral(".layer_norm1.bias"));
        n.n2w[i] = loadF32(file, p + QStringLiteral(".layer_norm2.weight"));
        n.n2b[i] = loadF32(file, p + QStringLiteral(".layer_norm2.bias"));
        n.fc1W[i] = mx::transpose(loadF32(file, p + QStringLiteral(".mlp.fc1.weight")));
        n.fc1B[i] = loadF32(file, p + QStringLiteral(".mlp.fc1.bias"));
        n.fc2W[i] = mx::transpose(loadF32(file, p + QStringLiteral(".mlp.fc2.weight")));
        n.fc2B[i] = loadF32(file, p + QStringLiteral(".mlp.fc2.bias"));
    }
    n.ctcW = mx::transpose(loadF32(file, QStringLiteral("head.head.weight")));
    n.ctcB = loadF32(file, QStringLiteral("head.head.bias"));
    return n;
}

mx::array layerNorm(const mx::array& x, const mx::array& w, const mx::array& b, float eps) {
    auto mu = mx::mean(x, -1, true);
    auto v = mx::mean(mx::square(x - mu), -1, true);
    return (x - mu) * mx::rsqrt(v + mx::array(eps)) * w + b;
}

mx::array svtrBlock(const RecNet& n, int i, const mx::array& x) {
    auto h = layerNorm(x, n.n1w[i], n.n1b[i], 1e-6f);
    auto qkv = mx::add(mx::matmul(h, n.qkvW[i]), n.qkvB[i]);
    const auto sh = qkv.shape();
    const int B = sh[0];
    const int L = sh[1];
    qkv = mx::reshape(qkv, {B, L, 3, 8, 15});
    qkv = mx::transpose(qkv, {2, 0, 3, 1, 4});
    auto q = mx::take(qkv, 0, 0);
    auto k = mx::take(qkv, 1, 0);
    auto v = mx::take(qkv, 2, 0);
    auto attn = mx::matmul(q, mx::transpose(k, {0, 1, 3, 2})) * mx::array(1.0f / std::sqrt(15.0f));
    attn = mx::softmax(attn, -1);
    auto o = mx::matmul(attn, v);
    o = mx::reshape(mx::transpose(o, {0, 2, 1, 3}), {B, L, 120});
    o = mx::add(mx::matmul(o, n.projW[i]), n.projB[i]);
    h = x + o;
    auto m = layerNorm(h, n.n2w[i], n.n2b[i], 1e-6f);
    m = silu(mx::add(mx::matmul(m, n.fc1W[i]), n.fc1B[i]));
    m = mx::add(mx::matmul(m, n.fc2W[i]), n.fc2B[i]);
    return h + m;
}

mx::array recForward(const RecNet& n, const mx::array& xNchw) {
    mx::array x = mx::transpose(xNchw, {0, 2, 3, 1});
    mx::array h = n.stem(x);
    for (const DwBlock& b : n.blocks)
        h = b(h);
    // avg_pool2d kernel [3,2] on NCHW -> convert
    h = mx::transpose(h, {0, 3, 1, 2});
    const auto sh = h.shape();
    if (sh[2] < 3)
        throw std::runtime_error("rec feature height < 3");
    // pool NCHW with kH=3 kW=2 stride same as kernel
    {
        const int nB = sh[0], c = sh[1], hh = sh[2], ww = sh[3];
        const int oh = (hh - 3) / 3 + 1;
        const int ow = (ww - 2) / 2 + 1;
        std::vector<mx::array> parts;
        for (int i = 0; i < oh; ++i) {
            for (int j = 0; j < ow; ++j) {
                auto sl = mx::slice(h, {0, 0, i * 3, j * 2}, {nB, c, i * 3 + 3, j * 2 + 2});
                parts.push_back(mx::mean(sl, {2, 3}, true));
            }
        }
        h = mx::concatenate(parts, 3);
        h = mx::reshape(h, {nB, c, oh, ow});
    }
    h = mx::transpose(h, {0, 2, 3, 1});  // NHWC
    mx::array residual = n.skip(h);
    mx::array hid = n.red(h);
    hid = hid + n.local(hid);
    const auto hs = hid.shape();
    hid = mx::reshape(hid, {hs[0], hs[1] * hs[2], hs[3]});
    hid = svtrBlock(n, 0, hid);
    hid = svtrBlock(n, 1, hid);
    hid = layerNorm(hid, n.lnW, n.lnB, 1e-6f);
    hid = mx::reshape(hid, {hs[0], hs[1], hs[2], hs[3]});
    hid = hid + residual;
    // squeeze H, permute to N,W,C
    hid = mx::squeeze(hid, 1);
    hid = mx::add(mx::matmul(hid, n.ctcW), n.ctcB);
    return hid;  // N,T,C
}

struct Nets {
    DetNet det;
    RecNet rec;
};

Nets* cached(QString* err) {
    static std::mutex mu;
    static Nets* n = nullptr;
    std::lock_guard<std::mutex> lock(mu);
    if (n)
        return n;
    try {
        const QString dir = resolveOfficialOcrDir();
        SafetensorsFile detF, recF;
        if (!openSafetensors(QDir(dir).filePath(QStringLiteral("ch_PP-OCRv6_small_det_infer.safetensors")), &detF, err))
            return nullptr;
        if (!openSafetensors(QDir(dir).filePath(QStringLiteral("ch_PP-OCRv6_small_rec_infer.safetensors")), &recF, err))
            return nullptr;
        auto* out = new Nets;
        out->det = loadDet(detF);
        out->rec = loadRec(recF);
        n = out;
        return n;
    } catch (const std::exception& ex) {
        if (err)
            *err = QString::fromUtf8(ex.what());
        return nullptr;
    }
}

}  // namespace

static bool officialOcrDetForwardMlx(const float* nchw,
                                     int height,
                                     int width,
                                     QVector<float>* maps,
                                     int* mapH,
                                     int* mapW,
                                     QString* err) {
    auto fail = [&](const QString& m) {
        if (err)
            *err = m;
        return false;
    };
    if (!nchw || !maps || !mapH || !mapW)
        return fail(QStringLiteral("null det args"));
    Nets* n = cached(err);
    if (!n)
        return false;
    try {
        mx::array x(nchw, {1, 3, height, width});
        mx::array y = detForward(n->det, x);
        mx::eval(y);
        const auto sh = y.shape();
        *mapH = sh[2];
        *mapW = sh[3];
        maps->resize(sh[0] * sh[1] * sh[2] * sh[3]);
        std::memcpy(maps->data(), y.data<float>(), size_t(maps->size()) * sizeof(float));
        if (err)
            err->clear();
        return true;
    } catch (const std::exception& ex) {
        return fail(QString::fromUtf8(ex.what()));
    }
}

static bool officialOcrRecForwardMlx(const float* nchw,
                                     int batch,
                                     int height,
                                     int width,
                                     QVector<float>* logits,
                                     int* time,
                                     int* classes,
                                     QString* err) {
    auto fail = [&](const QString& m) {
        if (err)
            *err = m;
        return false;
    };
    if (!nchw || !logits || !time || !classes)
        return fail(QStringLiteral("null rec args"));
    Nets* n = cached(err);
    if (!n)
        return false;
    try {
        mx::array x(nchw, {batch, 3, height, width});
        mx::array y = recForward(n->rec, x);
        mx::eval(y);
        const auto sh = y.shape();
        *time = sh[1];
        *classes = sh[2];
        logits->resize(sh[0] * sh[1] * sh[2]);
        std::memcpy(logits->data(), y.data<float>(), size_t(logits->size()) * sizeof(float));
        if (err)
            err->clear();
        return true;
    } catch (const std::exception& ex) {
        return fail(QString::fromUtf8(ex.what()));
    }
}

#endif

bool officialOcrDetForward(const float* nchw,
                           int height,
                           int width,
                           QVector<float>* maps,
                           int* mapH,
                           int* mapW,
                           QString* err) {
#if defined(SCANENGINE_MLX)
    return officialOcrDetForwardMlx(nchw, height, width, maps, mapH, mapW, err);
#else
    Q_UNUSED(nchw);
    Q_UNUSED(height);
    Q_UNUSED(width);
    Q_UNUSED(maps);
    Q_UNUSED(mapH);
    Q_UNUSED(mapW);
    if (err)
        *err = QStringLiteral("需要 GPU：C++ hybrid 不再提供 CPU OCR");
    return false;
#endif
}

bool officialOcrRecForward(const float* nchw,
                           int batch,
                           int height,
                           int width,
                           QVector<float>* logits,
                           int* time,
                           int* classes,
                           QString* err) {
#if defined(SCANENGINE_MLX)
    return officialOcrRecForwardMlx(nchw, batch, height, width, logits, time, classes, err);
#else
    Q_UNUSED(nchw);
    Q_UNUSED(batch);
    Q_UNUSED(height);
    Q_UNUSED(width);
    Q_UNUSED(logits);
    Q_UNUSED(time);
    Q_UNUSED(classes);
    if (err)
        *err = QStringLiteral("需要 GPU：C++ hybrid 不再提供 CPU OCR");
    return false;
#endif
}

}  // namespace hybrid
}  // namespace scanengine
