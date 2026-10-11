#pragma once

#include "esp_err.h"

/*
 * What the rest of the firmware tells the screen.
 *
 * mpv2-claw does this in main.c: the IMU feed for the alive eyes, the
 * network line under them, the Raspberry Pi's address and its "ROS" badge.
 * Here it is one small module so main.cc stays a boot sequence.
 */
namespace display_glue {

/** Panel + LVGL + face. Call after robot::init() (the eyes read the IMU)
 *  and before anything slow, so the face is the first thing on the panel.
 *  Safe with the display compiled out. */
void start_face();

/** After LittleFS is mounted: built-in GIFs into <fs>/display, then bring
 *  back a looping animation chosen before the reboot. */
void start_animations();

/** The event task: Wi-Fi line under the eyes (polled), Pi address, ROS
 *  badge. Call after Wi-Fi is up; registers the Pi link observers itself. */
void start_events();

/** From robot::set_master_observer -- runs on the Pi link task, so it only
 *  posts an event. */
void on_master_changed(bool active);

}  // namespace display_glue
