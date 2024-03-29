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

#include <AP_HAL/AP_HAL.h>
#include <AP_HAL/utility/dsm.h>
#include <AP_HAL/utility/sumd.h>
#include <AP_HAL/utility/st24.h>
#include <AP_HAL/utility/srxl.h>

#include "RCInput.h"
#include "sbus.h"

/* Alta Specific */
#define START_SBUS_FRAME 0x0f
#define END_SBUS_FRAME 0x00
#define MIN_START_DELTA 1000

#define SYSTEM_ID_QSMX_22 0xa2
#define SYSTEM_ID_QSMX_11 0xb2
/* Alta Specific */

#define MIN_NUM_CHANNELS 5

extern const AP_HAL::HAL& hal;

using namespace Linux;

RCInput::RCInput()
{
	ppm_state._channel_counter = -1;
//	hal.console->printf("Alta3:RCInput::RCInput() entered.");

	// Alta specific
	_lAltaFramesProcessed = 0;
	_bAltaFrameSynced = false;
	_lAltaConseqSyncedFrames = 0;
	_bAltaGapByteFound = false;
	_bAltaSysIdByteFound = false;
	_nAltaFrameBytesIn = 0;
	_lAltaSyncLosses = 0;
	// Alta specific
}

void RCInput::init()
{
	hal.console->printf("Alta1:RCInput::init() entered.\n");
	printf("Alta2:RCInput::init() entered.\n");
	AP_HAL::panic("Alta3:RCInput::init() entered.");
}

bool RCInput::new_input()
{
//	hal.console->printf("Alta:RCInput::new_input() entered.\n");

	bool ret = rc_input_count != last_rc_input_count;
	if (ret)
	{
		last_rc_input_count.store(rc_input_count);
//		hal.console->printf("Alta:RCInput::new_input() detected.\n");

	}
	return ret;
}

uint8_t RCInput::num_channels()
{
	if (_num_channels == 0)
	{
		hal.console->printf("Alta:RCInput::num_channels() (nc=%d)entered.\n", _num_channels);
	}
	return _num_channels;
}

uint16_t RCInput::read(uint8_t ch)
{
    if (ch >= _num_channels) {
        return 0;
    }
    return _pwm_values[ch];
}

uint8_t RCInput::read(uint16_t* periods, uint8_t len)
{
//	hal.console->printf("Alta: RCInput::read(p,l) entered.\n");

	uint8_t i;
	for (i=0; i<len; i++)
	{
		periods[i] = read(i);
	}
	return len;
}

/*
  process a PPM-sum pulse of the given width
 */
void RCInput::_process_ppmsum_pulse(uint16_t width_usec)
{
	hal.console->printf("Alta: RCInput::process_ppmsum_pulse() entered.\n");

	if (width_usec >= 2700)
	{
        	// a long pulse indicates the end of a frame. Reset the
        	// channel counter so next pulse is channel 0
        	if (ppm_state._channel_counter >= MIN_NUM_CHANNELS)
		{
			for (uint8_t i=0; i<ppm_state._channel_counter; i++) 
			{
				_pwm_values[i] = ppm_state._pulse_capt[i];
			}
			_num_channels = ppm_state._channel_counter;
			rc_input_count++;
		}
		ppm_state._channel_counter = 0;
		return;
	}
	if (ppm_state._channel_counter == -1)
	{
		// we are not synchronised
		return;
	}

	/*
	we limit inputs to between 700usec and 2300usec. This allows us
	to decode SBUS on the same pin, as SBUS will have a maximum
	pulse width of 100usec
	*/
	if (width_usec > 700 && width_usec < 2300)
	{
		// take a reading for the current channel
		// buffer these
		ppm_state._pulse_capt[ppm_state._channel_counter] = width_usec;

		// move to next channel
		ppm_state._channel_counter++;
	}

	// if we have reached the maximum supported channels then
	// mark as unsynchronised, so we wait for a wide pulse
	if (ppm_state._channel_counter >= LINUX_RC_INPUT_NUM_CHANNELS)
	{
		for (uint8_t i=0; i<ppm_state._channel_counter; i++)
		{
			_pwm_values[i] = ppm_state._pulse_capt[i];
		}
		_num_channels = ppm_state._channel_counter;
		rc_input_count++;
		ppm_state._channel_counter = -1;
	}
}

/*
  process a SBUS input pulse of the given width
 */
void RCInput::_process_sbus_pulse(uint16_t width_s0, uint16_t width_s1)
{
	hal.console->printf("Alta: RCInput::process_sbus_pulse() entered.\n");

    // convert to bit widths, allowing for up to 1usec error, assuming 100000 bps
    uint16_t bits_s0 = (width_s0+1) / 10;
    uint16_t bits_s1 = (width_s1+1) / 10;
    uint16_t nlow;

    uint8_t byte_ofs = sbus_state.bit_ofs/12;
    uint8_t bit_ofs = sbus_state.bit_ofs%12;

    if (bits_s0 == 0 || bits_s1 == 0) {
        // invalid data
        goto reset;
    }

    if (bits_s0+bit_ofs > 10) {
        // invalid data as last two bits must be stop bits
        goto reset;
    }

    // pull in the high bits
    sbus_state.bytes[byte_ofs] |= ((1U<<bits_s0)-1) << bit_ofs;
    sbus_state.bit_ofs += bits_s0;
    bit_ofs += bits_s0;

    // pull in the low bits
    nlow = bits_s1;
    if (nlow + bit_ofs > 12) {
        nlow = 12 - bit_ofs;
    }
    bits_s1 -= nlow;
    sbus_state.bit_ofs += nlow;

    if (sbus_state.bit_ofs == 25*12 && bits_s1 > 12) {
        // we have a full frame
        uint8_t bytes[25];
        uint8_t i;
        for (i=0; i<25; i++) {
            // get inverted data
            uint16_t v = ~sbus_state.bytes[i];
            // check start bit
            if ((v & 1) != 0) {
                goto reset;
            }
            // check stop bits
            if ((v & 0xC00) != 0xC00) {
                goto reset;
            }
            // check parity
            uint8_t parity = 0, j;
            for (j=1; j<=8; j++) {
                parity ^= (v & (1U<<j))?1:0;
            }
            if (parity != (v&0x200)>>9) {
                goto reset;
            }
            bytes[i] = ((v>>1) & 0xFF);
        }
        uint16_t values[LINUX_RC_INPUT_NUM_CHANNELS];
        uint16_t num_values=0;
        bool sbus_failsafe=false, sbus_frame_drop=false;
        if (sbus_decode(bytes, values, &num_values,
                        &sbus_failsafe, &sbus_frame_drop,
                        LINUX_RC_INPUT_NUM_CHANNELS) &&
            num_values >= MIN_NUM_CHANNELS) {
            for (i=0; i<num_values; i++) {
                _pwm_values[i] = values[i];
            }
            _num_channels = num_values;
            if (!sbus_failsafe) {
                rc_input_count++;
            }
        }
        goto reset;
    } else if (bits_s1 > 12) {
        // break
        goto reset;
    }
    return;
reset:
    memset(&sbus_state, 0, sizeof(sbus_state));
}

void RCInput::_process_sbus_byte(uint16_t data_delta, uint16_t data_val)
{
	hal.console->printf("Alta: RCInput::process_sbus_byte(dd=%04X, dv=%04X) entered.\n", data_delta, data_val);

	// Increment incoming byte counter
	_nAltaFrameBytesIn++;

	if (_bAltaFrameSynced)
	{
		// In the middle of frame acquisition
		if ((1 <= _nAltaFrameBytesIn) && (_nAltaFrameBytesIn <= 25))
			sbus_state.bytes[_nAltaFrameBytesIn-1] = data_val;
		else
			hal.console->printf("Alta: RCInput::process_sbus_byte() -- FATAL -- frame index (%d) out of range.\n", _nAltaFrameBytesIn);

		/* Are we still in SYNC ? */
		if ((_nAltaFrameBytesIn == 1) && (data_val != START_SBUS_FRAME))
		{
			hal.console->printf("Alta: RCInput::process_sbus_byte() -- ERROR -- start frame sync loss, frame: %u\n", _lAltaFramesProcessed);
			_bAltaFrameSynced = false;
			_bAltaStartFound = false;
			_lAltaSyncLosses++;
		}
		else if (_nAltaFrameBytesIn == 25)
		{
			if (data_val != END_SBUS_FRAME)
			{
				hal.console->printf("Alta: RCInput::process_sbus_byte() -- ERROR -- end frame sync loss, frame: %u\n", _lAltaFramesProcessed);
				_bAltaFrameSynced = false;
				_bAltaStartFound = false;
				_lAltaSyncLosses++;
			}
			else
			{
				/* We have a 'synced' frame */
				uint16_t values[LINUX_RC_INPUT_NUM_CHANNELS];
				uint16_t num_values = 0;
				bool sbus_failsafe = false;
				bool sbus_frame_drop = false;
				uint8_t bytes[25];
				uint8_t i;
				for (i=0; i<25; i++)
					bytes[i] = sbus_state.bytes[i];

//				hal.console->printf("Alta: RCInput::decoding frame()\n");
				if (sbus_decode (bytes, values, &num_values, &sbus_failsafe, &sbus_frame_drop, LINUX_RC_INPUT_NUM_CHANNELS)
					&& (num_values >= MIN_NUM_CHANNELS))
				{
//					hal.console->printf("Alta: RCInput::decoding frame successful, num_values = %d\n", num_values);
					for (i=0; i<num_values; i++)
						_pwm_values[i] = values[i];
					_num_channels = num_values;

					if (!sbus_failsafe)
						rc_input_count++;
				}
				else
					hal.console->printf("Alta: RCInput::process_sbus_byte() -- WARNING -- bad frame decode, frame: %u\n", _lAltaFramesProcessed);

				/* Reset for start of next frame */
				_lAltaFramesProcessed++;
				_nAltaFrameBytesIn = 0;
			}

//			if (0 == (_lAltaFramesProcessed % 2500))
//				hal.console->printf("Alta: RCInput::process_sbus_byte(), Frames processed: %u, Sync losses: %d\n", _lAltaFramesProcessed, _lAltaSyncLosses);
		}
	}
	else    /* Not in SYNC */
	{
		/* 'SkipNext' prevents locking into a frame while attempting to sync */
		if (!_bAltaSkipNextByte)
		{
			if (!_bAltaStartFound && (data_val == START_SBUS_FRAME) && (MIN_START_DELTA < data_delta))
			{
				hal.console->printf("Alta: RCInput::process_sbus_byte(), acquired SYNC start\n");
				_bAltaStartFound = true;
				_nAltaFrameBytesIn = 1;
			}
			else if (_bAltaStartFound && (_nAltaFrameBytesIn == 25))
			{
				if (data_val == END_SBUS_FRAME)
				{
					hal.console->printf("Alta: RCInput::process_sbus_byte(), acquired SYNC END\n");
					_bAltaFrameSynced = true;
					_nAltaFrameBytesIn = 0;
				}
				else
				{
					_bAltaStartFound = false;
					_bAltaSkipNextByte = true;
				}
			}
		}
		else
		{
			hal.console->printf("Alta: RCInput::process_sbus_byte(), skipping byte\n");
			_bAltaSkipNextByte = false;
		}
	}
}

void RCInput::_process_qsmx_byte(uint16_t data_delta, uint16_t data_val)
{
//	static uint8_t nLastByteVal = 0;
//	static uint16_t nLastByteDelta = 0;

//	hal.console->printf("Alta: RCInput::process_qsmx_byte(s0=%04X, s1=%04X) entered.\n", data_delta, data_val);

	// Increment incoming byte counter
	_nAltaFrameBytesIn++;

	if (_bAltaFrameSynced)
	{
		// In the middle of frame acquisition
		if ((1 <= _nAltaFrameBytesIn) && (_nAltaFrameBytesIn <= 16))
			dsm_state.bytes[_nAltaFrameBytesIn-1] = data_val;
		else
			hal.console->printf("Alta: RCInput::process_qsmx_byte() -- FATAL -- frame index (%d) out of range.\n", _nAltaFrameBytesIn);

		/* Are we still in SYNC ? */
		if ((_nAltaFrameBytesIn == 2) &&
			(data_val != SYSTEM_ID_QSMX_11) &&
			(data_val != SYSTEM_ID_QSMX_22))
		{
			hal.console->printf("Alta: RCInput::process_qsmx_byte() -- ERROR -- system ID sync loss, frame: %u\n", _lAltaFramesProcessed);
			_bAltaFrameSynced = false;
			_bAltaStartFound = false;
			_bAltaSysIdByteFound = false;
			_lAltaSyncLosses++;
		}
		else if (_nAltaFrameBytesIn == 16)
		{
			/* We have a 'synced' frame */
			uint16_t values[8];
			uint16_t num_values = 0;
			bool dsm_failsafe = false;
//			bool dsm_frame_drop = false;
			uint8_t bytes[16];
			uint8_t i;
			_lAltaConseqSyncedFrames++;
			for (i=0; i<16; i++)
				bytes[i] = dsm_state.bytes[i];

//			hal.console->printf("Alta: RCInput::decoding frame()\n");
			if (dsm_decode(AP_HAL::micros64(), bytes, values, &num_values, 8) && num_values >= MIN_NUM_CHANNELS)
			{
//				hal.console->printf("Alta: RCInput::decoding frame successful, num_values = %d\n", num_values);
				for (i=0; i<num_values; i++)
					_pwm_values[i] = values[i];
				_num_channels = num_values;

				if (!dsm_failsafe)
					rc_input_count++;
			}
			else
				hal.console->printf("Alta: RCInput::process_qsmx_byte() -- WARNING -- bad frame decode, frame: %u\n", _lAltaFramesProcessed);

			/* Reset for start of next frame */
			_lAltaFramesProcessed++;
			_nAltaFrameBytesIn = 0;
//			if (0 == (_lAltaFramesProcessed % 2500))
//				hal.console->printf("Alta: RCInput::process_sbus_byte(), Frames processed: %u, Sync losses: %d\n", _lAltaFramesProcessed, _lAltaSyncLosses);
		}
	}
	else    /* Not in SYNC */
	{
		_lAltaConseqSyncedFrames = 0;
		/* 'SkipNext' prevents locking into a frame while attempting to sync */
		if (!_bAltaSkipNextByte)
		{
			if (!_bAltaGapByteFound && (MIN_START_DELTA < data_delta))
                        {
                                _bAltaGapByteFound = true;
                                _bAltaSysIdByteFound = false;
                                hal.console->printf("Alta: RCInput::process_sbus_byte(), acquired gap-byte\n");
                                _nAltaFrameBytesIn = 1;
                        }
                        else if (_bAltaGapByteFound && !_bAltaSysIdByteFound && (_nAltaFrameBytesIn==2))
                        {
                                if ((data_val == SYSTEM_ID_QSMX_11) || (data_val == SYSTEM_ID_QSMX_22))
                                {
                                        _bAltaSysIdByteFound = true;
                                        hal.console->printf("Alta: RCInput::process_sbus_byte(), acquired ID-byte\n");
                                }
                           	else
                                {
                                        _bAltaGapByteFound = false;
                                        _bAltaSysIdByteFound = false;
                                        _bAltaFrameSynced = false;
                                }
                        }
                        else if (_bAltaGapByteFound && _bAltaSysIdByteFound && (_nAltaFrameBytesIn == 16))
                        {
                                hal.console->printf("Alta: RCInput::process_qsmx_byte(), acquired SYNC END\n");
                                _bAltaGapByteFound = false;
                                _bAltaSysIdByteFound = false;
                                _bAltaFrameSynced = true;
                                _nAltaFrameBytesIn = 0;
                        }
		}
		else
		{
			hal.console->printf("Alta: RCInput::process_qsmx_byte(), skipping byte\n");
			_bAltaSkipNextByte = false;
		}
	}
}

void RCInput::_process_dsm_pulse(uint16_t width_s0, uint16_t width_s1)
{
	hal.console->printf("Alta: RCInput::process_dsm_pulse() entered.\n");

    // convert to bit widths, allowing for up to 1usec error, assuming 115200 bps
    uint16_t bits_s0 = ((width_s0+4)*(uint32_t)115200) / 1000000;
    uint16_t bits_s1 = ((width_s1+4)*(uint32_t)115200) / 1000000;
    uint8_t bit_ofs, byte_ofs;
    uint16_t nbits;

    if (bits_s0 == 0 || bits_s1 == 0) {
        // invalid data
        goto reset;
    }

    byte_ofs = dsm_state.bit_ofs/10;
    bit_ofs = dsm_state.bit_ofs%10;

    if(byte_ofs > 15) {
        // invalid data
        goto reset;
    }

    // pull in the high bits
    nbits = bits_s0;
    if (nbits+bit_ofs > 10) {
        nbits = 10 - bit_ofs;
    }
    dsm_state.bytes[byte_ofs] |= ((1U<<nbits)-1) << bit_ofs;
    dsm_state.bit_ofs += nbits;
    bit_ofs += nbits;

    if (bits_s0 - nbits > 10) {
        if (dsm_state.bit_ofs == 16*10) {
            // we have a full frame
            uint8_t bytes[16];
            uint8_t i;
            for (i=0; i<16; i++) {
                // get raw data
                uint16_t v = dsm_state.bytes[i];

                // check start bit
                if ((v & 1) != 0) {
                    goto reset;
                }
                // check stop bits
                if ((v & 0x200) != 0x200) {
                    goto reset;
                }
                bytes[i] = ((v>>1) & 0xFF);
            }
            uint16_t values[8];
            uint16_t num_values=0;
            if (dsm_decode(AP_HAL::micros64(), bytes, values, &num_values, 8) &&
                num_values >= MIN_NUM_CHANNELS) {
                for (i=0; i<num_values; i++) {
                    _pwm_values[i] = values[i];
                }
                _num_channels = num_values;
                rc_input_count++;
            }
        }
        memset(&dsm_state, 0, sizeof(dsm_state));
    }

    byte_ofs = dsm_state.bit_ofs/10;
    bit_ofs = dsm_state.bit_ofs%10;

    if (bits_s1+bit_ofs > 10) {
        // invalid data
        goto reset;
    }

    // pull in the low bits
    dsm_state.bit_ofs += bits_s1;
    return;
reset:
    memset(&dsm_state, 0, sizeof(dsm_state));
}

/*
  process a RC input pulse of the given width
 */
void RCInput::_process_rc_pulse(uint16_t width_s0, uint16_t width_s1)
{
//	hal.console->printf("Alta:_process_rc_pulse() entered.\n");
#if 1
    // useful for debugging
    static FILE *rclog;
    if (rclog == nullptr) {
        rclog = fopen("/tmp/rcin.log", "w");
    }
    if (rclog) {
        fprintf(rclog, "%04X %04X\n", width_s0, width_s1);
    }
#endif
    // treat as PPM-sum
// Alta    _process_ppmsum_pulse(width_s0 + width_s1);

    // treat as SBUS
// Alta    _process_sbus_pulse(width_s0, width_s1);

    // treat as DSM
// Alta    _process_dsm_pulse(width_s0, width_s1);

	/* Alta Spektrum QSMX operation */
	_process_qsmx_byte (width_s0, width_s1);
}

/*
 * Update channel values directly
 */
void RCInput::_update_periods(uint16_t *periods, uint8_t len)
{
    if (len > LINUX_RC_INPUT_NUM_CHANNELS) {
        len = LINUX_RC_INPUT_NUM_CHANNELS;
    }
    for (unsigned int i=0; i < len; i++) {
        _pwm_values[i] = periods[i];
    }
    _num_channels = len;
    rc_input_count++;
}


/*
  add some bytes of input in DSM serial stream format, coping with partial packets
 */
bool RCInput::add_dsm_input(const uint8_t *bytes, size_t nbytes)
{
	hal.console->printf("Alta: RCInput::add_dsm_input() entered.\n");

    if (nbytes == 0) {
        return false;
    }
    const uint8_t dsm_frame_size = sizeof(dsm.frame);
    bool ret = false;
    
    uint32_t now = AP_HAL::millis();
    if (now - dsm.last_input_ms > 5) {
        // resync based on time
        dsm.partial_frame_count = 0;
    }
    dsm.last_input_ms = now;

    while (nbytes > 0) {
        size_t n = nbytes;
        if (dsm.partial_frame_count + n > dsm_frame_size) {
            n = dsm_frame_size - dsm.partial_frame_count;
        }
        if (n > 0) {
            memcpy(&dsm.frame[dsm.partial_frame_count], bytes, n);
            dsm.partial_frame_count += n;
            nbytes -= n;
            bytes += n;
        }

	if (dsm.partial_frame_count == dsm_frame_size) {
            dsm.partial_frame_count = 0;
            uint16_t values[16] {};
            uint16_t num_values=0;
            /*
              we only accept input when nbytes==0 as dsm is highly
              sensitive to framing, and extra bytes may be an
              indication this is really SRXL
             */
            if (dsm_decode(AP_HAL::micros64(), dsm.frame, values, &num_values, 16) &&
                num_values >= MIN_NUM_CHANNELS &&
                nbytes == 0) {
                for (uint8_t i=0; i<num_values; i++) {
                    if (values[i] != 0) {
                        _pwm_values[i] = values[i];
                    }
                }
                /*
                  the apparent number of channels can change on DSM,
                  as they are spread across multiple frames. We just
                  use the max num_values we get
                 */
                if (num_values > _num_channels) {
                    _num_channels = num_values;
                }
                rc_input_count++;
#if 0
                printf("Decoded DSM %u channels %u %u %u %u %u %u %u %u\n",
                       (unsigned)num_values,
                       values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]);
#endif
                ret = true;
            }
        }
    }
    return ret;
}


/*
  add some bytes of input in SUMD serial stream format, coping with partial packets
 */
bool RCInput::add_sumd_input(const uint8_t *bytes, size_t nbytes)
{
	hal.console->printf("Alta: RCInput::add_sumd_input() entered.\n");

    uint16_t values[LINUX_RC_INPUT_NUM_CHANNELS];
    uint8_t rssi;
    uint8_t rx_count;
    uint16_t channel_count;
    bool ret = false;
    
    while (nbytes > 0) {
        if (sumd_decode(*bytes++, &rssi, &rx_count, &channel_count, values, LINUX_RC_INPUT_NUM_CHANNELS) == 0) {
            if (channel_count > LINUX_RC_INPUT_NUM_CHANNELS) {
                continue;
            }
            for (uint8_t i=0; i<channel_count; i++) {
                if (values[i] != 0) {
                    _pwm_values[i] = values[i];
                }
            }
            _num_channels = channel_count;
            rc_input_count++;
            ret = true;
            _rssi = rssi;
        }
        nbytes--;
    }
    return ret;
}

/*
  add some bytes of input in ST24 serial stream format, coping with partial packets
 */
bool RCInput::add_st24_input(const uint8_t *bytes, size_t nbytes)
{
	hal.console->printf("Alta: RCInput::add_st24_input() entered.\n");

    uint16_t values[LINUX_RC_INPUT_NUM_CHANNELS];
    uint8_t rssi;
    uint8_t rx_count;
    uint16_t channel_count;
    bool ret = false;
    
    while (nbytes > 0) {
        if (st24_decode(*bytes++, &rssi, &rx_count, &channel_count, values, LINUX_RC_INPUT_NUM_CHANNELS) == 0) {
            if (channel_count > LINUX_RC_INPUT_NUM_CHANNELS) {
                continue;
            }
            for (uint8_t i=0; i<channel_count; i++) {
                if (values[i] != 0) {
                    _pwm_values[i] = values[i];
                }
            }
            _num_channels = channel_count;
            rc_input_count++;
            ret = true;
            _rssi = rssi;
        }
        nbytes--;
    }
    return ret;
}

/*
  add some bytes of input in SRXL serial stream format, coping with partial packets
 */
bool RCInput::add_srxl_input(const uint8_t *bytes, size_t nbytes)
{
	hal.console->printf("Alta: RCInput::add_srxl_input() entered.\n");

    uint16_t values[LINUX_RC_INPUT_NUM_CHANNELS];
    uint8_t channel_count;
    uint64_t now = AP_HAL::micros64();
    bool ret = false;
    bool failsafe_state;
    
    while (nbytes > 0) {
        if (srxl_decode(now, *bytes++, &channel_count, values, LINUX_RC_INPUT_NUM_CHANNELS, &failsafe_state) == 0) {
            if (channel_count > LINUX_RC_INPUT_NUM_CHANNELS) {
                continue;
            }
            for (uint8_t i=0; i<channel_count; i++) {
                _pwm_values[i] = values[i];
            }
            _num_channels = channel_count;
            if (failsafe_state == false) {
                rc_input_count++;
            }
            ret = true;
        }
        nbytes--;
    }
    return ret;
}


/*
  add some bytes of input in SBUS serial stream format, coping with partial packets
 */
void RCInput::add_sbus_input(const uint8_t *bytes, size_t nbytes)
{
	hal.console->printf("Alta: RCInput::add_sbus_input() entered.\n");

    if (nbytes == 0) {
        return;
    }
    const uint8_t sbus_frame_size = sizeof(sbus.frame);

    uint32_t now = AP_HAL::millis();
    if (now - sbus.last_input_ms > 5) {
        // resync based on time
        sbus.partial_frame_count = 0;
    }
    sbus.last_input_ms = now;

    while (nbytes > 0) {
        size_t n = nbytes;
        if (sbus.partial_frame_count + n > sbus_frame_size) {
            n = sbus_frame_size - sbus.partial_frame_count;
        }
        if (n > 0) {
            memcpy(&sbus.frame[sbus.partial_frame_count], bytes, n);
            sbus.partial_frame_count += n;
            nbytes -= n;
            bytes += n;
        }

	if (sbus.partial_frame_count == sbus_frame_size) {
            sbus.partial_frame_count = 0;
            uint16_t values[16] {};
            uint16_t num_values=0;
            bool sbus_failsafe;
            bool sbus_frame_drop;
            if (sbus_decode(sbus.frame, values, &num_values, &sbus_failsafe, &sbus_frame_drop, 16) &&
                num_values >= MIN_NUM_CHANNELS) {
                for (uint8_t i=0; i<num_values; i++) {
                    if (values[i] != 0) {
                        _pwm_values[i] = values[i];
                    }
                }
                /*
                  the apparent number of channels can change on SBUS,
                  as they are spread across multiple frames. We just
                  use the max num_values we get
                 */
                if (num_values > _num_channels) {
                    _num_channels = num_values;
                }
                if (!sbus_failsafe) {
                    rc_input_count++;
                }
#if 0
                printf("Decoded SBUS %u channels %u %u %u %u %u %u %u %u %s\n",
                       (unsigned)num_values,
                       values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
                       sbus_failsafe?"FAIL":"OK");
#endif
            }
        }
    }
}
