import json
import csv
import numpy as np

import serial
from io import StringIO

CSI_DATA_INDEX = 200
CSI_DATA_COLUMNS = 64

csi_data_complex = np.zeros([CSI_DATA_INDEX, CSI_DATA_COLUMNS], dtype=np.complex64)

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

    csi_data_complex[:-1] = csi_data_complex[1:]

    for i in range(csi_data_len // 2):
        csi_data_complex[-1][i] = complex(csi_raw_data[i * 2 + 1],
                                          csi_raw_data[i * 2])

    csi_amplitude_array = np.abs(csi_data_complex)
    csi_phase_array = np.angle(csi_data_complex)

    print(csi_amplitude_array.astype(int))
