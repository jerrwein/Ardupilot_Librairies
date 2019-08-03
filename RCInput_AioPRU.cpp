// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.


#include <AP_HAL/AP_HAL.h>

#if CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_BBBMINI || \
    CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_BLUE || \
    CONFIG_HAL_BOARD_SUBTYPE == HAL_BOARD_SUBTYPE_LINUX_POCKET

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "RCInput.h"
#include "RCInput_AioPRU.h"

extern const AP_HAL::HAL& hal;

using namespace Linux;

void RCInput_AioPRU::init()
{
	int mem_fd = open("/dev/mem", O_RDWR|O_SYNC|O_CLOEXEC);
	if (mem_fd == -1)
	{
		AP_HAL::panic("Unable to open /dev/mem");
	}
	ring_buffer = (volatile struct ring_buffer*) mmap(0, 0x1000, PROT_READ|PROT_WRITE, MAP_SHARED, mem_fd, RCIN_PRUSS_RAM_BASE);
	close(mem_fd);
	ring_buffer->ring_head = 0;

	hal.console->printf("Alta-RCInput_AioPRU::init() PRU memory map complete.\n");

	// enable the spektrum RC input power
	//  ALTA_TBD: ???    hal.gpio->pinMode(BBB_P8_17, HAL_GPIO_OUTPUT);
	//  ALTA_TBD: ???    hal.gpio->write(BBB_P8_17, 1);
}

/*
  called at 1kHz to check for new pulse capture data from the PRU
 */
void RCInput_AioPRU::_timer_tick()
{
//	hal.console->printf("Alta-RCInput_AioPRU::timer_tick() entered, H=%d, T=%d\n", ring_buffer->ring_head, ring_buffer->ring_tail);

	while (ring_buffer->ring_head != ring_buffer->ring_tail)
	{
		if (ring_buffer->ring_tail >= NUM_RING_ENTRIES)
		{
			// invalid ring_tail from PRU - ignore RC input
			hal.console->printf("Alta-RCInput_PRU::timer_tick() Invalid tail.\n");
			return;
		}

		/* ALTA: New byte based stuff */
//		hal.console->printf("tt(), H=%d, T=%d, V:%04X, D:%04X \n", ring_buffer->ring_head, ring_buffer->ring_tail, ring_buffer->buffer[ring_buffer->ring_head].pin_value, ring_buffer->buffer[ring_buffer->ring_head].delta_t);

                _process_rc_pulse(ring_buffer->buffer[ring_buffer->ring_head].pin_value, ring_buffer->buffer[ring_buffer->ring_head].delta_t);

		/* ALTA: Original bit-time based stuff */
#if 0
		_process_rc_pulse((ring_buffer->buffer[ring_buffer->ring_head].s1_t) / TICK_PER_US,
				   (ring_buffer->buffer[ring_buffer->ring_head].s0_t) / TICK_PER_US);
#endif

		// move to the next ring buffer entry
		ring_buffer->ring_head = (ring_buffer->ring_head + 1) % NUM_RING_ENTRIES;
	}
}

#endif // CONFIG_HAL_BOARD_SUBTYPE
