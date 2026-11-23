# Audio fixtures

## clean_en_sample.wav / clean_en_sample.ref.txt

- Source: LibriSpeech `dev-clean`, speaker 2277, chapter 149896, utterance 0032 (`http://www.openslr.org/12/`).
- License: CC BY 4.0. Attribution: Panayotov, Chen, Povey, Khudanpur, "LibriSpeech: an ASR corpus based on public domain audio books", ICASSP 2015.
- Copied from the consigliere project's `tests/fixtures/audio/` (same bytes).
- `.ref.txt` is the raw transcript of the utterance.

## clean_en_sample.silero-ref.txt

- Reference per-frame speech probabilities for `clean_en_sample.wav` from the Silero VAD v5.1.2 ONNX model through
  onnxruntime: 336 whole frames of 512 samples, a 64-sample previous-frame context, the LSTM state carried. The 288
  tail samples are not scored.
- Generator: `scripts/silero_reference_probs.py`. Command:
  `.venv/bin/python -I scripts/silero_reference_probs.py tests/fixtures/audio/clean_en_sample.wav --context previous > tests/fixtures/audio/clean_en_sample.silero-ref.txt`
- Versions and hashes (also in the file header): silero-vad 5.1.2, onnxruntime 1.32.0, numpy 2.5.5,
  `silero_vad.onnx` sha256 `2623a2953f6ff3d2c1e61740c6cdb7168133479b267dfef114a4a3cc5bdd788f`, wav sha256
  `fd1ef8864b5fdf0f75cc7d33ec93266709843447183f5883e98dda8686110821`.
- It is regenerated only on a machine with the local venv (onnxruntime and silero-vad 5.1.2, no torch). Tests and CI
  read the committed text and never run Python.
