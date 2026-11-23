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

The n_context difference was measured on 2026-11-23. The fork reflect-pads each frame and carries no previous-frame
context; the reference prepends the previous 64 samples. Over the 336 frames the maximum difference is 0.2007 (frame
216) and the mean 0.0021; the single segment's start and end shift by 0 frames, and the one threshold flip (frame 272)
lies within a frame of a reference edge. The decision is accept: the fork is unchanged. The reflect-context companion
`clean_en_sample.silero-ref-reflect.txt` (same generator, `--context reflect`) agrees with the fork to 0.0006, which
attributes the difference to the left context alone. `VadReferenceModel.AcceptedBoundsHold` pins the decision.

## clean_en_sample.golden.txt

- Golden segment boundaries for a two-copy stream: `clean_en_sample.wav`, 64 frames of zeros (32768 samples, 2.048 s),
  `clean_en_sample.wav` again, 64 frames of zeros; 410176 samples in all. The second copy starts at sample 205088, which
  is not on the 512-sample frame grid, so the stream also carries a partial frame across real audio.
- Produced by `SegmentationModel.GoldenBoundaries`: `VadEngine::open` on Silero v5.1.2, `Segmenter` with default
  `Options`, the stream pushed in 512-sample chunks, then flush. One line per segment: start sample, sample count, close
  reason, `continues_previous`, `discontinuous_before`. The header names the model and fork versions it was made with.
- It is a characterization record made by the code under test: it pins today's result against regressions and anchors
  the chunk-size comparison. Correctness is not read from it. `SegmentationModel.SegmentsCoverReferenceSpeechAndSkipSilence`
  checks the same segments against `clean_en_sample.silero-ref.txt`, made by the ONNX model, which the code under test
  did not produce: each copy's reference speech span is covered and the silence between and after the copies is not.
- Regenerate it only after a deliberate change to the engine, the fork or the windower: run the golden test, read the
  computed lines it prints, confirm the cover test still passes, then replace the data lines and update the header.
