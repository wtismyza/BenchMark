/*
 * INET		802.1Q VLAN
 *		Ethernet-type device handling.
 *
 * Authors:	Ben Greear <greearb@candelatech.com>
 *              Please send support related email to: netdev@vger.kernel.org
 *              VLAN Home Page: http://www.candelatech.com/~greear/vlan.html
 *
 * Fixes:
 *              Fix for packet capture - Nick Eggleston <nick@dccinc.com>;
 *		Add HW acceleration hooks - David S. Miller <davem@redhat.com>;
 *		Correct all the locking - David S. Miller <davem@redhat.com>;
 *		Use hash table for VLAN groups - David S. Miller <davem@redhat.com>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <asm/uaccess.h> /* for copy_from_user */
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/datalink.h>
#include <linux/mm.h>
#include <linux/in.h>
#include <linux/init.h>
#include <net/p8022.h>
#include <net/arp.h>
#include <linux/rtnetlink.h>
#include <linux/notifier.h>
#include <net/net_namespace.h>

#include <linux/if_vlan.h>
#include "vlan.h"
#include "vlanproc.h"

#define DRV_VERSION "1.8"

/* Global VLAN variables */

/* Our listing of VLAN group(s) */
static struct hlist_head vlan_group_hash[VLAN_GRP_HASH_SIZE];

static char vlan_fullname[] = "802.1Q VLAN Support";
static char vlan_version[] = DRV_VERSION;
static char vlan_copyright[] = "Ben Greear <greearb@candelatech.com>";
static char vlan_buggyright[] = "David S. Miller <davem@redhat.com>";

/* Determines interface naming scheme. */
unsigned short vlan_name_type = VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD;

static struct packet_type vlan_packet_type = {
	.type = __constant_htons(ETH_P_8021Q),
	.func = vlan_skb_recv, /* VLAN receive method */
};

/* End of global variables definitions. */

static inline unsigned int vlan_grp_hashfn(unsigned int idx)
{
	return ((idx >> VLAN_GRP_HASH_SHIFT) ^ idx) & VLAN_GRP_HASH_MASK;
}

/* Must be invoked with RCU read lock (no preempt) */
static struct vlan_group *__vlan_find_group(int real_dev_ifindex)
{
	struct vlan_group *grp;
	struct hlist_node *n;
	int hash = vlan_grp_hashfn(real_dev_ifindex);

	hlist_for_each_entry_rcu(grp, n, &vlan_group_hash[hash], hlist) {
		if (grp->real_dev_ifindex == real_dev_ifindex)
			return grp;
	}

	return NULL;
}

/*  Find the protocol handler.  Assumes VID < VLAN_VID_MASK.
 *
 * Must be invoked with RCU read lock (no preempt)
 */
struct net_device *__find_vlan_dev(struct net_device *real_dev,
				   unsigned short VID)
{
	struct vlan_group *grp = __vlan_find_group(real_dev->ifindex);

	if (grp)
		return vlan_group_get_device(grp, VID);

	return NULL;
}

static void vlan_group_free(struct vlan_group *grp)
{
	int i;

	for (i = 0; i < VLAN_GROUP_ARRAY_SPLIT_PARTS; i++)
		kfree(grp->vlan_devices_arrays[i]);
	kfree(grp);
}

static struct vlan_group *vlan_group_alloc(int ifindex)
{
	struct vlan_group *grp;
	unsigned int size;
	unsigned int i;

	grp = kzalloc(sizeof(struct vlan_group), GFP_KERNEL);
	if (!grp)
		return NULL;

	size = sizeof(struct net_device *) * VLAN_GROUP_ARRAY_PART_LEN;

	for (i = 0; i < VLAN_GROUP_ARRAY_SPLIT_PARTS; i++) {
		grp->vlan_devices_arrays[i] = kzalloc(size, GFP_KERNEL);
		if (!grp->vlan_devices_arrays[i])
			goto err;
	}

	grp->real_dev_ifindex = ifindex;
	hlist_add_head_rcu(&grp->hlist,
			   &vlan_group_hash[vlan_grp_hashfn(ifindex)]);
	return grp;

err:
	vlan_group_free(grp);
	return NULL;
}

static void vlan_rcu_free(struct rcu_head *rcu)
{
	vlan_group_free(container_of(rcu, struct vlan_group, rcu));
}

void unregister_vlan_dev(struct net_device *dev)
{
	struct vlan_dev_info *vlan = vlan_dev_info(dev);
	struct net_device *real_dev = vlan->real_dev;
	struct vlan_group *grp;
	unsigned short vlan_id = vlan->vlan_id;

	ASSERT_RTNL();

	grp = __vlan_find_group(real_dev->ifindex);
	BUG_ON(!grp);

	vlan_proc_rem_dev(dev);

	/* Take it out of our own structures, but be sure to interlock with
	 * HW accelerating devices or SW vlan input packet processing.
	 */
	if (real_dev->features & NETIF_F_HW_VLAN_FILTER)
		real_dev->vlan_rx_kill_vid(real_dev, vlan_id);

	vlan_group_set_device(grp, vlan_id, NULL);
	grp->nr_vlans--;

	synchronize_net();

	/* If the group is now empty, kill off the group. */
	if (grp->nr_vlans == 0) {
		if (real_dev->features & NETIF_F_HW_VLAN_RX)
			real_dev->vlan_rx_register(real_dev, NULL);

		hlist_del_rcu(&grp->hlist);

		/* Free the group, after all cpu's are done. */
		call_rcu(&grp->rcu, vlan_rcu_free);
	}

	/* Get rid of the vlan's reference to real_dev */
	dev_put(real_dev);

	unregister_netdevice(dev);
}

static void vlan_transfer_operstate(const struct net_device *dev,
				    struct net_device *vlandev)
{
	/* Have to respect userspace enforced dormant state
	 * of real device, also must allow supplicant running
	 * on VLAN device
	 */
	if (dev->operstate == IF_OPER_DORMANT)
		netif_dormant_on(vlandev);
	else
		netif_dormant_off(vlandev);

	if (netif_carrier_ok(dev)) {
		if (!netif_carrier_ok(vlandev))
			netif_carrier_on(vlandev);
	} else {
		if (netif_carrier_ok(vlandev))
			netif_carrier_off(vlandev);
	}
}

int vlan_check_real_dev(struct net_device *real_dev, unsigned short vlan_id)
{
	char *name = real_dev->name;

	if (real_dev->features & NETIF_F_VLAN_CHALLENGED) {
		pr_info("8021q: VLANs not supported on %s\n", name);
		return -EOPNOTSUPP;
	}

	if ((real_dev->features & NETIF_F_HW_VLAN_RX) &&
	    !real_dev->vlan_rx_register) {
		pr_info("8021q: device %s has buggy VLAN hw accel\n", name);
		return -EOPNOTSUPP;
	}

	if ((real_dev->features & NETIF_F_HW_VLAN_FILTER) &&
	    (!real_dev->vlan_rx_add_vid || !real_dev->vlan_rx_kill_vid)) {
		pr_info("8021q: Device %s has buggy VLAN hw accel\n", name);
		return -EOPNOTSUPP;
	}

	/* The real device must be up and operating in order to
	 * assosciate a VLAN device with it.
	 */
	if (!(real_dev->flags & IFF_UP))
		return -ENETDOWN;

	if (__find_vlan_dev(real_dev, vlan_id) != NULL)
		return -EEXIST;

	return 0;
}

int register_vlan_dev(struct net_device *dev)
{
	struct vlan_dev_info *vlan = vlan_dev_info(dev);
	struct net_device *real_dev = vlan->real_dev;
	unsigned short vlan_id = vlan->vlan_id;
	struct vlan_group *grp, *ngrp = NULL;
	int err;

	grp = __vlan_find_group(real_dev->ifindex);
	if (!grp) {
		ngrp = grp = vlan_group_alloc(real_dev->ifindex);
		if (!grp)
			return -ENOBUFS;
	}

	err = register_netdevice(dev);
	if (err < 0)
		goto out_free_group;

	/* Account for reference in struct vlan_dev_info */
	dev_hold(real_dev);

	vlan_transfer_operstate(real_dev, dev);
	linkwatch_fire_event(dev); /* _MUST_ call rfc2863_policy() */

	/* So, got the sucker initialized, now lets place
	 * it into our local structure.
	 */
	vlan_group_set_device(grp, vlan_id, dev);
	grp->nr_vlans++;

	if (ngrp && real_dev->features & NETIF_F_HW_VLAN_RX)
		real_dev->vlan_rx_register(real_dev, ngrp);
	if (real_dev->features & NETIF_F_HW_VLAN_FILTER)
		real_dev->vlan_rx_add_vid(real_dev, vlan_id);

	if (vlan_proc_add_dev(dev) < 0)
		pr_warning("8021q: failed to add proc entry for %s\n",
			   dev->name);
	return 0;

out_free_group:
	if (ngrp)
		vlan_group_free(ngrp);
	return err;
}

/*  Attach a VLAN device to a mac address (ie Ethernet Card).
 *  Returns 0 if the device was created or a negative error code otherwise.
 */
static int register_vlan_device(struct net_device *real_dev,
				unsigned short VLAN_ID)
{
	struct net_device *new_dev;
	char name[IFNAMSIZ];
	int err;

	if (VLAN_ID >= VLAN_VID_MASK)
		return -ERANGE;

	err = vlan_check_real_dev(real_dev, VLAN_ID);
	if (err < 0)
		return err;

	/* Gotta set up the fields for the device. */
	switch (vlan_name_type) {
	case VLAN_NAME_TYPE_RAW_PLUS_VID:
		/* name will look like:	 eth1.0005 */
		snprintf(name, IFNAMSIZ, "%s.%.4i", real_dev->name, VLAN_ID);
		break;
	case VLAN_NAME_TYPE_PLUS_VID_NO_PAD:
		/* Put our vlan.VID in the name.
		 * Name will look like:	 vlan5
		 */
		snprintf(name, IFNAMSIZ, "vlan%i", VLAN_ID);
		break;
	case VLAN_NAME_TYPE_RAW_PLUS_VID_NO_PAD:
		/* Put our vlan.VID in the name.
		 * Name will look like:	 eth0.5
		 */
		snprintf(name, IFNAMSIZ, "%s.%i", real_dev->name, VLAN_ID);
		break;
	case VLAN_NAME_TYPE_PLUS_VID:
		/* Put our vlan.VID in the name.
		 * Name will look like:	 vlan0005
		 */
	default:
		snprintf(name, IFNAMSIZ, "vlan%.4i", VLAN_ID);
	}

	new_dev = alloc_netdev(sizeof(struct vlan_dev_info), name,
			       vlan_setup);

	if (new_dev == NULL)
		return -ENOBUFS;

	/* need 4 bytes for extra VLAN header info,
	 * hope the underlying device can handle it.
	 */
	new_dev->mtu = real_dev->mtu;

	vlan_dev_info(new_dev)->vlan_id = VLAN_ID; /* 1 through VLAN_VID_MASK */
	vlan_dev_info(new_dev)->real_dev = real_dev;
	vlan_dev_info(new_dev)->dent = NULL;
	vlan_dev_info(new_dev)->flags = VLAN_FLAG_REORDER_HDR;

	new_dev->rtnl_link_ops = &vlan_link_ops;
	err = register_vlan_dev(new_dev);
	if (err < 0)
		goto out_free_newdev;

	return 0;

out_free_newdev:
	free_netdev(new_dev);
	return err;
}

static void vlan_sync_address(struct net_device *dev,
			      struct net_device *vlandev)
{
	struct vlan_dev_info *vlan = vlan_dev_info(vlandev);

	/* May be called without an actual change */
	if (!compare_ether_addr(vlan->real_dev_addr, dev->dev_addr))
		return;

	/* vlan address was different from the old address and is equal to
	 * the new address */
	if (compare_ether_addr(vlandev->dev_addr, vlan->real_dev_addr) &&
	    !compare_ether_addr(vlandev->dev_addr, dev->dev_addr))
		dev_unicast_delete(dev, vlandev->dev_addr, ETH_ALEN);

	/* vlan address was equal to the old address and is different from
	 * the new address */
	if (!compare_ether_addr(vlandev->dev_addr, vlan->real_dev_addr) &&
	    compare_ether_addr(vlandev->dev_addr, dev->dev_addr))
		dev_unicast_add(dev, vlandev->dev_addr, ETH_ALEN);

	memcpy(vlan->real_dev_addr, dev->dev_addr, ETH_ALEN);
}

static int vlan_device_event(struct notifier_block *unused, unsigned long event,
			     void *ptr)
{
	struct net_device *dev = ptr;
	struct vlan_group *grp = __vlan_find_group(dev->ifindex);
	int i, flgs;
	struct net_device *vlandev;

	if (dev->nd_net != &init_net)
		return NOTIFY_DONE;

	if (!grp)
		goto out;

	/* It is OK that we do not hold the group lock right now,
	 * as we run under the RTNL lock.
	 */

	switch (event) {
	case NETDEV_CHANGE:
		/* Propagate real device state to vlan devices */
		for (i = 0; i < VLAN_GROUP_ARRAY_LEN; i++) {
			vlandev = vlan_group_get_device(grp, i);
			if (!vlandev)
				continue;

			vlan_transfer_operstate(dev, vlandev);
		}
		break;

	case NETDEV_CHANGEADDR:
		/* Adjust unicast filters on underlying device */
		for (i = 0; i < VLAN_GROUP_ARRAY_LEN; i++) {
			vlandev = vlan_group_get_device(grp, i);
			if (!vlandev)
				continue;

			flgs = vlandev->flags;
			if (!(flgs & IFF_UP))
				continue;

			vlan_sync_address(dev, vlandev);
		}
		break;

	case NETDEV_DOWN:
		/* Put all VLANs for this dev in the down state too.  */
		for (i = 0; i < VLAN_GROUP_ARRAY_LEN; i++) {
			vlandev = vlan_group_get_device(grp, i);
			if (!vlandev)
				continue;

			flgs = vlandev->flags;
			if (!(flgs & IFF_UP))
				continue;

			dev_change_flags(vlandev, flgs & ~IFF_UP);
		}
		break;

	case NETDEV_UP:
		/* Put all VLANs for this dev in the up state too.  */
		for (i = 0; i < VLAN_GROUP_ARRAY_LEN; i++) {
			vlandev = vlan_group_get_device(grp, i);
			if (!vlandev)
				continue;

			flgs = vlandev->flags;
			if (flgs & IFF_UP)
				continue;

			dev_change_flags(vlandev, flgs | IFF_UP);
		}
		break;

	case NETDEV_UNREGISTER:
		/* Delete all VLANs for this dev. */
		for (i = 0; i < VLAN_GROUP_ARRAY_LEN; i++) {
			vlandev = vlan_group_get_device(grp, i);
			if (!vlandev)
				continue;

			/* unregistration of last vlan destroys group, abort
			 * afterwards */
			if (grp->nr_vlans == 1)
				i = VLAN_GROUP_ARRAY_LEN;

			unregister_vlan_dev(vlandev);
		}
		break;
	}

out:
	return NOTIFY_DONE;
}

static struct notifier_block vlan_notifier_block __read_mostly = {
	.notifier_call = vlan_device_event,
};

/*
 *	VLAN IOCTL handler.
 *	o execute requested action or pass command to the device driver
 *   arg is really a struct vlan_ioctl_args __user *.
 */
static int vlan_ioctl_handler(struct net *net, void __user *arg)
{
	int err;
	unsigned short vid = 0;
	struct vlan_ioctl_args args;
	struct net_device *dev = NULL;

	if (copy_from_user(&args, arg, sizeof(struct vlan_ioctl_args)))
		return -EFAULT;

	/* Null terminate this sucker, just in case. */
	args.device1[23] = 0;
	args.u.device2[23] = 0;

	rtnl_lock();

	switch (args.cmd) {
	case SET_VLAN_INGRESS_PRIORITY_CMD:
	case SET_VLAN_EGRESS_PRIORITY_CMD:
	case SET_VLAN_FLAG_CMD:
	case ADD_VLAN_CMD:
	case DEL_VLAN_CMD:
	case GET_VLAN_REALDEV_NAME_CMD:
	case GET_VLAN_VID_CMD:
		err = -ENODEV;
		dev = __dev_get_by_name(&init_net, args.device1);
		if (!dev)
			goto out;

		err = -EINVAL;
		if (args.cmd != ADD_VLAN_CMD &&
		    !(dev->priv_flags & IFF_802_1Q_VLAN))
			goto out;
	}

	switch (args.cmd) {
	case SET_VLAN_INGRESS_PRIORITY_CMD:
		err = -EPERM;
		if (!capable(CAP_NET_ADMIN))
			break;
		vlan_dev_set_ingress_priority(dev,
					      args.u.skb_priority,
					      args.vlan_qos);
		err = 0;
		break;

	case SET_VLAN_EGRESS_PRIORITY_CMD:
		err = -EPERM;
		if (!capable(CAP_NET_ADMIN))
			break;
		err = vlan_dev_set_egress_priority(dev,
						   args.u.skb_priority,
						   args.vlan_qos);
		break;

	case SET_VLAN_FLAG_CMD:
		err = -EPERM;
		if (!capable(CAP_NET_ADMIN))
			break;
		err = vlan_dev_set_vlan_flag(dev,
					     args.u.flag,
					     args.vlan_qos);
		break;

	case SET_VLAN_NAME_TYPE_CMD:
		err = -EPERM;
		if (!capable(CAP_NET_ADMIN))
			break;
		if ((args.u.name_type >= 0) &&
		    (args.u.name_type < VLAN_NAME_TYPE_HIGHEST)) {
			vlan_name_type = args.u.name_type;
			err = 0;
		} else {
			err = -EINVAL;
		}
		break;

	case ADD_VLAN_CMD:
		err = -EPERM;
		if (!capable(CAP_NET_ADMIN))
			break;
		err = register_vlan_device(dev, args.u.VID);
		break;

	case DEL_VLAN_CMD:
		err = -EPERM;
		if (!capable(CAP_NET_ADMIN))
			break;
		unregister_vlan_dev(dev);
		err = 0;
		break;

	case GET_VLAN_REALDEV_NAME_CMD:
		err = 0;
		vlan_dev_get_realdev_name(dev, args.u.device2);
		if (copy_to_user(arg, &args,
				 sizeof(struct vlan_ioctl_args)))
			err = -EFAULT;
		break;

	case GET_VLAN_VID_CMD:
		err = 0;
		vlan_dev_get_vid(dev, &vid);
		args.u.VID = vid;
		if (copy_to_user(arg, &args,
				 sizeof(struct vlan_ioctl_args)))
		      err = -EFAULT;
		break;

	default:
		err = -EOPNOTSUPP;
		break;
	}
out:
	rtnl_unlock();
	return err;
}

static int __init vlan_proto_init(void)
{
	int err;

	pr_info("%s v%s %s\n", vlan_fullname, vlan_version, vlan_copyright);
	pr_info("All bugs added by %s\n", vlan_buggyright);

	err = vlan_proc_init();
	if (err < 0)
		goto err1;

	err = register_netdevice_notifier(&vlan_notifier_block);
	if (err < 0)
		goto err2;

	err = vlan_netlink_init();
	if (err < 0)
		goto err3;

	dev_add_pack(&vlan_packet_type);
	vlan_ioctl_set(vlan_ioctl_handler);
	return 0;

err3:
	unregister_netdevice_notifier(&vlan_notifier_block);
err2:
	vlan_proc_cleanup();
err1:
	return err;
}

static void __exit vlan_cleanup_module(void)
{
	unsigned int i;

	vlan_ioctl_set(NULL);
	vlan_netlink_fini();

	unregister_netdevice_notifier(&vlan_notifier_block);

	dev_remove_pack(&vlan_packet_type);

	/* This table must be empty if there are no module references left. */
	for (i = 0; i < VLAN_GRP_HASH_SIZE; i++)
		BUG_ON(!hlist_empty(&vlan_group_hash[i]));

	vlan_proc_cleanup();

	synchronize_net();
}

module_init(vlan_proto_init);
module_exit(vlan_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
