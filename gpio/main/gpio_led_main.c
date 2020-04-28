#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

#define ESP_INTR_FLAG_DEFAULT 0

gpio_num_t gpio_led_num = GPIO_NUM_32;            // 连接LED的GPIO
gpio_num_t gpio_contact_switch_num = GPIO_NUM_33; // 连接触点开关GPIO

static xQueueHandle gpio_evt_queue = NULL; // 事务队列

int cnt = 0; // 计数，反转LED
/**
 * 中断处理
 */
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    cnt++;
    // 反转gpio的电平，让LED灯出现显示的反转
    gpio_set_level(gpio_led_num, cnt % 2);
    uint32_t gpio_num = (uint32_t)arg;
    // 向gpio事件处理的队列中添加一条消息
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_led(void *arg)
{
    uint32_t io_num;
    for (;;)
    {
        if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
        {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level(io_num));
        }
    }
}

void app_main(void)
{
    // 设置控制LED的GPIO为输出模式
    gpio_set_direction(gpio_led_num, GPIO_MODE_OUTPUT);

    // 设置作为中断的GPIO pin为输出模式
    gpio_set_direction(gpio_contact_switch_num, GPIO_MODE_INPUT);
    // 设置作为中断模式为沿上升沿触发
    gpio_set_intr_type(gpio_contact_switch_num, GPIO_INTR_POSEDGE);

    // 创建队列处理来自isr的gpio事件
    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    // 启动gpio的任务
    xTaskCreate(gpio_task_led, "gpio_task_led", 2048, NULL, 10, NULL);

    // 初始化全局GPIO的中断服务程序，并设置中断的电平等级
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    // 将指定的GPIO挂载到中断服务上，并设定中断触发的回调函数和传入参数
    gpio_isr_handler_add(gpio_contact_switch_num, gpio_isr_handler, (void *)gpio_contact_switch_num);
}
