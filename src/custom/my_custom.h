/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#pragma once

#include <stdint.h>

// Custom hooks called by openHASP core (when HASP_USE_CUSTOM > 0)
void custom_setup();
void custom_loop();
void custom_every_second();
void custom_every_5seconds();

bool custom_pin_in_use(uint8_t pin);

// Optional hooks used by MQTT/custom topics & sensors
void custom_get_sensors(JsonDocument& doc);
void custom_topic_payload(const char* topic, const char* payload, uint8_t source);
void custom_state_subtopic(const char* subtopic, const char* payload);

