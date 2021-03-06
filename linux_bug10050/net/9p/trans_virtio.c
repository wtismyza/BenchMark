/*
 * The Guest 9p transport driver
 *
 * This is a block based transport driver based on the lguest block driver
 * code.
 *
 */
/*
 *  Copyright (C) 2007 Eric Van Hensbergen, IBM Corporation
 *
 *  Based on virtio console driver
 *  Copyright (C) 2006, 2007 Rusty Russell, IBM Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2
 *  as published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to:
 *  Free Software Foundation
 *  51 Franklin Street, Fifth Floor
 *  Boston, MA  02111-1301  USA
 *
 */

#include <linux/in.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/ipv6.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/un.h>
#include <linux/uaccess.h>
#include <linux/inet.h>
#include <linux/idr.h>
#include <linux/file.h>
#include <net/9p/9p.h>
#include <linux/parser.h>
#include <net/9p/transport.h>
#include <linux/scatterlist.h>
#include <linux/virtio.h>
#include <linux/virtio_9p.h>

#define VIRTQUEUE_NUM	128

/* a single mutex to manage channel initialization and attachment */
static DECLARE_MUTEX(virtio_9p_lock);
/* global which tracks highest initialized channel */
static int chan_index;

#define P9_INIT_MAXTAG	16

#define REQ_STATUS_IDLE	0
#define REQ_STATUS_SENT 1
#define REQ_STATUS_RCVD 2
#define REQ_STATUS_FLSH 3

struct p9_req_t {
	int status;
	wait_queue_head_t *wq;
};

/* We keep all per-channel information in a structure.
 * This structure is allocated within the devices dev->mem space.
 * A pointer to the structure will get put in the transport private.
 */
static struct virtio_chan {
	bool initialized;		/* channel is initialized */
	bool inuse;			/* channel is in use */

	spinlock_t lock;

	struct virtio_device *vdev;
	struct virtqueue *vq;

	struct p9_idpool *tagpool;
	struct p9_req_t *reqs;
	int max_tag;

	/* Scatterlist: can be too big for stack. */
	struct scatterlist sg[VIRTQUEUE_NUM];
} channels[MAX_9P_CHAN];

/* Lookup requests by tag */
static struct p9_req_t *p9_lookup_tag(struct virtio_chan *c, u16 tag)
{
	/* This looks up the original request by tag so we know which
	 * buffer to read the data into */
	tag++;

	while (tag >= c->max_tag) {
		int old_max = c->max_tag;
		int count;

		if (c->max_tag)
			c->max_tag *= 2;
		else
			c->max_tag = P9_INIT_MAXTAG;

		c->reqs = krealloc(c->reqs, sizeof(struct p9_req_t)*c->max_tag,
								GFP_ATOMIC);
		if (!c->reqs) {
			printk(KERN_ERR "Couldn't grow tag array\n");
			BUG();
		}
		for (count = old_max; count < c->max_tag; count++) {
			c->reqs[count].status = REQ_STATUS_IDLE;
			c->reqs[count].wq = kmalloc(sizeof(wait_queue_t),
								GFP_ATOMIC);
			if (!c->reqs[count].wq) {
				printk(KERN_ERR "Couldn't grow tag array\n");
				BUG();
			}
			init_waitqueue_head(c->reqs[count].wq);
		}
	}

	return &c->reqs[tag];
}


/* How many bytes left in this page. */
static unsigned int rest_of_page(void *data)
{
	return PAGE_SIZE - ((unsigned long)data % PAGE_SIZE);
}

static void p9_virtio_close(struct p9_trans *trans)
{
	struct virtio_chan *chan = trans->priv;
	int count;
	unsigned int flags;

	spin_lock_irqsave(&chan->lock, flags);
	p9_idpool_destroy(chan->tagpool);
	for (count = 0; count < chan->max_tag; count++)
		kfree(chan->reqs[count].wq);
	kfree(chan->reqs);
	chan->max_tag = 0;
	spin_unlock_irqrestore(&chan->lock, flags);

	down(&virtio_9p_lock);
	chan->inuse = false;
	up(&virtio_9p_lock);

	kfree(trans);
}

static void req_done(struct virtqueue *vq)
{
	struct virtio_chan *chan = vq->vdev->priv;
	struct p9_fcall *rc;
	unsigned int len;
	unsigned long flags;
	struct p9_req_t *req;

	spin_lock_irqsave(&chan->lock, flags);
	while ((rc = chan->vq->vq_ops->get_buf(chan->vq, &len)) != NULL) {
		req = p9_lookup_tag(chan, rc->tag);
		req->status = REQ_STATUS_RCVD;
		wake_up(req->wq);
	}
	/* In case queue is stopped waiting for more buffers. */
	spin_unlock_irqrestore(&chan->lock, flags);
}

static int
pack_sg_list(struct scatterlist *sg, int start, int limit, char *data,
								int count)
{
	int s;
	int index = start;

	while (count) {
		s = rest_of_page(data);
		if (s > count)
			s = count;
		sg_set_buf(&sg[index++], data, s);
		count -= s;
		data += s;
		if (index > limit)
			BUG();
	}

	return index-start;
}

static int
p9_virtio_rpc(struct p9_trans *t, struct p9_fcall *tc, struct p9_fcall **rc)
{
	int in, out;
	int n, err, size;
	struct virtio_chan *chan = t->priv;
	char *rdata;
	struct p9_req_t *req;
	unsigned long flags;

	if (*rc == NULL) {
		*rc = kmalloc(sizeof(struct p9_fcall) + t->msize, GFP_KERNEL);
		if (!*rc)
			return -ENOMEM;
	}

	rdata = (char *)*rc+sizeof(struct p9_fcall);

	n = P9_NOTAG;
	if (tc->id != P9_TVERSION) {
		n = p9_idpool_get(chan->tagpool);
		if (n < 0)
			return -ENOMEM;
	}

	spin_lock_irqsave(&chan->lock, flags);
	req = p9_lookup_tag(chan, n);
	spin_unlock_irqrestore(&chan->lock, flags);

	p9_set_tag(tc, n);

	P9_DPRINTK(P9_DEBUG_TRANS, "9p debug: virtio rpc tag %d\n", n);

	out = pack_sg_list(chan->sg, 0, VIRTQUEUE_NUM, tc->sdata, tc->size);
	in = pack_sg_list(chan->sg, out, VIRTQUEUE_NUM-out, rdata, t->msize);

	req->status = REQ_STATUS_SENT;

	if (chan->vq->vq_ops->add_buf(chan->vq, chan->sg, out, in, tc)) {
		P9_DPRINTK(P9_DEBUG_TRANS,
			"9p debug: virtio rpc add_buf returned failure");
		return -EIO;
	}

	chan->vq->vq_ops->kick(chan->vq);

	wait_event(*req->wq, req->status == REQ_STATUS_RCVD);

	size = le32_to_cpu(*(__le32 *) rdata);

	err = p9_deserialize_fcall(rdata, size, *rc, t->extended);
	if (err < 0) {
		P9_DPRINTK(P9_DEBUG_TRANS,
			"9p debug: virtio rpc deserialize returned %d\n", err);
		return err;
	}

#ifdef CONFIG_NET_9P_DEBUG
	if ((p9_debug_level&P9_DEBUG_FCALL) == P9_DEBUG_FCALL) {
		char buf[150];

		p9_printfcall(buf, sizeof(buf), *rc, t->extended);
		printk(KERN_NOTICE ">>> %p %s\n", t, buf);
	}
#endif

	if (n != P9_NOTAG && p9_idpool_check(n, chan->tagpool))
		p9_idpool_put(n, chan->tagpool);

	req->status = REQ_STATUS_IDLE;

	return 0;
}

static int p9_virtio_probe(struct virtio_device *vdev)
{
	int err;
	struct virtio_chan *chan;
	int index;

	down(&virtio_9p_lock);
	index = chan_index++;
	chan = &channels[index];
	up(&virtio_9p_lock);

	if (chan_index > MAX_9P_CHAN) {
		printk(KERN_ERR "9p: virtio: Maximum channels exceeded\n");
		BUG();
		err = -ENOMEM;
		goto fail;
	}

	chan->vdev = vdev;

	/* We expect one virtqueue, for requests. */
	chan->vq = vdev->config->find_vq(vdev, 0, req_done);
	if (IS_ERR(chan->vq)) {
		err = PTR_ERR(chan->vq);
		goto out_free_vq;
	}
	chan->vq->vdev->priv = chan;
	spin_lock_init(&chan->lock);

	sg_init_table(chan->sg, VIRTQUEUE_NUM);

	chan->inuse = false;
	chan->initialized = true;
	return 0;

out_free_vq:
	vdev->config->del_vq(chan->vq);
fail:
	down(&virtio_9p_lock);
	chan_index--;
	up(&virtio_9p_lock);
	return err;
}

/* This sets up a transport channel for 9p communication.  Right now
 * we only match the first available channel, but eventually we couldlook up
 * alternate channels by matching devname versus a virtio_config entry.
 * We use a simple reference count mechanism to ensure that only a single
 * mount has a channel open at a time. */
static struct p9_trans *
p9_virtio_create(const char *devname, char *args, int msize,
							unsigned char extended)
{
	struct p9_trans *trans;
	struct virtio_chan *chan = channels;
	int index = 0;

	down(&virtio_9p_lock);
	while (index < MAX_9P_CHAN) {
		if (chan->initialized && !chan->inuse) {
			chan->inuse = true;
			break;
		} else {
			index++;
			chan = &channels[index];
		}
	}
	up(&virtio_9p_lock);

	if (index >= MAX_9P_CHAN) {
		printk(KERN_ERR "9p: no channels available\n");
		return ERR_PTR(-ENODEV);
	}

	chan->tagpool = p9_idpool_create();
	if (IS_ERR(chan->tagpool)) {
		printk(KERN_ERR "9p: couldn't allocate tagpool\n");
		return ERR_PTR(-ENOMEM);
	}
	p9_idpool_get(chan->tagpool); /* reserve tag 0 */
	chan->max_tag = 0;
	chan->reqs = NULL;

	trans = kmalloc(sizeof(struct p9_trans), GFP_KERNEL);
	if (!trans) {
		printk(KERN_ERR "9p: couldn't allocate transport\n");
		return ERR_PTR(-ENOMEM);
	}
	trans->extended = extended;
	trans->msize = msize;
	trans->close = p9_virtio_close;
	trans->rpc = p9_virtio_rpc;
	trans->priv = chan;

	return trans;
}

static void p9_virtio_remove(struct virtio_device *vdev)
{
	struct virtio_chan *chan = vdev->priv;

	BUG_ON(chan->inuse);

	if (chan->initialized) {
		vdev->config->del_vq(chan->vq);
		chan->initialized = false;
	}
}

#define VIRTIO_ID_9P 9

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_9P, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

/* The standard "struct lguest_driver": */
static struct virtio_driver p9_virtio_drv = {
	.driver.name = 	KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table =	id_table,
	.probe = 	p9_virtio_probe,
	.remove =	p9_virtio_remove,
};

static struct p9_trans_module p9_virtio_trans = {
	.name = "virtio",
	.create = p9_virtio_create,
	.maxsize = PAGE_SIZE*16,
	.def = 0,
};

/* The standard init function */
static int __init p9_virtio_init(void)
{
	int count;

	for (count = 0; count < MAX_9P_CHAN; count++)
		channels[count].initialized = false;

	v9fs_register_trans(&p9_virtio_trans);
	return register_virtio_driver(&p9_virtio_drv);
}

static void __exit p9_virtio_cleanup(void)
{
	unregister_virtio_driver(&p9_virtio_drv);
}

module_init(p9_virtio_init);
module_exit(p9_virtio_cleanup);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_AUTHOR("Eric Van Hensbergen <ericvh@gmail.com>");
MODULE_DESCRIPTION("Virtio 9p Transport");
MODULE_LICENSE("GPL");
