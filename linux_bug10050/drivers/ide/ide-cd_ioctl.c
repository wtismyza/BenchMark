/*
 * cdrom.c IOCTLs handling for ide-cd driver.
 *
 * Copyright (C) 1994-1996  Scott Snyder <snyder@fnald0.fnal.gov>
 * Copyright (C) 1996-1998  Erik Andersen <andersee@debian.org>
 * Copyright (C) 1998-2000  Jens Axboe <axboe@suse.de>
 */

#include <linux/kernel.h>
#include <linux/cdrom.h>
#include <linux/ide.h>
#include <scsi/scsi.h>

#include "ide-cd.h"

/****************************************************************************
 * Other driver requests (open, close, check media change).
 */
int ide_cdrom_open_real(struct cdrom_device_info *cdi, int purpose)
{
	return 0;
}

/*
 * Close down the device.  Invalidate all cached blocks.
 */
void ide_cdrom_release_real(struct cdrom_device_info *cdi)
{
	ide_drive_t *drive = cdi->handle;
	struct cdrom_info *cd = drive->driver_data;

	if (!cdi->use_count)
		cd->cd_flags &= ~IDE_CD_FLAG_TOC_VALID;
}

/*
 * add logic to try GET_EVENT command first to check for media and tray
 * status. this should be supported by newer cd-r/w and all DVD etc
 * drives
 */
int ide_cdrom_drive_status(struct cdrom_device_info *cdi, int slot_nr)
{
	ide_drive_t *drive = cdi->handle;
	struct media_event_desc med;
	struct request_sense sense;
	int stat;

	if (slot_nr != CDSL_CURRENT)
		return -EINVAL;

	stat = cdrom_check_status(drive, &sense);
	if (!stat || sense.sense_key == UNIT_ATTENTION)
		return CDS_DISC_OK;

	if (!cdrom_get_media_event(cdi, &med)) {
		if (med.media_present)
			return CDS_DISC_OK;
		else if (med.door_open)
			return CDS_TRAY_OPEN;
		else
			return CDS_NO_DISC;
	}

	if (sense.sense_key == NOT_READY && sense.asc == 0x04
			&& sense.ascq == 0x04)
		return CDS_DISC_OK;

	/*
	 * If not using Mt Fuji extended media tray reports,
	 * just return TRAY_OPEN since ATAPI doesn't provide
	 * any other way to detect this...
	 */
	if (sense.sense_key == NOT_READY) {
		if (sense.asc == 0x3a && sense.ascq == 1)
			return CDS_NO_DISC;
		else
			return CDS_TRAY_OPEN;
	}
	return CDS_DRIVE_NOT_READY;
}

int ide_cdrom_check_media_change_real(struct cdrom_device_info *cdi,
				       int slot_nr)
{
	ide_drive_t *drive = cdi->handle;
	struct cdrom_info *cd = drive->driver_data;
	int retval;

	if (slot_nr == CDSL_CURRENT) {
		(void) cdrom_check_status(drive, NULL);
		retval = (cd->cd_flags & IDE_CD_FLAG_MEDIA_CHANGED) ? 1 : 0;
		cd->cd_flags &= ~IDE_CD_FLAG_MEDIA_CHANGED;
		return retval;
	} else {
		return -EINVAL;
	}
}

/* Eject the disk if EJECTFLAG is 0.
   If EJECTFLAG is 1, try to reload the disk. */
static
int cdrom_eject(ide_drive_t *drive, int ejectflag,
		struct request_sense *sense)
{
	struct cdrom_info *cd = drive->driver_data;
	struct cdrom_device_info *cdi = &cd->devinfo;
	struct request req;
	char loej = 0x02;

	if ((cd->cd_flags & IDE_CD_FLAG_NO_EJECT) && !ejectflag)
		return -EDRIVE_CANT_DO_THIS;

	/* reload fails on some drives, if the tray is locked */
	if ((cd->cd_flags & IDE_CD_FLAG_DOOR_LOCKED) && ejectflag)
		return 0;

	ide_cd_init_rq(drive, &req);

	/* only tell drive to close tray if open, if it can do that */
	if (ejectflag && (cdi->mask & CDC_CLOSE_TRAY))
		loej = 0;

	req.sense = sense;
	req.cmd[0] = GPCMD_START_STOP_UNIT;
	req.cmd[4] = loej | (ejectflag != 0);

	return ide_cd_queue_pc(drive, &req);
}

/* Lock the door if LOCKFLAG is nonzero; unlock it otherwise. */
static
int ide_cd_lockdoor(ide_drive_t *drive, int lockflag,
		    struct request_sense *sense)
{
	struct cdrom_info *cd = drive->driver_data;
	struct request_sense my_sense;
	struct request req;
	int stat;

	if (sense == NULL)
		sense = &my_sense;

	/* If the drive cannot lock the door, just pretend. */
	if (cd->cd_flags & IDE_CD_FLAG_NO_DOORLOCK) {
		stat = 0;
	} else {
		ide_cd_init_rq(drive, &req);
		req.sense = sense;
		req.cmd[0] = GPCMD_PREVENT_ALLOW_MEDIUM_REMOVAL;
		req.cmd[4] = lockflag ? 1 : 0;
		stat = ide_cd_queue_pc(drive, &req);
	}

	/* If we got an illegal field error, the drive
	   probably cannot lock the door. */
	if (stat != 0 &&
	    sense->sense_key == ILLEGAL_REQUEST &&
	    (sense->asc == 0x24 || sense->asc == 0x20)) {
		printk(KERN_ERR "%s: door locking not supported\n",
			drive->name);
		cd->cd_flags |= IDE_CD_FLAG_NO_DOORLOCK;
		stat = 0;
	}

	/* no medium, that's alright. */
	if (stat != 0 && sense->sense_key == NOT_READY && sense->asc == 0x3a)
		stat = 0;

	if (stat == 0) {
		if (lockflag)
			cd->cd_flags |= IDE_CD_FLAG_DOOR_LOCKED;
		else
			cd->cd_flags &= ~IDE_CD_FLAG_DOOR_LOCKED;
	}

	return stat;
}

int ide_cdrom_tray_move(struct cdrom_device_info *cdi, int position)
{
	ide_drive_t *drive = cdi->handle;
	struct request_sense sense;

	if (position) {
		int stat = ide_cd_lockdoor(drive, 0, &sense);

		if (stat)
			return stat;
	}

	return cdrom_eject(drive, !position, &sense);
}

int ide_cdrom_lock_door(struct cdrom_device_info *cdi, int lock)
{
	ide_drive_t *drive = cdi->handle;

	return ide_cd_lockdoor(drive, lock, NULL);
}

/*
 * ATAPI devices are free to select the speed you request or any slower
 * rate. :-(  Requesting too fast a speed will _not_ produce an error.
 */
int ide_cdrom_select_speed(struct cdrom_device_info *cdi, int speed)
{
	ide_drive_t *drive = cdi->handle;
	struct cdrom_info *cd = drive->driver_data;
	struct request rq;
	struct request_sense sense;
	u8 buf[ATAPI_CAPABILITIES_PAGE_SIZE];
	int stat;

	ide_cd_init_rq(drive, &rq);

	rq.sense = &sense;

	if (speed == 0)
		speed = 0xffff; /* set to max */
	else
		speed *= 177;   /* Nx to kbytes/s */

	rq.cmd[0] = GPCMD_SET_SPEED;
	/* Read Drive speed in kbytes/second MSB/LSB */
	rq.cmd[2] = (speed >> 8) & 0xff;
	rq.cmd[3] = speed & 0xff;
	if ((cdi->mask & (CDC_CD_R | CDC_CD_RW | CDC_DVD_R)) !=
	    (CDC_CD_R | CDC_CD_RW | CDC_DVD_R)) {
		/* Write Drive speed in kbytes/second MSB/LSB */
		rq.cmd[4] = (speed >> 8) & 0xff;
		rq.cmd[5] = speed & 0xff;
	}

	stat = ide_cd_queue_pc(drive, &rq);

	if (!ide_cdrom_get_capabilities(drive, buf)) {
		ide_cdrom_update_speed(drive, buf);
		cdi->speed = cd->current_speed;
	}

	return 0;
}

int ide_cdrom_get_last_session(struct cdrom_device_info *cdi,
			       struct cdrom_multisession *ms_info)
{
	struct atapi_toc *toc;
	ide_drive_t *drive = cdi->handle;
	struct cdrom_info *info = drive->driver_data;
	struct request_sense sense;
	int ret;

	if ((info->cd_flags & IDE_CD_FLAG_TOC_VALID) == 0 || !info->toc) {
		ret = ide_cd_read_toc(drive, &sense);
		if (ret)
			return ret;
	}

	toc = info->toc;
	ms_info->addr.lba = toc->last_session_lba;
	ms_info->xa_flag = toc->xa_flag;

	return 0;
}

int ide_cdrom_get_mcn(struct cdrom_device_info *cdi,
		      struct cdrom_mcn *mcn_info)
{
	ide_drive_t *drive = cdi->handle;
	int stat, mcnlen;
	struct request rq;
	char buf[24];

	ide_cd_init_rq(drive, &rq);

	rq.data = buf;
	rq.data_len = sizeof(buf);

	rq.cmd[0] = GPCMD_READ_SUBCHANNEL;
	rq.cmd[1] = 2;		/* MSF addressing */
	rq.cmd[2] = 0x40;	/* request subQ data */
	rq.cmd[3] = 2;		/* format */
	rq.cmd[8] = sizeof(buf);

	stat = ide_cd_queue_pc(drive, &rq);
	if (stat)
		return stat;

	mcnlen = sizeof(mcn_info->medium_catalog_number) - 1;
	memcpy(mcn_info->medium_catalog_number, buf + 9, mcnlen);
	mcn_info->medium_catalog_number[mcnlen] = '\0';

	return 0;
}

int ide_cdrom_reset(struct cdrom_device_info *cdi)
{
	ide_drive_t *drive = cdi->handle;
	struct cdrom_info *cd = drive->driver_data;
	struct request_sense sense;
	struct request req;
	int ret;

	ide_cd_init_rq(drive, &req);
	req.cmd_type = REQ_TYPE_SPECIAL;
	req.cmd_flags = REQ_QUIET;
	ret = ide_do_drive_cmd(drive, &req, ide_wait);

	/*
	 * A reset will unlock the door. If it was previously locked,
	 * lock it again.
	 */
	if (cd->cd_flags & IDE_CD_FLAG_DOOR_LOCKED)
		(void)ide_cd_lockdoor(drive, 1, &sense);

	return ret;
}

static int ide_cd_get_toc_entry(ide_drive_t *drive, int track,
				struct atapi_toc_entry **ent)
{
	struct cdrom_info *info = drive->driver_data;
	struct atapi_toc *toc = info->toc;
	int ntracks;

	/*
	 * don't serve cached data, if the toc isn't valid
	 */
	if ((info->cd_flags & IDE_CD_FLAG_TOC_VALID) == 0)
		return -EINVAL;

	/* Check validity of requested track number. */
	ntracks = toc->hdr.last_track - toc->hdr.first_track + 1;

	if (toc->hdr.first_track == CDROM_LEADOUT)
		ntracks = 0;

	if (track == CDROM_LEADOUT)
		*ent = &toc->ent[ntracks];
	else if (track < toc->hdr.first_track || track > toc->hdr.last_track)
		return -EINVAL;
	else
		*ent = &toc->ent[track - toc->hdr.first_track];

	return 0;
}

static int ide_cd_fake_play_trkind(ide_drive_t *drive, void *arg)
{
	struct cdrom_ti *ti = arg;
	struct atapi_toc_entry *first_toc, *last_toc;
	unsigned long lba_start, lba_end;
	int stat;
	struct request rq;
	struct request_sense sense;

	stat = ide_cd_get_toc_entry(drive, ti->cdti_trk0, &first_toc);
	if (stat)
		return stat;

	stat = ide_cd_get_toc_entry(drive, ti->cdti_trk1, &last_toc);
	if (stat)
		return stat;

	if (ti->cdti_trk1 != CDROM_LEADOUT)
		++last_toc;
	lba_start = first_toc->addr.lba;
	lba_end   = last_toc->addr.lba;

	if (lba_end <= lba_start)
		return -EINVAL;

	ide_cd_init_rq(drive, &rq);

	rq.sense = &sense;
	rq.cmd[0] = GPCMD_PLAY_AUDIO_MSF;
	lba_to_msf(lba_start,   &rq.cmd[3], &rq.cmd[4], &rq.cmd[5]);
	lba_to_msf(lba_end - 1, &rq.cmd[6], &rq.cmd[7], &rq.cmd[8]);

	return ide_cd_queue_pc(drive, &rq);
}

static int ide_cd_read_tochdr(ide_drive_t *drive, void *arg)
{
	struct cdrom_info *cd = drive->driver_data;
	struct cdrom_tochdr *tochdr = arg;
	struct atapi_toc *toc;
	int stat;

	/* Make sure our saved TOC is valid. */
	stat = ide_cd_read_toc(drive, NULL);
	if (stat)
		return stat;

	toc = cd->toc;
	tochdr->cdth_trk0 = toc->hdr.first_track;
	tochdr->cdth_trk1 = toc->hdr.last_track;

	return 0;
}

static int ide_cd_read_tocentry(ide_drive_t *drive, void *arg)
{
	struct cdrom_tocentry *tocentry = arg;
	struct atapi_toc_entry *toce;
	int stat;

	stat = ide_cd_get_toc_entry(drive, tocentry->cdte_track, &toce);
	if (stat)
		return stat;

	tocentry->cdte_ctrl = toce->control;
	tocentry->cdte_adr  = toce->adr;
	if (tocentry->cdte_format == CDROM_MSF) {
		lba_to_msf(toce->addr.lba,
			   &tocentry->cdte_addr.msf.minute,
			   &tocentry->cdte_addr.msf.second,
			   &tocentry->cdte_addr.msf.frame);
	} else
		tocentry->cdte_addr.lba = toce->addr.lba;

	return 0;
}

int ide_cdrom_audio_ioctl(struct cdrom_device_info *cdi,
			  unsigned int cmd, void *arg)
{
	ide_drive_t *drive = cdi->handle;

	switch (cmd) {
	/*
	 * emulate PLAY_AUDIO_TI command with PLAY_AUDIO_10, since
	 * atapi doesn't support it
	 */
	case CDROMPLAYTRKIND:
		return ide_cd_fake_play_trkind(drive, arg);
	case CDROMREADTOCHDR:
		return ide_cd_read_tochdr(drive, arg);
	case CDROMREADTOCENTRY:
		return ide_cd_read_tocentry(drive, arg);
	default:
		return -EINVAL;
	}
}

/* the generic packet interface to cdrom.c */
int ide_cdrom_packet(struct cdrom_device_info *cdi,
			    struct packet_command *cgc)
{
	struct request req;
	ide_drive_t *drive = cdi->handle;

	if (cgc->timeout <= 0)
		cgc->timeout = ATAPI_WAIT_PC;

	/* here we queue the commands from the uniform CD-ROM
	   layer. the packet must be complete, as we do not
	   touch it at all. */
	ide_cd_init_rq(drive, &req);
	memcpy(req.cmd, cgc->cmd, CDROM_PACKET_SIZE);
	if (cgc->sense)
		memset(cgc->sense, 0, sizeof(struct request_sense));
	req.data = cgc->buffer;
	req.data_len = cgc->buflen;
	req.timeout = cgc->timeout;

	if (cgc->quiet)
		req.cmd_flags |= REQ_QUIET;

	req.sense = cgc->sense;
	cgc->stat = ide_cd_queue_pc(drive, &req);
	if (!cgc->stat)
		cgc->buflen -= req.data_len;
	return cgc->stat;
}
