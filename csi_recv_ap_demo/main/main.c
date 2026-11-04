#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "nvs_flash.h"

#include "esp_mac.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "lwip/inet.h"
#include "ping/ping_sock.h"

#include "protocol_examples_common.h"
#include "esp_csi_gain_ctrl.h"
#include "mqtt_client.h"

#define CONFIG_SEND_FREQUENCY       10
#define CSI_MQTT_PAYLOAD_SIZE       2048
#define CSI_MQTT_QUEUE_LENGTH       10
#define MQTT_CONNECTED_BIT          BIT0

#define CONFIG_GAIN_CONTROL         1
#define CONFIG_FORCE_GAIN           0

static const char *TAG = "csi_recv_ap";
static QueueHandle_t s_csi_queue;
static EventGroupHandle_t s_mqtt_event_group;
static esp_mqtt_client_handle_t s_mqtt_client;

static bool append_csi_value(char *payload, size_t capacity, size_t *length, int value)
{
    int written = snprintf(payload + *length, capacity - *length, ",%d", value);
    if (written < 0 || (size_t)written >= capacity - *length) {
        return false;
    }

    *length += (size_t)written;
    return true;
}

static void wifi_csi_callback(void *ctx, wifi_csi_info_t *info)
{
    if (!info || !info->buf || info->len <= 0) {
        ESP_LOGW(TAG, "<%s> wifi_csi_cb", esp_err_to_name(ESP_ERR_INVALID_ARG));
        return;
    }

    if (memcmp(info->mac, ctx, 6)) {
        return;
    }

    const wifi_pkt_rx_ctrl_t *rx_ctrl = &info->rx_ctrl;
    static uint32_t s_count = 0;
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

    static char payload[CSI_MQTT_PAYLOAD_SIZE];
    int written = snprintf(payload, sizeof(payload),
                           "CSI_DATA,%lu," MACSTR ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,"
                           "%u,%d,%d,%d",
                           (unsigned long)s_count, MAC2STR(info->mac), rx_ctrl->rssi, rx_ctrl->rate,
                           rx_ctrl->sig_mode, rx_ctrl->mcs, rx_ctrl->cwb, rx_ctrl->smoothing,
                           rx_ctrl->not_sounding, rx_ctrl->aggregation, rx_ctrl->stbc,
                           rx_ctrl->fec_coding, rx_ctrl->sgi, rx_ctrl->noise_floor,
                           rx_ctrl->ampdu_cnt, rx_ctrl->channel, rx_ctrl->secondary_channel,
                           (unsigned int)rx_ctrl->timestamp, rx_ctrl->ant, rx_ctrl->sig_len,
                           rx_ctrl->sig_mode);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        ESP_LOGW(TAG, "CSI payload is too large");
        s_count++;
        return;
    }

    size_t payload_length = (size_t)written;
    written = snprintf(payload + payload_length, sizeof(payload) - payload_length,
                       ",%d,%d,\"[%d", info->len, info->first_word_invalid,
                       (int16_t)(compensate_gain * info->buf[0]));
    if (written < 0 || (size_t)written >= sizeof(payload) - payload_length) {
        ESP_LOGW(TAG, "CSI payload is too large");
        s_count++;
        return;
    }
    payload_length += (size_t)written;

    for (int i = 1; i < info->len; i++) {
        if (!append_csi_value(payload, sizeof(payload), &payload_length,
                              (int16_t)(compensate_gain * info->buf[i]))) {
            ESP_LOGW(TAG, "CSI payload is too large");
            s_count++;
            return;
        }
    }

    written = snprintf(payload + payload_length, sizeof(payload) - payload_length, "]\"\n");
    if (written < 0 || (size_t)written >= sizeof(payload) - payload_length) {
        ESP_LOGW(TAG, "CSI payload is too large");
        s_count++;
        return;
    }

    static uint32_t dropped_count = 0;
    if (xQueueSend(s_csi_queue, payload, 0) != pdTRUE) {
        if (dropped_count++ % CONFIG_SEND_FREQUENCY == 0) {
            ESP_LOGW(TAG, "MQTT queue full; dropping CSI records (%lu dropped)",
                     (unsigned long)dropped_count);
        }
    }
    s_count++;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Connected to MQTT broker");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from MQTT broker");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "MQTT connection error");
    }
}

static void mqtt_publish_task(void *arg)
{
    (void)arg;
    char payload[CSI_MQTT_PAYLOAD_SIZE];
    while (true) {
        xEventGroupWaitBits(s_mqtt_event_group, MQTT_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        if (xQueueReceive(s_csi_queue, payload, portMAX_DELAY) == pdTRUE) {
            int message_id = esp_mqtt_client_publish(s_mqtt_client, CONFIG_CSI_MQTT_TOPIC,
                                                     payload, 0, 0, 0);
            if (message_id < 0) {
                ESP_LOGW(TAG, "Failed to publish CSI record");
            }
        }
    }
}

static void mqtt_init(void)
{
    s_csi_queue = xQueueCreate(CSI_MQTT_QUEUE_LENGTH, CSI_MQTT_PAYLOAD_SIZE);
    ESP_ERROR_CHECK(s_csi_queue ? ESP_OK : ESP_ERR_NO_MEM);
    s_mqtt_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(s_mqtt_event_group ? ESP_OK : ESP_ERR_NO_MEM);

    const esp_mqtt_client_config_t mqtt_config = {
        .broker.address.uri = CONFIG_CSI_MQTT_BROKER_URI,
    };
    s_mqtt_client = esp_mqtt_client_init(&mqtt_config);
    ESP_ERROR_CHECK(s_mqtt_client ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    ESP_ERROR_CHECK(xTaskCreate(mqtt_publish_task, "mqtt_publish", 4096, NULL, 5, NULL) == pdPASS
                        ? ESP_OK
                        : ESP_FAIL);
    ESP_LOGI(TAG, "Publishing CSI to %s on topic %s",
             CONFIG_CSI_MQTT_BROKER_URI, CONFIG_CSI_MQTT_TOPIC);
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

    mqtt_init();
    wifi_csi_init();
    wifi_ping_gateway_start();
}
