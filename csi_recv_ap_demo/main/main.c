#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"

#include "esp_mac.h"
#include "rom/ets_sys.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_now.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "ping/ping_sock.h"

#include "protocol_examples_common.h"
#include "esp_csi_gain_ctrl.h"

#define CONFIG_SEND_FREQUENCY       50

#define CONFIG_GAIN_CONTROL         1
#define CONFIG_FORCE_GAIN           0

static const char *TAG = "csi_recv_ap";

static void wifi_csi_callback(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf) {
        ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }

    if (memcmp(info->mac, ctx, 6)) {
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
        }
        if (CONFIG_FORCE_GAIN) {
            esp_csi_gain_ctrl_set_rx_force_gain(agc_gain_baseline, fft_gain_baseline);
            ESP_LOGI(TAG, "fft_force %d, agc_force %d", fft_gain_baseline, agc_gain_baseline);
        }
        esp_csi_gain_ctrl_get_gain_compensation(&compensate_gain, agc_gain, fft_gain);
        ESP_LOGD(TAG, "compensate_gain %f, agc_gain %d, fft_gain %d", compensate_gain, agc_gain, fft_gain);
    }

    if (!s_count) {
        ESP_LOGI(TAG, "================ CSI RECV ================");
        ets_printf("type,id,mac,rssi,rate,sig_mode,mcs,bandwidth,smoothing,not_sounding,aggregation,stbc,fec_coding,sgi,noise_floor,ampdu_cnt,channel,secondary_channel,local_timestamp,ant,sig_len,rx_format,len,first_word,data\n");
    }
    ets_printf("CSI_DATA,%d," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
               s_count, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate, rx_ctrl->sig_mode,
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

static void wifi_csi_init()
{
    wifi_csi_config_t csi_config = {
        .lltf_en           = true,
        .htltf_en          = false,
        .stbc_htltf2_en    = false,
        .ltf_merge_en      = true,
        .channel_filter_en = true,
        .manu_scale        = true,
        .shift             = true,
    };

    static wifi_ap_record_t s_ap_info = {0};
    ESP_ERROR_CHECK(esp_wifi_sta_get_ap_info(&s_ap_info));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_config));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(wifi_csi_callback, s_ap_info.bssid));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

/*  wifi_ping_gateway_start:
    gateway に ping を送信するためのを設定を行い，ping セッションを開始する関数

    1. ping セッションのハンドルを定義する
    2. ping の送信回数，送信間隔，タスクのスタックサイズ，データサイズを設定する
    3. Wi-Fi インタフェースから，ESP32 自身の IP アドレスと gateway の IP アドレスを取得する
    4. 取得した gateway の IP アドレスを ping の宛先に設定する
    5. ping の宛先アドレスの種類を IPv4 に設定する
    6. コールバック関数を格納する構造体を初期化する (現在はコールバック関数を用意していないため，何も実行しない)
    7. ping の設定とコールバック構造体を用いて ping セッションを作成する
    8. ping セッションを開始する
*/
static esp_err_t wifi_ping_gateway_start()
{
    static esp_ping_handle_t ping_handle = NULL;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.count             = 0;
    ping_config.interval_ms       = 1000 / CONFIG_SEND_FREQUENCY;
    ping_config.task_stack_size   = 3072;
    ping_config.data_size         = 1;

    esp_netif_ip_info_t local_ip;
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &local_ip);
    ESP_LOGI(TAG, "got ip:" IPSTR ", gw: " IPSTR, IP2STR(&local_ip.ip), IP2STR(&local_ip.gw));
    ping_config.target_addr.u_addr.ip4.addr = ip4_addr_get_u32(&local_ip.gw);
    ping_config.target_addr.type = ESP_IPADDR_TYPE_V4;

    esp_ping_callbacks_t cbs = { 0 };
    esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    esp_ping_start(ping_handle);

    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(example_connect());

    wifi_csi_init();
    wifi_ping_gateway_start();
}
