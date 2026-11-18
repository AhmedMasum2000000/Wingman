# ONNX Runtime Web: where these files come from

| | |
|---|---|
| Project | ONNX Runtime Web (onnxruntime-web), by Microsoft |
| Package | npm `onnxruntime-web@1.20.1` |
| Tarball | https://registry.npmjs.org/onnxruntime-web/-/onnxruntime-web-1.20.1.tgz |
| npm integrity | `sha512-TePF6XVpLL1rWVMIl5Y9ACBQcyCNFThZON/jgElNd9Txb73CIEGlklhYR3UEr1cp5r0rbGI6nDwwrs79g7WjoA==` |
| npm shasum (SHA-1 of the tarball) | `71dc4495bb75cdd56a9dc981cf27723a599189f7` |
| Source | https://github.com/microsoft/onnxruntime, tag `v1.20.1` |
| Licence | MIT, "Copyright (c) Microsoft Corporation" (`LICENSE` here) |

The integrity string is the one the npm registry publishes for 1.20.1 and the one in the lockfile that installed it.
The tarball was downloaded again and its SHA-512 matched.

## Files

The three runtime files are copied unchanged from the tarball's `package/dist/` folder:

| File here | In the npm package | Bytes | SHA-256 | Changes |
|---|---|---|---|---|
| `ort.wasm.min.js` | `dist/ort.wasm.min.js` | 49,026 | `e66568724f8848e57cc2a56e4bea3a5b86ce3ff81b2da11eac8ed7ab02b27bd4` | unmodified |
| `ort-wasm-simd-threaded.mjs` | `dist/ort-wasm-simd-threaded.mjs` | 24,618 | `745eb7c0ce6f18a6aa521971b2877babc7ffb27eecb58ab3bc6e5ef4692672e8` | unmodified |
| `ort-wasm-simd-threaded.wasm` | `dist/ort-wasm-simd-threaded.wasm` | 11,246,032 | `207d02be4591c156b0a98f024f3d58005b5b04c92274d759fb390338c63559ea` | unmodified |

The npm package contains no licence or notice file, so these two come from the source repository's root at tag
`v1.20.1`:

| File here | In microsoft/onnxruntime at `v1.20.1` | Bytes | SHA-256 |
|---|---|---|---|
| `LICENSE` | `LICENSE` | 1,073 | `2f07c72751aed99790b8a4869cf2311df85a860b22ded05fa22803587a48922c` |
| `ThirdPartyNotices.txt` | `ThirdPartyNotices.txt` | 338,538 | `cf7342f7ba482ef715ae58f5f497a8d3564fa255164175aea324cd293c5701a0` |

Both were compared byte for byte with the files at the tag (through `https://cdn.jsdelivr.net/gh/microsoft/onnxruntime@v1.20.1/`)
and with `onnxruntime/LICENSE` and `onnxruntime/ThirdPartyNotices.txt` in the official PyPI wheel
`onnxruntime-1.20.1-cp311-cp311-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl`
(SHA-256 `5eec64c0269dcdb8d9a9a53dc4d64f87b9e0c19801d9321246a53b7eb5a7d1bc`). Later versions ship a different
`ThirdPartyNotices.txt` (1.32.0's differs), so keep these two files with this version.

## What the files are, and their source

- **`ort.wasm.min.js`**: the minified JavaScript API (the WebAssembly-only build). It starts with a `/*! ... */`
  banner of three lines: "ONNX Runtime Web v1.20.1", "Copyright (c) Microsoft Corporation. All rights reserved.",
  "Licensed under the MIT License.". Its source map (`dist/ort.wasm.min.js.map`, in the npm package) lists only
  ONNX Runtime's own TypeScript: 21 files from `js/common/lib/` and 15 from `js/web/lib/` (the WebAssembly backend,
  its proxy worker, `index.ts` and `version.ts`). It bundles no other npm package.
- **`ort-wasm-simd-threaded.mjs`**: the loader that Emscripten generates for the WebAssembly module.
- **`ort-wasm-simd-threaded.wasm`**: ONNX Runtime's C++ inference engine (with MLAS, its CPU kernels and its
  contributed operators) compiled to WebAssembly with SIMD and threads by Emscripten. In v1.20.1,
  `tools/ci_build/build.py` installs emsdk `3.1.59` by default (`--emsdk_version`). The C++ source is
  `onnxruntime/` and `include/` at the tag; the build script is `tools/ci_build/build.py` (`--build_wasm`). This
  repository does not record the exact flags of Microsoft's release build.

The `.wasm` also contains these third-party libraries. They were identified from the source paths and symbol names
left in the binary (for example `_deps/re2-src/re2/dfa.cc`, `EigenNonBlockingThreadPool.h`, `nlohmann::detail`,
`gsl::span`). Versions are the ones pinned in `cmake/deps.txt` at the tag, which also gives each archive's URL and SHA-1.

| Library | Version (`cmake/deps.txt`, v1.20.1) | Licence | Licence text |
|---|---|---|---|
| Eigen | GitLab commit `e7248b26a1ed53fa030c5c459f7ea095dfd276ac` | MPL-2.0 | `ThirdPartyNotices.txt`, "Eigen" |
| Protocol Buffers (lite) | `v21.12` | BSD-3-Clause | "protocolbuffers/protobuf" |
| Abseil | `20240722.0` | Apache-2.0 | "abseil-cpp" |
| ONNX | `v1.16.1` | Apache-2.0 | "onnx" |
| RE2 | `2024-07-02` | BSD-3-Clause | "google/re2" |
| FlatBuffers | `v23.5.26` | Apache-2.0 | "google/flatbuffers" |
| SafeInt | `3.0.28` | MIT | "dcleblanc/SafeInt" |
| nlohmann/json | `v3.10.5` | MIT | "nlohmann/json" |
| Microsoft GSL | `v4.0.0` | MIT | "Microsoft GSL" |
| Boost.Mp11 (header-only) | `boost-1.82.0` | BSL-1.0 | "Boost" |
| HowardHinnant/date (header-only) | `v3.0.1` | MIT | "HowardHinnant/date" |
| dlib (matrix code) | not pinned in `deps.txt` | BSL-1.0 | not named; the Boost Software License text is under "Boost" |
| Emscripten runtime (emsdk) | `3.1.59` by default | MIT (emsdk); Emscripten's system libraries carry their own permissive licences | "emsdk" |

Boost.Mp11 and date are header-only libraries that ONNX Runtime's core compiles in; they leave no strings in the
binary, so their presence there is inferred, not seen. XNNPACK, cpuinfo and pthreadpool are named in `deps.txt`
but their code is not in this `.wasm` (only the execution-provider name "XNNPACK" appears as text).

Where the source of each library is:

- Eigen: https://gitlab.com/libeigen/eigen/-/archive/e7248b26a1ed53fa030c5c459f7ea095dfd276ac/eigen-e7248b26a1ed53fa030c5c459f7ea095dfd276ac.zip
  (SHA-1 `be8be39fdbc6e60e94fa7870b280707069b5b81a`)
- the others: the archive URLs in `cmake/deps.txt` at https://github.com/microsoft/onnxruntime/blob/v1.20.1/cmake/deps.txt

## How M1D1 uses them

`web/src/make-plugin-page.py` gzips each runtime file, base64-encodes it and embeds it in `web/m1d1.html` (script
blocks `bp-ort-js`, `bp-ort-mjs` and `bp-ort-wasm`), after checking the SHA-256 values above. The page loads the
runtime in a Web Worker from blob URLs, with one thread. The blocks in `m1d1.html` are generated from these files;
change these files, not the base64. The licence banner of `ort.wasm.min.js` stays inside the embedded copy because
the file is embedded unchanged.

Keep the version pinned to exactly 1.20.1: the page sets `env.wasm.wasmPaths` to blob URLs, and that way of loading
the `.wasm` depends on this version's loader.
