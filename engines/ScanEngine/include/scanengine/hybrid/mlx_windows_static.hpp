#pragma once

// mlx/api.h maps MLX_API to __declspec(dllimport) unless MLX_STATIC is set.
// The Windows product links the static mlx.lib through
// scanengine_mlx_cuda_bridge. Compiling these TUs without MLX_STATIC makes
// inline methods such as mlx::core::array::status() emit
// __imp_?status@array@core@mlx@@... and fail LNK2019 at ScanEngineTool.
#if defined(_WIN32) && defined(SCANENGINE_MLX) && !defined(MLX_STATIC)
#error "Windows MLX CUDA TUs must compile with MLX_STATIC"
#endif
