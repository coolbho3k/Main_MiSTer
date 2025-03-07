#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../../hardware.h"
#include "../../fpga_io.h"
#include "../../menu.h"
#include "../../debug.h"
#include "../../user_io.h"
#include "../../input.h"
#include "../../support.h"
#include "../../ide.h"
#include "archie.h"

#define CONFIG_FILENAME  "ARCHIE.CFG"

typedef struct
{
	unsigned long system_ctrl;     // system control word
	char rom_img[1024];            // rom image file name
	char hdd_img[2][1024];
} archie_config_t;

static archie_config_t config = {};

#define archie_debugf(a, ...) printf("\033[1;31mARCHIE: " a "\033[0m\n", ##__VA_ARGS__)
// #define archie_debugf(a, ...)
#define archie_x_debugf(a, ...) printf("\033[1;32mARCHIE: " a "\033[0m\n", ##__VA_ARGS__)

enum state {
	STATE_HRST, STATE_RAK1, STATE_RAK2, STATE_IDLE,
	STATE_WAIT4ACK1, STATE_WAIT4ACK2, STATE_HOLD_OFF
} kbd_state;

// archie keyboard controller commands
#define HRST    0xff
#define RAK1    0xfe
#define RAK2    0xfd
#define RQPD    0x40         // mask 0xf0
#define PDAT    0xe0         // mask 0xf0
#define RQID    0x20
#define KBID    0x80         // mask 0xc0
#define KDDA    0xc0         // new key down data, mask 0xf0
#define KUDA    0xd0         // new key up data, mask 0xf0
#define RQMP    0x22         // request mouse data
#define MDAT    0x00         // mouse data, mask 0x80
#define BACK    0x3f
#define NACK    0x30         // disable kbd scan, disable mouse
#define SACK    0x31         // enable kbd scan, disable mouse
#define MACK    0x32         // disable kbd scan, enable mouse
#define SMAK    0x33         // enable kbd scan, enable mouse
#define LEDS    0x00         // mask 0xf8
#define PRST    0x21         // nop

#define QUEUE_LEN 8
static unsigned char tx_queue[QUEUE_LEN][2];
static unsigned char tx_queue_rptr, tx_queue_wptr;
#define QUEUE_NEXT(a)  ((a+1)&(QUEUE_LEN-1))

static unsigned long ack_timeout;
static short mouse_x, mouse_y;

#define FLAG_SCAN_ENABLED  0x01
#define FLAG_MOUSE_ENABLED 0x02
static unsigned char flags;

// #define HOLD_OFF_TIME 2
#ifdef HOLD_OFF_TIME
static unsigned long hold_off_timer;
#endif

const char *archie_get_rom_name(void)
{
	char *p = strrchr(config.rom_img, '/');
	if (!p) p = config.rom_img; else p++;

	return p;
}

void archie_set_ar(char i)
{
	config.system_ctrl &= ~0b11000000;
	config.system_ctrl |= (i & 3) << 6;
	user_io_status(config.system_ctrl, 0b11000000);
}

int archie_get_ar()
{
	return (config.system_ctrl >> 6) & 3;
}

void archie_set_scale(char i)
{
	config.system_ctrl &= ~0b1100000000;
	config.system_ctrl |= (i & 3) << 8;
	user_io_status(config.system_ctrl, 0b1100000000);
}

int archie_get_scale()
{
	return (config.system_ctrl >> 8) & 3;
}

void archie_set_60(char i)
{
	if (i) config.system_ctrl |=  0b1000;
	else config.system_ctrl   &= ~0b1000;
	user_io_status(config.system_ctrl, 0b10000);
}

int archie_get_60()
{
	return config.system_ctrl & 0b1000;
}

void archie_set_afix(char i)
{
	if (i) config.system_ctrl |= 0b10000;
	else config.system_ctrl &= ~0b10000;
	user_io_status(config.system_ctrl, 0b100000);
}

int archie_get_afix()
{
	return config.system_ctrl & 0b10000;
}

static int mswap = 0;
void archie_set_mswap(char i)
{
	mswap = i;
}

int archie_get_mswap()
{
	return mswap;
}

void archie_set_amix(char i)
{
	config.system_ctrl = (config.system_ctrl & ~0b110) | ((i & 3)<<1);
	user_io_status(config.system_ctrl << 1, 0b1100);
}

int archie_get_amix()
{
	return (config.system_ctrl>>1) & 3;
}

void archie_save_config(void)
{
	FileSaveConfig(CONFIG_FILENAME, &config, sizeof(config));
}

void archie_set_rom(char *name)
{
	if (!name) return;

	printf("archie_set_rom(%s)\n", name);

	// save file name
	strcpy(config.rom_img, name);
	user_io_file_tx(name, 1);
}

static void archie_kbd_enqueue(unsigned char state, unsigned char byte)
{
	if (QUEUE_NEXT(tx_queue_wptr) == tx_queue_rptr)
	{
		archie_debugf("KBD tx queue overflow");
		return;
	}

	//archie_debugf("KBD ENQUEUE %x (%x)", byte, state);
	tx_queue[tx_queue_wptr][0] = state;
	tx_queue[tx_queue_wptr][1] = byte;
	tx_queue_wptr = QUEUE_NEXT(tx_queue_wptr);
}

static void archie_kbd_tx(unsigned char state, unsigned char byte)
{
	//archie_debugf("KBD TX %x (%x)", byte, state);
	spi_uio_cmd_cont(0x05);
	spi8(byte);
	DisableIO();

	kbd_state = (enum state)state;
	ack_timeout = GetTimer(10);  // 10ms timeout
}

static void archie_kbd_send(unsigned char state, unsigned char byte)
{
	// don't send if we are waiting for an ack
	if ((kbd_state != STATE_WAIT4ACK1) && (kbd_state != STATE_WAIT4ACK2))
		archie_kbd_tx(state, byte);
	else
		archie_kbd_enqueue(state, byte);
}

static void archie_kbd_reset(void)
{
	archie_debugf("KBD reset");
	tx_queue_rptr = tx_queue_wptr = 0;
	kbd_state = STATE_HRST;
	mouse_x = mouse_y = 0;
	flags = 0;
}

#define cmos_name user_io_make_filepath(HomeDir(), "cmos.dat")

static uint8_t rtc[16] = {};
static uint8_t year = 0;
static void update_rtc()
{
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);
	year = tm.tm_year - 100;
	rtc[2] = (tm.tm_sec  % 10) | ((tm.tm_sec  / 10) << 4);
	rtc[3] = (tm.tm_min  % 10) | ((tm.tm_min  / 10) << 4);
	rtc[4] = (tm.tm_hour % 10) | ((tm.tm_hour / 10) << 4);
	rtc[5] = (tm.tm_mday % 10) | ((tm.tm_mday / 10) << 4) | (year << 6);
	rtc[6] = ((tm.tm_mon + 1) % 10) | (((tm.tm_mon + 1) / 10) << 4) | (tm.tm_wday << 5);
}

static void load_cmos()
{
	static uint8_t cmos_data[256] = {};
	if (FileLoad(cmos_name, cmos_data, sizeof(cmos_data)))
	{
		update_rtc();
		memcpy(cmos_data, rtc, 16);
		cmos_data[0xC0] = year;
		cmos_data[0xC1] = 20;

		user_io_set_index(3);
		user_io_set_download(1);
		user_io_file_tx_data(cmos_data, sizeof(cmos_data));
		user_io_set_download(0);
	}
}

static void check_cmos(uint8_t cnt)
{
	static uint8_t old_cnt = 0;
	static uint32_t cmos_timer = 0;
	static uint32_t rtc_timer = 0;

	if (!cmos_timer || CheckTimer(cmos_timer))
	{
		cmos_timer = GetTimer(1000);
		if (old_cnt != cnt)
		{
			old_cnt = cnt;

			static uint8_t cmos_data[256];
			user_io_set_index(3);
			user_io_set_upload(1);
			user_io_file_rx_data(cmos_data, sizeof(cmos_data));
			user_io_set_upload(0);
			memset(cmos_data, 0, 16);
			cmos_data[0xC0] = 0;
			cmos_data[0xC1] = 0;

			static uint8_t cmos_old[256];
			FileLoad(cmos_name, cmos_old, sizeof(cmos_old));
			memset(cmos_old, 0, 16);
			cmos_old[0xC0] = 0;
			cmos_old[0xC1] = 0;

			if (memcmp(cmos_old, cmos_data, 256))
			{
				printf("update cmos.dat\n");
				//hexdump(cmos_data, 256); printf("\n");
				FileSave(cmos_name, cmos_data, sizeof(cmos_data));
			}
		}
	}

	if (!rtc_timer || CheckTimer(rtc_timer))
	{
		rtc_timer = GetTimer(60000);
		update_rtc();
		user_io_set_index(3);
		user_io_set_download(1);
		user_io_file_tx_data(rtc, 16);
		user_io_set_download(0);
	}
}

inline int hdd_open(int unit, char *filename)
{
	return (ide_check() & 0x8000) ? ide_open(unit, filename) : OpenHardfile(unit, filename);
}

void archie_init()
{
	archie_debugf("init");
	user_io_status(UIO_STATUS_RESET, UIO_STATUS_RESET);

	// set config defaults
	config.system_ctrl = 0;
	snprintf(config.rom_img, 1024, user_io_make_filepath(HomeDir(), "RISCOS.ROM"));

	// try to load config from card
	int size = FileLoadConfig(CONFIG_FILENAME, 0, 0);
	if (size>0)
	{
		if (size == sizeof(archie_config_t))
		{
			FileLoadConfig(CONFIG_FILENAME, &config, sizeof(archie_config_t));
		}
		else
			archie_debugf("Unexpected config size %d != %d", size, sizeof(archie_config_t));
	}
	else
		archie_debugf("No %s config found", CONFIG_FILENAME);

	archie_set_ar(archie_get_ar());
	archie_set_amix(archie_get_amix());
	archie_set_60(archie_get_60());
	archie_set_afix(archie_get_afix());

	if (!config.hdd_img[0][0] || !hdd_open(0, config.hdd_img[0]))
	{
		memset(config.hdd_img[0], 0, sizeof(config.hdd_img[0]));
	}

	if (!config.hdd_img[1][0] || !hdd_open(1, config.hdd_img[1]))
	{
		memset(config.hdd_img[1], 0, sizeof(config.hdd_img[1]));
	}

	// upload rom file
	archie_set_rom(config.rom_img);

	// upload ext file
	//user_io_file_tx("Archie/RISCOS.EXT", 2);

	load_cmos();

	user_io_status(~UIO_STATUS_RESET, UIO_STATUS_RESET);

/*
	int i;
	// try to open default floppies
	for (i = 0; i<MAX_FLOPPY; i++)
	{
		char fdc_name[] = "Archie/FLOPPY0.ADF";
		fdc_name[13] = '0' + i;
		if (FileOpen(&floppy[i], fdc_name))
			archie_debugf("Inserted floppy %d with %llu bytes", i, floppy[i].size);
		else
			floppy[i].size = 0;
	}
*/
	archie_kbd_send(STATE_RAK1, HRST);
	ack_timeout = GetTimer(20);  // give archie 20ms to reply
}

void archie_kbd(unsigned short code)
{
	//archie_debugf("KBD key code %x", code);

	// don't send anything yet if we are still in reset state
	if (kbd_state <= STATE_RAK2)
	{
		archie_debugf("KBD still in reset");
		return;
	}

	// ignore any key event if key scanning is disabled
	if (!(flags & FLAG_SCAN_ENABLED))
	{
		archie_debugf("KBD keyboard scan is disabled!");
		return;
	}

	// select prefix for up or down event
	unsigned char prefix = (code & 0x8000) ? KUDA : KDDA;

	archie_kbd_send(STATE_WAIT4ACK1, prefix | (code >> 4));
	archie_kbd_send(STATE_WAIT4ACK2, prefix | (code & 0x0f));
}

void archie_mouse(unsigned char b, int16_t x, int16_t y)
{
	//archie_debugf("KBD MOUSE X:%d Y:%d B:%d", x, y, b);

	// max values -64 .. 63
	mouse_x += x;
	if (mouse_x >  63) mouse_x = 63;
	if (mouse_x < -64) mouse_x = -64;

	mouse_y -= y;
	if (mouse_y >  63) mouse_y = 63;
	if (mouse_y < -64) mouse_y = -64;

	// don't send anything yet if we are still in reset state
	if (kbd_state <= STATE_RAK2)
	{
		archie_debugf("KBD still in reset");
		return;
	}

	// ignore any mouse movement if mouse is disabled or if nothing to report
	if ((flags & FLAG_MOUSE_ENABLED) && (mouse_x || mouse_y))
	{
		// send asap if no pending byte
		if (kbd_state == STATE_IDLE) {
			archie_kbd_send(STATE_WAIT4ACK1, mouse_x & 0x7f);
			archie_kbd_send(STATE_WAIT4ACK2, mouse_y & 0x7f);
			mouse_x = mouse_y = 0;
		}
	}

	// ignore mouse buttons if key scanning is disabled
	if (flags & FLAG_SCAN_ENABLED)
	{
		static const uint8_t remap[] = { 0, 2, 1 };
		static unsigned char buts = 0;
		uint8_t s;

		// map all three buttons
		for (s = 0; s<3; s++)
		{
			uint8_t mask = (1 << s);
			if ((b&mask) != (buts&mask))
			{
				unsigned char prefix = (b&mask) ? KDDA : KUDA;
				archie_kbd_send(STATE_WAIT4ACK1, prefix | 0x07);
				archie_kbd_send(STATE_WAIT4ACK2, prefix | ((!mswap ^ !(get_key_mod() & (RGUI|LGUI))) ? s : remap[s]));
			}
		}
		buts = b;
	}
}

static void archie_check_queue(void)
{
	if (tx_queue_rptr == tx_queue_wptr)
		return;

	archie_kbd_tx(tx_queue[tx_queue_rptr][0], tx_queue[tx_queue_rptr][1]);
	tx_queue_rptr = QUEUE_NEXT(tx_queue_rptr);
}

static void check_reset()
{
	static uint32_t timer = 0;

	// check the user button
	if (!user_io_osd_is_visible() && (user_io_user_button() || user_io_get_kbd_reset()))
	{
		if (!timer) timer = GetTimer(1000);
		else if (timer != 1)
		{
			if (CheckTimer(timer))
			{
				archie_set_rom(config.rom_img);
				timer = 1;
			}
		}
	}
	else
	{
		timer = 0;
	}
}

void archie_poll(void)
{
	EnableFpga();
	uint16_t status = spi_w(0);
	DisableFpga();

	uint16_t sd_req = ide_check();
	if (sd_req & 0x8000) ide_io(0, sd_req & 7);
	else HandleHDD(status >> 8, 0);

	check_cmos(status);
	check_reset();

#ifdef HOLD_OFF_TIME
	if ((kbd_state == STATE_HOLD_OFF) && CheckTimer(hold_off_timer)) {
		archie_debugf("KBD resume after hold off");
		kbd_state = STATE_IDLE;
		archie_check_queue();
	}
#endif

	// timeout waiting for ack?
	if ((kbd_state == STATE_WAIT4ACK1) || (kbd_state == STATE_WAIT4ACK2)) {
		if (CheckTimer(ack_timeout)) {
			if (kbd_state == STATE_WAIT4ACK1)
				archie_debugf(">>>> KBD ACK TIMEOUT 1ST BYTE <<<<");
			if (kbd_state == STATE_WAIT4ACK2)
				archie_debugf(">>>> KBD ACK TIMEOUT 2ND BYTE <<<<");

			kbd_state = STATE_IDLE;
		}
	}

	// timeout in reset sequence?
	if (kbd_state <= STATE_RAK2)
	{
		if (CheckTimer(ack_timeout))
		{
			//archie_debugf("KBD timeout in reset state");

			archie_kbd_send(STATE_RAK1, HRST);
			ack_timeout = GetTimer(20);  // 20ms timeout
		}
	}

	spi_uio_cmd_cont(0x04);
	if (spi_in() == 0xa1)
	{
		unsigned char data = spi_in();
		DisableIO();

		//archie_debugf("KBD RX %x", data);

		switch (data) {
			// arm requests reset
		case HRST:
			archie_kbd_reset();
			archie_kbd_send(STATE_RAK1, HRST);
			ack_timeout = GetTimer(20);  // 20ms timeout
			break;

			// arm sends reset ack 1
		case RAK1:
			if (kbd_state == STATE_RAK1) {
				archie_kbd_send(STATE_RAK2, RAK1);
				ack_timeout = GetTimer(20);  // 20ms timeout
			}
			else
				kbd_state = STATE_HRST;
			break;

			// arm sends reset ack 2
		case RAK2:
			if (kbd_state == STATE_RAK2) {
				archie_kbd_send(STATE_IDLE, RAK2);
				ack_timeout = GetTimer(20);  // 20ms timeout
			}
			else
				kbd_state = STATE_HRST;
			break;

			// arm request keyboard id
		case RQID:
			archie_kbd_send(STATE_IDLE, KBID | 1);
			break;

			// arm acks first byte
		case BACK:
			if (kbd_state != STATE_WAIT4ACK1) {
				archie_debugf("KBD unexpected BACK, resetting KBD");
				kbd_state = STATE_HRST;
			}
			else {
#ifdef HOLD_OFF_TIME
				// wait some time before sending next byte
				archie_debugf("KBD starting hold off");
				kbd_state = STATE_HOLD_OFF;
				hold_off_timer = GetTimer(10);
				// wait some time before sending next byte
				archie_debugf("KBD starting hold off");
				kbd_state = STATE_HOLD_OFF;
				hold_off_timer = GetTimer(10);
#else
				kbd_state = STATE_IDLE;
				archie_check_queue();
				kbd_state = STATE_IDLE;
				archie_check_queue();
#endif
			}
			break;

			// arm acks second byte
		case NACK:
		case SACK:
		case MACK:
		case SMAK:

			if (((data == SACK) || (data == SMAK)) && !(flags & FLAG_SCAN_ENABLED)) {
				archie_debugf("KBD Enabling key scanning");
				flags |= FLAG_SCAN_ENABLED;
			}

			if (((data == NACK) || (data == MACK)) && (flags & FLAG_SCAN_ENABLED)) {
				archie_debugf("KBD Disabling key scanning");
				flags &= ~FLAG_SCAN_ENABLED;
			}

			if (((data == MACK) || (data == SMAK)) && !(flags & FLAG_MOUSE_ENABLED)) {
				archie_debugf("KBD Enabling mouse");
				flags |= FLAG_MOUSE_ENABLED;
			}

			if (((data == NACK) || (data == SACK)) && (flags & FLAG_MOUSE_ENABLED)) {
				archie_debugf("KBD Disabling mouse");
				flags &= ~FLAG_MOUSE_ENABLED;
			}

			// wait another 10ms before sending next byte
#ifdef HOLD_OFF_TIME
			archie_debugf("KBD starting hold off");
			kbd_state = STATE_HOLD_OFF;
			hold_off_timer = GetTimer(10);
#else
			kbd_state = STATE_IDLE;
			archie_check_queue();
#endif
			break;
		}
	}
	else
		DisableIO();
}

const char *archie_get_hdd_name(int i)
{
	if (!config.hdd_img[i][0]) return NULL;

	char *p = strrchr(config.hdd_img[i], '/');
	if (!p) p = config.hdd_img[i]; else p++;

	return p;
}

void archie_hdd_mount(char *filename, int idx)
{
	memset(config.hdd_img[idx], 0, sizeof(config.hdd_img[idx]));
	if (hdd_open(idx, filename)) strcpy(config.hdd_img[idx], filename);
}
