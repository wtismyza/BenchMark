/*
 *  drivers/media/radio/radio-si470x.c
 *
 *  Driver for USB radios for the Silicon Labs Si470x FM Radio Receivers:
 *   - Silicon Labs USB FM Radio Reference Design
 *   - ADS/Tech FM Radio Receiver (formerly Instant FM Music) (RDX-155-EF)
 *
 *  Copyright (c) 2008 Tobias Lorenz <tobias.lorenz@gmx.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


/*
 * History:
 * 2008-01-12	Tobias Lorenz <tobias.lorenz@gmx.net>
 *		Version 1.0.0
 *		- First working version
 * 2008-01-13   Tobias Lorenz <tobias.lorenz@gmx.net>
 *		Version 1.0.1
 *		- Improved error handling, every function now returns errno
 *		- Improved multi user access (start/mute/stop)
 *		- Channel doesn't get lost anymore after start/mute/stop
 *		- RDS support added (polling mode via interrupt EP 1)
 *		- marked default module parameters with *value*
 *		- switched from bit structs to bit masks
 *		- header file cleaned and integrated
 * 2008-01-14	Tobias Lorenz <tobias.lorenz@gmx.net>
 * 		Version 1.0.2
 * 		- hex values are now lower case
 * 		- commented USB ID for ADS/Tech moved on todo list
 * 		- blacklisted si470x in hid-quirks.c
 * 		- rds buffer handling functions integrated into *_work, *_read
 * 		- rds_command in si470x_poll exchanged against simple retval
 * 		- check for firmware version 15
 * 		- code order and prototypes still remain the same
 * 		- spacing and bottom of band codes remain the same
 * 2008-01-16	Tobias Lorenz <tobias.lorenz@gmx.net>
 *		Version 1.0.3
 * 		- code reordered to avoid function prototypes
 *		- switch/case defaults are now more user-friendly
 *		- unified comment style
 *		- applied all checkpatch.pl v1.12 suggestions
 *		  except the warning about the too long lines with bit comments
 *		- renamed FMRADIO to RADIO to cut line length (checkpatch.pl)
 * 2008-01-22	Tobias Lorenz <tobias.lorenz@gmx.net>
 *		Version 1.0.4
 *		- avoid poss. locking when doing copy_to_user which may sleep
 *		- RDS is automatically activated on read now
 *		- code cleaned of unnecessary rds_commands
 *		- USB Vendor/Product ID for ADS/Tech FM Radio Receiver verified
 *		  (thanks to Guillaume RAMOUSSE)
 *
 * ToDo:
 * - add seeking support
 * - add firmware download/update support
 * - RDS support: interrupt mode, instead of polling
 * - add LED status output (check if that's not already done in firmware)
 */


/* driver definitions */
#define DRIVER_AUTHOR "Tobias Lorenz <tobias.lorenz@gmx.net>"
#define DRIVER_NAME "radio-si470x"
#define DRIVER_VERSION KERNEL_VERSION(1, 0, 4)
#define DRIVER_CARD "Silicon Labs Si470x FM Radio Receiver"
#define DRIVER_DESC "USB radio driver for Si470x FM Radio Receivers"


/* kernel includes */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/version.h>
#include <linux/videodev2.h>
#include <media/v4l2-common.h>
#include <media/rds.h>


/* USB Device ID List */
static struct usb_device_id si470x_usb_driver_id_table[] = {
	/* Silicon Labs USB FM Radio Reference Design */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x10c4, 0x818a,	USB_CLASS_HID, 0, 0) },
	/* ADS/Tech FM Radio Receiver (formerly Instant FM Music) */
	{ USB_DEVICE_AND_INTERFACE_INFO(0x06e1, 0xa155,	USB_CLASS_HID, 0, 0) },
	/* Terminating entry */
	{ }
};
MODULE_DEVICE_TABLE(usb, si470x_usb_driver_id_table);



/**************************************************************************
 * Module Parameters
 **************************************************************************/

/* Radio Nr */
static int radio_nr = -1;
module_param(radio_nr, int, 0);
MODULE_PARM_DESC(radio_nr, "Radio Nr");

/* Spacing (kHz) */
/* 0: 200 kHz (USA, Australia) */
/* 1: 100 kHz (Europe, Japan) */
/* 2:  50 kHz */
static int space = 2;
module_param(space, int, 0);
MODULE_PARM_DESC(radio_nr, "Spacing: 0=200kHz 1=100kHz *2=50kHz*");

/* Bottom of Band (MHz) */
/* 0: 87.5 - 108 MHz (USA, Europe)*/
/* 1: 76   - 108 MHz (Japan wide band) */
/* 2: 76   -  90 MHz (Japan) */
static int band = 1;
module_param(band, int, 0);
MODULE_PARM_DESC(radio_nr, "Band: 0=87.5..108MHz *1=76..108MHz* 2=76..90MHz");

/* De-emphasis */
/* 0: 75 us (USA) */
/* 1: 50 us (Europe, Australia, Japan) */
static int de = 1;
module_param(de, int, 0);
MODULE_PARM_DESC(radio_nr, "De-emphasis: 0=75us *1=50us*");

/* USB timeout */
static int usb_timeout = 500;
module_param(usb_timeout, int, 0);
MODULE_PARM_DESC(usb_timeout, "USB timeout (ms): *500*");

/* Seek retries */
static int seek_retries = 100;
module_param(seek_retries, int, 0);
MODULE_PARM_DESC(seek_retries, "Seek retries: *100*");

/* RDS buffer blocks */
static int rds_buf = 100;
module_param(rds_buf, int, 0);
MODULE_PARM_DESC(rds_buf, "RDS buffer entries: *100*");

/* RDS maximum block errors */
static int max_rds_errors = 1;
/* 0 means   0  errors requiring correction */
/* 1 means 1-2  errors requiring correction (used by original USBRadio.exe) */
/* 2 means 3-5  errors requiring correction */
/* 3 means   6+ errors or errors in checkword, correction not possible */
module_param(max_rds_errors, int, 0);
MODULE_PARM_DESC(max_rds_errors, "RDS maximum block errors: *1*");

/* RDS poll frequency */
static int rds_poll_time = 40;
/* 40 is used by the original USBRadio.exe */
/* 50 is used by radio-cadet */
/* 75 should be okay */
/* 80 is the usual RDS receive interval */
module_param(rds_poll_time, int, 0);
MODULE_PARM_DESC(rds_poll_time, "RDS poll time (ms): *40*");



/**************************************************************************
 * Register Definitions
 **************************************************************************/
#define RADIO_REGISTER_SIZE	2	/* 16 register bit width */
#define RADIO_REGISTER_NUM	16	/* DEVICEID   ... RDSD */
#define RDS_REGISTER_NUM	6	/* STATUSRSSI ... RDSD */

#define DEVICEID		0	/* Device ID */
#define DEVICEID_PN		0xf000	/* bits 15..12: Part Number */
#define DEVICEID_MFGID		0x0fff	/* bits 11..00: Manufacturer ID */

#define CHIPID			1	/* Chip ID */
#define CHIPID_REV		0xfc00	/* bits 15..10: Chip Version */
#define CHIPID_DEV		0x0200	/* bits 09..09: Device */
#define CHIPID_FIRMWARE		0x01ff	/* bits 08..00: Firmware Version */

#define POWERCFG		2	/* Power Configuration */
#define POWERCFG_DSMUTE		0x8000	/* bits 15..15: Softmute Disable */
#define POWERCFG_DMUTE		0x4000	/* bits 14..14: Mute Disable */
#define POWERCFG_MONO		0x2000	/* bits 13..13: Mono Select */
#define POWERCFG_RDSM		0x0800	/* bits 11..11: RDS Mode (Si4701 only) */
#define POWERCFG_SKMODE		0x0400	/* bits 10..10: Seek Mode */
#define POWERCFG_SEEKUP		0x0200	/* bits 09..09: Seek Direction */
#define POWERCFG_SEEK		0x0100	/* bits 08..08: Seek */
#define POWERCFG_DISABLE	0x0040	/* bits 06..06: Powerup Disable */
#define POWERCFG_ENABLE		0x0001	/* bits 00..00: Powerup Enable */

#define CHANNEL			3	/* Channel */
#define CHANNEL_TUNE		0x8000	/* bits 15..15: Tune */
#define CHANNEL_CHAN		0x03ff	/* bits 09..00: Channel Select */

#define SYSCONFIG1		4	/* System Configuration 1 */
#define SYSCONFIG1_RDSIEN	0x8000	/* bits 15..15: RDS Interrupt Enable (Si4701 only) */
#define SYSCONFIG1_STCIEN	0x4000	/* bits 14..14: Seek/Tune Complete Interrupt Enable */
#define SYSCONFIG1_RDS		0x1000	/* bits 12..12: RDS Enable (Si4701 only) */
#define SYSCONFIG1_DE		0x0800	/* bits 11..11: De-emphasis (0=75us 1=50us) */
#define SYSCONFIG1_AGCD		0x0400	/* bits 10..10: AGC Disable */
#define SYSCONFIG1_BLNDADJ	0x00c0	/* bits 07..06: Stereo/Mono Blend Level Adjustment */
#define SYSCONFIG1_GPIO3	0x0030	/* bits 05..04: General Purpose I/O 3 */
#define SYSCONFIG1_GPIO2	0x000c	/* bits 03..02: General Purpose I/O 2 */
#define SYSCONFIG1_GPIO1	0x0003	/* bits 01..00: General Purpose I/O 1 */

#define SYSCONFIG2		5	/* System Configuration 2 */
#define SYSCONFIG2_SEEKTH	0xff00	/* bits 15..08: RSSI Seek Threshold */
#define SYSCONFIG2_BAND		0x0080	/* bits 07..06: Band Select */
#define SYSCONFIG2_SPACE	0x0030	/* bits 05..04: Channel Spacing */
#define SYSCONFIG2_VOLUME	0x000f	/* bits 03..00: Volume */

#define SYSCONFIG3		6	/* System Configuration 3 */
#define SYSCONFIG3_SMUTER	0xc000	/* bits 15..14: Softmute Attack/Recover Rate */
#define SYSCONFIG3_SMUTEA	0x3000	/* bits 13..12: Softmute Attenuation */
#define SYSCONFIG3_SKSNR	0x00f0	/* bits 07..04: Seek SNR Threshold */
#define SYSCONFIG3_SKCNT	0x000f	/* bits 03..00: Seek FM Impulse Detection Threshold */

#define TEST1			7	/* Test 1 */
#define TEST1_AHIZEN		0x4000	/* bits 14..14: Audio High-Z Enable */

#define TEST2			8	/* Test 2 */
/* TEST2 only contains reserved bits */

#define BOOTCONFIG		9	/* Boot Configuration */
/* BOOTCONFIG only contains reserved bits */

#define STATUSRSSI		10	/* Status RSSI */
#define STATUSRSSI_RDSR		0x8000	/* bits 15..15: RDS Ready (Si4701 only) */
#define STATUSRSSI_STC		0x4000	/* bits 14..14: Seek/Tune Complete */
#define STATUSRSSI_SF		0x2000	/* bits 13..13: Seek Fail/Band Limit */
#define STATUSRSSI_AFCRL	0x1000	/* bits 12..12: AFC Rail */
#define STATUSRSSI_RDSS		0x0800	/* bits 11..11: RDS Synchronized (Si4701 only) */
#define STATUSRSSI_BLERA	0x0600	/* bits 10..09: RDS Block A Errors (Si4701 only) */
#define STATUSRSSI_ST		0x0100	/* bits 08..08: Stereo Indicator */
#define STATUSRSSI_RSSI		0x00ff	/* bits 07..00: RSSI (Received Signal Strength Indicator) */

#define READCHAN		11	/* Read Channel */
#define READCHAN_BLERB		0xc000	/* bits 15..14: RDS Block D Errors (Si4701 only) */
#define READCHAN_BLERC		0x3000	/* bits 13..12: RDS Block C Errors (Si4701 only) */
#define READCHAN_BLERD		0x0c00	/* bits 11..10: RDS Block B Errors (Si4701 only) */
#define READCHAN_READCHAN	0x03ff	/* bits 09..00: Read Channel */

#define RDSA			12	/* RDSA */
#define RDSA_RDSA		0xffff	/* bits 15..00: RDS Block A Data (Si4701 only) */

#define RDSB			13	/* RDSB */
#define RDSB_RDSB		0xffff	/* bits 15..00: RDS Block B Data (Si4701 only) */

#define RDSC			14	/* RDSC */
#define RDSC_RDSC		0xffff	/* bits 15..00: RDS Block C Data (Si4701 only) */

#define RDSD			15	/* RDSD */
#define RDSD_RDSD		0xffff	/* bits 15..00: RDS Block D Data (Si4701 only) */



/**************************************************************************
 * USB HID Reports
 **************************************************************************/

/* Reports 1-16 give direct read/write access to the 16 Si470x registers */
/* with the (REPORT_ID - 1) corresponding to the register address across USB */
/* endpoint 0 using GET_REPORT and SET_REPORT */
#define REGISTER_REPORT_SIZE	(RADIO_REGISTER_SIZE + 1)
#define REGISTER_REPORT(reg)	((reg) + 1)

/* Report 17 gives direct read/write access to the entire Si470x register */
/* map across endpoint 0 using GET_REPORT and SET_REPORT */
#define ENTIRE_REPORT_SIZE	(RADIO_REGISTER_NUM * RADIO_REGISTER_SIZE + 1)
#define ENTIRE_REPORT		17

/* Report 18 is used to send the lowest 6 Si470x registers up the HID */
/* interrupt endpoint 1 to Windows every 20 milliseconds for status */
#define RDS_REPORT_SIZE		(RDS_REGISTER_NUM * RADIO_REGISTER_SIZE + 1)
#define RDS_REPORT		18

/* Report 19: LED state */
#define LED_REPORT_SIZE		3
#define LED_REPORT		19

/* Report 19: stream */
#define STREAM_REPORT_SIZE	3
#define	STREAM_REPORT		19

/* Report 20: scratch */
#define SCRATCH_PAGE_SIZE	63
#define SCRATCH_REPORT_SIZE	(SCRATCH_PAGE_SIZE + 1)
#define SCRATCH_REPORT		20

/* Reports 19-22: flash upgrade of the C8051F321 */
#define WRITE_REPORT		19
#define FLASH_REPORT		20
#define CRC_REPORT		21
#define RESPONSE_REPORT		22

/* Report 23: currently unused, but can accept 60 byte reports on the HID */
/* interrupt out endpoint 2 every 1 millisecond */
#define UNUSED_REPORT		23



/**************************************************************************
 * Software/Hardware Versions
 **************************************************************************/
#define RADIO_SW_VERSION_NOT_BOOTLOADABLE	6
#define RADIO_SW_VERSION			7
#define RADIO_SW_VERSION_CURRENT		15
#define RADIO_HW_VERSION			1

#define SCRATCH_PAGE_SW_VERSION	1
#define SCRATCH_PAGE_HW_VERSION	2



/**************************************************************************
 * LED State Definitions
 **************************************************************************/
#define LED_COMMAND		0x35

#define NO_CHANGE_LED		0x00
#define ALL_COLOR_LED		0x01	/* streaming state */
#define BLINK_GREEN_LED		0x02	/* connect state */
#define BLINK_RED_LED		0x04
#define BLINK_ORANGE_LED	0x10	/* disconnect state */
#define SOLID_GREEN_LED		0x20	/* tuning/seeking state */
#define SOLID_RED_LED		0x40	/* bootload state */
#define SOLID_ORANGE_LED	0x80



/**************************************************************************
 * Stream State Definitions
 **************************************************************************/
#define STREAM_COMMAND	0x36
#define STREAM_VIDPID	0x00
#define STREAM_AUDIO	0xff



/**************************************************************************
 * Bootloader / Flash Commands
 **************************************************************************/

/* unique id sent to bootloader and required to put into a bootload state */
#define UNIQUE_BL_ID		0x34

/* mask for the flash data */
#define FLASH_DATA_MASK		0x55

/* bootloader commands */
#define GET_SW_VERSION_COMMAND	0x00
#define	SET_PAGE_COMMAND	0x01
#define ERASE_PAGE_COMMAND	0x02
#define WRITE_PAGE_COMMAND	0x03
#define CRC_ON_PAGE_COMMAND	0x04
#define READ_FLASH_BYTE_COMMAND	0x05
#define RESET_DEVICE_COMMAND	0x06
#define GET_HW_VERSION_COMMAND	0x07
#define BLANK			0xff

/* bootloader command responses */
#define COMMAND_OK		0x01
#define COMMAND_FAILED		0x02
#define COMMAND_PENDING		0x03

/* buffer sizes */
#define COMMAND_BUFFER_SIZE	4
#define RESPONSE_BUFFER_SIZE	2
#define FLASH_BUFFER_SIZE	64
#define CRC_BUFFER_SIZE		3



/**************************************************************************
 * General Driver Definitions
 **************************************************************************/

/*
 * si470x_device - private data
 */
struct si470x_device {
	/* reference to USB and video device */
	struct usb_device *usbdev;
	struct video_device *videodev;

	/* are these really necessary ? */
	int users;

	/* report buffer (maximum 64 bytes) */
	unsigned char buf[64];

	/* Silabs internal registers (0..15) */
	unsigned short registers[RADIO_REGISTER_NUM];

	/* RDS receive buffer */
	struct work_struct work;
	wait_queue_head_t read_queue;
	struct timer_list timer;
	spinlock_t lock;		/* buffer locking */
	unsigned char *buffer;		/* size is always multiple of three */
	unsigned int buf_size;
	unsigned int rd_index;
	unsigned int wr_index;
};


/*
 * The frequency is set in units of 62.5 Hz when using V4L2_TUNER_CAP_LOW,
 * 62.5 kHz otherwise.
 * The tuner is able to have a channel spacing of 50, 100 or 200 kHz.
 * tuner->capability is therefore set to V4L2_TUNER_CAP_LOW
 * The FREQ_MUL is then: 1 MHz / 62.5 Hz = 16000
 */
#define FREQ_MUL (1000000 / 62.5)



/**************************************************************************
 * General Driver Functions
 **************************************************************************/

/*
 * si470x_get_report - receive a HID report
 */
static int si470x_get_report(struct si470x_device *radio, int size)
{
	return usb_control_msg(radio->usbdev,
		usb_rcvctrlpipe(radio->usbdev, 0),
		HID_REQ_GET_REPORT,
		USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN,
		radio->buf[0], 2,
		radio->buf, size, usb_timeout);
}


/*
 * si470x_set_report - send a HID report
 */
static int si470x_set_report(struct si470x_device *radio, int size)
{
	return usb_control_msg(radio->usbdev,
		usb_sndctrlpipe(radio->usbdev, 0),
		HID_REQ_SET_REPORT,
		USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
		radio->buf[0], 2,
		radio->buf, size, usb_timeout);
}


/*
 * si470x_get_register - read register
 */
static int si470x_get_register(struct si470x_device *radio, int regnr)
{
	int retval;

	radio->buf[0] = REGISTER_REPORT(regnr);

	retval = si470x_get_report(radio, REGISTER_REPORT_SIZE);
	if (retval >= 0)
		radio->registers[regnr] = (radio->buf[1] << 8) | radio->buf[2];

	return (retval < 0) ? -EINVAL : 0;
}


/*
 * si470x_set_register - write register
 */
static int si470x_set_register(struct si470x_device *radio, int regnr)
{
	int retval;

	radio->buf[0] = REGISTER_REPORT(regnr);
	radio->buf[1] = (radio->registers[regnr] & 0xff00) >> 8;
	radio->buf[2] = (radio->registers[regnr] & 0x00ff);

	retval = si470x_set_report(radio, REGISTER_REPORT_SIZE);

	return (retval < 0) ? -EINVAL : 0;
}


/*
 * si470x_get_all_registers - read entire registers
 */
static int si470x_get_all_registers(struct si470x_device *radio)
{
	int retval;
	int regnr;

	radio->buf[0] = ENTIRE_REPORT;

	retval = si470x_get_report(radio, ENTIRE_REPORT_SIZE);

	if (retval >= 0)
		for (regnr = 0; regnr < RADIO_REGISTER_NUM; regnr++)
			radio->registers[regnr] =
			(radio->buf[regnr * RADIO_REGISTER_SIZE + 1] << 8) |
			 radio->buf[regnr * RADIO_REGISTER_SIZE + 2];

	return (retval < 0) ? -EINVAL : 0;
}


/*
 * si470x_get_rds_registers - read rds registers
 */
static int si470x_get_rds_registers(struct si470x_device *radio)
{
	int retval;
	int regnr;
	int size;

	radio->buf[0] = RDS_REPORT;

	retval = usb_interrupt_msg(radio->usbdev,
		usb_rcvctrlpipe(radio->usbdev, 1),
		radio->buf, RDS_REPORT_SIZE, &size, usb_timeout);

	if (retval >= 0)
		for (regnr = 0; regnr < RDS_REGISTER_NUM; regnr++)
			radio->registers[STATUSRSSI + regnr] =
			(radio->buf[regnr * RADIO_REGISTER_SIZE + 1] << 8) |
			 radio->buf[regnr * RADIO_REGISTER_SIZE + 2];

	return (retval < 0) ? -EINVAL : 0;
}


/*
 * si470x_set_chan - set the channel
 */
static int si470x_set_chan(struct si470x_device *radio, int chan)
{
	int retval, i;

	/* start tuning */
	radio->registers[CHANNEL] &= ~CHANNEL_CHAN;
	radio->registers[CHANNEL] |= CHANNEL_TUNE | chan;
	retval = si470x_set_register(radio, CHANNEL);
	if (retval < 0)
		return retval;

	/* wait till seek operation has completed */
	i = 0;
	do {
		retval = si470x_get_register(radio, STATUSRSSI);
		if (retval < 0)
			return retval;
	} while ((radio->registers[STATUSRSSI] & STATUSRSSI_STC) &&
		(++i < seek_retries));
	if (i >= seek_retries)
		printk(KERN_WARNING DRIVER_NAME
			": seek does not finish after %d tries\n", i);

	/* stop tuning */
	radio->registers[CHANNEL] &= ~CHANNEL_TUNE;
	return si470x_set_register(radio, CHANNEL);
}


/*
 * si470x_get_freq - get the frequency
 */
static int si470x_get_freq(struct si470x_device *radio)
{
	int spacing, band_bottom, chan, freq;
	int retval;

	/* Spacing (kHz) */
	switch (space) {
	/* 0: 200 kHz (USA, Australia) */
	case 0 : spacing = 0.200 * FREQ_MUL; break;
	/* 1: 100 kHz (Europe, Japan) */
	case 1 : spacing = 0.100 * FREQ_MUL; break;
	/* 2:  50 kHz */
	default: spacing = 0.050 * FREQ_MUL; break;
	};

	/* Bottom of Band (MHz) */
	switch (band) {
	/* 0: 87.5 - 108 MHz (USA, Europe) */
	case 0 : band_bottom = 87.5 * FREQ_MUL; break;
	/* 1: 76   - 108 MHz (Japan wide band) */
	default: band_bottom = 76   * FREQ_MUL; break;
	/* 2: 76   -  90 MHz (Japan) */
	case 2 : band_bottom = 76   * FREQ_MUL; break;
	};

	/* read channel */
	retval = si470x_get_register(radio, READCHAN);
	if (retval < 0)
		return retval;
	chan = radio->registers[READCHAN] & READCHAN_READCHAN;

	/* Frequency (MHz) = Spacing (kHz) x Channel + Bottom of Band (MHz) */
	freq = chan * spacing + band_bottom;

	return freq;
}


/*
 * si470x_set_freq - set the frequency
 */
static int si470x_set_freq(struct si470x_device *radio, int freq)
{
	int spacing, band_bottom, chan;

	/* Spacing (kHz) */
	switch (space) {
	/* 0: 200 kHz (USA, Australia) */
	case 0 : spacing = 0.200 * FREQ_MUL; break;
	/* 1: 100 kHz (Europe, Japan) */
	case 1 : spacing = 0.100 * FREQ_MUL; break;
	/* 2:  50 kHz */
	default: spacing = 0.050 * FREQ_MUL; break;
	};

	/* Bottom of Band (MHz) */
	switch (band) {
	/* 0: 87.5 - 108 MHz (USA, Europe) */
	case 0 : band_bottom = 87.5 * FREQ_MUL; break;
	/* 1: 76   - 108 MHz (Japan wide band) */
	default: band_bottom = 76   * FREQ_MUL; break;
	/* 2: 76   -  90 MHz (Japan) */
	case 2 : band_bottom = 76   * FREQ_MUL; break;
	};

	/* Chan = [ Freq (Mhz) - Bottom of Band (MHz) ] / Spacing (kHz) */
	chan = (freq - band_bottom) / spacing;

	return si470x_set_chan(radio, chan);
}


/*
 * si470x_start - switch on radio
 */
static int si470x_start(struct si470x_device *radio)
{
	int retval;

	/* powercfg */
	radio->registers[POWERCFG] =
		POWERCFG_DMUTE | POWERCFG_ENABLE | POWERCFG_RDSM;
	retval = si470x_set_register(radio, POWERCFG);
	if (retval < 0)
		return retval;

	/* sysconfig 1 */
	radio->registers[SYSCONFIG1] = SYSCONFIG1_DE;
	retval = si470x_set_register(radio, SYSCONFIG1);
	if (retval < 0)
		return retval;

	/* sysconfig 2 */
	radio->registers[SYSCONFIG2] =
		(0x3f  << 8) |	/* SEEKTH */
		(band  << 6) |	/* BAND */
		(space << 4) |	/* SPACE */
		15;		/* VOLUME (max) */
	retval = si470x_set_register(radio, SYSCONFIG2);
	if (retval < 0)
		return retval;

	/* reset last channel */
	return si470x_set_chan(radio,
		radio->registers[CHANNEL] & CHANNEL_CHAN);
}


/*
 * si470x_stop - switch off radio
 */
static int si470x_stop(struct si470x_device *radio)
{
	int retval;

	/* sysconfig 1 */
	radio->registers[SYSCONFIG1] &= ~SYSCONFIG1_RDS;
	retval = si470x_set_register(radio, SYSCONFIG1);
	if (retval < 0)
		return retval;

	/* powercfg */
	radio->registers[POWERCFG] &= ~POWERCFG_DMUTE;
	/* POWERCFG_ENABLE has to automatically go low */
	radio->registers[POWERCFG] |= POWERCFG_ENABLE |	POWERCFG_DISABLE;
	return si470x_set_register(radio, POWERCFG);
}


/*
 * si470x_rds_on - switch on rds reception
 */
static int si470x_rds_on(struct si470x_device *radio)
{
	/* sysconfig 1 */
	radio->registers[SYSCONFIG1] |= SYSCONFIG1_RDS;
	return si470x_set_register(radio, SYSCONFIG1);
}



/**************************************************************************
 * RDS Driver Functions
 **************************************************************************/

/*
 * si470x_rds - rds processing function
 */
static void si470x_rds(struct si470x_device *radio)
{
	unsigned char tmpbuf[3];
	unsigned char blocknum;
	unsigned char bler; /* rds block errors */
	unsigned short rds;
	unsigned int i;

	/* get rds blocks */
	if (si470x_get_rds_registers(radio) < 0)
		return;
	if ((radio->registers[STATUSRSSI] & STATUSRSSI_RDSR) == 0) {
		/* No RDS group ready */
		return;
	}
	if ((radio->registers[STATUSRSSI] & STATUSRSSI_RDSS) == 0) {
		/* RDS decoder not synchronized */
		return;
	}

	/* copy four RDS blocks to internal buffer */
	if (spin_trylock(&radio->lock)) {
		/* process each rds block */
		for (blocknum = 0; blocknum < 4; blocknum++) {
			switch (blocknum) {
			default:
				bler = (radio->registers[STATUSRSSI] &
						STATUSRSSI_BLERA) >> 9;
				rds = radio->registers[RDSA];
				break;
			case 1:
				bler = (radio->registers[READCHAN] &
						READCHAN_BLERB) >> 14;
				rds = radio->registers[RDSB];
				break;
			case 2:
				bler = (radio->registers[READCHAN] &
						READCHAN_BLERC) >> 12;
				rds = radio->registers[RDSC];
				break;
			case 3:
				bler = (radio->registers[READCHAN] &
						READCHAN_BLERD) >> 10;
				rds = radio->registers[RDSD];
				break;
			};

			/* Fill the V4L2 RDS buffer */
			tmpbuf[0] = rds & 0x00ff;	/* LSB */
			tmpbuf[1] = (rds & 0xff00) >> 8;/* MSB */
			tmpbuf[2] = blocknum;		/* offset name */
			tmpbuf[2] |= blocknum << 3;	/* received offset */
			if (bler > max_rds_errors)
				tmpbuf[2] |= 0x80; /* uncorrectable errors */
			else if (bler > 0)
				tmpbuf[2] |= 0x40; /* corrected error(s) */

			/* copy RDS block to internal buffer */
			for (i = 0; i < 3; i++) {
				radio->buffer[radio->wr_index] = tmpbuf[i];
				radio->wr_index++;
			}

			/* wrap write pointer */
			if (radio->wr_index >= radio->buf_size)
				radio->wr_index = 0;

			/* check for overflow */
			if (radio->wr_index == radio->rd_index) {
				/* increment and wrap read pointer */
				radio->rd_index += 3;
				if (radio->rd_index >= radio->buf_size)
					radio->rd_index = 0;
			}
		}
		spin_unlock(&radio->lock);
	}

	/* wake up read queue */
	if (radio->wr_index != radio->rd_index)
		wake_up_interruptible(&radio->read_queue);
}


/*
 * si470x_timer - rds timer function
 */
static void si470x_timer(unsigned long data)
{
	struct si470x_device *radio = (struct si470x_device *) data;

	schedule_work(&radio->work);
}


/*
 * si470x_work - rds work function
 */
static void si470x_work(struct work_struct *work)
{
	struct si470x_device *radio = container_of(work, struct si470x_device,
		work);

	if ((radio->registers[SYSCONFIG1] & SYSCONFIG1_RDS) == 0)
		return;

	si470x_rds(radio);
	mod_timer(&radio->timer, jiffies + msecs_to_jiffies(rds_poll_time));
}



/**************************************************************************
 * File Operations Interface
 **************************************************************************/

/*
 * si470x_fops_read - read RDS data
 */
static ssize_t si470x_fops_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));
	int retval = 0;
	unsigned int block_count = 0;

	/* switch on rds reception */
	if ((radio->registers[SYSCONFIG1] & SYSCONFIG1_RDS) == 0) {
		si470x_rds_on(radio);
		schedule_work(&radio->work);
	}

	/* block if no new data available */
	while (radio->wr_index == radio->rd_index) {
		if (file->f_flags & O_NONBLOCK)
			return -EWOULDBLOCK;
		interruptible_sleep_on(&radio->read_queue);
	}

	/* calculate block count from byte count */
	count /= 3;

	/* copy RDS block out of internal buffer and to user buffer */
	if (spin_trylock(&radio->lock)) {
		while (block_count < count) {
			if (radio->rd_index == radio->wr_index)
				break;

			/* always transfer rds complete blocks */
			if (copy_to_user(buf,
					&radio->buffer[radio->rd_index], 3))
				/* retval = -EFAULT; */
				break;

			/* increment and wrap read pointer */
			radio->rd_index += 3;
			if (radio->rd_index >= radio->buf_size)
				radio->rd_index = 0;

			/* increment counters */
			block_count++;
			buf += 3;
			retval += 3;
		}

		spin_unlock(&radio->lock);
	}

	return retval;
}


/*
 * si470x_fops_poll - poll RDS data
 */
static unsigned int si470x_fops_poll(struct file *file,
		struct poll_table_struct *pts)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	/* switch on rds reception */
	if ((radio->registers[SYSCONFIG1] & SYSCONFIG1_RDS) == 0) {
		si470x_rds_on(radio);
		schedule_work(&radio->work);
	}

	poll_wait(file, &radio->read_queue, pts);

	if (radio->rd_index != radio->wr_index)
		return POLLIN | POLLRDNORM;

	return 0;
}


/*
 * si470x_fops_open - file open
 */
static int si470x_fops_open(struct inode *inode, struct file *file)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	radio->users++;
	if (radio->users == 1)
		return si470x_start(radio);

	return 0;
}


/*
 * si470x_fops_release - file release
 */
static int si470x_fops_release(struct inode *inode, struct file *file)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	if (!radio)
		return -ENODEV;

	radio->users--;
	if (radio->users == 0) {
		/* stop rds reception */
		del_timer_sync(&radio->timer);
		flush_scheduled_work();

		/* cancel read processes */
		wake_up_interruptible(&radio->read_queue);

		return si470x_stop(radio);
	}

	return 0;
}


/*
 * si470x_fops - file operations interface
 */
static const struct file_operations si470x_fops = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= si470x_fops_read,
	.poll		= si470x_fops_poll,
	.ioctl		= video_ioctl2,
	.compat_ioctl	= v4l_compat_ioctl32,
	.open		= si470x_fops_open,
	.release	= si470x_fops_release,
};



/**************************************************************************
 * Video4Linux Interface
 **************************************************************************/

/*
 * si470x_v4l2_queryctrl - query control
 */
static struct v4l2_queryctrl si470x_v4l2_queryctrl[] = {
/* HINT: the disabled controls are only here to satify kradio and such apps */
	{
		.id		= V4L2_CID_AUDIO_VOLUME,
		.type		= V4L2_CTRL_TYPE_INTEGER,
		.name		= "Volume",
		.minimum	= 0,
		.maximum	= 15,
		.step		= 1,
		.default_value	= 15,
	},
	{
		.id		= V4L2_CID_AUDIO_BALANCE,
		.flags		= V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id		= V4L2_CID_AUDIO_BASS,
		.flags		= V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id		= V4L2_CID_AUDIO_TREBLE,
		.flags		= V4L2_CTRL_FLAG_DISABLED,
	},
	{
		.id		= V4L2_CID_AUDIO_MUTE,
		.type		= V4L2_CTRL_TYPE_BOOLEAN,
		.name		= "Mute",
		.minimum	= 0,
		.maximum	= 1,
		.step		= 1,
		.default_value	= 1,
	},
	{
		.id		= V4L2_CID_AUDIO_LOUDNESS,
		.flags		= V4L2_CTRL_FLAG_DISABLED,
	},
};


/*
 * si470x_vidioc_querycap - query device capabilities
 */
static int si470x_vidioc_querycap(struct file *file, void *priv,
		struct v4l2_capability *capability)
{
	strlcpy(capability->driver, DRIVER_NAME, sizeof(capability->driver));
	strlcpy(capability->card, DRIVER_CARD, sizeof(capability->card));
	sprintf(capability->bus_info, "USB");
	capability->version = DRIVER_VERSION;
	capability->capabilities = V4L2_CAP_TUNER | V4L2_CAP_RADIO;

	return 0;
}


/*
 * si470x_vidioc_g_input - get input
 */
static int si470x_vidioc_g_input(struct file *filp, void *priv,
		unsigned int *i)
{
	*i = 0;

	return 0;
}


/*
 * si470x_vidioc_s_input - set input
 */
static int si470x_vidioc_s_input(struct file *filp, void *priv, unsigned int i)
{
	if (i != 0)
		return -EINVAL;

	return 0;
}


/*
 * si470x_vidioc_queryctrl - enumerate control items
 */
static int si470x_vidioc_queryctrl(struct file *file, void *priv,
		struct v4l2_queryctrl *qc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(si470x_v4l2_queryctrl); i++) {
		if (qc->id && qc->id == si470x_v4l2_queryctrl[i].id) {
			memcpy(qc, &(si470x_v4l2_queryctrl[i]), sizeof(*qc));
			return 0;
		}
	}

	return -EINVAL;
}


/*
 * si470x_vidioc_g_ctrl - get the value of a control
 */
static int si470x_vidioc_g_ctrl(struct file *file, void *priv,
		struct v4l2_control *ctrl)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_VOLUME:
		ctrl->value = radio->registers[SYSCONFIG2] &
				SYSCONFIG2_VOLUME;
		break;
	case V4L2_CID_AUDIO_MUTE:
		ctrl->value = ((radio->registers[POWERCFG] &
				POWERCFG_DMUTE) == 0) ? 1 : 0;
		break;
	}

	return 0;
}


/*
 * si470x_vidioc_s_ctrl - set the value of a control
 */
static int si470x_vidioc_s_ctrl(struct file *file, void *priv,
		struct v4l2_control *ctrl)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_VOLUME:
		radio->registers[SYSCONFIG2] &= ~SYSCONFIG2_VOLUME;
		radio->registers[SYSCONFIG2] |= ctrl->value;
		return si470x_set_register(radio, SYSCONFIG2);
	case V4L2_CID_AUDIO_MUTE:
		if (ctrl->value == 1)
			radio->registers[POWERCFG] &= ~POWERCFG_DMUTE;
		else
			radio->registers[POWERCFG] |= POWERCFG_DMUTE;
		return si470x_set_register(radio, POWERCFG);
	}

	return -EINVAL;
}


/*
 * si470x_vidioc_g_audio - get audio attributes
 */
static int si470x_vidioc_g_audio(struct file *file, void *priv,
		struct v4l2_audio *audio)
{
	if (audio->index > 1)
		return -EINVAL;

	strcpy(audio->name, "Radio");
	audio->capability = V4L2_AUDCAP_STEREO;

	return 0;
}


/*
 * si470x_vidioc_s_audio - set audio attributes
 */
static int si470x_vidioc_s_audio(struct file *file, void *priv,
		struct v4l2_audio *audio)
{
	if (audio->index != 0)
		return -EINVAL;

	return 0;
}


/*
 * si470x_vidioc_g_tuner - get tuner attributes
 */
static int si470x_vidioc_g_tuner(struct file *file, void *priv,
		struct v4l2_tuner *tuner)
{
	int retval;
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	if (tuner->index > 0)
		return -EINVAL;

	/* read status rssi */
	retval = si470x_get_register(radio, STATUSRSSI);
	if (retval < 0)
		return retval;

	strcpy(tuner->name, "FM");
	tuner->type = V4L2_TUNER_RADIO;
	switch (band) {
	/* 0: 87.5 - 108 MHz (USA, Europe, default) */
	default:
		tuner->rangelow  =  87.5 * FREQ_MUL;
		tuner->rangehigh = 108   * FREQ_MUL;
		break;
	/* 1: 76   - 108 MHz (Japan wide band) */
	case 1 :
		tuner->rangelow  =  76   * FREQ_MUL;
		tuner->rangehigh = 108   * FREQ_MUL;
		break;
	/* 2: 76   -  90 MHz (Japan) */
	case 2 :
		tuner->rangelow  =  76   * FREQ_MUL;
		tuner->rangehigh =  90   * FREQ_MUL;
		break;
	};
	tuner->rxsubchans = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
	tuner->capability = V4L2_TUNER_CAP_LOW;

	/* Stereo indicator == Stereo (instead of Mono) */
	if ((radio->registers[STATUSRSSI] & STATUSRSSI_ST) == 1)
		tuner->audmode = V4L2_TUNER_MODE_STEREO;
	else
		tuner->audmode = V4L2_TUNER_MODE_MONO;

	/* min is worst, max is best; signal:0..0xffff; rssi: 0..0xff */
	tuner->signal = (radio->registers[STATUSRSSI] & STATUSRSSI_RSSI)
				* 0x0101;

	/* automatic frequency control: -1: freq to low, 1 freq to high */
	tuner->afc = 0;

	return 0;
}


/*
 * si470x_vidioc_s_tuner - set tuner attributes
 */
static int si470x_vidioc_s_tuner(struct file *file, void *priv,
		struct v4l2_tuner *tuner)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	if (tuner->index > 0)
		return -EINVAL;

	if (tuner->audmode == V4L2_TUNER_MODE_MONO)
		radio->registers[POWERCFG] |= POWERCFG_MONO;  /* force mono */
	else
		radio->registers[POWERCFG] &= ~POWERCFG_MONO; /* try stereo */

	return si470x_set_register(radio, POWERCFG);
}


/*
 * si470x_vidioc_g_frequency - get tuner or modulator radio frequency
 */
static int si470x_vidioc_g_frequency(struct file *file, void *priv,
		struct v4l2_frequency *freq)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	freq->type = V4L2_TUNER_RADIO;
	freq->frequency = si470x_get_freq(radio);

	return 0;
}


/*
 * si470x_vidioc_s_frequency - set tuner or modulator radio frequency
 */
static int si470x_vidioc_s_frequency(struct file *file, void *priv,
		struct v4l2_frequency *freq)
{
	struct si470x_device *radio = video_get_drvdata(video_devdata(file));

	if (freq->type != V4L2_TUNER_RADIO)
		return -EINVAL;

	return si470x_set_freq(radio, freq->frequency);
}


/*
 * si470x_viddev_tamples - video device interface
 */
static struct video_device si470x_viddev_template = {
	.fops			= &si470x_fops,
	.name			= DRIVER_NAME,
	.type			= VID_TYPE_TUNER,
	.release		= video_device_release,
	.vidioc_querycap	= si470x_vidioc_querycap,
	.vidioc_g_input		= si470x_vidioc_g_input,
	.vidioc_s_input		= si470x_vidioc_s_input,
	.vidioc_queryctrl	= si470x_vidioc_queryctrl,
	.vidioc_g_ctrl		= si470x_vidioc_g_ctrl,
	.vidioc_s_ctrl		= si470x_vidioc_s_ctrl,
	.vidioc_g_audio		= si470x_vidioc_g_audio,
	.vidioc_s_audio		= si470x_vidioc_s_audio,
	.vidioc_g_tuner		= si470x_vidioc_g_tuner,
	.vidioc_s_tuner		= si470x_vidioc_s_tuner,
	.vidioc_g_frequency	= si470x_vidioc_g_frequency,
	.vidioc_s_frequency	= si470x_vidioc_s_frequency,
	.owner			= THIS_MODULE,
};



/**************************************************************************
 * USB Interface
 **************************************************************************/

/*
 * si470x_usb_driver_probe - probe for the device
 */
static int si470x_usb_driver_probe(struct usb_interface *intf,
		const struct usb_device_id *id)
{
	struct si470x_device *radio;

	/* memory and interface allocations */
	radio = kmalloc(sizeof(struct si470x_device), GFP_KERNEL);
	if (!radio)
		return -ENOMEM;
	radio->videodev = video_device_alloc();
	if (!radio->videodev) {
		kfree(radio);
		return -ENOMEM;
	}
	memcpy(radio->videodev, &si470x_viddev_template,
			sizeof(si470x_viddev_template));
	radio->users = 0;
	radio->usbdev = interface_to_usbdev(intf);
	video_set_drvdata(radio->videodev, radio);
	if (video_register_device(radio->videodev, VFL_TYPE_RADIO, radio_nr)) {
		printk(KERN_WARNING DRIVER_NAME
				": Could not register video device\n");
		video_device_release(radio->videodev);
		kfree(radio);
		return -EIO;
	}
	usb_set_intfdata(intf, radio);

	/* show some infos about the specific device */
	if (si470x_get_all_registers(radio) < 0) {
		video_device_release(radio->videodev);
		kfree(radio);
		return -EIO;
	}
	printk(KERN_INFO DRIVER_NAME ": DeviceID=0x%4.4x ChipID=0x%4.4x\n",
			radio->registers[DEVICEID], radio->registers[CHIPID]);

	/* check if firmware is current */
	if ((radio->registers[CHIPID] & CHIPID_FIRMWARE)
			< RADIO_SW_VERSION_CURRENT)
		printk(KERN_WARNING DRIVER_NAME
			": This driver is known to work with chip version %d, "
			"but the device has firmware %d.\n"
			DRIVER_NAME
			"If you have some trouble using this driver, please "
			"report to V4L ML at video4linux-list@redhat.com\n",
			radio->registers[CHIPID] & CHIPID_FIRMWARE,
			RADIO_SW_VERSION_CURRENT);

	/* set initial frequency */
	si470x_set_freq(radio, 87.5 * FREQ_MUL); /* available in all regions */

	/* rds initialization */
	radio->buf_size = rds_buf * 3;
	radio->buffer = kmalloc(radio->buf_size, GFP_KERNEL);
	if (!radio->buffer) {
		video_device_release(radio->videodev);
		kfree(radio);
		return -ENOMEM;
	}
	radio->wr_index = 0;
	radio->rd_index = 0;
	init_waitqueue_head(&radio->read_queue);

	/* prepare polling via eventd */
	INIT_WORK(&radio->work, si470x_work);
	init_timer(&radio->timer);
	radio->timer.function = si470x_timer;
	radio->timer.data = (unsigned long) radio;

	return 0;
}


/*
 * si470x_usb_driver_disconnect - disconnect the device
 */
static void si470x_usb_driver_disconnect(struct usb_interface *intf)
{
	struct si470x_device *radio = usb_get_intfdata(intf);

	del_timer_sync(&radio->timer);
	flush_scheduled_work();

	usb_set_intfdata(intf, NULL);
	if (radio) {
		video_unregister_device(radio->videodev);
		kfree(radio->buffer);
		kfree(radio);
	}
}


/*
 * si470x_usb_driver - usb driver interface
 */
static struct usb_driver si470x_usb_driver = {
	.name		= DRIVER_NAME,
	.probe		= si470x_usb_driver_probe,
	.disconnect	= si470x_usb_driver_disconnect,
	.id_table	= si470x_usb_driver_id_table,
};



/**************************************************************************
 * Module Interface
 **************************************************************************/

/*
 * si470x_module_init - module init
 */
static int __init si470x_module_init(void)
{
	printk(KERN_INFO DRIVER_DESC "\n");
	return usb_register(&si470x_usb_driver);
}


/*
 * si470x_module_exit - module exit
 */
static void __exit si470x_module_exit(void)
{
	usb_deregister(&si470x_usb_driver);
}


module_init(si470x_module_init);
module_exit(si470x_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION("1.0.4");
