#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "nvs_flash.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"

#define CONFIG_LESS_INTERFERENCE_CHANNEL    11

#define CONFIG_WIFI_BANDWIDTH               WIFI_BW40

#define CONFIG_ESP_NOW_PHYMODE              WIFI_PHY_MODE_HT40
#define CONFIG_ESP_NOW_RATE                 WIFI_PHY_RATE_MCS0_LGI
#define CONFIG_SEND_FREQUENCY               3

static const uint8_t CONFIG_CSI_SEND_MAC[] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x00};
static const char *TAG = "csi_send";

/*  wifi_init:
    Wi-Fi 機能の初期化を行い，ESP-NOW で通信するための設定を行う関数

    1.  イベントループを作成し，Wi-Fi の接続・切断などのイベントを処理できるようにする
    2.  ネットワークインタフェースを初期化する
    3.  Wi-Fi ドライバのデフォルト設定を作成し，初期化する
    4.  Wi-Fi の動作モードを STA モードに設定する
    5.  Wi-Fi 設定の保存先ストレージを RAM に設定する
    6.  Wi-Fi 通信の帯域幅を 40MHz に設定する
    7.  Wi-Fi ドライバを開始して，Wi-Fi 通信が可能な状態にする
    8.  Wi-Fi の省電力機能 (Power Save) を無効にする
    9.  Wi-Fi チャネルとセカンダリチャネルを設定する
    10. STA インタフェースの MAC アドレスを CONFIG_CSI_SEND_MAC に設定する
*/
static void wifi_init()
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(ESP_MAC_WIFI_STA, CONFIG_WIFI_BANDWIDTH));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_LESS_INTERFERENCE_CHANNEL, WIFI_SECOND_CHAN_BELOW));

    ESP_ERROR_CHECK(esp_wifi_set_mac(WIFI_IF_STA, CONFIG_CSI_SEND_MAC));
}

/*  wifi_esp_now_init:
    ESP-NOW 機能の初期化を行う関数

    1. ESP-NOW 機能を初期化する
    2. ESP-NOW で暗号化通信を行うための鍵 (Primary Master Key) を設定する
    3. ESP-NOW の通信方式・通信レートの条件を設定する
    4. 通信相手 (peer) の情報を ESP-NOW に登録する
    5. 登録した peer に設定した通信方式・通信レートを適用する
*/
static void wifi_esp_now_init(esp_now_peer_info_t peer)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));
    esp_now_rate_config_t rate_config = {
        .phymode = CONFIG_ESP_NOW_PHYMODE,
        .rate = CONFIG_ESP_NOW_RATE,
        .ersu = false,
        .dcm = false
    };
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rate_config));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    esp_now_peer_info_t peer = {
        .channel   = CONFIG_LESS_INTERFERENCE_CHANNEL,
        .ifidx     = WIFI_IF_STA,
        .encrypt   = false,
        .peer_addr = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
    };
    wifi_esp_now_init(peer);

    ESP_LOGI(TAG, "================ CSI SEND ================");
    ESP_LOGI(TAG, "wifi_channel: %d, send_frequency: %d, mac: " MACSTR,
             CONFIG_LESS_INTERFERENCE_CHANNEL, CONFIG_SEND_FREQUENCY, MAC2STR(CONFIG_CSI_SEND_MAC));

    for (uint32_t count = 0; ; ++count) {
        esp_err_t ret = esp_now_send(peer.peer_addr, (const uint8_t *)&count, sizeof(count));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "free_heap: %ld <%s> ESP-NOW send error", esp_get_free_heap_size(), esp_err_to_name(ret));
        }

        usleep(1000 * 1000 / CONFIG_SEND_FREQUENCY);
    }
}
