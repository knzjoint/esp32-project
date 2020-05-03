#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "cJSON.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "mqtt_client.h"
#include "sign_api.h" // 包含签名所需的各种数据结构定义

static const char *TAG = "MQTT_ALI_IOT";
// 下面的几个宏用于定义设备的阿里云身份认证信息：ProductKey、ProductSecret、DeviceSecret、DeviceName
// 在实际产品开发中，设备的身份认证信息应该是设备厂商将其加密后存放于设备Flash中或者某个文件中，
// 设备上电时将其读出后使用
#define LIGHT_PRODUCT_KEY "a1CEwF7bES6"
#define LIGHT_PRODUCT_SECRET "xxxxxxxxxxx"
#define LIGHT_DEVICE_SECRET "xxxxxxxxxxx"
#define LIGHT_DEVICE_NAME "light-1"

// 控制Light开关的Topic
#define LIGHT_CONTROL_TOPIC "/a1CEwF7bES6/light-1/user/light_control"
// 上报Light状态的Topic
#define LIGHT_STATUS_TOPIC "/a1CEwF7bES6/light-1/user/light_status"

// 初始化mqtt的客户端
esp_mqtt_client_handle_t client;

// 控制LED的 GPIO
gpio_num_t gpio_led_num = GPIO_NUM_32; // 连接LED的GPIO

/**
 * switch LED
 * @param level 0 off, 1 on
 */
static void switch_led(int level)
{
    gpio_set_level(gpio_led_num, level);
}

/**
 * parse json and set config and save config，parse json fromat
 * {
 *     "switch": "on" 
 * }
 * switch value can select on or off.
 * @param buffer mqtt message payload
 * @return
 */
static esp_err_t parse_json_data(char *buffer)
{
    // root是JSON的根，item是内部对象
    cJSON *root, *item;
    char *value_str = NULL; // 保存value的值
    ESP_LOGI(TAG, "parse data:%s", buffer);
    int return_value = ESP_OK; // 返回值
    int msg_id;

    // 解析从阿里云物联网平台指定设备发来的消息
    root = cJSON_Parse((char *)buffer);
    if (!root)
    {
        ESP_LOGE(TAG, "Error before: [%s]", cJSON_GetErrorPtr());
        return ESP_ERR_INVALID_ARG;
    }

    // 判断根数据下的key value 的数量
    int json_item_num = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "Total JSON Items:%d", json_item_num);

    int32_t i = 0;
    for (i = 0; i < json_item_num; ++i)
    {
        ESP_LOGI(TAG, "Start Parse JSON Items:%d", i);
        item = cJSON_GetArrayItem(root, i);
        if (!item)
        {
            break;
        }
        ESP_LOGI(TAG, "parse JSON Items:%d found", i);
        ESP_LOGI(TAG, "item<%s>", item->string);

        // 判断是否存在switch的key
        if (0 == strncmp(item->string, "switch", sizeof("switch")))
        {
            // 获取switch的value值
            value_str = item->valuestring;
            ESP_LOGI(TAG, "parsed cmd_id:%s", value_str);
            // 判断 switch的值是否为on
            if (0 == strncmp(value_str, "on", sizeof("on")))
            {
                // 开灯
                switch_led(1);
                printf("开灯\n");
                // 上报当前灯的状态
                msg_id = esp_mqtt_client_publish(client, LIGHT_STATUS_TOPIC, "{\"lightStatus\": \"on\"}", 0, 0, 0);
                ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
                break;
            }
            else if (0 == strncmp(value_str, "off", sizeof("off")))
            {
                // 关灯
                switch_led(0);
                printf("关灯\n");
                // 上传当前灯的状态
                msg_id = esp_mqtt_client_publish(client, LIGHT_STATUS_TOPIC, "{\"lightStatus\": \"off\"}", 0, 0, 0);
                ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
                break;
            }
            else
            {
                return_value = ESP_ERR_NOT_FOUND;
                break;
            }
        }
    }

    // 释放内存
    cJSON_Delete(root);
    return return_value;
}

/**
 * deal with event according to topic.
 * @param event event data
 */
static esp_err_t topic_router_handler(esp_mqtt_event_handle_t event)
{
    char topic[512] = "";
    memcpy(topic, event->topic, event->topic_len);

    // 处理指定的订阅主题
    if (0 == strncmp(event->topic, LIGHT_CONTROL_TOPIC, event->topic_len))
    {
        ESP_LOGI(TAG, "deal with topic :%s", event->topic);
        char dest[512] = ""; // event事件未初始化，使用ESP_LOGI()打印的内容部分会出现乱码
        memcpy(dest, event->data, event->data_len);
        ESP_LOGI(TAG, "DATA=%s", dest);
        // 解析JSON格式数据
        parse_json_data(dest);
    }
    else
    {
        ESP_LOGE(TAG, "Topics %s that do not need to be processed", topic);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

/**
 * mqtt event callback function.
 * @param event event data
 */
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    // your_context_t *context = event->context;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED: // MQTT 客户端连接上服务器事件
        msg_id = esp_mqtt_client_subscribe(client, LIGHT_CONTROL_TOPIC, 1);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED: // MQTT 客户端断开连接事件
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED: // MQTT 订阅事件
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED: // MQTT 取消订阅事件
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED: // MQTT 发布消息到指定主题事件
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA: // MQTT 客户端接收到数据事件
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        // mqtt data router to specified topic
        topic_router_handler(event);
        break;
    case MQTT_EVENT_ERROR: // 错误事件
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }

    return ESP_OK;
}

/**
 * mqtt event handler function.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

/**
 * start mqtt client.
 */
static void mqtt_app_start(void)
{
    // 设备的元信息
    iotx_dev_meta_info_t meta_info;
    // 阿里云认证签名
    iotx_sign_mqtt_t sign_mqtt;

    memset(&meta_info, 0, sizeof(iotx_dev_meta_info_t));
    // 下面的代码是将上面静态定义的设备身份信息赋值给meta_info
    memcpy(meta_info.product_key, LIGHT_PRODUCT_KEY, strlen(LIGHT_PRODUCT_KEY));
    memcpy(meta_info.product_secret, LIGHT_PRODUCT_SECRET, strlen(LIGHT_PRODUCT_SECRET));
    memcpy(meta_info.device_name, LIGHT_DEVICE_NAME, strlen(LIGHT_DEVICE_NAME));
    memcpy(meta_info.device_secret, LIGHT_DEVICE_SECRET, strlen(LIGHT_DEVICE_SECRET));

    // 调用签名函数，生成MQTT连接时需要的各种数据，IOTX_CLOUD_REGION_SHANGHAI 指连接站点是华东2(上海)
    IOT_Sign_MQTT(IOTX_CLOUD_REGION_SHANGHAI, &meta_info, &sign_mqtt);

    esp_mqtt_client_config_t mqtt_cfg = {
        // .uri = CONFIG_BROKER_URL,
        .host = sign_mqtt.hostname,      // 完整的阿里云物联网站点域名
        .port = 1883,                    // 阿里云站点的端口号
        .password = sign_mqtt.password,  // MQTT建立连接时需要指定的Password。把提交给服务器的参数按字典排序并拼接后，使用hmacsha256方法和设备的DeviceSecret，加签生成Password。
        .client_id = sign_mqtt.clientid, // MQTT建立连接时需要指定的ClientID。建议使用设备的MAC地址或SN码，64字符内。
        .username = sign_mqtt.username,  // MQTT建立连接时需要指定的Username。由设备名DeviceName、符号（&）和产品ProductKey组成，格式：deviceName+"&"+productKey。示例：Device1&alSseIs****。
        // .event_handle = mqtt_event_handler, // mqtt客户端启动成功后对连接、断开连接、订阅、取消订阅、发布、接收数据等事件的处理。
    };
#if CONFIG_BROKER_URL_FROM_STDIN
    char line[128];

    if (strcmp(mqtt_cfg.uri, "FROM_STDIN") == 0)
    {
        int count = 0;
        printf("Please enter url of mqtt broker\n");
        while (count < 128)
        {
            int c = fgetc(stdin);
            if (c == '\n')
            {
                line[count] = '\0';
                break;
            }
            else if (c > 0 && c < 127)
            {
                line[count] = c;
                ++count;
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
        mqtt_cfg.uri = line;
        printf("Broker url: %s\n", line);
    }
    else
    {
        ESP_LOGE(TAG, "Configuration mismatch: wrong broker url");
        abort();
    }
#endif /* CONFIG_BROKER_URL_FROM_STDIN */

    // 初始化mqtt的客户端
    client = esp_mqtt_client_init(&mqtt_cfg);
    // 注册mqtt连接、断开连接、订阅、取消订阅、发布、接收数据等事件的处理。
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    // 启动esp mqtt的客户端
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    // 打印启动的INFO信息
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("MQTT_ALI_IOT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    // 设置控制LED的GPIO为输出模式
    gpio_set_direction(gpio_led_num, GPIO_MODE_OUTPUT);

    mqtt_app_start();
}
