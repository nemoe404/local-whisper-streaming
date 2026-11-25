#!/usr/bin/env python3
"""Write raw 16 kHz mono little-endian float32 audio, the input lws-cli reads from stdin.

Usage (run as `python3 -I scripts/make-f32.py ...`; only the standard library is used):

  make-f32.py WAV OUT [--copies N] [--gap-seconds S]
      Convert a 16 kHz mono 16-bit WAV file. Every sample becomes int16 / 32768.0. The audio is
      written N times (default 1), each copy followed by S seconds of zeros (default 0).

  make-f32.py --zeros-seconds S OUT
      Write S seconds of zeros and nothing else.

Exit status: 0 on success, 2 for a bad command line or a WAV that is not 16 kHz, mono and 16-bit.
"""

import array
import struct
import sys
import wave

SAMPLE_RATE = 16000


def fail(message):
    print(f"make-f32: {message}", file=sys.stderr)
    sys.exit(2)


def zeros(seconds):
    return array.array("f", bytes(4 * round(seconds * SAMPLE_RATE)))


def read_wav(path):
    try:
        with wave.open(path, "rb") as clip:
            if clip.getframerate() != SAMPLE_RATE:
                fail(f"{path}: the sample rate is not 16000 Hz")
            if clip.getnchannels() != 1:
                fail(f"{path}: the file is not mono")
            if clip.getsampwidth() != 2:
                fail(f"{path}: the samples are not 16-bit")
            frames = clip.readframes(clip.getnframes())
    except (OSError, wave.Error, EOFError) as error:
        fail(f"{path}: {error}")
    count = len(frames) // 2
    return array.array("f", (value / 32768.0 for value in struct.unpack(f"<{count}h", frames)))


def parse(argv):
    positional = []
    options = {"--copies": "1", "--gap-seconds": "0", "--zeros-seconds": None}
    pending = None
    for word in argv:
        if pending is not None:
            options[pending] = word
            pending = None
        elif word in options:
            pending = word
        elif word.startswith("--"):
            fail(f"unknown option {word}")
        else:
            positional.append(word)
    if pending is not None:
        fail(f"{pending} needs a value")
    try:
        copies = int(options["--copies"])
        gap = float(options["--gap-seconds"])
        only_zeros = None if options["--zeros-seconds"] is None else float(options["--zeros-seconds"])
    except ValueError as error:
        fail(str(error))
    if copies < 1 or gap < 0 or (only_zeros is not None and only_zeros < 0):
        fail("counts and durations must not be negative, and there must be at least one copy")
    return positional, copies, gap, only_zeros


def main(argv):
    positional, copies, gap, only_zeros = parse(argv)
    if only_zeros is not None:
        if len(positional) != 1:
            fail("usage: make-f32.py --zeros-seconds S OUT")
        samples = zeros(only_zeros)
        out = positional[0]
    else:
        if len(positional) != 2:
            fail("usage: make-f32.py WAV OUT [--copies N] [--gap-seconds S]")
        clip = read_wav(positional[0])
        samples = array.array("f")
        for _ in range(copies):
            samples.extend(clip)
            samples.extend(zeros(gap))
        out = positional[1]
    if sys.byteorder != "little":
        samples.byteswap()
    try:
        with open(out, "wb") as handle:
            samples.tofile(handle)
    except OSError as error:
        fail(f"{out}: {error}")


if __name__ == "__main__":
    main(sys.argv[1:])
