/*
 *  thermal.h  ($Revision: 0 $)
 *
 *  Copyright (C) 2008  Intel Corp
 *  Copyright (C) 2008  Zhang Rui <rui.zhang@intel.com>
 *  Copyright (C) 2008  Sujith Thomas <sujith.thomas@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#ifndef __THERMAL_H__
#define __THERMAL_H__

#include <linux/idr.h>
#include <linux/device.h>

struct thermal_zone_device;
struct thermal_cooling_device;

struct thermal_zone_device_ops {
	int (*bind) (struct thermal_zone_device *,
		     struct thermal_cooling_device *);
	int (*unbind) (struct thermal_zone_device *,
		       struct thermal_cooling_device *);
	int (*get_temp) (struct thermal_zone_device *, char *);
	int (*get_mode) (struct thermal_zone_device *, char *);
	int (*set_mode) (struct thermal_zone_device *, const char *);
	int (*get_trip_type) (struct thermal_zone_device *, int, char *);
	int (*get_trip_temp) (struct thermal_zone_device *, int, char *);
};

struct thermal_cooling_device_ops {
	int (*get_max_state) (struct thermal_cooling_device *, char *);
	int (*get_cur_state) (struct thermal_cooling_device *, char *);
	int (*set_cur_state) (struct thermal_cooling_device *, unsigned int);
};

#define THERMAL_TRIPS_NONE -1
#define THERMAL_MAX_TRIPS 10
#define THERMAL_NAME_LENGTH 20
struct thermal_cooling_device {
	int id;
	char type[THERMAL_NAME_LENGTH];
	struct device device;
	void *devdata;
	struct thermal_cooling_device_ops *ops;
	struct list_head node;
};

#define KELVIN_TO_CELSIUS(t)	(long)(((long)t-2732 >= 0) ?	\
				((long)t-2732+5)/10 : ((long)t-2732-5)/10)
#define CELSIUS_TO_KELVIN(t)	((t)*10+2732)

struct thermal_zone_device {
	int id;
	char type[THERMAL_NAME_LENGTH];
	struct device device;
	void *devdata;
	int trips;
	struct thermal_zone_device_ops *ops;
	struct list_head cooling_devices;
	struct idr idr;
	struct mutex lock;	/* protect cooling devices list */
	struct list_head node;
};

struct thermal_zone_device *thermal_zone_device_register(char *, int, void *,
							 struct
							 thermal_zone_device_ops
							 *);
void thermal_zone_device_unregister(struct thermal_zone_device *);

int thermal_zone_bind_cooling_device(struct thermal_zone_device *, int,
				     struct thermal_cooling_device *);
int thermal_zone_unbind_cooling_device(struct thermal_zone_device *, int,
				       struct thermal_cooling_device *);

#ifdef	CONFIG_THERMAL
struct thermal_cooling_device *thermal_cooling_device_register(char *, void *,
							       struct
							       thermal_cooling_device_ops
							       *);
void thermal_cooling_device_unregister(struct thermal_cooling_device *);
#else
static inline struct thermal_cooling_device
*thermal_cooling_device_register(char *c, void *v,
				 struct thermal_cooling_device_ops *t)
{
	return NULL;
}
static inline
    void thermal_cooling_device_unregister(struct thermal_cooling_device *t)
{
};
#endif

#endif /* __THERMAL_H__ */
