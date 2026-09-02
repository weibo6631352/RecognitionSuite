#include "scanengine/hybrid/layout_v2.hpp"

#include "scanengine/hybrid/layout.hpp"
#include "scanengine/hybrid/safetensors.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
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

constexpr int kD = 256;
constexpr int kHeads = 8;
constexpr int kHeadDim = 32;
constexpr int kQueries = 300;
constexpr int kClasses = 25;
constexpr int kLevels = 3;
constexpr int kPoints = 4;
constexpr int kOrderHidden = 512;
constexpr int kOrderHeads = 8;
constexpr int kOrderHeadDim = 64;

mx::array relu(const mx::array& x) {
    return mx::maximum(x, mx::array(0.0f));
}
// Official ACT2FN / F.silu is x / (1 + exp(-x)). MLX exp is a different
// rounding than the CUDA F.silu kernel; match that expression in-register.
mx::array silu(const mx::array& x) {
    if (x.ndim() != 4)
        return x / (mx::array(1.0f) + mx::exp(-x));
    static auto kernel = mx::fast::cuda_kernel(
        "scanengine_official_silu",
        {"x"},
        {"y"},
        R"(
          auto idx = cg::this_grid().thread_rank();
          const int n = x_shape[0] * x_shape[1] * x_shape[2] * x_shape[3];
          if (static_cast<int>(idx) >= n) return;
          const float v = x[idx];
          y[idx] = v / (1.0f + exp(-v));
        )");
    const auto sh = x.shape();
    const int n = sh[0] * sh[1] * sh[2] * sh[3];
    auto outs = kernel(
        {mx::contiguous(x)},
        {sh},
        {x.dtype()},
        {n, 1, 1},
        {256, 1, 1},
        {},
        std::nullopt,
        false,
        {});
    return outs.front();
}

// Official nn.BatchNorm2d eval is aten/cudnn batch_norm (bitwise match).
// SiLU is F.silu: x / (1 + exp(-x)).
mx::array officialBnEval(const mx::array& x, const mx::array& mean, const mx::array& var,
                         const mx::array& gamma, const mx::array& beta, bool applySilu) {
    mx::array y = mx::fast::batch_norm_eval(x, mean, var, gamma, beta, 1e-5f);
    if (applySilu)
        return silu(y);
    return y;
}
mx::array loadF32(const SafetensorsFile& file, const QString& name) {
    QVector<float> data;
    QVector<qint64> shape;
    QString err;
    if (!loadTensorF32(file, name, &data, &shape, &err))
        throw std::runtime_error(("missing " + name + ": " + err).toStdString());
    std::vector<int> sh;
    for (qint64 d : shape)
        sh.push_back(int(d));
    return mx::array(data.constData(), mx::Shape(sh.begin(), sh.end()));
}

mx::array convW(const SafetensorsFile& file, const QString& name) {
    return mx::transpose(loadF32(file, name), {0, 2, 3, 1});
}

struct ConvBN {
    mx::array w = mx::array(0.0f);
    mx::array mean = mx::array(0.0f);
    mx::array var = mx::array(0.0f);
    mx::array gamma = mx::array(0.0f);
    mx::array beta = mx::array(0.0f);
    int strideH = 1, strideW = 1, padH = 0, padW = 0, groups = 1;
    QString act;
    bool evalBn = false;
    bool cudnnBn = false;
    mx::array fwd(const mx::array& x, bool officialBn) const {
        mx::array y = mx::conv2d(x, w, {strideH, strideW}, {padH, padW}, {1, 1}, groups);
        if (officialBn || cudnnBn) {
            y = officialBnEval(y, mean, var, gamma, beta, act == QLatin1String("silu"));
            if (act == QLatin1String("relu"))
                return relu(y);
            return y;
        }
        const mx::array eps = mx::array(1e-5f);
        if (evalBn) {
            const mx::array inv = mx::rsqrt(var + eps);
            y = (y - mean) * inv * gamma + beta;
        } else {
            const mx::array scale = gamma * mx::rsqrt(var + eps);
            const mx::array shift = beta - mean * scale;
            y = y * scale + shift;
        }
        if (act == QLatin1String("relu"))
            return relu(y);
        if (act == QLatin1String("silu"))
            return silu(y);
        return y;
    }
    mx::array operator()(const mx::array& x) const { return fwd(x, false); }
};

ConvBN loadConvBN(const SafetensorsFile& f, const QString& p, int sH, int sW, int pH, int pW, int g,
                  const QString& act, const QString& cn = QStringLiteral("convolution"),
                  const QString& bn = QStringLiteral("normalization"), bool evalBn = false,
                  bool cudnnBn = false) {
    ConvBN c;
    c.w = convW(f, p + QLatin1Char('.') + cn + QStringLiteral(".weight"));
    c.mean = loadF32(f, p + QLatin1Char('.') + bn + QStringLiteral(".running_mean"));
    c.var = loadF32(f, p + QLatin1Char('.') + bn + QStringLiteral(".running_var"));
    c.gamma = loadF32(f, p + QLatin1Char('.') + bn + QStringLiteral(".weight"));
    c.beta = loadF32(f, p + QLatin1Char('.') + bn + QStringLiteral(".bias"));
    const int oc = c.w.shape()[0];
    c.mean = mx::reshape(c.mean, {1, 1, 1, oc});
    c.var = mx::reshape(c.var, {1, 1, 1, oc});
    c.gamma = mx::reshape(c.gamma, {1, 1, 1, oc});
    c.beta = mx::reshape(c.beta, {1, 1, 1, oc});
    c.strideH = sH;
    c.strideW = sW;
    c.padH = pH;
    c.padW = pW;
    c.groups = g;
    c.act = act;
    c.evalBn = evalBn;
    c.cudnnBn = cudnnBn;
    return c;
}

mx::array padBR(const mx::array& x) {
    return mx::pad(x, {{0, 0}, {0, 1}, {0, 1}, {0, 0}});
}
mx::array maxPool2s1(const mx::array& x) {
    const auto sh = x.shape();
    auto a = mx::slice(x, {0, 0, 0, 0}, {sh[0], sh[1] - 1, sh[2] - 1, sh[3]});
    auto b = mx::slice(x, {0, 0, 1, 0}, {sh[0], sh[1] - 1, sh[2], sh[3]});
    auto c = mx::slice(x, {0, 1, 0, 0}, {sh[0], sh[1], sh[2] - 1, sh[3]});
    auto d = mx::slice(x, {0, 1, 1, 0}, {sh[0], sh[1], sh[2], sh[3]});
    return mx::maximum(mx::maximum(a, b), mx::maximum(c, d));
}
mx::array nearestUp(const mx::array& x, int s) {
    return mx::repeat(mx::repeat(x, s, 1), s, 2);
}

struct Lin {
    std::vector<float> w;  // (out, in) row-major
    std::vector<float> b;
    int in = 0, out = 0;
    mx::array wT = mx::array(0.0f);  // [in, out]
    mx::array bias = mx::array(0.0f);
    bool hasB = false;
};
void bindLin(Lin* l) {
    l->wT = mx::transpose(mx::array(l->w.data(), {l->out, l->in}));
    if (!l->b.empty()) {
        l->bias = mx::array(l->b.data(), {l->out});
        l->hasB = true;
        mx::eval(l->wT, l->bias);
    } else {
        l->hasB = false;
        mx::eval(l->wT);
    }
}
Lin loadLin(const SafetensorsFile& f, const QString& prefix) {
    QVector<float> w, b;
    QVector<qint64> ws, bs;
    QString err;
    if (!loadTensorF32(f, prefix + QStringLiteral(".weight"), &w, &ws, &err))
        throw std::runtime_error(err.toStdString());
    Lin l;
    l.out = int(ws[0]);
    l.in = int(ws[1]);
    l.w.assign(w.begin(), w.end());
    if (loadTensorF32(f, prefix + QStringLiteral(".bias"), &b, &bs, &err))
        l.b.assign(b.begin(), b.end());
    bindLin(&l);
    return l;
}
mx::array linearMx(const mx::array& x, const Lin& l) {
    auto y = mx::matmul(x, l.wT);
    return l.hasB ? y + l.bias : y;
}
mx::array lnMx(const mx::array& x, const std::vector<float>& w, const std::vector<float>& b) {
    return mx::fast::layer_norm(x, mx::array(w.data(), {int(w.size())}), mx::array(b.data(), {int(b.size())}),
                                1e-5f);
}
mx::array geluErf(const mx::array& x) {
    return mx::array(0.5f) * x * (mx::array(1.0f) + mx::erf(x * mx::array(0.7071067811865476f)));
}

struct HgLayer {
    bool light = false;
    ConvBN a, b;
};
struct HgBlock {
    bool residual = false;
    std::vector<HgLayer> layers;
    ConvBN agg0, agg1;
};
struct HgStage {
    bool hasDown = false;
    ConvBN down;
    std::vector<HgBlock> blocks;
};

struct Csp {
    ConvBN conv1, conv2;
    ConvBN bot1[3], bot2[3];
};

struct EncAttn {
    Lin q, k, v, o, n1w, n2w;
    std::vector<float> n1g, n1b, n2g, n2b;
    Lin fc1, fc2;
};

struct DecLayerW {
    Lin sq, sk, sv, so;
    std::vector<float> snW, snB, enW, enB, fnW, fnB;
    Lin off, aw, vp, op, fc1, fc2;
};

struct OrderLayer {
    Lin q, k, v, od, inter, outd;
    std::vector<float> onW, onB, fnW, fnB;
};

struct LayoutNet {
    ConvBN stem1, stem2a, stem2b, stem3, stem4;
    HgStage stages[4];
    ConvBN encProj[3];
    EncAttn enc;
    ConvBN lat[2], down[2];
    Csp fpn[2], pan[2];
    ConvBN decProj[3];
    Lin encOut, encScore;
    std::vector<float> encOutLnW, encOutLnB;
    Lin encBox[3];
    Lin qpos[2];
    DecLayerW dec[6];
    Lin cls[6];
    Lin box[6][3];
    // reading order
    std::vector<float> wordE, posE, typeE, xE, yE, wE, hE;
    Lin spat, labProj, gp;
    std::vector<float> labE, embLnW, embLnB;
    Lin relProj;
    std::vector<float> relB;
    OrderLayer ol[6];
};

HgLayer loadHgLayer(const SafetensorsFile& f, const QString& p, bool light, int k, int groups) {
    HgLayer L;
    L.light = light;
    if (light) {
        L.a = loadConvBN(f, p + QStringLiteral(".conv1"), 1, 1, 0, 0, 1, QString());
        L.b = loadConvBN(f, p + QStringLiteral(".conv2"), 1, 1, k / 2, k / 2, groups, QStringLiteral("relu"));
    } else {
        L.a = loadConvBN(f, p, 1, 1, k / 2, k / 2, 1, QStringLiteral("relu"));
    }
    return L;
}

HgBlock loadHgBlock(const SafetensorsFile& f, const QString& p, int nLayer, bool light, int k, int mid, bool residual) {
    HgBlock b;
    b.residual = residual;
    for (int i = 0; i < nLayer; ++i)
        b.layers.push_back(loadHgLayer(f, p + QStringLiteral(".layers.%1").arg(i), light, k, mid));
    b.agg0 = loadConvBN(f, p + QStringLiteral(".aggregation.0"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    b.agg1 = loadConvBN(f, p + QStringLiteral(".aggregation.1"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    return b;
}

Csp loadCsp(const SafetensorsFile& f, const QString& p) {
    Csp c;
    c.conv1 = loadConvBN(f, p + QStringLiteral(".conv1"), 1, 1, 0, 0, 1, QStringLiteral("silu"), QStringLiteral("conv"),
                         QStringLiteral("norm"), false, true);
    c.conv2 = loadConvBN(f, p + QStringLiteral(".conv2"), 1, 1, 0, 0, 1, QStringLiteral("silu"), QStringLiteral("conv"),
                         QStringLiteral("norm"), false, true);
    for (int i = 0; i < 3; ++i) {
        // Official RepVGG: ConvNorm has no activation; SiLU is applied after conv1+conv2.
        // Keep MLX eval BN on bots. NCHW cuDNN here is bitwise on the same fused
        // input but reshuffles live scores (closes dd3b, regresses 8d26; 9f62 stays 0.4799).
        c.bot1[i] = loadConvBN(f, p + QStringLiteral(".bottlenecks.%1.conv1").arg(i), 1, 1, 1, 1, 1, QString(),
                               QStringLiteral("conv"), QStringLiteral("norm"), true);
        c.bot2[i] = loadConvBN(f, p + QStringLiteral(".bottlenecks.%1.conv2").arg(i), 1, 1, 0, 0, 1, QString(),
                               QStringLiteral("conv"), QStringLiteral("norm"), true);
    }
    return c;
}

mx::array cspFwd(const Csp& c, const mx::array& x, bool officialBn = false) {
    mx::array h1 = c.conv1.fwd(x, officialBn);
    for (int i = 0; i < 3; ++i)
        h1 = silu(c.bot1[i].fwd(h1, officialBn) + c.bot2[i].fwd(h1, officialBn));
    return h1 + c.conv2.fwd(x, officialBn);
}

mx::array hgLayerFwd(const HgLayer& L, const mx::array& x) {
    if (!L.light)
        return L.a(x);
    return L.b(L.a(x));
}

mx::array hgBlockFwd(const HgBlock& b, const mx::array& x) {
    std::vector<mx::array> outs{x};
    mx::array h = x;
    for (const HgLayer& L : b.layers) {
        h = hgLayerFwd(L, h);
        outs.push_back(h);
    }
    h = mx::concatenate(outs, 3);
    h = b.agg1(b.agg0(h));
    if (b.residual)
        h = h + x;
    return h;
}

LayoutNet loadNet(const SafetensorsFile& f) {
    LayoutNet n;
    const QString emb = QStringLiteral("model.backbone.model.embedder");
    n.stem1 = loadConvBN(f, emb + QStringLiteral(".stem1"), 2, 2, 1, 1, 1, QStringLiteral("relu"));
    n.stem2a = loadConvBN(f, emb + QStringLiteral(".stem2a"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    n.stem2b = loadConvBN(f, emb + QStringLiteral(".stem2b"), 1, 1, 0, 0, 1, QStringLiteral("relu"));
    n.stem3 = loadConvBN(f, emb + QStringLiteral(".stem3"), 2, 2, 1, 1, 1, QStringLiteral("relu"));
    n.stem4 = loadConvBN(f, emb + QStringLiteral(".stem4"), 1, 1, 0, 0, 1, QStringLiteral("relu"));

    const int nBlocks[4] = {1, 1, 3, 1};
    const int nLayers[4] = {6, 6, 6, 6};
    const int ks[4] = {3, 3, 5, 5};
    const int mid[4] = {48, 96, 192, 384};
    const bool light[4] = {false, false, true, true};
    const bool down[4] = {false, true, true, true};
    const int inC[4] = {48, 128, 512, 1024};
    for (int s = 0; s < 4; ++s) {
        const QString sp = QStringLiteral("model.backbone.model.encoder.stages.%1").arg(s);
        n.stages[s].hasDown = down[s];
        if (down[s])
            n.stages[s].down = loadConvBN(f, sp + QStringLiteral(".downsample"), 2, 2, 1, 1, inC[s], QString());
        for (int b = 0; b < nBlocks[s]; ++b)
            n.stages[s].blocks.push_back(
                loadHgBlock(f, sp + QStringLiteral(".blocks.%1").arg(b), nLayers[s], light[s], ks[s], mid[s], b != 0));
    }

    for (int i = 0; i < 3; ++i) {
        n.encProj[i] = loadConvBN(f, QStringLiteral("model.encoder_input_proj.%1").arg(i), 1, 1, 0, 0, 1, QString(),
                                  QStringLiteral("0"), QStringLiteral("1"), true);
        n.decProj[i] = loadConvBN(f, QStringLiteral("model.decoder_input_proj.%1").arg(i), 1, 1, 0, 0, 1, QString(),
                                  QStringLiteral("0"), QStringLiteral("1"), true);
    }
    const QString ep = QStringLiteral("model.encoder.encoder.0.layers.0");
    n.enc.q = loadLin(f, ep + QStringLiteral(".self_attn.q_proj"));
    n.enc.k = loadLin(f, ep + QStringLiteral(".self_attn.k_proj"));
    n.enc.v = loadLin(f, ep + QStringLiteral(".self_attn.v_proj"));
    n.enc.o = loadLin(f, ep + QStringLiteral(".self_attn.out_proj"));
    n.enc.fc1 = loadLin(f, ep + QStringLiteral(".fc1"));
    n.enc.fc2 = loadLin(f, ep + QStringLiteral(".fc2"));
    {
        QVector<float> w, b;
        QVector<qint64> s;
        QString e;
        loadTensorF32(f, ep + QStringLiteral(".self_attn_layer_norm.weight"), &w, &s, &e);
        n.enc.n1g.assign(w.begin(), w.end());
        loadTensorF32(f, ep + QStringLiteral(".self_attn_layer_norm.bias"), &b, &s, &e);
        n.enc.n1b.assign(b.begin(), b.end());
        loadTensorF32(f, ep + QStringLiteral(".final_layer_norm.weight"), &w, &s, &e);
        n.enc.n2g.assign(w.begin(), w.end());
        loadTensorF32(f, ep + QStringLiteral(".final_layer_norm.bias"), &b, &s, &e);
        n.enc.n2b.assign(b.begin(), b.end());
    }
    for (int i = 0; i < 2; ++i) {
        n.lat[i] = loadConvBN(f, QStringLiteral("model.encoder.lateral_convs.%1").arg(i), 1, 1, 0, 0, 1,
                              QStringLiteral("silu"), QStringLiteral("conv"), QStringLiteral("norm"), true);
        n.down[i] = loadConvBN(f, QStringLiteral("model.encoder.downsample_convs.%1").arg(i), 2, 2, 1, 1, 1,
                               QStringLiteral("silu"), QStringLiteral("conv"), QStringLiteral("norm"), true);
        n.fpn[i] = loadCsp(f, QStringLiteral("model.encoder.fpn_blocks.%1").arg(i));
        n.pan[i] = loadCsp(f, QStringLiteral("model.encoder.pan_blocks.%1").arg(i));
    }
    n.encOut = loadLin(f, QStringLiteral("model.enc_output.0"));
    {
        QVector<float> w, b;
        QVector<qint64> s;
        QString e;
        loadTensorF32(f, QStringLiteral("model.enc_output.1.weight"), &w, &s, &e);
        n.encOutLnW.assign(w.begin(), w.end());
        loadTensorF32(f, QStringLiteral("model.enc_output.1.bias"), &b, &s, &e);
        n.encOutLnB.assign(b.begin(), b.end());
    }
    n.encScore = loadLin(f, QStringLiteral("model.enc_score_head"));
    n.encBox[0] = loadLin(f, QStringLiteral("model.enc_bbox_head.layers.0"));
    n.encBox[1] = loadLin(f, QStringLiteral("model.enc_bbox_head.layers.1"));
    n.encBox[2] = loadLin(f, QStringLiteral("model.enc_bbox_head.layers.2"));
    n.qpos[0] = loadLin(f, QStringLiteral("model.decoder.query_pos_head.layers.0"));
    n.qpos[1] = loadLin(f, QStringLiteral("model.decoder.query_pos_head.layers.1"));
    for (int i = 0; i < 6; ++i) {
        const QString p = QStringLiteral("model.decoder.layers.%1").arg(i);
        n.dec[i].sq = loadLin(f, p + QStringLiteral(".self_attn.q_proj"));
        n.dec[i].sk = loadLin(f, p + QStringLiteral(".self_attn.k_proj"));
        n.dec[i].sv = loadLin(f, p + QStringLiteral(".self_attn.v_proj"));
        n.dec[i].so = loadLin(f, p + QStringLiteral(".self_attn.out_proj"));
        n.dec[i].off = loadLin(f, p + QStringLiteral(".encoder_attn.sampling_offsets"));
        n.dec[i].aw = loadLin(f, p + QStringLiteral(".encoder_attn.attention_weights"));
        n.dec[i].vp = loadLin(f, p + QStringLiteral(".encoder_attn.value_proj"));
        n.dec[i].op = loadLin(f, p + QStringLiteral(".encoder_attn.output_proj"));
        n.dec[i].fc1 = loadLin(f, p + QStringLiteral(".fc1"));
        n.dec[i].fc2 = loadLin(f, p + QStringLiteral(".fc2"));
        QVector<float> w, b;
        QVector<qint64> s;
        QString e;
        loadTensorF32(f, p + QStringLiteral(".self_attn_layer_norm.weight"), &w, &s, &e);
        n.dec[i].snW.assign(w.begin(), w.end());
        loadTensorF32(f, p + QStringLiteral(".self_attn_layer_norm.bias"), &b, &s, &e);
        n.dec[i].snB.assign(b.begin(), b.end());
        loadTensorF32(f, p + QStringLiteral(".encoder_attn_layer_norm.weight"), &w, &s, &e);
        n.dec[i].enW.assign(w.begin(), w.end());
        loadTensorF32(f, p + QStringLiteral(".encoder_attn_layer_norm.bias"), &b, &s, &e);
        n.dec[i].enB.assign(b.begin(), b.end());
        loadTensorF32(f, p + QStringLiteral(".final_layer_norm.weight"), &w, &s, &e);
        n.dec[i].fnW.assign(w.begin(), w.end());
        loadTensorF32(f, p + QStringLiteral(".final_layer_norm.bias"), &b, &s, &e);
        n.dec[i].fnB.assign(b.begin(), b.end());
        n.cls[i] = loadLin(f, QStringLiteral("model.decoder.class_embed.%1").arg(i));
        n.box[i][0] = loadLin(f, QStringLiteral("model.decoder.bbox_embed.%1.layers.0").arg(i));
        n.box[i][1] = loadLin(f, QStringLiteral("model.decoder.bbox_embed.%1.layers.1").arg(i));
        n.box[i][2] = loadLin(f, QStringLiteral("model.decoder.bbox_embed.%1.layers.2").arg(i));
    }

    auto loadVec = [&](const QString& name, std::vector<float>* o) {
        QVector<float> d;
        QVector<qint64> s;
        QString e;
        if (!loadTensorF32(f, name, &d, &s, &e))
            throw std::runtime_error(e.toStdString());
        o->assign(d.begin(), d.end());
    };
    loadVec(QStringLiteral("reading_order.embeddings.word_embeddings.weight"), &n.wordE);
    loadVec(QStringLiteral("reading_order.embeddings.position_embeddings.weight"), &n.posE);
    loadVec(QStringLiteral("reading_order.embeddings.token_type_embeddings.weight"), &n.typeE);
    loadVec(QStringLiteral("reading_order.embeddings.x_position_embeddings.weight"), &n.xE);
    loadVec(QStringLiteral("reading_order.embeddings.y_position_embeddings.weight"), &n.yE);
    loadVec(QStringLiteral("reading_order.embeddings.w_position_embeddings.weight"), &n.wE);
    loadVec(QStringLiteral("reading_order.embeddings.h_position_embeddings.weight"), &n.hE);
    loadVec(QStringLiteral("reading_order.embeddings.norm.weight"), &n.embLnW);
    loadVec(QStringLiteral("reading_order.embeddings.norm.bias"), &n.embLnB);
    loadVec(QStringLiteral("reading_order.label_embeddings.weight"), &n.labE);
    n.spat = loadLin(f, QStringLiteral("reading_order.embeddings.spatial_proj"));
    n.labProj = loadLin(f, QStringLiteral("reading_order.label_features_projection"));
    n.gp = loadLin(f, QStringLiteral("reading_order.relative_head.dense"));
    n.relProj = loadLin(f, QStringLiteral("reading_order.encoder.rel_bias_module.pos_proj"));
    // pos_proj is Conv2d 8x64x1x1 - load as linear 8x64
    {
        QVector<float> w, b;
        QVector<qint64> s;
        QString e;
        loadTensorF32(f, QStringLiteral("reading_order.encoder.rel_bias_module.pos_proj.weight"), &w, &s, &e);
        n.relProj.w.assign(w.begin(), w.end());
        n.relProj.out = 8;
        n.relProj.in = 64;
        loadTensorF32(f, QStringLiteral("reading_order.encoder.rel_bias_module.pos_proj.bias"), &b, &s, &e);
        n.relProj.b.assign(b.begin(), b.end());
        n.relB = n.relProj.b;
        bindLin(&n.relProj);
    }
    for (int i = 0; i < 6; ++i) {
        const QString p = QStringLiteral("reading_order.encoder.layer.%1").arg(i);
        n.ol[i].q = loadLin(f, p + QStringLiteral(".attention.self.query"));
        n.ol[i].k = loadLin(f, p + QStringLiteral(".attention.self.key"));
        n.ol[i].v = loadLin(f, p + QStringLiteral(".attention.self.value"));
        n.ol[i].od = loadLin(f, p + QStringLiteral(".attention.output.dense"));
        n.ol[i].inter = loadLin(f, p + QStringLiteral(".intermediate.dense"));
        n.ol[i].outd = loadLin(f, p + QStringLiteral(".output.dense"));
        QVector<float> w, b;
        QVector<qint64> s;
        QString e;
        loadTensorF32(f, p + QStringLiteral(".attention.output.norm.weight"), &w, &s, &e);
        n.ol[i].onW.assign(w.begin(), w.end());
        loadTensorF32(f, p + QStringLiteral(".attention.output.norm.bias"), &b, &s, &e);
        n.ol[i].onB.assign(b.begin(), b.end());
        loadTensorF32(f, p + QStringLiteral(".output.norm.weight"), &w, &s, &e);
        n.ol[i].fnW.assign(w.begin(), w.end());
        loadTensorF32(f, p + QStringLiteral(".output.norm.bias"), &b, &s, &e);
        n.ol[i].fnB.assign(b.begin(), b.end());
    }
    return n;
}

mx::array stemFwd(const LayoutNet& n, const mx::array& x) {
    mx::array e = n.stem1(x);
    e = padBR(e);
    mx::array a = n.stem2a(e);
    a = padBR(a);
    a = n.stem2b(a);
    e = mx::concatenate({maxPool2s1(e), a}, 3);
    const mx::array stem = n.stem4(n.stem3(e));
    return stem;
}

mx::array sincosPos(int h, int w, int dim) {
    // Official RTDetrHybridEncoder.build_2d_sincos_position_embedding uses
    // meshgrid(arange(W), arange(H), indexing="ij") then flatten, i.e. x*H+y.
    const int pd = dim / 4;
    std::vector<float> out(size_t(h * w * dim), 0.f);
    for (int xi = 0; xi < w; ++xi) {
        for (int yi = 0; yi < h; ++yi) {
            float* row = out.data() + (size_t(xi * h + yi) * dim);
            for (int k = 0; k < pd; ++k) {
                const float omega = 1.0f / std::pow(10000.0f, float(k) / float(pd));
                const float ow = float(xi) * omega;
                const float oh = float(yi) * omega;
                row[k] = std::sin(ow);
                row[pd + k] = std::cos(ow);
                row[2 * pd + k] = std::sin(oh);
                row[3 * pd + k] = std::cos(oh);
            }
        }
    }
    return mx::array(out.data(), {1, h * w, dim});
}

// Official RTDetrMultiheadAttention: Q = q_proj(x+pos)*scale, K = k_proj(x+pos),
// V = v_proj(x), attn = softmax(Q @ K^T) @ V. That is torch.bmm + softmax
// (matmul.allow_tf32=False), not cuDNN SDPA.
mx::array mathSdpa(const mx::array& q, const mx::array& k, const mx::array& v, float scale) {
    auto qs = q * mx::array(scale);
    auto scores = mx::matmul(qs, mx::transpose(k, {0, 1, 3, 2}));
    auto probs = mx::softmax(scores, -1);
    return mx::matmul(probs, v);
}

mx::array encoderLayer(const EncAttn& e, const mx::array& featNhwc) {
    const int h = featNhwc.shape(1), w = featNhwc.shape(2);
    const int n = h * w;
    auto x = mx::reshape(featNhwc, {n, kD});
    auto pos = mx::reshape(sincosPos(h, w, kD), {n, kD});
    auto xp = x + pos;
    const float scale = 1.0f / std::sqrt(float(kHeadDim));
    auto q = mx::transpose(mx::reshape(linearMx(xp, e.q), {n, kHeads, kHeadDim}), {1, 0, 2});
    auto k = mx::transpose(mx::reshape(linearMx(xp, e.k), {n, kHeads, kHeadDim}), {1, 0, 2});
    auto v = mx::transpose(mx::reshape(linearMx(x, e.v), {n, kHeads, kHeadDim}), {1, 0, 2});
    q = mx::expand_dims(q, 0);
    k = mx::expand_dims(k, 0);
    v = mx::expand_dims(v, 0);
    auto attn = mathSdpa(q, k, v, scale);
    attn = mx::reshape(mx::transpose(attn, {0, 2, 1, 3}), {n, kD});
    x = lnMx(x + linearMx(attn, e.o), e.n1g, e.n1b);
    x = lnMx(x + linearMx(geluErf(linearMx(x, e.fc1)), e.fc2), e.n2g, e.n2b);
    return mx::reshape(x, {1, h, w, kD});
}

mx::array mhaDecMx(const DecLayerW& L, const mx::array& x, const mx::array& pos) {
    auto xp = x + pos;
    const float scale = 1.0f / std::sqrt(float(kHeadDim));
    auto q = mx::expand_dims(mx::transpose(mx::reshape(linearMx(xp, L.sq), {kQueries, kHeads, kHeadDim}), {1, 0, 2}), 0);
    auto k = mx::expand_dims(mx::transpose(mx::reshape(linearMx(xp, L.sk), {kQueries, kHeads, kHeadDim}), {1, 0, 2}), 0);
    auto v = mx::expand_dims(mx::transpose(mx::reshape(linearMx(x, L.sv), {kQueries, kHeads, kHeadDim}), {1, 0, 2}), 0);
    auto attn = mathSdpa(q, k, v, scale);
    attn = mx::reshape(mx::transpose(attn, {0, 2, 1, 3}), {kQueries, kD});
    return lnMx(x + linearMx(attn, L.so), L.snW, L.snB);
}

mx::array bilinearSample(const mx::array& val, int H, int W, const mx::array& ix, const mx::array& iy) {
    // val [H*W, 256], ix/iy broadcastable. Official grid_sample align_corners=False, OOB=0.
    auto x0 = mx::floor(ix);
    auto y0 = mx::floor(iy);
    auto tx = mx::expand_dims(ix - x0, -1);
    auto ty = mx::expand_dims(iy - y0, -1);
    auto gather = [&](const mx::array& yy, const mx::array& xx) {
        const auto inside = mx::logical_and(mx::logical_and(mx::greater_equal(yy, mx::array(0.0f)),
                                                            mx::less(yy, mx::array(float(H)))),
                                            mx::logical_and(mx::greater_equal(xx, mx::array(0.0f)),
                                                            mx::less(xx, mx::array(float(W)))));
        const auto yi = mx::clip(yy, mx::array(0.0f), mx::array(float(H - 1)));
        const auto xi = mx::clip(xx, mx::array(0.0f), mx::array(float(W - 1)));
        const auto idx = mx::astype(yi, mx::int32) * mx::array(W) + mx::astype(xi, mx::int32);
        auto t = mx::take(val, idx, 0);
        return mx::where(mx::expand_dims(inside, -1), t, mx::zeros_like(t));
    };
    auto v00 = gather(y0, x0);
    auto v10 = gather(y0, x0 + mx::array(1.0f));
    auto v01 = gather(y0 + mx::array(1.0f), x0);
    auto v11 = gather(y0 + mx::array(1.0f), x0 + mx::array(1.0f));
    const auto otx = mx::array(1.0f) - tx;
    const auto oty = mx::array(1.0f) - ty;
    return oty * (otx * v00 + tx * v10) + ty * (otx * v01 + tx * v11);
}

mx::array deformAttnMx(const DecLayerW& L, const mx::array& x, const mx::array& pos, const mx::array& mem,
                       const int hs[3], const int ws[3], const mx::array& ref) {
    auto xp = x + pos;
    auto value = linearMx(mem, L.vp);
    auto offsets = mx::reshape(linearMx(xp, L.off), {kQueries, kHeads, kLevels, kPoints, 2});
    auto attnw = mx::softmax(mx::reshape(linearMx(xp, L.aw), {kQueries, kHeads, kLevels * kPoints}), -1);
    attnw = mx::reshape(attnw, {kQueries, kHeads, kLevels, kPoints});
    const auto cx = mx::reshape(mx::slice(ref, {0, 0}, {kQueries, 1}), {kQueries, 1, 1});
    const auto cy = mx::reshape(mx::slice(ref, {0, 1}, {kQueries, 2}), {kQueries, 1, 1});
    const auto rw = mx::reshape(mx::slice(ref, {0, 2}, {kQueries, 3}), {kQueries, 1, 1});
    const auto rh = mx::reshape(mx::slice(ref, {0, 3}, {kQueries, 4}), {kQueries, 1, 1});
    int base = 0;
    mx::array acc = mx::zeros({kQueries, kD});
    for (int lv = 0; lv < kLevels; ++lv) {
        const int H = hs[lv], W = ws[lv], n = H * W;
        auto valLv = mx::slice(value, {base, 0}, {base + n, kD});
        auto offx = mx::slice(offsets, {0, 0, lv, 0, 0}, {kQueries, kHeads, lv + 1, kPoints, 1});
        auto offy = mx::slice(offsets, {0, 0, lv, 0, 1}, {kQueries, kHeads, lv + 1, kPoints, 2});
        offx = mx::reshape(offx, {kQueries, kHeads, kPoints});
        offy = mx::reshape(offy, {kQueries, kHeads, kPoints});
        const auto sx = cx + offx / mx::array(float(kPoints)) * rw * mx::array(0.5f);
        const auto sy = cy + offy / mx::array(float(kPoints)) * rh * mx::array(0.5f);
        const auto ix = (((sx * mx::array(2.0f) - mx::array(1.0f)) + mx::array(1.0f)) * mx::array(0.5f))
                            * mx::array(float(W))
                        - mx::array(0.5f);
        const auto iy = (((sy * mx::array(2.0f) - mx::array(1.0f)) + mx::array(1.0f)) * mx::array(0.5f))
                            * mx::array(float(H))
                        - mx::array(0.5f);
        auto samp = bilinearSample(valLv, H, W, ix, iy);  // [Q, heads, P, 256]
        auto aw = mx::reshape(mx::slice(attnw, {0, 0, lv, 0}, {kQueries, kHeads, lv + 1, kPoints}),
                              {kQueries, kHeads, kPoints, 1});
        auto part = mx::sum(samp * aw, 2);  // [Q, heads, 256]
        // Official samples the matching head channels only. Keep head block.
        part = mx::reshape(part, {kQueries, kHeads, kD});
        std::vector<mx::array> heads;
        heads.reserve(kHeads);
        for (int h = 0; h < kHeads; ++h) {
            auto one = mx::reshape(mx::slice(part, {0, h, h * kHeadDim}, {kQueries, h + 1, (h + 1) * kHeadDim}),
                                   {kQueries, kHeadDim});
            heads.push_back(one);
        }
        acc = acc + mx::concatenate(heads, 1);
        base += n;
    }
    return lnMx(x + linearMx(acc, L.op), L.enW, L.enB);
}

mx::array mlpBoxMx(const Lin* ls, const mx::array& x) {
    return linearMx(relu(linearMx(relu(linearMx(x, ls[0])), ls[1])), ls[2]);
}

mx::array invSigmoidMx(const mx::array& x) {
    auto xs = mx::minimum(mx::maximum(x, mx::array(1e-5f)), mx::array(1.0f - 1e-5f));
    return mx::log(xs / (mx::array(1.0f) - xs));
}

mx::array takeRows(const std::vector<float>& table, int rows, int dim, const mx::array& idx) {
    return mx::take(mx::array(table.data(), {rows, dim}), idx, 0);
}

void readingOrder(const LayoutNet& n, const float* padBoxes, const int* labels, const char* mask,
                  float* orderLogits) {
    // Official PPDocLayoutV2ReadingOrder: embeddings + 2d rel bias + 6 encoder layers + GlobalPointer.
    const int seq = kQueries;
    const int full = seq + 2;
    int numPred = 0;
    for (int i = 0; i < seq; ++i)
        if (mask[i])
            ++numPred;
    std::vector<int> ids(size_t(full), 1);
    ids[0] = 0;
    for (int i = 0; i < numPred; ++i)
        ids[size_t(1 + i)] = 3;
    ids[size_t(numPred + 1)] = 2;
    std::vector<int> pos(size_t(full), 1);
    int acc = 0;
    for (int i = 0; i < full; ++i) {
        if (ids[size_t(i)] != 1) {
            ++acc;
            pos[size_t(i)] = acc + 1;
        }
    }
    std::vector<float> boxes(size_t(full * 4), 0);
    for (int i = 0; i < seq; ++i)
        for (int k = 0; k < 4; ++k)
            boxes[size_t((i + 1) * 4 + k)] = padBoxes[i * 4 + k];
    std::vector<int> labIds(seq, 0);
    for (int i = 0; i < seq; ++i)
        labIds[size_t(i)] = labels[i];

    const auto idArr = mx::array(ids.data(), {full});
    const auto posArr = mx::array(pos.data(), {full});
    const auto boxesMx = mx::array(boxes.data(), {full, 4});
    auto clampIdx = [](const mx::array& x) {
        return mx::astype(mx::minimum(mx::maximum(x, mx::array(0.0f)), mx::array(1023.0f)), mx::int32);
    };
    auto x0 = clampIdx(mx::reshape(mx::slice(boxesMx, {0, 0}, {full, 1}), {full}));
    auto y0 = clampIdx(mx::reshape(mx::slice(boxesMx, {0, 1}, {full, 2}), {full}));
    auto x1 = clampIdx(mx::reshape(mx::slice(boxesMx, {0, 2}, {full, 3}), {full}));
    auto y1 = clampIdx(mx::reshape(mx::slice(boxesMx, {0, 3}, {full, 4}), {full}));
    auto ww = mx::minimum(mx::maximum(x1 - x0, mx::array(0)), mx::array(1023));
    auto hh = mx::minimum(mx::maximum(y1 - y0, mx::array(0)), mx::array(1023));
    auto spat = mx::concatenate({takeRows(n.xE, 1024, 171, x0), takeRows(n.yE, 1024, 171, y0),
                                 takeRows(n.xE, 1024, 171, x1), takeRows(n.yE, 1024, 171, y1),
                                 takeRows(n.hE, 1024, 170, hh), takeRows(n.wE, 1024, 170, ww)},
                                1);
    auto emb = takeRows(n.wordE, 4, kOrderHidden, idArr) + mx::array(n.typeE.data(), {kOrderHidden})
               + takeRows(n.posE, 514, kOrderHidden, posArr) + linearMx(spat, n.spat);
    auto labTok = linearMx(takeRows(n.labE, 20, kOrderHidden, mx::array(labIds.data(), {seq})), n.labProj);
    auto lab = mx::concatenate({mx::zeros({1, kOrderHidden}), labTok, mx::zeros({1, kOrderHidden})}, 0);
    emb = lnMx(emb + lab, n.embLnW, n.embLnB);

    auto x0f = mx::reshape(mx::slice(boxesMx, {0, 0}, {full, 1}), {full, 1});
    auto y0f = mx::reshape(mx::slice(boxesMx, {0, 1}, {full, 2}), {full, 1});
    auto x1f = mx::reshape(mx::slice(boxesMx, {0, 2}, {full, 3}), {full, 1});
    auto y1f = mx::reshape(mx::slice(boxesMx, {0, 3}, {full, 4}), {full, 1});
    auto cx = (x0f + x1f) * mx::array(0.5f);
    auto cy = (y0f + y1f) * mx::array(0.5f);
    auto bw = mx::maximum(x1f - x0f, mx::array(1e-3f));
    auto bh = mx::maximum(y1f - y0f, mx::array(1e-3f));
    auto sxy = mx::concatenate({cx, cy}, 1);
    auto swh = mx::concatenate({bw, bh}, 1);
    auto srcXY = mx::expand_dims(sxy, 1);
    auto tgtXY = mx::expand_dims(sxy, 0);
    auto srcWH = mx::expand_dims(swh, 1);
    auto tgtWH = mx::expand_dims(swh, 0);
    const auto eps = mx::array(1e-5f);
    auto relEnc = mx::concatenate({mx::log(mx::abs(srcXY - tgtXY) / (srcWH + eps) + mx::array(1.0f)),
                                   mx::log((srcWH + eps) / (tgtWH + eps))},
                                  2);
    const int halfDim = 8;
    std::vector<float> ropeInv(halfDim);
    for (int i = 0; i < halfDim; ++i)
        ropeInv[size_t(i)] = 1.0f / std::pow(10000.0f, float(2 * i) / float(halfDim));
    auto ang = mx::expand_dims(relEnc, -1) * mx::array(100.0f) * mx::array(ropeInv.data(), {halfDim});
    auto feat = mx::reshape(mx::concatenate({mx::sin(ang), mx::cos(ang)}, -1), {full * full, 64});
    auto rel = mx::transpose(mx::reshape(linearMx(feat, n.relProj), {full, full, kOrderHeads}), {2, 0, 1});
    rel = mx::expand_dims(rel, 0);

    auto keyOk = mx::less(mx::arange(full), mx::array(numPred + 2));
    auto attnBias = mx::reshape(mx::where(keyOk, mx::array(0.0f), mx::array(-1.0e4f)), {1, 1, 1, full});
    const float scale = 1.0f / std::sqrt(float(kOrderHeadDim));
    for (int li = 0; li < 6; ++li) {
        auto q = mx::expand_dims(
            mx::transpose(mx::reshape(linearMx(emb, n.ol[li].q), {full, kOrderHeads, kOrderHeadDim}), {1, 0, 2}), 0);
        auto k = mx::expand_dims(
            mx::transpose(mx::reshape(linearMx(emb, n.ol[li].k), {full, kOrderHeads, kOrderHeadDim}), {1, 0, 2}), 0);
        auto v = mx::expand_dims(
            mx::transpose(mx::reshape(linearMx(emb, n.ol[li].v), {full, kOrderHeads, kOrderHeadDim}), {1, 0, 2}), 0);
        auto scores = mx::matmul(q, mx::transpose(k, {0, 1, 3, 2})) * mx::array(scale) + rel + attnBias;
        auto scaled = scores / mx::array(32.0f);
        auto probs = mx::softmax((scaled - mx::max(scaled, -1, true)) * mx::array(32.0f), -1);
        auto ctx = mx::reshape(mx::transpose(mx::matmul(probs, v), {0, 2, 1, 3}), {full, kOrderHidden});
        auto attOut = lnMx(emb + linearMx(ctx, n.ol[li].od), n.ol[li].onW, n.ol[li].onB);
        emb = lnMx(attOut + linearMx(geluErf(linearMx(attOut, n.ol[li].inter)), n.ol[li].outd), n.ol[li].fnW,
                   n.ol[li].fnB);
    }

    auto tok = mx::slice(emb, {1, 0}, {seq + 1, kOrderHidden});
    auto proj = mx::reshape(linearMx(tok, n.gp), {seq, 2, 64});
    auto parts = mx::split(proj, 2, 1);
    auto qh = mx::reshape(parts[0], {seq, 64});
    auto kh = mx::reshape(parts[1], {seq, 64});
    auto gp = mx::matmul(qh, mx::transpose(kh, {1, 0})) / mx::array(8.0f);
    auto ii = mx::reshape(mx::arange(seq), {seq, 1});
    auto jj = mx::reshape(mx::arange(seq), {1, seq});
    gp = mx::where(mx::less_equal(jj, ii), mx::array(-1.0e4f), gp);
    mx::eval(gp);
    std::memcpy(orderLogits, gp.data<float>(), size_t(seq * seq) * sizeof(float));
}

LayoutNet* cachedNet(QString* err) {
    static std::mutex mu;
    static LayoutNet* n = nullptr;
    std::lock_guard<std::mutex> lock(mu);
    if (n)
        return n;
    try {
        SafetensorsFile f;
        const QString path = QDir(resolveOfficialLayoutDir()).filePath(QStringLiteral("model.safetensors"));
        if (!openSafetensors(path, &f, err))
            return nullptr;
        n = new LayoutNet(loadNet(f));
        return n;
    } catch (const std::exception& ex) {
        if (err)
            *err = QString::fromUtf8(ex.what());
        return nullptr;
    }
}

}  // namespace

bool officialLayoutForwardMlx(const float* nchw, int height, int width, QVector<float>* logits,
                              QVector<float>* predBoxes, QVector<float>* orderLogits, QString* err) {
    auto fail = [&](const QString& m) {
        if (err)
            *err = m;
        return false;
    };
    if (!nchw || !logits || !predBoxes || !orderLogits)
        return fail(QStringLiteral("null layout args"));
    LayoutNet* n = cachedNet(err);
    if (!n)
        return false;
    try {
        auto now = []() { return std::chrono::steady_clock::now(); };
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const auto t0 = now();
        mx::array x(nchw, {1, 3, height, width});
        x = mx::transpose(x, {0, 2, 3, 1});
        mx::array h = stemFwd(*n, x);
        std::vector<mx::array> feats;
        for (int s = 0; s < 4; ++s) {
            if (n->stages[s].hasDown)
                h = n->stages[s].down(h);
            for (const HgBlock& b : n->stages[s].blocks)
                h = hgBlockFwd(b, h);
            feats.push_back(h);
        }
        mx::eval(feats.back());
        const auto tBb = now();
        mx::array p0 = n->encProj[0](feats[1]);
        mx::array p1 = n->encProj[1](feats[2]);
        mx::array p2 = n->encProj[2](feats[3]);
        p2 = encoderLayer(n->enc, p2);
        std::vector<mx::array> hs{p0, p1, p2};
        std::vector<mx::array> fpn{hs[2]};
        for (int idx = 0; idx < 2; ++idx) {
            mx::array back = hs[1 - idx];
            mx::array top = n->lat[idx](fpn.back());
            fpn.back() = top;
            mx::array fused = mx::concatenate({nearestUp(top, 2), back}, 3);
            fpn.push_back(cspFwd(n->fpn[idx], fused));
        }
        std::reverse(fpn.begin(), fpn.end());
        std::vector<mx::array> pan{fpn[0]};
        for (int idx = 0; idx < 2; ++idx) {
            mx::array fused = mx::concatenate({n->down[idx](pan.back()), fpn[size_t(idx + 1)]}, 3);
            pan.push_back(cspFwd(n->pan[idx], fused));
        }
        mx::array d0 = n->decProj[0](pan[0]);
        mx::array d1 = n->decProj[1](pan[1]);
        mx::array d2 = n->decProj[2](pan[2]);
        mx::eval(d0, d1, d2);
        const auto tEnc = now();
        const int hh[3] = {d0.shape(1), d1.shape(1), d2.shape(1)};
        const int ww[3] = {d0.shape(2), d1.shape(2), d2.shape(2)};
        const int tot = hh[0] * ww[0] + hh[1] * ww[1] + hh[2] * ww[2];
        auto mem = mx::concatenate({mx::reshape(d0, {hh[0] * ww[0], kD}), mx::reshape(d1, {hh[1] * ww[1], kD}),
                                    mx::reshape(d2, {hh[2] * ww[2], kD})},
                                   0);
        std::vector<float> anchors(size_t(tot * 4)), valid(size_t(tot), 1);
        int ai = 0;
        for (int lv = 0; lv < 3; ++lv) {
            for (int y = 0; y < hh[lv]; ++y) {
                for (int x0 = 0; x0 < ww[lv]; ++x0) {
                    const float gx = (float(x0) + 0.5f) / float(ww[lv]);
                    const float gy = (float(y) + 0.5f) / float(hh[lv]);
                    const float wh = 0.05f * float(1 << lv);
                    float a[4] = {gx, gy, wh, wh};
                    // Official generate_anchors: (coord > 1e-2) && (coord < 1-1e-2),
                    // evaluated in CUDA float32. CPU (n-0.5)/n at n==50 rounds
                    // above 0.99 and drops the last row/col; CUDA rounds below
                    // and keeps them. Use the integer form that matches CUDA.
                    auto coordValid = [](int index, int extent) {
                        if (extent <= 0)
                            return false;
                        const int scaled = 100 * index + 50;
                        return scaled > extent && scaled <= 99 * extent;
                    };
                    const bool ok = coordValid(x0, ww[lv]) && coordValid(y, hh[lv])
                        && wh > 0.01f && wh < 0.99f;
                    if (!ok) {
                        valid[size_t(ai)] = 0;
                        for (int k = 0; k < 4; ++k)
                            anchors[size_t(ai * 4 + k)] = 3.402823e+38f;
                    } else {
                        for (int k = 0; k < 4; ++k)
                            anchors[size_t(ai * 4 + k)] = std::log(a[k] / (1.f - a[k]));
                    }
                    ++ai;
                }
            }
        }
        // Official RTDetrModel: decoder cross-attn uses unmasked source_flatten.
        // valid_mask is only applied to enc_output / query selection.
        auto validCol = mx::reshape(mx::array(valid.data(), {tot}), {tot, 1});
        auto encm = lnMx(linearMx(mem * validCol, n->encOut), n->encOutLnW, n->encOutLnB);
        auto encCls = linearMx(encm, n->encScore);
        auto anc = mx::array(anchors.data(), {tot, 4});
        auto finite = mx::isfinite(anc);
        auto encBox = mx::where(finite, mlpBoxMx(n->encBox, encm) + anc, anc);
        mx::eval(encCls);
        std::vector<float> bests(tot, 0.f);
        const float* cls = encCls.data<float>();
        for (int i = 0; i < tot; ++i) {
            float m = cls[size_t(i * kClasses)];
            for (int c = 1; c < kClasses; ++c)
                m = std::max(m, cls[size_t(i * kClasses + c)]);
            bests[size_t(i)] = m;
        }
        // Official NCHW BN FPN/PAN/decProj from the same encProj features,
        // used only to pick the top-300 set. Decoder still reads shipped
        // MLX-eval memory. Dropping this second pass did not cut 23-image
        // parse wall (369.1s vs 369.9s) and made 8d26 fail (many extra
        // crops, MLX conv cache thrash). Keep it on the product path.
        {
            std::vector<mx::array> fpnO{p2};
            for (int idx = 0; idx < 2; ++idx) {
                mx::array back = idx == 0 ? p1 : p0;
                mx::array top = n->lat[idx].fwd(fpnO.back(), true);
                fpnO.back() = top;
                mx::array fused = mx::concatenate({nearestUp(top, 2), back}, 3);
                fpnO.push_back(cspFwd(n->fpn[idx], fused, true));
            }
            std::reverse(fpnO.begin(), fpnO.end());
            std::vector<mx::array> panO{fpnO[0]};
            for (int idx = 0; idx < 2; ++idx) {
                mx::array fused =
                    mx::concatenate({n->down[idx].fwd(panO.back(), true), fpnO[size_t(idx + 1)]}, 3);
                panO.push_back(cspFwd(n->pan[idx], fused, true));
            }
            mx::array od0 = n->decProj[0].fwd(panO[0], true);
            mx::array od1 = n->decProj[1].fwd(panO[1], true);
            mx::array od2 = n->decProj[2].fwd(panO[2], true);
            auto memO = mx::concatenate({mx::reshape(od0, {hh[0] * ww[0], kD}),
                                         mx::reshape(od1, {hh[1] * ww[1], kD}),
                                         mx::reshape(od2, {hh[2] * ww[2], kD})},
                                        0);
            auto encmO = lnMx(linearMx(memO * validCol, n->encOut), n->encOutLnW, n->encOutLnB);
            auto encClsO = linearMx(encmO, n->encScore);
            mx::eval(encClsO);
            const float* clsO = encClsO.data<float>();
            for (int i = 0; i < tot; ++i) {
                float m = clsO[size_t(i * kClasses)];
                for (int c = 1; c < kClasses; ++c)
                    m = std::max(m, clsO[size_t(i * kClasses + c)]);
                bests[size_t(i)] = m;
            }
        }
        std::vector<int> order(tot);
        std::iota(order.begin(), order.end(), 0);
        auto scoreGreater = [&bests](int a, int b) -> bool {
            // Match torch.topk: higher score first, lower index on ties.
            const float sa = bests[size_t(a)];
            const float sb = bests[size_t(b)];
            if (sa != sb)
                return sa > sb;
            return a < b;
        };
        std::partial_sort(order.begin(), order.begin() + kQueries, order.end(), scoreGreater);
        auto top = mx::array(order.data(), {kQueries});
        auto hidden = mx::take(encm, top, 0);
        auto ref = mx::sigmoid(mx::take(encBox, top, 0));
        for (int li = 0; li < 6; ++li) {
            auto qpos = linearMx(relu(linearMx(ref, n->qpos[0])), n->qpos[1]);
            hidden = mhaDecMx(n->dec[li], hidden, qpos);
            hidden = deformAttnMx(n->dec[li], hidden, qpos, mem, hh, ww, ref);
            hidden = lnMx(hidden + linearMx(relu(linearMx(hidden, n->dec[li].fc1)), n->dec[li].fc2),
                          n->dec[li].fnW, n->dec[li].fnB);
            ref = mx::sigmoid(mlpBoxMx(n->box[li], hidden) + invSigmoidMx(ref));
        }
        auto logitsMx = linearMx(hidden, n->cls[5]);
        mx::eval(logitsMx, ref);
        logits->resize(kQueries * kClasses);
        predBoxes->resize(kQueries * 4);
        std::memcpy(logits->data(), logitsMx.data<float>(), size_t(kQueries * kClasses) * sizeof(float));
        std::memcpy(predBoxes->data(), ref.data<float>(), size_t(kQueries * 4) * sizeof(float));
        const auto tDec = now();

        // reading order glue
        std::vector<float> padBoxes(size_t(kQueries * 4), 0);
        std::vector<int> labs(kQueries, 0);
        std::vector<char> keep(kQueries, 0);
        for (int q = 0; q < kQueries; ++q) {
            int best = 0;
            float bv = (*logits)[q * kClasses];
            for (int c = 1; c < kClasses; ++c)
                if ((*logits)[q * kClasses + c] > bv) {
                    bv = (*logits)[q * kClasses + c];
                    best = c;
                }
            const float p = 1.f / (1.f + std::exp(-bv));
            static const float kThr[25] = {0.5, 0.5, 0.5, 0.5, 0.5, 0.4, 0.4, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5,
                                           0.5, 0.5, 0.4, 0.5, 0.4, 0.5, 0.5, 0.45, 0.5, 0.4, 0.4, 0.5};
            keep[size_t(q)] = char(p >= kThr[best]);
            labs[size_t(q)] = best;
        }
        std::vector<int> ord(kQueries);
        std::iota(ord.begin(), ord.end(), 0);
        std::stable_sort(ord.begin(), ord.end(), [&](int a, int b) {
            if (keep[size_t(a)] != keep[size_t(b)])
                return keep[size_t(a)] > keep[size_t(b)];
            return a < b;
        });
        static const int kOrd[25] = {4, 2, 14, 1, 5, 7, 8, 6, 11, 11, 9, 13, 10, 10, 1, 2, 3, 0, 2, 2, 12, 1, 2, 15, 6};
        std::vector<float> sLogits(size_t(kQueries * kClasses)), sBoxes(size_t(kQueries * 4));
        std::vector<int> sLab(kQueries, 0);
        std::vector<char> sKeep(kQueries, 0);
        for (int i = 0; i < kQueries; ++i) {
            const int s = ord[size_t(i)];
            std::memcpy(sLogits.data() + i * kClasses, logits->constData() + s * kClasses, size_t(kClasses) * sizeof(float));
            std::memcpy(sBoxes.data() + i * 4, predBoxes->constData() + s * 4, 4 * sizeof(float));
            sKeep[size_t(i)] = keep[size_t(s)];
            int cls = 0;
            if (sKeep[size_t(i)]) {
                const float cx = sBoxes[size_t(i * 4)], cy = sBoxes[size_t(i * 4 + 1)];
                const float bw = sBoxes[size_t(i * 4 + 2)], bh = sBoxes[size_t(i * 4 + 3)];
                padBoxes[size_t(i * 4 + 0)] = std::clamp((cx - 0.5f * bw) * 1000.f, 0.f, 1000.f);
                padBoxes[size_t(i * 4 + 1)] = std::clamp((cy - 0.5f * bh) * 1000.f, 0.f, 1000.f);
                padBoxes[size_t(i * 4 + 2)] = std::clamp((cx + 0.5f * bw) * 1000.f, 0.f, 1000.f);
                padBoxes[size_t(i * 4 + 3)] = std::clamp((cy + 0.5f * bh) * 1000.f, 0.f, 1000.f);
                cls = labs[size_t(s)];
            }
            sLab[size_t(i)] = kOrd[cls];
        }
        logits->resize(kQueries * kClasses);
        predBoxes->resize(kQueries * 4);
        std::memcpy(logits->data(), sLogits.data(), sLogits.size() * sizeof(float));
        std::memcpy(predBoxes->data(), sBoxes.data(), sBoxes.size() * sizeof(float));
        orderLogits->resize(kQueries * kQueries);
        readingOrder(*n, padBoxes.data(), sLab.data(), sKeep.data(), orderLogits->data());
        const auto tOrd = now();
        fprintf(stderr, "layout: backbone=%.0fms enc/fpn=%.0fms decoder=%.0fms order=%.0fms tot=%.0fms\n",
                ms(t0, tBb), ms(tBb, tEnc), ms(tEnc, tDec), ms(tDec, tOrd), ms(t0, tOrd));
        if (err)
            err->clear();
        return true;
    } catch (const std::exception& ex) {
        return fail(QString::fromUtf8(ex.what()));
    }
}

#endif

bool officialLayoutForward(const float* nchw, int height, int width, QVector<float>* logits,
                           QVector<float>* predBoxes, QVector<float>* orderLogits, QString* err) {
#if defined(SCANENGINE_MLX)
    return officialLayoutForwardMlx(nchw, height, width, logits, predBoxes, orderLogits, err);
#else
    Q_UNUSED(nchw);
    Q_UNUSED(height);
    Q_UNUSED(width);
    Q_UNUSED(logits);
    Q_UNUSED(predBoxes);
    Q_UNUSED(orderLogits);
    if (err)
        *err = QStringLiteral("需要 GPU：C++ hybrid 不再提供 CPU layout");
    return false;
#endif
}
}  // namespace hybrid
}  // namespace scanengine
