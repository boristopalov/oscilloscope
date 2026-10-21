#!/usr/bin/env python3
"""Create repeatable 48 kHz stereo fixtures for the manual Live experiments."""
import argparse
import array
import json
import math
from pathlib import Path
import sys
import wave


def write_stereo(path, samples):
    data = array.array("h", (value for sample in samples for value in (sample, sample)))
    if sys.byteorder != "little":
        data.byteswap()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(48000)
        output.writeframes(data.tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    rate, seconds = 48000, 8
    impulses = [0] * (rate * seconds)
    positions = list(range(4800, rate * seconds, rate))
    for position in positions:
        impulses[position] = 24576
    write_stereo(args.directory / "impulses-48k.wav", impulses)
    write_stereo(args.directory / "sine-48k.wav",
                 (round(8192 * math.sin(2 * math.pi * 440 * n / rate)) for n in range(rate * seconds)))
    (args.directory / "fixtures.json").write_text(json.dumps({
        "sample_rate": rate, "channels": 2, "length_samples": rate * seconds,
        "impulse_sample_positions": positions, "impulse_amplitude": 0.75,
        "sine_frequency_hz": 440, "sine_amplitude": 0.25,
        "live_setup": "Disable Warp, fades, clip gain changes, and transport looping."
    }, indent=2) + "\n")
    print(f"Created fixtures in {args.directory}")


if __name__ == "__main__":
    main()
