import json
import csv
import numpy as np
import serial

CSI_DATA_INDEX = 200
CSI_DATA_COLUMNS = 490

csi_data_complex = np.zeros([CSI_DATA_INDEX, CSI_DATA_COLUMNS], dtype=np.complex64)

ser = serial.Serial("/dev/ttyACM0", 115200, timeout=1)

while True:
    line = str(ser.readline())
    line = line.lstrip('b\'').rstrip('\\r\\n\'')
    if not line.startswith("CSI_DATA"):
        continue

    try:
        first_csi_data = line.index("[")
        csi_raw_data = json.loads(line[first_csi_data:].rstrip('"'))
    except ValueError:
        continue

    print("csi_raw_data:", csi_raw_data)
