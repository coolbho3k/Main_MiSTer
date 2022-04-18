#include <inttypes.h>
#include <map>

#include "wheel.h"

std::map<wheel_ids, wheel_info> wheel_info_map = {
	/* {{vid, pid}, { steering_axis, throttle_axis, brake_axis, clutch_axis, handbrake_axis }} */
	/* FANATEC */
	/* Podium Wheel Base DD1 */
	{{0x0eb7, 0x0006}, { 0, 2, 5, 1, 6, false, false, false, false }}
	/* Podium Wheel Base DD2 */
	{{0x0eb7, 0x0007}, { 0, 2, 5, 1, 6, false, false, false, false }}
	/* TODO: Fill this in once we support/test other common wheels */
};

const wheel_info* get_wheel_info(uint16_t vid, uint16_t pid) {
	wheel_ids ids = std::make_pair(vid, pid);
	std::map<wheel_ids, wheel_info>::iterator it = wheel_info_map.find(ids);

  	if (it != wheel_info_map.end())
    	return &it->second;

    return nullptr;
}
