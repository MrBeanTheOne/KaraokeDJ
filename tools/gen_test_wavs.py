"""Generate stereo test tones for the playback harness."""
import math
import os
import struct
import wave

OUT = os.path.join(os.path.dirname(__file__), "..", "tests", "media-fixtures")


def tone(name, freq, secs=12, rate=48000, amp=0.25):
    os.makedirs(OUT, exist_ok=True)
    with wave.open(os.path.join(OUT, name), "w") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(secs * rate):
            v = int(amp * 32767 * math.sin(2 * math.pi * freq * i / rate))
            frames += struct.pack("<hh", v, v)
        w.writeframes(bytes(frames))


tone("tone_a.wav", 440)
tone("tone_b.wav", 660)
tone("tone_short_a.wav", 523, secs=4)
tone("tone_short_b.wav", 784, secs=4)
print("wrote tone_a/b.wav (12s), tone_short_a/b.wav (4s)")
