"""Per-frame speech probabilities from Silero VAD v5.1.2, run through onnxruntime.

This is the reference the fork's probabilities are compared against. It re-implements the call
wrapper of silero-vad 5.1.2 (512 samples per frame at 16 kHz, a 64-sample context in front of the
frame, the LSTM state carried) with numpy and onnxruntime only. It never imports torch or the
silero_vad package; it only locates the ONNX file that package ships.

It needs a local virtual environment holding onnxruntime and silero-vad==5.1.2 (no torch). Run it
with isolated mode so nothing from the current directory is imported:

    .venv/bin/python -I scripts/silero_reference_probs.py WAV [--context previous|reflect]

Output goes to stdout: header lines starting with '#', then one "<frame> <probability>" line
per whole frame. A short tail is not scored. Tests and CI read the committed output and never run
this script.

Exit codes: 2 for an unreadable or unsupported WAV, 3 for a missing or wrong reference runtime.
"""

import argparse
import hashlib
import sys
import wave

SAMPLE_RATE = 16000
FRAME = 512
CONTEXT = 64
REQUIRED_VERSION = "5.1.2"
RUNTIME_HINT = (
    "the reference needs a local venv with onnxruntime and silero-vad==5.1.2 and no torch"
)


def fail(code, message):
    print(f"silero_reference_probs: {message}", file=sys.stderr)
    sys.exit(code)


def locate_model():
    """Return (path, onnxruntime module, numpy module, versions) without importing silero_vad."""
    import importlib.metadata as metadata

    try:
        distribution = metadata.distribution("silero-vad")
    except metadata.PackageNotFoundError:
        fail(3, f"silero-vad is not installed; {RUNTIME_HINT}")
    if distribution.version != REQUIRED_VERSION:
        fail(3, f"silero-vad {distribution.version} found; {RUNTIME_HINT}")
    try:
        import numpy
        import onnxruntime
    except ImportError as error:
        fail(3, f"{error}; {RUNTIME_HINT}")
    path = distribution.locate_file("silero_vad/data/silero_vad.onnx")
    return path, onnxruntime, numpy


def read_wav(path, numpy):
    try:
        with wave.open(str(path), "rb") as handle:
            channels = handle.getnchannels()
            width = handle.getsampwidth()
            rate = handle.getframerate()
            raw = handle.readframes(handle.getnframes())
    except (OSError, EOFError, wave.Error) as error:
        fail(2, f"cannot read {path}: {error}")
    if channels != 1 or width != 2 or rate != SAMPLE_RATE:
        fail(2, f"{path} must be 1 channel, 2-byte samples, {SAMPLE_RATE} Hz")
    return numpy.frombuffer(raw, dtype="<i2").astype(numpy.float32) / 32768.0


def sha256_of(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def probabilities(samples, session, numpy, context_mode):
    frames = len(samples) // FRAME
    state = numpy.zeros((2, 1, 128), dtype=numpy.float32)
    sr = numpy.array(SAMPLE_RATE, dtype=numpy.int64)
    context = numpy.zeros(CONTEXT, dtype=numpy.float32)
    values = []
    for index in range(frames):
        frame = samples[index * FRAME : (index + 1) * FRAME]
        if context_mode == "reflect":
            context = frame[CONTEXT:0:-1]
        data = numpy.concatenate([context, frame]).reshape(1, FRAME + CONTEXT)
        output, state = session.run(None, {"input": data, "state": state, "sr": sr})
        values.append(numpy.float32(output[0][0]))
        if context_mode == "previous":
            context = data[0, -CONTEXT:]
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("wav")
    parser.add_argument("--context", choices=["previous", "reflect"], default="previous")
    args = parser.parse_args()

    model_path, onnxruntime, numpy = locate_model()
    samples = read_wav(args.wav, numpy)

    options = onnxruntime.SessionOptions()
    options.intra_op_num_threads = 1
    options.inter_op_num_threads = 1
    session = onnxruntime.InferenceSession(
        str(model_path), sess_options=options, providers=["CPUExecutionProvider"]
    )
    values = probabilities(samples, session, numpy, args.context)

    wav_name = args.wav.replace("\\", "/").rsplit("/", 1)[-1]
    print("# silero_reference_probs.py: per-frame speech probability, Silero v5.1.2 ONNX through onnxruntime")
    print(
        f"# silero-vad {REQUIRED_VERSION}; onnxruntime {onnxruntime.__version__}; "
        f"numpy {numpy.__version__}; silero_vad.onnx sha256 {sha256_of(model_path)}"
    )
    print(f"# wav {wav_name} sha256 {sha256_of(args.wav)}; {SAMPLE_RATE} Hz mono; int16 / 32768")
    print(
        f"# context {args.context} ({CONTEXT} samples, zeros after reset); frames {len(values)}; "
        f"tail samples not scored {len(samples) - len(values) * FRAME}"
    )
    print("# frame probability")
    for index, value in enumerate(values):
        print(f"{index} {float(value):.9g}")


if __name__ == "__main__":
    main()
