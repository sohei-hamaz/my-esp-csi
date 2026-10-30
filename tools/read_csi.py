import argparse
import csv
import json
import logging
from io import StringIO
from urllib.parse import urlparse

import numpy as np
import paho.mqtt.client as mqtt

CSI_DATA_COLUMNS = 64

prev = None


def phase_bar(d, width=41, rng=90):
    d = max(-rng, min(rng, d))
    bar = ["-"] * width
    bar[width // 2] = ":"
    bar[int((d + rng) / (2 * rng) * (width - 1))] = "|"
    return "".join(bar)


def display_csi_data(payload):
    global prev

    try:
        csi_data = next(csv.reader(StringIO(payload)))
        if not csi_data or csi_data[0] != "CSI_DATA":
            return
        csi_data_len = int(csi_data[-3])
        csi_raw_data = json.loads(csi_data[-1])
    except (StopIteration, ValueError, IndexError, csv.Error):
        logging.warning("Ignoring malformed CSI MQTT message")
        return

    if csi_data_len != 128:
        return

    n = min(csi_data_len // 2, CSI_DATA_COLUMNS, len(csi_raw_data) // 2)

    csi_data_complex = np.array(
        [complex(csi_raw_data[i * 2 + 1], csi_raw_data[i * 2]) for i in range(n)]
    )

    csi_amplitude = np.abs(csi_data_complex)
    csi_phase = np.angle(csi_data_complex)

    csi_amplitude_int = csi_amplitude.astype(int)
    csi_phase_deg = np.degrees(csi_phase).astype(int)

    if prev is None or len(prev) != n:
        prev = csi_data_complex
        return

    diff = np.angle(csi_data_complex * np.conj(prev))
    valid = (csi_amplitude > 0) & (np.abs(prev) > 0)
    prev = csi_data_complex

    diff_deg = np.zeros(n, dtype=int)
    if valid.sum() > 4:
        k = np.arange(n)
        ph = np.unwrap(diff[valid])
        slope, intercept = np.polyfit(k[valid], ph, 1)
        diff_deg[valid] = np.degrees(ph - (slope * k[valid] + intercept)).astype(int)

    lines = [f"subcarriers: {n}"]
    for idx, value in enumerate(csi_amplitude_int):
        lines.append(
            f"{idx:2d} {csi_phase_deg[idx]:4d}゜ {diff_deg[idx]:4d}°  "
            f"[{phase_bar(diff_deg[idx])}] {value:3d} "
            + "*" * (value * (value // 5))
        )

    print("\033[H\033[J" + "\n".join(lines), flush=True)


def on_connect(client, userdata, flags, reason_code):
    if reason_code != 0:
        logging.error("MQTT connection failed with status %s", reason_code)
        return

    topic = userdata["topic"]
    result, _ = client.subscribe(topic)
    if result != mqtt.MQTT_ERR_SUCCESS:
        logging.error("Failed to subscribe to MQTT topic %s", topic)
        return
    logging.info("Connected to MQTT broker and subscribed to %s", topic)


def on_message(client, userdata, message):
    try:
        payload = message.payload.decode("utf-8")
    except UnicodeDecodeError:
        logging.warning("Ignoring non-UTF-8 MQTT message")
        return

    display_csi_data(payload)


def main():
    parser = argparse.ArgumentParser(description="Receive and display CSI data over MQTT.")
    parser.add_argument(
        "--broker",
        default="mqtt://192.168.1.100",
        help="MQTT broker URI (default: mqtt://192.168.1.100)",
    )
    parser.add_argument("--topic", default="csi/data", help="MQTT topic (default: csi/data)")
    args = parser.parse_args()

    broker = urlparse(args.broker)
    if broker.scheme != "mqtt" or not broker.hostname or broker.path not in ("", "/"):
        parser.error("--broker must be an mqtt:// URI with a hostname and no path")
    try:
        broker_port = broker.port or 1883
    except ValueError as error:
        parser.error(f"invalid MQTT broker port: {error}")

    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    if hasattr(mqtt, "CallbackAPIVersion"):
        client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION1)
    else:
        client = mqtt.Client()
    client.user_data_set({"topic": args.topic})
    client.on_connect = on_connect
    client.on_message = on_message

    if broker.username:
        client.username_pw_set(broker.username, broker.password)

    client.connect(broker.hostname, broker_port)
    client.loop_forever()


if __name__ == "__main__":
    main()
