#include "scanengine/hybrid/qwen2_language.hpp"
#include "vlm_mlx.hpp"

#if defined(SCANENGINE_MLX)
#include "mlx_gemm.hpp"
#endif

#include "scanengine/hybrid/safetensors.hpp"
#include "scanengine/hybrid/vlm_paths.hpp"

#include <QString>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

bool loadVec(const OfficialWeightIndex& weights, const QString& name, QVector<float>* out, QString* err) {
    QVector<qint64> shape;
    return loadTensorF32(weights.file, name, out, &shape, err);
}

void buildTextCosSin(const Qwen2LanguageModel& model,
                     int length,
                     const int* posT,
                     const int* posH,
                     const int* posW,
                     QVector<float>* cos,
                     QVector<float>* sin) {
    const int headDim = model.cfg.hiddenSize / model.cfg.numAttentionHeads;
    const int frequencyCount = headDim / 2;
    QVector<float> frequencies(3 * length * frequencyCount);
    const int* axes[3] = {posT, posH, posW};
    for (int axis = 0; axis < 3; ++axis) {
        for (int token = 0; token < length; ++token) {
            const float position = axes[axis]
                ? float(axes[axis][token]) : float(token);
            for (int i = 0; i < frequencyCount; ++i) {
                frequencies[(axis * length + token) * frequencyCount + i] =
                    position * model.invFreq[i];
            }
        }
    }

    QVector<float> merged(length * frequencyCount);
    std::memcpy(merged.data(), frequencies.constData(),
                size_t(length * frequencyCount) * sizeof(float));
    int offset = model.mropeSection[0];
    for (int axis = 1; axis < 3; ++axis) {
        const int sectionLength = model.mropeSection[axis];
        for (int token = 0; token < length; ++token) {
            std::memcpy(
                merged.data() + token * frequencyCount + offset,
                frequencies.constData()
                    + (axis * length + token) * frequencyCount + offset,
                size_t(sectionLength) * sizeof(float));
        }
        offset += sectionLength;
    }

    cos->resize(length * headDim);
    sin->resize(length * headDim);
    for (int token = 0; token < length; ++token) {
        for (int i = 0; i < frequencyCount; ++i) {
            const float frequency = merged[token * frequencyCount + i];
            const float cosine = std::cos(frequency);
            const float sine = std::sin(frequency);
            (*cos)[token * headDim + i] = cosine;
            (*cos)[token * headDim + frequencyCount + i] = cosine;
            (*sin)[token * headDim + i] = sine;
            (*sin)[token * headDim + frequencyCount + i] = sine;
        }
    }
}

#if defined(SCANENGINE_MLX)
// Official mlx_vlm loads safetensors as-is#if defined(SCANENGINE_MLX)
// Official mlx_vlm loads safetensors as-is: MinerU2.5-Pro-1.2B is bfloat16.
mx::array officialW(const mx::array& a) {
    return mx::astype(a, mx::bfloat16);
}

struct LangMlxLinear {
    mx::array weightT;
    std::optional<mx::array> bias;

    static LangMlxLinear fromHost(const Qwen2Linear& w) {
        LangMlxLinear o{mx::transpose(officialW(mx::array(w.weight.constData(), {w.out, w.in}))), std::nullopt};
        if (!w.bias.isEmpty())
            o.bias = officialW(mx::array(w.bias.constData(), {w.out}));
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

struct LangMlxLayer {
    LangMlxLinear q;
    LangMlxLinear k;
    LangMlxLinear v;
    LangMlxLinear o;
    LangMlxLinear gate;
    LangMlxLinear up;
    LangMlxLinear down;
    mx::array inputNorm;
    mx::array postNorm;
};

struct LangMlxWeights {
    mx::array embed;
    mx::array embedT;
    mx::array finalNorm;
    mx::array invFreq;
    std::vector<LangMlxLayer> layers;

    LangMlxWeights(mx::array e, mx::array et, mx::array n, mx::array inv)
        : embed(std::move(e)), embedT(std::move(et)), finalNorm(std::move(n)), invFreq(std::move(inv)) {}
};

LangMlxWeights& langMlxWeights(const Qwen2LanguageModel& model) {
    static LangMlxWeights* cache = nullptr;
    static const float* key = nullptr;
    if (cache && key == model.embed.constData())
        return *cache;
    // Leak: mlx compiled graphs cannot be torn down after Metal shutdown.
    cache = new LangMlxWeights(
        officialW(mx::array(model.embed.constData(), {model.cfg.vocabSize, model.cfg.hiddenSize})),
        officialW(mx::array(model.embed.constData(), {model.cfg.hiddenSize, model.cfg.vocabSize})),
        officialW(mx::array(model.finalNorm.constData(), {model.cfg.hiddenSize})),
        mx::array(model.invFreq.constData(), {int(model.invFreq.size())}));
    cache->embedT = mx::transpose(cache->embed);
    cache->layers.reserve(size_t(model.layers.size()));
    for (const Qwen2DecoderLayerWeights& ly : model.layers) {
        cache->layers.push_back(LangMlxLayer{
            LangMlxLinear::fromHost(ly.qProj),
            LangMlxLinear::fromHost(ly.kProj),
            LangMlxLinear::fromHost(ly.vProj),
            LangMlxLinear::fromHost(ly.oProj),
            LangMlxLinear::fromHost(ly.gateProj),
            LangMlxLinear::fromHost(ly.upProj),
            LangMlxLinear::fromHost(ly.downProj),
            officialW(mx::array(ly.inputNorm.constData(), {model.cfg.hiddenSize})),
            officialW(mx::array(ly.postNorm.constData(), {model.cfg.hiddenSize})),
        });
    }
    std::vector<mx::array> leaves;
    leaves.push_back(cache->embed);
    leaves.push_back(cache->embedT);
    leaves.push_back(cache->finalNorm);
    for (const LangMlxLayer& ly : cache->layers) {
        ly.q.collect(&leaves);
        ly.k.collect(&leaves);
        ly.v.collect(&leaves);
        ly.o.collect(&leaves);
        ly.gate.collect(&leaves);
        ly.up.collect(&leaves);
        ly.down.collect(&leaves);
        leaves.push_back(ly.inputNorm);
        leaves.push_back(ly.postNorm);
    }
    mx::eval(leaves);
    key = model.embed.constData();
    fprintf(stderr, "vlm: official weight dtype=bfloat16\n");
    return *cache;
}

struct LangMlxKv {
    int len = 0;
    int cap = 0;
    std::vector<mx::array> k;
    std::vector<mx::array> v;
};

constexpr int kOfficialKvStep = 256;  // mlx_lm.models.cache.KVCache.step

void officialKvGrow(LangMlxKv* mk, int li, int need, int nKv, int headDim, mx::Dtype dt) {
    const int have = (mk->k[size_t(li)].ndim() == 3) ? mk->k[size_t(li)].shape(1) : 0;
    if (have >= need)
        return;
    const int add = ((kOfficialKvStep + need - have - 1) / kOfficialKvStep) * kOfficialKvStep;
    auto zk = mx::zeros({nKv, add, headDim}, dt);
    auto zv = mx::zeros({nKv, add, headDim}, dt);
    if (have > 0) {
        mk->k[size_t(li)] = mx::concatenate({mk->k[size_t(li)], zk}, 1);
        mk->v[size_t(li)] = mx::concatenate({mk->v[size_t(li)], zv}, 1);
    } else {
        mk->k[size_t(li)] = std::move(zk);
        mk->v[size_t(li)] = std::move(zv);
    }
}

void officialKvUpdateFetch(LangMlxKv* mk,
                           int li,
                           const mx::array& kNew,
                           const mx::array& vNew,
                           int past,
                           int nKv,
                           int headDim,
                           mx::array* kOut,
                           mx::array* vOut) {
    // mlx_lm KVCache.update_and_fetch: pad by 256, write at offset, return [:offset].
    const int addLen = kNew.shape(1);
    const int need = past + addLen;
    officialKvGrow(mk, li, need, nKv, headDim, kNew.dtype());
    const mx::Shape start = {0, past, 0};
    const mx::Shape stop = {nKv, need, headDim};
    mk->k[size_t(li)] = mx::slice_update(mk->k[size_t(li)], kNew, start, stop);
    mk->v[size_t(li)] = mx::slice_update(mk->v[size_t(li)], vNew, start, stop);
    *kOut = mx::slice(mk->k[size_t(li)], {0, 0, 0}, {nKv, need, headDim});
    *vOut = mx::slice(mk->v[size_t(li)], {0, 0, 0}, {nKv, need, headDim});
}

std::unordered_map<const Qwen2KvCache*, LangMlxKv>& langMlxKvMap() {
    static std::unordered_map<const Qwen2KvCache*, LangMlxKv> m;
    return m;
}

// Official Qwen2RMSNorm on a bf16 residual: fp32 variance, cast back, then
// weight * y in the input dtype. Residual stays bf16 — do not upcast the net.
// One thread per row; grid is total thread extent (same as layout silu).
mx::array officialRmsNorm(const mx::array& x, const mx::array& weight, float eps) {
    if (x.ndim() != 2 || x.dtype() != mx::bfloat16)
        return mx::fast::rms_norm(x, weight, eps);
    const int rows = x.shape(0);
    static auto kernel = mx::fast::cuda_kernel(
        "scanengine_official_rmsnorm_bf16",
        {"x", "w"},
        {"y"},
        R"(
          auto row = cg::this_grid().thread_rank();
          const int D = x_shape[1];
          const int nRows = x_shape[0];
          if (static_cast<int>(row) >= nRows) return;
          float acc = 0.f;
          for (int i = 0; i < D; ++i) {
            const float v = __bfloat162float(x[row * D + i]);
            acc += v * v;
          }
          const float inv = rsqrtf(acc / static_cast<float>(D) + 1.0e-6f);
          for (int i = 0; i < D; ++i) {
            const float v = __bfloat162float(x[row * D + i]);
            const float yv = __bfloat162float(__float2bfloat16(v * inv));
            const float wv = __bfloat162float(w[i]);
            y[row * D + i] = __float2bfloat16(wv * yv);
          }
        )");
    (void)eps;
    auto outs = kernel(
        {mx::contiguous(x), mx::contiguous(weight)},
        {x.shape()},
        {x.dtype()},
        {rows, 1, 1},
        {256, 1, 1},
        {},
        std::nullopt,
        false,
        {});
    return outs.front();
}

mx::array applyTextRopeMx(const mx::array& x, const mx::array& cos, const mx::array& sin) {
    // x: [L, nH, headDim], cos/sin: [L, headDim]
    const int L = x.shape(0);
    const int nH = x.shape(1);
    const int hd = x.shape(2);
    const int half = hd / 2;
    const auto c = mx::expand_dims(mx::astype(cos, x.dtype()), 1);
    const auto s = mx::expand_dims(mx::astype(sin, x.dtype()), 1);
    const auto x1 = mx::slice(x, {0, 0, 0}, {L, nH, half});
    const auto x2 = mx::slice(x, {0, 0, half}, {L, nH, hd});
    return x * c + mx::concatenate({-x2, x1}, 2) * s;
}

bool forwardOfficialLanguageMlx(const Qwen2LanguageModel& model,
                                const QVector<int>& ids,
                                QVector<float>* lastLogits,
                                int* lastArgmax,
                                float* lastMax,
                                QString* err,
                                const float* inputsEmbeds,
                                const int* posT,
                                const int* posH,
                                const int* posW,
                                Qwen2KvCache* cache,
                                const mx::array* embedsMx = nullptr,
                                const mx::array* tokenIn = nullptr,
                                mx::array* tokenOut = nullptr) {
    const int L = tokenIn ? 1 : (embedsMx ? embedsMx->shape(0) : ids.size());
    const int D = model.cfg.hiddenSize;
    const int V = model.cfg.vocabSize;
    const int nHeads = model.cfg.numAttentionHeads;
    const int nKv = model.cfg.numKeyValueHeads;
    const int headDim = D / nHeads;
    const float scale = 1.0f / std::sqrt(float(headDim));
    LangMlxWeights& w = langMlxWeights(model);

    LangMlxKv* mk = nullptr;
    int past = 0;
    if (cache) {
        mk = &langMlxKvMap()[cache];
        if (mk->k.size() != size_t(model.layers.size())) {
            mk->k.assign(size_t(model.layers.size()), mx::array(0.0f));
            mk->v.assign(size_t(model.layers.size()), mx::array(0.0f));
            mk->len = 0;
            mk->cap = 0;
        }
        past = mk->len;
    }

    const int nLayers = int(w.layers.size());
    // Official mlx_vlm LanguageModel: one lazy graph for all layers, async_eval the token only.
    if (L == 1 && past > 0) {
        static bool logged = false;
        if (!logged) {
            fprintf(stderr, "vlm: official module decode (%d layers, no per-layer compile)\n", nLayers);
            logged = true;
        }
    }

    mx::array h = embedsMx ? *embedsMx
                           : (inputsEmbeds ? mx::array(inputsEmbeds, {L, D})
                                           : (tokenIn ? mx::take(w.embed, mx::reshape(*tokenIn, {1}), 0)
                                                      : mx::take(w.embed, mx::array(ids.constData(), {L}), 0)));
    if (h.dtype() != mx::bfloat16)
        h = officialW(h);
    QVector<float> cosH, sinH;
    buildTextCosSin(model, L, posT, posH, posW, &cosH, &sinH);
    const auto cos = mx::array(cosH.constData(), {L, headDim});
    const auto sin = mx::array(sinH.constData(), {L, headDim});

    const std::string attnMask = (L == 1) ? std::string() : std::string("causal");
    for (int li = 0; li < nLayers; ++li) {
        const LangMlxLayer& ly = w.layers[size_t(li)];
        const auto n1 = officialRmsNorm(h, ly.inputNorm, float(model.cfg.rmsNormEps));
        auto q = mx::reshape(ly.q.apply(n1), {L, nHeads, headDim});
        auto k = mx::reshape(ly.k.apply(n1), {L, nKv, headDim});
        auto v = mx::reshape(ly.v.apply(n1), {L, nKv, headDim});
        q = applyTextRopeMx(q, cos, sin);
        k = applyTextRopeMx(k, cos, sin);
        k = mx::transpose(k, {1, 0, 2});
        v = mx::transpose(v, {1, 0, 2});
        if (mk) {
            officialKvUpdateFetch(mk, li, k, v, past, nKv, headDim, &k, &v);
        }
        q = mx::expand_dims(mx::transpose(q, {1, 0, 2}), 0);
        auto kH = mx::expand_dims(k, 0);
        auto vH = mx::expand_dims(v, 0);
        auto attn = mx::fast::scaled_dot_product_attention(q, kH, vH, scale, attnMask);
        attn = mx::reshape(mx::transpose(attn, {0, 2, 1, 3}), {L, D});
        h = h + ly.o.apply(attn);
        const auto n2 = officialRmsNorm(h, ly.postNorm, float(model.cfg.rmsNormEps));
        const auto gate = ly.gate.apply(n2);
        // Official ACT2FN / F.silu is x/(1+exp(-x)). Keep the 1 in gate.dtype()
        // so the residual stream is not promoted to fp32 (that path is ~11 tok/s
        // and blows the 8.025 s window).
        const auto one = mx::array(1.0f, gate.dtype());
        const auto silu = gate / (one + mx::exp(-gate));
        const auto hid = silu * ly.up.apply(n2);
        h = h + ly.down.apply(hid);
    }

    const auto last = mx::slice(h, {L - 1, 0}, {L, D});
    const auto normed = officialRmsNorm(last, w.finalNorm, float(model.cfg.rmsNormEps));
    const auto logits = mx::matmul(normed, w.embedT);
    // Official mlx_vlm.generate_step: async_eval the sampled token only.
    if (lastLogits) {
        const auto logitsF32 = mx::astype(logits, mx::float32);
        mx::eval(logitsF32);
        lastLogits->resize(V);
        std::memcpy(lastLogits->data(), logitsF32.data<float>(), size_t(V) * sizeof(float));
    } else {
        const auto best = mx::argmax(logits, -1);
        const auto bestv = mx::take(logits, best);
        mx::async_eval(best, bestv);
        if (tokenOut) {
            *tokenOut = best;
            if (lastMax)
                *lastMax = 0;
        } else {
            if (lastArgmax)
                *lastArgmax = int(best.item<uint32_t>());
            if (lastMax)
                *lastMax = mx::astype(bestv, mx::float32).item<float>();
        }
    }
    if (cache)
        cache->len += L;
    if (mk) {
        mk->len += L;
        if (mk->len > 0 && (mk->len % 256) == 0)
            mx::clear_cache();
    }
    if (err)
        err->clear();
    return true;
}

bool mergeOfficialImageEmbedsImpl(const Qwen2LanguageModel& model,
                                  const QVector<int>& ids,
                                  const mx::array& imageFeatures,
                                  mx::array* outEmbeds,
                                  QString* err) {
    if (!outEmbeds) {
        if (err)
            *err = QStringLiteral("outEmbeds is null");
        return false;
    }
    const int L = ids.size();
    const int imageTok = model.cfg.imageTokenId > 0 ? model.cfg.imageTokenId : 151655;
    LangMlxWeights& w = langMlxWeights(model);
    const auto idArr = mx::array(ids.constData(), {L});
    const auto embeds = mx::take(w.embed, idArr, 0);
    const auto imageMask = mx::equal(idArr, mx::array(imageTok));
    const auto cum = mx::cumsum(mx::astype(imageMask, mx::int32));
    const auto featIdx = mx::where(imageMask, cum - mx::array(1), mx::array(0));
    const auto feats = imageFeatures.dtype() == mx::bfloat16 ? imageFeatures : officialW(imageFeatures);
    const auto gathered = mx::take(feats, featIdx, 0);
    *outEmbeds = mx::where(mx::expand_dims(imageMask, -1), gathered, embeds);
    if (err)
        err->clear();
    return true;
}
#endif  // SCANENGINE_MLX

}  // namespace

bool loadOfficialLanguageModel(const OfficialWeightIndex& weights,
                               const VlmModelConfig& cfg,
                               Qwen2LanguageModel* out,
                               QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    if (cfg.hiddenSize <= 0 || cfg.numHiddenLayers <= 0 || cfg.vocabSize <= 0)
        return fail(QStringLiteral("invalid VLM config"));

    Qwen2LanguageModel m;
    m.cfg = cfg;
    QVector<qint64> eshape;
    if (!loadTensorF32(weights.file, QStringLiteral("model.embed_tokens.weight"), &m.embed, &eshape, err))
        return false;
    if (eshape.size() != 2 || int(eshape[0]) != cfg.vocabSize || int(eshape[1]) != cfg.hiddenSize)
        return fail(QStringLiteral("embed_tokens shape mismatch"));
    if (!loadVec(weights, QStringLiteral("model.norm.weight"), &m.finalNorm, err))
        return false;

    m.layers.resize(cfg.numHiddenLayers);
    for (int i = 0; i < cfg.numHiddenLayers; ++i) {
        const QString p = QStringLiteral("model.layers.%1.").arg(i);
        Qwen2DecoderLayerWeights& ly = m.layers[i];
        if (!loadLinear(weights, p + QStringLiteral("self_attn.q_proj.weight"),
                        p + QStringLiteral("self_attn.q_proj.bias"), &ly.qProj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("self_attn.k_proj.weight"),
                        p + QStringLiteral("self_attn.k_proj.bias"), &ly.kProj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("self_attn.v_proj.weight"),
                        p + QStringLiteral("self_attn.v_proj.bias"), &ly.vProj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("self_attn.o_proj.weight"), QString(), &ly.oProj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("mlp.gate_proj.weight"), QString(), &ly.gateProj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("mlp.up_proj.weight"), QString(), &ly.upProj, err))
            return false;
        if (!loadLinear(weights, p + QStringLiteral("mlp.down_proj.weight"), QString(), &ly.downProj, err))
            return false;
        if (!loadVec(weights, p + QStringLiteral("input_layernorm.weight"), &ly.inputNorm, err))
            return false;
        if (!loadVec(weights, p + QStringLiteral("post_attention_layernorm.weight"), &ly.postNorm, err))
            return false;
    }

    const int headDim = cfg.hiddenSize / cfg.numAttentionHeads;
    m.invFreq.resize(headDim / 2);
    const double base = cfg.ropeTheta > 0 ? cfg.ropeTheta : 1000000.0;
    for (int i = 0; i < headDim / 2; ++i)
        m.invFreq[i] = float(1.0 / std::pow(base, double(2 * i) / double(headDim)));
    m.mropeSection[0] = 8;
    m.mropeSection[1] = 12;
    m.mropeSection[2] = 12;

    *out = std::move(m);
    if (err)
        err->clear();
    return true;
}

bool officialLanguageModel(Qwen2LanguageModel** out, QString* err) {
    static Qwen2LanguageModel cached;
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
            || !loadOfficialLanguageModel(idx, cfg, &cached, &cachedErr)) {
            ready = false;
        } else {
            ready = true;
#if defined(SCANENGINE_MLX)
            langMlxWeights(cached);
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

void resetOfficialKvCache(const Qwen2LanguageModel& model, Qwen2KvCache* cache) {
    if (!cache)
        return;
    cache->len = 0;
    cache->k.resize(model.layers.size());
    cache->v.resize(model.layers.size());
    for (QVector<float>& t : cache->k)
        t.clear();
    for (QVector<float>& t : cache->v)
        t.clear();
#if defined(SCANENGINE_MLX)
    langMlxKvMap().erase(cache);
#endif
}

bool forwardOfficialLanguage(const Qwen2LanguageModel& model,
                             const QVector<int>& ids,
                             QVector<float>* lastLogits,
                             QString* err,
                             const float* inputsEmbeds,
                             const int* posT,
                             const int* posH,
                             const int* posW,
                             Qwen2KvCache* cache) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!lastLogits)
        return fail(QStringLiteral("lastLogits is null"));
    if (ids.isEmpty())
        return fail(QStringLiteral("empty ids"));

#if defined(SCANENGINE_MLX)
    return forwardOfficialLanguageMlx(model, ids, lastLogits, nullptr, nullptr, err, inputsEmbeds, posT,
                                      posH, posW, cache);
#else
    Q_UNUSED(model);
    Q_UNUSED(inputsEmbeds);
    Q_UNUSED(posT);
    Q_UNUSED(posH);
    Q_UNUSED(posW);
    Q_UNUSED(cache);
    lastLogits->clear();
    return fail(QStringLiteral("language inference requires the certified GPU backend"));
#endif
}

bool forwardOfficialLanguageArgmax(const Qwen2LanguageModel& model,
                                   const QVector<int>& ids,
                                   int* lastArgmax,
                                   float* lastMax,
                                   QString* err,
                                   const float* inputsEmbeds,
                                   const int* posT,
                                   const int* posH,
                                   const int* posW,
                                   Qwen2KvCache* cache) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!lastArgmax || !lastMax)
        return fail(QStringLiteral("lastArgmax/lastMax is null"));
    if (ids.isEmpty())
        return fail(QStringLiteral("empty ids"));

#if defined(SCANENGINE_MLX)
    return forwardOfficialLanguageMlx(model, ids, nullptr, lastArgmax, lastMax, err, inputsEmbeds, posT,
                                      posH, posW, cache);
#else
    Q_UNUSED(model);
    Q_UNUSED(inputsEmbeds);
    Q_UNUSED(posT);
    Q_UNUSED(posH);
    Q_UNUSED(posW);
    Q_UNUSED(cache);
    *lastArgmax = -1;
    *lastMax = 0.0f;
    return fail(QStringLiteral("language inference requires the certified GPU backend"));
#endif
}

bool generateOfficialLanguageGreedy(const Qwen2LanguageModel& model,
                                    const OfficialTokenizer& tok,
                                    const QVector<int>& promptIds,
                                    int maxNewTokens,
                                    LanguageGenerateResult* out,
                                    QString* err) {
    auto fail = [&](const QString& msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (!out)
        return fail(QStringLiteral("out is null"));
    if (promptIds.isEmpty())
        return fail(QStringLiteral("empty prompt"));
    if (maxNewTokens <= 0)
        return fail(QStringLiteral("maxNewTokens must be > 0"));

    LanguageGenerateResult r;
    r.promptIds = promptIds;
    QVector<int> ids = promptIds;
    const int eos = model.cfg.eosTokenId > 0 ? model.cfg.eosTokenId : 151645;
    const int pad = model.cfg.bosTokenId > 0 ? model.cfg.bosTokenId : 151643;
    Qwen2KvCache cache;
    resetOfficialKvCache(model, &cache);

    int best = 0;
    float bestV = 0;
    if (!forwardOfficialLanguageArgmax(model, ids, &best, &bestV, err, nullptr, nullptr, nullptr, nullptr,
                                       &cache))
        return false;
    for (int step = 0; step < maxNewTokens; ++step) {
        r.lastArgmax = best;
        r.lastMax = bestV;
        ids.push_back(best);
        r.newIds.push_back(best);
        if (best == eos || best == pad)
            break;
        const int pos = cache.len;
        if (!forwardOfficialLanguageArgmax(model, QVector<int>{best}, &best, &bestV, err, nullptr, &pos, &pos,
                                           &pos, &cache))
            return false;
    }
    r.decoded = decodeOfficial(tok, r.newIds, false);
    *out = r;
    if (err)
        err->clear();
    return true;
}

#if defined(SCANENGINE_MLX)
bool mergeOfficialImageEmbeds(const Qwen2LanguageModel& model,
                              const QVector<int>& ids,
                              const mlx::core::array& imageFeatures,
                              mlx::core::array* outEmbeds,
                              QString* err) {
    return mergeOfficialImageEmbedsImpl(model, ids, imageFeatures, outEmbeds, err);
}

bool forwardOfficialLanguageToken(const Qwen2LanguageModel& model,
                                  const QVector<int>& ids,
                                  mlx::core::array* outToken,
                                  QString* err,
                                  const mlx::core::array* inputsEmbeds,
                                  const mlx::core::array* tokenIn,
                                  const int* posT,
                                  const int* posH,
                                  const int* posW,
                                  Qwen2KvCache* cache) {
    if (!outToken) {
        if (err)
            *err = QStringLiteral("outToken is null");
        return false;
    }
    if (ids.isEmpty() && !tokenIn && !inputsEmbeds) {
        if (err)
            *err = QStringLiteral("empty ids");
        return false;
    }
    return forwardOfficialLanguageMlx(model, ids, nullptr, nullptr, nullptr, err, nullptr, posT, posH, posW,
                                      cache, inputsEmbeds, tokenIn, outToken);
}
#endif

}  // namespace hybrid
}  // namespace scanengine
