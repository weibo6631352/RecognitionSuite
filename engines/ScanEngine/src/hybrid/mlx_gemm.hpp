#pragma once

#if !defined(SCANENGINE_MLX)
#error "mlx_gemm.hpp requires the certified MLX GPU backend"
#endif

#include <mlx/mlx.h>

#include <cstring>

namespace mx = mlx::core;

namespace scanengine {
namespace hybrid {

// Lazy device graphs: callers evaluate at explicit model boundaries.
inline mx::array gemmNT(const mx::array& a, const mx::array& b) {
    return mx::matmul(a, mx::transpose(b));
}

inline mx::array gemmNN(const mx::array& a, const mx::array& b) {
    return mx::matmul(a, b);
}

// Official mlx.nn.Linear: addmm(bias, x, weight.T) when bias exists.
inline mx::array linearNN(const mx::array& x,
                          const mx::array& weightT,
                          const mx::array* bias) {
    if (bias)
        return mx::addmm(*bias, x, weightT);
    return mx::matmul(x, weightT);
}

inline void copyEval(const mx::array& value, float* dst, size_t count) {
    mx::eval(value);
    std::memcpy(dst, value.data<float>(), count * sizeof(float));
}

}  // namespace hybrid
}  // namespace scanengine
