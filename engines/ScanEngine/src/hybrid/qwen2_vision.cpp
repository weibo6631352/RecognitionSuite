#include "scanengine/hybrid/qwen2_vision.hpp"
#include "vlm_mlx.hpp"

#if defined(SCANENGINE_MLX)
#include "mlx_gemm.hpp"
#endif

#include "scanengine/hybrid/safetensors.hpp"
#include "scanengine/hybrid/vlm_paths.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <utility>
#if defined(SCANENGINE_MLX)
#include <memory>
#include <optional>
#include <vector>
#endif

namespace scanengine {
namespace hybrid {

namespace {

bool loadLinear(const OfficialWeightIndex& weights,
                const QString& weightName,
                const QString& biasName,
                Qwen2Linear* out,
                QString* err) {
    QVector<qint64> shape;
    if (!loadTensorF32(weights.file, weightName, &out->weight, &shape, err))
        return false;
    if (shape.size() != 2)
        return false;
    out->out = int(shape[0]);
    out->in = int(shape[1]);
    if (!biasName.isEmpty()) {
        QVector<qint64> bshape;
        if (!loadTensorF32(weights.file, biasName, &out->bias, &bshape, err))
            return false;
    } else {
        out->bias.clear();
    }
    return true;
}

void buildVisionRot(const Qwen2VisionModel& model,
                    int gridT,
                    int gridH,
                    int gridW,
                    int merge,
                    QVector<float>* rot) {
    // vision.py VisionModel.rot_pos_emb + apply_rotary tile to head_dim
    const int embed = model.cfg.visionEmbedDim;
    const int nHeads = model.cfg.visionNumHeads;
    const int headDim = embed / nHeads;
    const int nFreq = int(model.visionInvFreq.size());  // 20
    const int S = gridT * gridH * gridW;
    QVector<int> hpos(S);
    QVector<int> wpos(S);
    int idx = 0;
    for (int t = 0; t < gridT; ++t) {
        const int ghm = gridH / merge;
        const int gwm = gridW / merge;
        for (int hm = 0; hm < ghm; ++hm) {
            for (int wm = 0; wm < gwm; ++wm) {
                for (int mh = 0; mh < merge; ++mh) {
                    for (int mw = 0; mw < merge; ++mw) {
                        hpos[idx] = hm * merge + mh;
                        wpos[idx] = wm * merge + mw;
                        ++idx;
                    }
                }
            }
        }
    }
    // After reshape/transpose in official rot_pos_emb, flatten order is
    // (h/m, w/m, merge, merge) which matches the preprocessor flatten.
    rot->resize(S * headDim);
    for (int s = 0; s < S; ++s) {
        for (int i = 0; i < nFreq; ++i) {
            const float fh = float(hpos[s]) * model.visionInvFreq[i];
            const float fw = float(wpos[s]) * model.visionInvFreq[i];
            // concat h_freq[20] + w_freq[20] = 40, then tile x2 → 80
            (*rot)[s * headDim + i] = fh;
            (*rot)[s * headDim + nFreq + i] = fw;
            (*rot)[s * headDim + 2 * nFreq + i] = fh;
            (*rot)[s * headDim + 3 * nFreq + i] = fw;
        }
    }
}

#if defined(SCANENGINE_MLX)
struct MlxLinear {
    mx::array weightT;
    std::optional<mx::array> bias;

    static MlxLinear fromHost(const Qwen2Linear& w) {
        MlxLinear o{mx::transpose(mx::astype(mx::array(w.weight.constData(), {w.out, w.in}), mx::bfloat16)),
                    std::nullopt};
        if (!w.bias.isEmpty())
            o.bias = mx::astype(mx::array(w.bias.constData(), {w.out}), mx::bfloat16);
        return o;
    }

    mx::array apply(const mx::array& x) const {
        return linearNN(x, weightT, bias ? &*bias : nullptr);
    }

    void collect(std::vector<mx::array>* leaves) const {
        leaves->push_back(weightT);
        if (bias)
            leaves->push_back(*bias);
    }
};

struct MlxBlock {
    MlxLinear qkv;
    MlxLinear proj;
    MlxLinear fc1;
    MlxLinear fc2;
    mx::array norm1W;
    mx::array norm1B;
    mx::array norm2W;
    mx::array norm2B;
};

struct VisionMlxCache {
    mx::array patchEmbedT;
    std::vector<MlxBlock> blocks;
    mx::array lnqW;
    mx::array lnqB;
    MlxLinear merge0;
    MlxLinear merge2;

    VisionMlxCache(mx::array pe, mx::array w, mx::array b, MlxLinear m0, MlxLinear m2)
        : patchEmbedT(std::move(pe)),
          lnqW(std::move(w)),
          lnqB(std::move(b)),
          merge0(std::move(m0)),
          merge2(std::move(m2)) {}
};

VisionMlxCache& mlxWeights(const Qwen2VisionModel& model) {
    static VisionMlxCache* cache = nullptr;
    static const float* key = nullptr;
    if (cache && key == model.patchEmbed.constData())
        return *cache;

    const int D = model.cfg.visionEmbedDim;
    const int inner = int(model.patchEmbed.size()) / D;
    // Leak: compiled vision graphs cannot be torn down after Metal shutdown.
    cache = new VisionMlxCache(
        mx::transpose(mx::astype(mx::array(model.patchEmbed.constData(), {D, inner}), mx::bfloat16)),
        mx::astype(mx::array(model.lnqW.constData(), {D}), mx::bfloat16),
        mx::astype(mx::array(model.lnqB.constData(), {D}), mx::bfloat16),
        MlxLinear::fromHost(model.merge0),
        MlxLinear::fromHost(model.merge2));
    cache->blocks.reserve(size_t(model.blocks.size()));
    for (const Qwen2VisionBlockWeights& b : model.blocks) {
        cache->blocks.push_back(MlxBlock{
            MlxLinear::fromHost(b.qkv),
            MlxLinear::fromHost(b.proj),
            MlxLinear::fromHost(b.fc1),
            MlxLinear::fromHost(b.fc2),
            mx::astype(mx::array(b.norm1W.constData(), {D}), mx::bfloat16),
            mx::astype(mx::array(b.norm1B.constData(), {D}), mx::bfloat16),
            mx::astype(mx::array(b.norm2W.constData(), {D}), mx::bfloat16),
            mx::astype(mx::array(b.norm2B.constData(), {D}), mx::bfloat16),
        });
    }

    std::vector<mx::array> leaves;
    leaves.reserve(8 + size_t(cache->blocks.size()) * 12);
    leaves.push_back(cache->patchEmbedT);
    leaves.push_back(cache->lnqW);
    leaves.push_back(cache->lnqB);
    cache->merge0.collect(&leaves);
    cache->merge2.collect(&leaves);
    for (const MlxBlock& b : cache->blocks) {
        b.qkv.collect(&leaves);
        b.proj.collect(&leaves);
        b.fc1.collect(&leaves);
        b.fc2.collect(&leaves);
        leaves.push_back(b.norm1W);
        leaves.push_back(b.norm1B);
        leaves.push_back(b.norm2W);
        leaves.push_back(b.norm2B);
    }
    mx::eval(leaves);
    key = model.patchEmbed.constData();
    return *cache;
}

mx::array applyVisionRopeMx(const mx::array& x, const mx::array& freqs) {
    // Official apply_rotary_pos_emb_vision: cos/sin in float32, then astype back.
    const int S = x.shape(0);
    const int nHeads = x.shape(1);
    const int headDim = x.shape(2);
    const int half = headDim / 2;
    const auto cos = mx::expand_dims(mx::cos(freqs), 1);
    const auto sin = mx::expand_dims(mx::sin(freqs), 1);
    const auto x1 = mx::slice(x, {0, 0, 0}, {S, nHeads, half});
    const auto x2 = mx::slice(x, {0, 0, half}, {S, nHeads, headDim});
    const auto rot = mx::concatenate({-x2, x1}, 2);
    return mx::astype(x * cos + rot * sin, x.dtype());
}

mx::array visionAttentionMx(const MlxBlock& block,
                            const mx::array& x,
                            const mx::array& freqs,
                            int S,
                            int nHeads,
                            int headDim) {
    auto qkv = mx::reshape(block.qkv.apply(x), {S, 3, nHeads, headDim});
    const auto parts = mx::split(qkv, 3, 1);
    auto q = applyVisionRopeMx(mx::reshape(parts[0], {S, nHeads, headDim}), freqs);
    auto k = applyVisionRopeMx(mx::reshape(parts[1], {S, nHeads, headDim}), freqs);
    auto v = mx::reshape(parts[2], {S, nHeads, headDim});
    q = mx::expand_dims(mx::transpose(q, {1, 0, 2}), 0);
    k = mx::expand_dims(mx::transpose(k, {1, 0, 2}), 0);
    v = mx::expand_dims(mx::transpose(v, {1, 0, 2}), 0);
    const float scale = 1.0f / std::sqrt(float(headDim));
    auto out = mx::fast::scaled_dot_product_attention(q, k, v, scale);
    out = mx::reshape(mx::transpose(out, {0, 2, 1, 3}), {S, nHeads * headDim});
    return block.proj.apply(out);
}

bool visionForwardDevice(const Qwen2VisionModel& model,
                         const OfficialImagePatches& patches,
                         mx::array* outFeatures,
                         QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!outFeatures)
        return fail(QStringLiteral("outFeatures is null"));
    const int S = patches.numPatches();
    const int D = model.cfg.visionEmbedDim;
    const int inner = patches.patchInner;
    const int nHeads = model.cfg.visionNumHeads;
    const int headDim = D / nHeads;
    if (S <= 0 || patches.patches.size() != S * inner)
        return fail(QStringLiteral("bad patch tensor"));
    if (model.patchEmbed.size() != D * inner)
        return fail(QStringLiteral("patch_embed size mismatch"));

    const VisionMlxCache& w = mlxWeights(model);
    auto now = []() { return std::chrono::steady_clock::now(); };
    auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    const auto t0 = now();
    // Official VisionModel: pixel_values.astype(patch_embed.weight.dtype) — bfloat16.
    auto h = mx::matmul(mx::astype(mx::array(patches.patches.constData(), {S, inner}), mx::bfloat16),
                        w.patchEmbedT);

    QVector<float> rot;
    buildVisionRot(model, patches.gridT, patches.gridH, patches.gridW, patches.mergeSize, &rot);
    const auto freqs = mx::array(rot.constData(), {S, headDim});

    for (const MlxBlock& b : w.blocks) {
        const auto n1 = mx::fast::layer_norm(h, b.norm1W, b.norm1B, 1e-6f);
        h = h + visionAttentionMx(b, n1, freqs, S, nHeads, headDim);
        const auto n2 = mx::fast::layer_norm(h, b.norm2W, b.norm2B, 1e-6f);
        const auto fc1 = b.fc1.apply(n2);
        // Official nn.GELU(approx="fast") keeps the activation dtype.
        const auto hid = fc1 * mx::sigmoid(fc1 * mx::array(1.702f, fc1.dtype()));
        h = h + b.fc2.apply(hid);
    }

    auto ln = mx::fast::layer_norm(h, w.lnqW, w.lnqB, 1e-6f);
    const int merge = patches.mergeSize;
    const int grouped = S / (merge * merge);
    const int mergedDim = D * merge * merge;
    auto m1 = w.merge0.apply(mx::reshape(ln, {grouped, mergedDim}));
    // Official PatchMerger nn.GELU() (erf) stays on the activation dtype.
    const auto half = mx::array(0.5f, m1.dtype());
    const auto one = mx::array(1.0f, m1.dtype());
    const auto invSqrt2 = mx::array(float(1.0 / std::sqrt(2.0)), m1.dtype());
    m1 = m1 * half * (one + mx::erf(m1 * invSqrt2));
    *outFeatures = w.merge2.apply(m1);
    // Official VisionModel.__call__ does not eval; generate_step evals the first token.
    fprintf(stderr, "vision: S=%d blocks=%d graph=%.0fms\n", S, int(w.blocks.size()),
            ms(t0, now()));
    if (err)
        err->clear();
    return true;
}

bool forwardOfficialVisionMlx(const Qwen2VisionModel& model,
                              const OfficialImagePatches& patches,
                              QVector<float>* outFeatures,
                              QString* err) {
    if (!outFeatures) {
        if (err)
            *err = QStringLiteral("outFeatures is null");
        return false;
    }
    mx::array out(0.0f);
    if (!visionForwardDevice(model, patches, &out, err))
        return false;
    const auto outF32 = mx::astype(out, mx::float32);
    mx::eval(outF32);
    outFeatures->resize(int(outF32.size()));
    std::memcpy(outFeatures->data(), outF32.data<float>(), outF32.size() * sizeof(float));
    return true;
}
#endif  // SCANENGINE_MLX

}  // namespace

bool loadOfficialVisionModel(const OfficialWeightIndex& weights,
                             const VlmModelConfig& cfg,
                             Qwen2VisionModel* out,
                             QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));

    Qwen2VisionModel m;
    m.cfg = cfg;
    QVector<qint64> pshape;
    if (!loadTensorF32(weights.file, QStringLiteral("visual.patch_embed.proj.weight"), &m.patchEmbed, &pshape, err))
        return false;
    m.blocks.resize(cfg.visionDepth);
    for (int i = 0; i < cfg.visionDepth; ++i) {
        const QString p = QStringLiteral("visual.blocks.%1.").arg(i);
        Qwen2VisionBlockWeights& b = m.blocks[i];
        if (!loadLinear(weights, p + QStringLiteral("attn.qkv.weight"), p + QStringLiteral("attn.qkv.bias"), &b.qkv, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("attn.proj.weight"), p + QStringLiteral("attn.proj.bias"), &b.proj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("mlp.fc1.weight"), p + QStringLiteral("mlp.fc1.bias"), &b.fc1, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("mlp.fc2.weight"), p + QStringLiteral("mlp.fc2.bias"), &b.fc2, err))
            return false;
        QVector<qint64> sh;
        if (!loadTensorF32(weights.file, p + QStringLiteral("norm1.weight"), &b.norm1W, &sh, err))
            return false;
        if (!loadTensorF32(weights.file, p + QStringLiteral("norm1.bias"), &b.norm1B, &sh, err))
            return false;
        if (!loadTensorF32(weights.file, p + QStringLiteral("norm2.weight"), &b.norm2W, &sh, err))
            return false;
        if (!loadTensorF32(weights.file, p + QStringLiteral("norm2.bias"), &b.norm2B, &sh, err))
            return false;
    }
    QVector<qint64> sh;
    if (!loadTensorF32(weights.file, QStringLiteral("visual.merger.ln_q.weight"), &m.lnqW, &sh, err))
        return false;
    if (!loadTensorF32(weights.file, QStringLiteral("visual.merger.ln_q.bias"), &m.lnqB, &sh, err))
        return false;
    if (!loadLinear(weights, QStringLiteral("visual.merger.mlp.0.weight"),
                    QStringLiteral("visual.merger.mlp.0.bias"), &m.merge0, err))
        return false;
    if (!loadLinear(weights, QStringLiteral("visual.merger.mlp.2.weight"),
                    QStringLiteral("visual.merger.mlp.2.bias"), &m.merge2, err))
        return false;

    // VisionRotaryEmbedding(head_dim // 2) with dim=40, theta=10000
    const int ropeDim = (cfg.visionEmbedDim / cfg.visionNumHeads) / 2;  // 40
    m.visionInvFreq.resize(ropeDim / 2);  // 20
    for (int i = 0; i < ropeDim / 2; ++i)
        m.visionInvFreq[i] = float(1.0 / std::pow(10000.0, double(2 * i) / double(ropeDim)));

    *out = std::move(m);
    if (err)
        err->clear();
    return true;
}

bool officialVisionModel(Qwen2VisionModel** out, QString* err) {
    static Qwen2VisionModel cached;
    static bool ready = false;
    static QString cachedErr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        const VlmPaths p = resolveOfficialVlmPaths();
        VlmModelConfig cfg;
        OfficialWeightIndex idx;
        if (!loadOfficialVlmConfig(p.configJson, &cfg, &cachedErr)
            || !indexOfficialWeights(p.weights, &idx, &cachedErr)
            || !loadOfficialVisionModel(idx, cfg, &cached, &cachedErr)) {
            ready = false;
        } else {
            ready = true;
#if defined(SCANENGINE_MLX)
            mlxWeights(cached);
#endif
        }
    }
    if (!ready) {
        if (err)
            *err = cachedErr;
        return false;
    }
    if (out)
        *out = &cached;
    if (err)
        err->clear();
    return true;
}

#if defined(SCANENGINE_MLX)
bool forwardOfficialVisionDevice(const Qwen2VisionModel& model,
                                 const OfficialImagePatches& patches,
                                 mlx::core::array* outFeatures,
                                 QString* err) {
    return visionForwardDevice(model, patches, outFeatures, err);
}
#endif

bool forwardOfficialVision(const Qwen2VisionModel& model,
                           const OfficialImagePatches& patches,
                           QVector<float>* outFeatures,
                           QString* err) {
#if defined(SCANENGINE_MLX)
    return forwardOfficialVisionMlx(model, patches, outFeatures, err);
#else
    Q_UNUSED(model);
    Q_UNUSED(patches);
    if (outFeatures)
        outFeatures->clear();
    if (err)
        *err = QStringLiteral("vision inference requires the certified GPU backend");
    return false;
#endif
}

}  // namespace hybrid
}  // namespace scanengine
