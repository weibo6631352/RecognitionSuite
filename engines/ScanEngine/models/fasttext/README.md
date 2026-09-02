# fast-langdetect low-memory language model

`lid.176.ftz` is the compressed 176-language fastText identification model
shipped by `fast-langdetect==0.2.5`.  ScanEngine uses this exact package payload
for MinerU Hybrid paragraph-joining parity; it does not download a model at
runtime.

## Content lock

- Package payload path:
  `fast_langdetect/ft_detect/resources/lid.176.ftz`
- Model size: `938,013` bytes
- Model SHA-256:
  `8f3472cfe8738a7b6099e8e999c3cbfae0dcd15696aac7d7738a8039db603e83`
- Package notice size: `340` bytes
- Package notice SHA-256:
  `6f60e42631e1be07ad7b6220c90041fda419d0c81f0c129013c54f7c716c21ea`
- License legal-code size: `22,240` bytes
- License legal-code SHA-256:
  `3f941b3b89cf7b8370ceb83cc76d2120d471b58735d8ca60238a751a48d7f72f`
- fastText v0.9.2 MIT license: `1,080` bytes; SHA-256
  `d9cfdfdf548c48712f05c67eb76c79e7c9f45cba78db50794b780e3564b85ef7`
- fast-langdetect 0.2.5 MIT license: `1,065` bytes; SHA-256
  `f2edbc2b5b18529f14065ec9729aad3093efb38a693eed4606b972b8ee451fa7`
- bundled fasttext-langdetect component MIT license: `1,070` bytes; SHA-256
  `a088ca936d559ce90c4f03bfc78b7bf01acbead3ddd2b7cdfc8c2d23739dad86`
- fast-langdetect 0.2.5 notice: `186` bytes; SHA-256
  `d6262477338a8bd337571c3c71d30f7ab743c3746c52e7036f17310c4ef84194`

The work is the **Language identification model `lid.176.ftz`**, created and
published by Facebook Research fastText at
`https://fasttext.cc/docs/en/language-identification.html`.  The package bytes
are copied unchanged; ScanEngine makes no model modification.  The model is
distributed under Creative Commons Attribution-ShareAlike 3.0 Unported, as
recorded by the upstream package in `NOTICE.MD`.  The complete offline-readable
license text is preserved in `LICENSE.CC-BY-SA-3.0.txt`; it was retrieved from
the official Creative Commons legal-code endpoint:
`https://creativecommons.org/licenses/by-sa/3.0/legalcode.txt`.

The C++ inference source has a separate MIT license and is preserved under
`third_party/common/fasttext-0.9.2/`.  Standalone product deployments retain
that text as `LICENSE.fastText-MIT.txt`.  The fast-langdetect 0.2.5 MIT license
and its provenance notice are retained as `LICENSE.fast-langdetect-MIT.txt`
and `NOTICE.fast-langdetect.md`; the nested fasttext-langdetect component's
Zafer Çavdar MIT text is retained as `LICENSE.fasttext-langdetect-MIT.txt`.
The native boundary deliberately reproduces preprocessing and postprocessing
behavior from these components.
