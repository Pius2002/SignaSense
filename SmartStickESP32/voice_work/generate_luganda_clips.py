from pathlib import Path
import subprocess

import imageio_ffmpeg


ROOT = Path(__file__).resolve().parents[2]
RECORDING_DIR = ROOT / "Luganda Recordings"
SKETCH_DIR = ROOT / "SmartStickESP32"
OUT_HEADER = SKETCH_DIR / "LugandaClips.h"
WORK_DIR = SKETCH_DIR / "voice_work" / "luganda_preview"

CLIPS = [
    (1, "luganda_connected", "LUGANDA_CONNECTED"),
    (2, "luganda_stop_now", "LUGANDA_STOP_NOW"),
    (3, "luganda_obstacle_ahead", "LUGANDA_OBSTACLE_AHEAD"),
    (4, "luganda_obstacle_ahead_move_carefully", "LUGANDA_OBSTACLE_AHEAD_MOVE_CAREFULLY"),
    (5, "luganda_scan_left_slowly", "LUGANDA_SCAN_LEFT_SLOWLY"),
    (6, "luganda_scan_right_slowly", "LUGANDA_SCAN_RIGHT_SLOWLY"),
    (7, "luganda_left_seems_clearer", "LUGANDA_LEFT_SEEMS_CLEARER"),
    (8, "luganda_right_seems_clearer", "LUGANDA_RIGHT_SEEMS_CLEARER"),
    (9, "luganda_no_clear_side", "LUGANDA_NO_CLEAR_SIDE"),
    (10, "luganda_path_clear", "LUGANDA_PATH_CLEAR"),
]


def convert_to_mulaw(ffmpeg: str, source: Path, target: Path) -> bytes:
    target.parent.mkdir(parents=True, exist_ok=True)
    # Do not trim or remove silence. These filters keep the full recording length
    # while reducing rumble/noise and raising spoken voice loudness.
    voice_filter = (
        "highpass=f=90,"
        "lowpass=f=3700,"
        "afftdn=nf=-25,"
        "dynaudnorm=f=150:g=17:p=0.95,"
        "volume=1.35,"
        "alimiter=limit=0.94"
    )
    cmd = [
        ffmpeg,
        "-y",
        "-i",
        str(source),
        "-vn",
        "-ac",
        "1",
        "-ar",
        "8000",
        "-af",
        voice_filter,
        "-f",
        "mulaw",
        str(target),
    ]
    subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    return target.read_bytes()


def format_array(name: str, data: bytes) -> str:
    lines = [f"const uint8_t {name}_data[] PROGMEM = {{"]
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        rendered = ", ".join(f"0x{byte:02X}" for byte in chunk)
        lines.append(f"  {rendered},")
    lines.append("};")
    return "\n".join(lines)


def main() -> None:
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    arrays = []
    table_rows = []
    enum_names = []
    total_bytes = 0

    for number, data_name, enum_name in CLIPS:
        source = RECORDING_DIR / f"{number}.m4a"
        if not source.exists():
            raise FileNotFoundError(source)

        raw = convert_to_mulaw(ffmpeg, source, WORK_DIR / f"{number}.mulaw")
        arrays.append(format_array(data_name, raw))
        table_rows.append(f"  {{{data_name}_data, {len(raw)}}},")
        enum_names.append(enum_name)
        total_bytes += len(raw)
        print(f"{number}.m4a -> {len(raw)} bytes ({len(raw) / 8000:.2f} s)")

    header = [
        "#pragma once",
        "#include <Arduino.h>",
        "",
        "// Generated from D:/desktop/Glove/Luganda Recordings/*.m4a",
        "// Format: 8 kHz, mono, mu-law bytes stored in PROGMEM.",
        "",
        "enum LugandaClipId {",
    ]
    header.extend(f"  {name}," for name in enum_names)
    header.append("  LUGANDA_CLIP_COUNT")
    header.append("};")
    header.append("")
    header.extend(arrays)
    header.append("")
    header.append("const SpeechClip LUGANDA_CLIPS[LUGANDA_CLIP_COUNT] = {")
    header.extend(table_rows)
    header.append("};")
    header.append("")
    header.append(f"const uint32_t LUGANDA_TOTAL_BYTES = {total_bytes};")
    header.append("")

    OUT_HEADER.write_text("\n".join(header), encoding="utf-8")
    print(f"total bytes {total_bytes}")
    print(f"wrote {OUT_HEADER}")


if __name__ == "__main__":
    main()
