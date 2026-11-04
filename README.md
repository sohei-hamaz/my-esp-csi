# my-esp-csi
濱崎の研究をまとめたリポジトリ

## csi_recv_ap_demo
ESP32-S3 で CSI を推定するプログラム
MQTT 経由でブローカに推定した CSI を送信する

### Prerequisites
- ESP-IDF v6.0.3 commit ID (76f5dedd9950a3012fee8fb7d5586df21fc67802)

### How to use
1. `cd csi_recv_ap_demo`
2. `idf.py set-target esp32s3`
3. `idf.py menuconfig`
    以下項目を設定
    - Example Connection Configuration -> WiFi SSID
    - Example Connection Configuration -> WiFi Password
    - MQTT Configuration -> MQTT Broker URI
    - MQTT Configuration -> MQTT topic
4. `idf.py flash monitor`

## read_csi.py
MQTT ブローカ経由で CSI を取得した後，振幅や位相を算出し，ターミナル上にグラフとして表示するプログラム

### Prerequisites
- Python 3.14.4

### How to use
1. `cd tools`
2. `python3 -m venv .venv`
3. `source .venv/bin/activate`
4. `pip install -r requirements.txt`
5. `python3 read_csi.py --broker mqtt://<your-broker-IP>`
