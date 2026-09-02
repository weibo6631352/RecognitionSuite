#include "mlx/fast.h"

#include "mlx/ops.h"

namespace mlx::core::fast {

array batch_norm_eval(const array& x,
                      const array& mean,
                      const array& var,
                      const array& weight,
                      const array& bias,
                      float eps,
                      StreamOrDevice s) {
  const auto stream = to_stream(s);
  const auto scale = multiply(weight,
                              rsqrt(add(var, array(eps, var.dtype()), stream),
                                    stream),
                              stream);
  return add(multiply(subtract(x, mean, stream), scale, stream),
             bias,
             stream);
}

}  // namespace mlx::core::fast
