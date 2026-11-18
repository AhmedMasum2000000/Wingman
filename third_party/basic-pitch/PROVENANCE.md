# Basic Pitch: where these files come from

| | |
|---|---|
| Project | Basic Pitch, by Spotify's Audio Intelligence Lab |
| Repository | https://github.com/spotify/basic-pitch |
| Commit | `fa5997af0a8210982619003269994a1be25eddf3` (`fa5997a`, 2025-11-13, "Update mirdata, add local index fixtures (#185)") |
| Licence | Apache License 2.0 (`LICENSE` here; also `licenses/Apache-2.0.txt` at the root of this repository) |
| Copyright | Copyright 2022 Spotify AB |

## Files

| File here | Path in the upstream repository at `fa5997a` | Bytes | SHA-256 | Changes |
|---|---|---|---|---|
| `nmp.onnx` | `basic_pitch/saved_models/icassp_2022/nmp.onnx` | 230,444 | `2c3c1d144bfa61ad236e92e169c13535c880469a12a047d4e73451f2c059a0ec` | unmodified |
| `LICENSE` | `LICENSE` | 11,384 | `929c910bae2152fa87199a5d0660e09263419b7eee6d4b301d05ee2aaf211c37` | unmodified |
| `NOTICE` | `NOTICE` | 1,034 | `b810e55c0e3b520fabb45fc2ccc74880187bf84e309971968541cc812dcde905` | unmodified |

All three were compared byte for byte with `https://raw.githubusercontent.com/spotify/basic-pitch/fa5997a/<path>`.

## How M1D1 uses them

- `nmp.onnx` is the trained Basic Pitch model ("ICASSP 2022" weights, ONNX export). M1D1 does not change it.
  `web/src/make-plugin-page.py` gzips it, base64-encodes it and embeds it in `web/m1d1.html` (script block
  `bp-model`), after checking the SHA-256 above. The block in `m1d1.html` is generated from this file; this file is
  the form to change, not the base64.
- The model contains a constant-Q transform whose kernels come from nnAudio (MIT, Copyright (c) 2019 KinWaiCheuk);
  see `third_party/nnAudio/LICENSE`. Basic Pitch's `basic_pitch/layers/nnaudio.py` says its layers are ported from
  nnAudio, which is released under an MIT licence.
- The page's note reader, `m1d1-pitch.js` (in `web/m1d1.html` as the `m1d1-pitch-src` block), ports Basic Pitch's
  windowing and stitching from `basic_pitch/inference.py` at this commit (`window_audio_file`, `get_audio_input`,
  `unwrap_output`, `run_inference`). That file keeps the Apache-2.0 header and "Copyright 2022 Spotify AB", and
  states what M1D1 changed.
- The rest of the model's sources (the TensorFlow SavedModel `basic_pitch/saved_models/icassp_2022/nmp/`, the
  model code in `basic_pitch/models.py` and `basic_pitch/layers/`, and the training code in `basic_pitch/train.py`
  and `basic_pitch/data/`) are in the upstream repository at the commit above.

## Training data

Spotify publishes the trained weights in this repository under Apache-2.0 and states no other terms for them.
The training code at this commit prepares five datasets (`basic_pitch/data/datasets/`): GuitarSet, iKala, MAESTRO,
MedleyDB-Pitch and Slakh. Some of these datasets are published under their own terms, and some of those terms
are non-commercial or for research only. M1D1 relies on the Apache-2.0 licence of the weights; it does not
redistribute any training data. This is a note on provenance, not legal advice.

## Notes

- Upstream's README, in its "Copyright and License" section, mentions "the GPL License" in a garbled sentence. The
  `LICENSE` file and the package metadata (`pyproject.toml`: `license = { file = "LICENSE" }`) say Apache-2.0 only.
- "Spotify" and "Basic Pitch" are named here only to say where the files come from. They are not M1D1's branding.
