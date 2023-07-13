/*
 * utils.c
 *
 *  Created on: 13 Jul 2023
 *      Author: emanu
 */

#include "utils.h"

unsigned long millis() {
	return (unsigned long) (esp_timer_get_time() / 1000ULL);
}
