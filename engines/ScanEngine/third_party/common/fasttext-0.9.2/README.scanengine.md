# fastText v0.9.2 inference source subset

This directory contains the unmodified upstream C++ translation units needed
to load and run the quantized `lid.176.ftz` language-identification model.  The
same sources are compiled into a static library on Windows and macOS; no
Python extension or host fastText installation is used at runtime.

## Upstream lock

- Project: Facebook Research fastText
- Version/tag: `v0.9.2` (`5b5943c`)
- Release: `https://github.com/facebookresearch/fastText/releases/tag/v0.9.2`
- Source archive:
  `https://codeload.github.com/facebookresearch/fastText/zip/refs/tags/v0.9.2`
- Archive size: `4,369,852` bytes
- Archive SHA-256:
  `93cc8a21633e6c6ecf4b5164925ad68a5eda332373d4316d2f56d05f4bfbe823`

`SOURCE.MANIFEST.sha256` records every copied upstream file.  The project
build deliberately excludes the command-line entry point and autotune source;
the retained upstream implementation covers model parsing, the compressed
matrix/product-quantizer path, dictionary subwords, supervised prediction, and
the matrix/loss classes referenced by `FastText::loadModel`.

## Local integration boundary

The upstream files under `src/` are byte-for-byte copies and are not patched.
Project-specific model discovery, SHA-256 enforcement, Unicode preprocessing,
and the fast-langdetect compatibility rules live outside this directory in
`src/hybrid/official_language_detection.cpp`.

On Windows the static library inherits the repository-wide v143 dynamic CRT
selection (`/MD` in Release and `/MDd` in Debug).  macOS builds the identical
source set.  Prediction is CPU-only on both platforms because this compact
fastText graph has no GPU execution path or external numerical dependency.

## License

fastText is MIT-licensed.  The complete upstream license is preserved in
`LICENSE`.  The separately distributed language model and its CC BY-SA 3.0
license material are preserved under `models/fasttext/`.
