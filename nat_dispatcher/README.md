# nat_lan
Wi-Fi AP と PC を有線接続すると NAT 機能を自動で有効化，切断すると無効化するディスパッチャ

## How to use
1. Wi-Fi AP と PC を有線接続する
2. PC の有線側インタフェース名を確認し，`LAN_IF` に設定する
    - ```
      LAN_IF=$(nmcli -t -f DEVICE,TYPE device status |
        awk -F: '$2 == "ethernet" {print $1; exit}')
3. `LAN_IF` への接続設定を `nat-lan` という名前で作成し，PC の有線側インタフェースの IP アドレスを AP がデフォルトゲートウェイとして使う IP アドレスに設定する
    - ```
      sudo nmcli connection add \
        type ethernet \
        ifname "$LAN_IF" \
        con-name nat-lan \
        ipv4.method manual \
        ipv4.addresses <IP address>/<prefix length> \
        ipv4.gateway "" \
        ipv4.never-default yes \
        ipv6.method disabled \
        connection.autoconnect yes
4. ディスパッチャを配置する
   - `sudo cp nat_lan /etc/NetworkManager/dispatcher.d/90-nat`
5. 実行権限と所有者を設定する
   - `sudo chmod 755 /etc/NetworkManager/dispatcher.d/90-nat`
   - `sudo chown root:root /etc/NetworkManager/dispatcher.d/90-nat`
