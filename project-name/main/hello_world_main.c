// toggle rele

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "driver/gpio.h"
#include "rom/gpio.h"

#define RELAY_GPIO 32
#define OLIMEX_BUT_PIN 34

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    gpio_set_level(RELAY_GPIO, !gpio_get_level(OLIMEX_BUT_PIN));
}

void app_main(void)
{
    printf("Hello world!\n");

    /* Make pads GPIO */
    gpio_pad_select_gpio(RELAY_GPIO);
    gpio_pad_select_gpio(OLIMEX_BUT_PIN);

    /* Set the Relay as a push/pull output */
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);

    /* Set Button as input */
    gpio_set_direction(OLIMEX_BUT_PIN, GPIO_MODE_INPUT);
    /* Enable interrupt on both edges */
    gpio_set_intr_type(OLIMEX_BUT_PIN, GPIO_INTR_ANYEDGE);

    /* Install ISR routine */
    gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
    gpio_isr_handler_add(OLIMEX_BUT_PIN, gpio_isr_handler, 0);
}
