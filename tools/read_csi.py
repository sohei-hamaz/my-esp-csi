import json
import csv
import numpy as np

import serial
from io import StringIO

CSI_DATA_COLUMNS = 64

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

    for value in csi_amplitude_int:
        print(value)
