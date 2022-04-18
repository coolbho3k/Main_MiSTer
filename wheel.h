#ifndef WHEEL_H
#define WHEEL_H

#include <utility>

/* Pair of vid, pid of the wheel, for map indexing */
typedef std::pair<uint16_t, uint16_t> wheel_ids;

struct wheel_info
{
  int steering_axis;
  int throttle_axis;
  int brake_axis;
  int clutch_axis;
  int handbrake_axis;
};

const wheel_info* get_wheel_info(uint16_t vid, uint16_t pid);

/* Returns whether the axis code represents a vehicle control axis */
inline bool is_control_axis(const wheel_info* info, int axis) {
  return axis == info->steering_axis ||
    axis == info->throttle_axis ||
    axis == info->brake_axis ||
    axis == info->clutch_axis ||
    axis == info->handbrake_axis;
}

#endif
