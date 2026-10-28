import json
import csv
import numpy as np
import sys

import serial
from io import StringIO

CSI_DATA_COLUMNS = 64

prev = None

def phase_bar(d, width=41, rng=90):
    d = max(-rng, min(rng, d))
    bar = ["-"] * width
    bar[width // 2] = ":"
    bar[int((d + rng) / (2 * rng) * (width - 1))] = "|"
    return "".join(bar)

ser = serial.Serial("/dev/ttyACM0", 115200, timeout=1)

while True:
    strings = str(ser.readline())
    strings = strings.lstrip('b\'').rstrip('\\r\\n\'')
    if not strings.startswith("CSI_DATA"):
        continue

    csv_reader = csv.reader(StringIO(strings))
    csi_data = next(csv_reader)
    csi_data_len = int (csi_data[-3])

    try:
        csi_raw_data = json.loads(csi_data[-1])
    except ValueError:
        continue

    n = min(csi_data_len // 2, CSI_DATA_COLUMNS, len(csi_raw_data) // 2)

    csi_data_complex = np.array(
        [complex(csi_raw_data[i * 2 + 1], csi_raw_data[i * 2]) for i in range(n)]
    )

    csi_amplitude = np.abs(csi_data_complex)
    csi_phase = np.angle(csi_data_complex)

    csi_amplitude_int = csi_amplitude.astype(int)
    csi_phase_deg = np.degrees(csi_phase).astype(int) # deg: 度表記

    if prev is None or len(prev) != n:
        prev = csi_data_complex
        continue
    diff_deg = np.degrees(np.angle(csi_data_complex * np.conj(prev))).astype(int)
    prev = csi_data_complex

    lines = [f"subcarriers: {n}"]
    for idx, value in enumerate(csi_amplitude_int):
        lines.append(f"{idx:2d} {csi_phase_deg[idx]:4d}゜ {diff_deg[idx]:4d}°  [{phase_bar(diff_deg[idx])}] {value:3d} " + "*" * (value * (value // 4)))

    sys.stdout.write("\033[H\033[J" + "\n".join(lines) + "\n")
    sys.stdout.flush()
