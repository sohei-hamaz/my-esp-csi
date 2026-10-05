#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nvs_flash.h"

#include "esp_mac.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_csi_gain_ctrl.h"

#define CONFIG_LESS_INTERFERENCE_CHANNEL    11
#define CONFIG_WIFI_BANDWIDTH               WIFI_BW40

#define CONFIG_ESP_NOW_PHYMODE              WIFI_PHY_MODE_HT40
#define CONFIG_ESP_NOW_RATE                 WIFI_PHY_RATE_MCS0_LGI

#define CONFIG_GAIN_CONTROL                 1
#define CONFIG_FORCE_GAIN                   0

static const uint8_t CONFIG_CSI_SEND_MAC[] = {0x1a, 0x00, 0x00, 0x00, 0x00, 0x00};
static const char *TAG = "csi_recv";

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
            (受信用 MAC アドレスを準備するか，使用する場面がないためそもそも不必要？)
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
    2. ESP-NOW の暗号化通信に使用する鍵 (Primary Master Key) を設定する
    3. ESP-NOW で使用する通信方式・通信レートの条件を設定する
    4. 通信相手 (peer) の情報を ESP-NOW に登録する
    5. 登録した peer に設定した通信方式・通信レートを適用する
*/
static void wifi_esp_now_init(esp_now_peer_info_t peer)
{
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)"pmk1234567890123"));
    esp_now_rate_config_t rate_config = {
        .phymode = CONFIG_ESP_NOW_PHYMODE,
        .rate = CONFIG_ESP_NOW_RATE,//  WIFI_PHY_RATE_MCS0_LGI,
        .ersu = false,
        .dcm = false
    };
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
    ESP_ERROR_CHECK(esp_now_set_peer_rate_config(peer.peer_addr, &rate_config));
}

/*  wifi_csi_callback:
    CSI を受信したときに呼び出され，受信した CSI や情報をシリアル出力するコールバック関数

    1. 受信した情報や CSI データがない場合，エラーとして処理を終了する
    2. 受信したパケットの送信元 MACアドレスが指定した送信元 MAC アドレスと一致するか確認する
    3. 必要な変数を定義する
    4. 受信時の AGC ゲインと FFT ゲインを取得する
    5. 最初の100回分の AGC ゲインと FFT ゲインを記録する
    6. 101回目のコールバックで，記録した受信ゲインから基準となるゲインを取得する
    7. CONFIG_FORCE_GAIN が1なら，取得した基準ゲインを受信ゲインとして固定する
    8. 現在の取得した AGC ゲインと FFT ゲインからゲイン補正倍率を取得する
    9. 受信した情報やゲイン補正を行った CSI データをシリアル出力する
*/
static void wifi_csi_callback(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }

    if (memcmp(info->mac, CONFIG_CSI_SEND_MAC, 6)) {
        return;
    }

    const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;
    static int s_count = 0;
    float compensate_gain = 1.0f;
    static uint8_t agc_gain = 0;
    static int8_t fft_gain = 0;
    if (CONFIG_GAIN_CONTROL) {
        static uint8_t agc_gain_baseline = 0;
        static int8_t fft_gain_baseline = 0;
        esp_csi_gain_ctrl_get_rx_gain(rx_ctrl, &agc_gain, &fft_gain);
        if (s_count < 100) {
            esp_csi_gain_ctrl_record_rx_gain(agc_gain, fft_gain);
        } else if (s_count == 100) {
            esp_csi_gain_ctrl_get_rx_gain_baseline(&agc_gain_baseline, &fft_gain_baseline);
            if (CONFIG_FORCE_GAIN) {
                esp_csi_gain_ctrl_set_rx_force_gain(agc_gain_baseline, fft_gain_baseline);
                ESP_LOGD(TAG, "fft_force %d, agc_force %d", fft_gain_baseline, agc_gain_baseline);
            }
        }
    }
    esp_csi_gain_ctrl_get_gain_compensation(&compensate_gain, agc_gain, fft_gain);
    ESP_LOGI(TAG, "compensate_gain %f, agc_gain %d, fft_gain %d", compensate_gain, agc_gain, fft_gain);

    uint32_t rx_id = *(uint32_t *)(info->payload + 15);

    if (!s_count) {
        ESP_LOGI(TAG, "================ CSI RECV ================");
        ets_printf("type,id,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_format,len,first_word,data\n");
    }

    ets_printf("CSI_DATA,%d," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
               rx_id, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate, rx_ctrl->sig_mode,
               rx_ctrl->mcs, rx_ctrl->cwb, rx_ctrl->smoothing, rx_ctrl->not_sounding,
               rx_ctrl->aggregation, rx_ctrl->stbc, rx_ctrl->fec_coding, rx_ctrl->sgi,
               rx_ctrl->noise_floor, rx_ctrl->ampdu_cnt, rx_ctrl->channel, rx_ctrl->secondary_channel,
               rx_ctrl->timestamp, rx_ctrl->ant, rx_ctrl->sig_len, rx_ctrl->sig_mode);

    ets_printf(",%d,%d,\"[%d", info->len, info->first_word_invalid, (int16_t)(compensate_gain * info->buf[0]));
    for (int i = 1; i < info->len; i++) {
        ets_printf(",%d", (int16_t)(compensate_gain * info->buf[i]));
    }

    ets_printf("]\"\n");
    s_count++;
}

/*  wifi_csi_init:
    CSI 取得機能の初期化関数

    1. プロミスキャスモードを有効にする
    2. CSI の取得条件を設定する
    3. 設定した CSI の取得条件を ESP32 に適用する
    4. CSI を受信したときのコールバック関数を登録する
    5. CSI 取得機能を有効にする
*/
static void wifi_csi_init(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    wifi_csi_config_t csi_config = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = false,
        .shift             = false,
    };

    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_callback, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
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

    wifi_csi_init();
}
