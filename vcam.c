/*
 * vcam.c — V4L2 虚拟摄像头驱动
 *
 * 注册 /dev/video0，kthread 以 30fps 生成 YUYV 彩条测试图案。
 * FFmpeg / GStreamer / v4l2-ctl 可直接使用，无需真实摄像头硬件。
 *
 * 移植到真实硬件（如 OV5647）时：
 *   - 删除 kthread + fill_frame，改为 I2C 初始化传感器后从 MIPI CSI DMA 取帧
 *   - queue_setup 中的 sizes[0] 改为实际帧大小
 *   - 其余 V4L2 / vb2 框架代码不动
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hayes");
MODULE_DESCRIPTION("V4L2 virtual camera: 640x480 YUYV 30fps test pattern");

#define VCAM_W   640
#define VCAM_H   480
#define VCAM_FPS 30
#define VCAM_BPP 2   /* YUYV: 2 bytes per pixel */
#define VCAM_BUF_SIZE (VCAM_W * VCAM_H * VCAM_BPP)

/* YUYV 标准彩条（8色）：Y / Cb(U) / Cr(V) */
static const u8 bar_y[8] = {235, 210, 170, 145, 106,  81,  41,  16};
static const u8 bar_u[8] = {128,  16, 166,  54, 202,  90, 240, 128};
static const u8 bar_v[8] = {128, 146,  16,  34, 222, 240, 110, 128};

/* ---- 私有结构 ---- */

struct vcam_buf {
	struct vb2_v4l2_buffer vbuf;   /* 必须是第一个成员，供 container_of */
	struct list_head       list;
};

struct vcam_dev {
	struct v4l2_device   v4l2_dev;
	struct video_device  vdev;
	struct vb2_queue     queue;
	struct mutex         lock;    /* V4L2 序列化锁 */
	spinlock_t           slock;   /* buf_list 保护锁（中断安全） */
	struct list_head     buf_list;
	struct task_struct  *kthread;
	u32                  sequence;
};

static struct vcam_dev *vcam;

/* ---- 帧生成 ---- */

/*
 * 生成 YUYV 彩条图案：
 *   - 8 条等宽色块横向排列
 *   - 一条白色扫描线随帧序号向下移动，视觉上确认驱动在持续工作
 */
static void fill_frame(void *buf, u32 seq)
{
	u8 *p = (u8 *)buf;
	int bar_w = VCAM_W / 8;
	int scan_y = (seq * 4) % VCAM_H;  /* 扫描线位置 */
	int x, y;

	for (y = 0; y < VCAM_H; y++) {
		if (y == scan_y) {
			/* 白色扫描线：Y=235 U=128 V=128 */
			memset(p, 0, VCAM_W * VCAM_BPP);
			for (x = 0; x < VCAM_W * VCAM_BPP; x += 4) {
				p[x]     = 235; /* Y0 */
				p[x + 1] = 128; /* U  */
				p[x + 2] = 235; /* Y1 */
				p[x + 3] = 128; /* V  */
			}
			p += VCAM_W * VCAM_BPP;
			continue;
		}
		for (x = 0; x < VCAM_W; x += 2) {
			int bar = min(x / bar_w, 7);
			*p++ = bar_y[bar]; /* Y0 */
			*p++ = bar_u[bar]; /* U  */
			*p++ = bar_y[bar]; /* Y1 */
			*p++ = bar_v[bar]; /* V  */
		}
	}
}

/* ---- kthread ---- */

static int vcam_thread(void *data)
{
	struct vcam_dev *dev = data;

	while (!kthread_should_stop()) {
		struct vcam_buf *vbuf = NULL;
		unsigned long flags;

		spin_lock_irqsave(&dev->slock, flags);
		if (!list_empty(&dev->buf_list)) {
			vbuf = list_first_entry(&dev->buf_list,
						struct vcam_buf, list);
			list_del(&vbuf->list);
		}
		spin_unlock_irqrestore(&dev->slock, flags);

		if (vbuf) {
			void *mem = vb2_plane_vaddr(&vbuf->vbuf.vb2_buf, 0);

			fill_frame(mem, dev->sequence);
			vb2_set_plane_payload(&vbuf->vbuf.vb2_buf, 0, VCAM_BUF_SIZE);

			vbuf->vbuf.vb2_buf.timestamp = ktime_get_ns();
			vbuf->vbuf.sequence          = dev->sequence++;
			vbuf->vbuf.field             = V4L2_FIELD_NONE;

			vb2_buffer_done(&vbuf->vbuf.vb2_buf, VB2_BUF_STATE_DONE);
		}

		msleep(1000 / VCAM_FPS);
	}
	return 0;
}

/* ---- vb2 ops ---- */

static int vcam_queue_setup(struct vb2_queue *q,
			    unsigned int *nbuffers, unsigned int *nplanes,
			    unsigned int sizes[], struct device *alloc_devs[])
{
	if (*nbuffers < 2)
		*nbuffers = 2;
	*nplanes  = 1;
	sizes[0]  = VCAM_BUF_SIZE;
	return 0;
}

static int vcam_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < VCAM_BUF_SIZE)
		return -EINVAL;
	return 0;
}

static void vcam_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vcam_buf *buf = container_of(vbuf, struct vcam_buf, vbuf);
	unsigned long flags;

	spin_lock_irqsave(&vcam->slock, flags);
	list_add_tail(&buf->list, &vcam->buf_list);
	spin_unlock_irqrestore(&vcam->slock, flags);
}

static int vcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	vcam->sequence = 0;
	vcam->kthread  = kthread_run(vcam_thread, vcam, "vcam");
	if (IS_ERR(vcam->kthread)) {
		int ret = PTR_ERR(vcam->kthread);
		vcam->kthread = NULL;
		return ret;
	}
	return 0;
}

static void vcam_stop_streaming(struct vb2_queue *q)
{
	struct vcam_buf *buf, *tmp;
	unsigned long flags;

	if (vcam->kthread) {
		kthread_stop(vcam->kthread);
		vcam->kthread = NULL;
	}

	/* 将队列中未处理的 buffer 全部标记为错误返回给 vb2 */
	spin_lock_irqsave(&vcam->slock, flags);
	list_for_each_entry_safe(buf, tmp, &vcam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vbuf.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vcam->slock, flags);
}

static const struct vb2_ops vcam_vb2_ops = {
	.queue_setup     = vcam_queue_setup,
	.buf_prepare     = vcam_buf_prepare,
	.buf_queue       = vcam_buf_queue,
	.start_streaming = vcam_start_streaming,
	.stop_streaming  = vcam_stop_streaming,
	.wait_prepare    = vb2_ops_wait_prepare,
	.wait_finish     = vb2_ops_wait_finish,
};

/* ---- V4L2 ioctl ops ---- */

static int vcam_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	strscpy(cap->driver, "vcam",       sizeof(cap->driver));
	strscpy(cap->card,   "vcam",       sizeof(cap->card));
	strscpy(cap->bus_info, "platform:vcam", sizeof(cap->bus_info));
	cap->device_caps  = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;
	return 0;
}

static int vcam_enum_fmt(struct file *file, void *priv,
			 struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;
	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static void vcam_fill_pix_format(struct v4l2_pix_format *pix)
{
	pix->width        = VCAM_W;
	pix->height       = VCAM_H;
	pix->pixelformat  = V4L2_PIX_FMT_YUYV;
	pix->field        = V4L2_FIELD_NONE;
	pix->bytesperline = VCAM_W * VCAM_BPP;
	pix->sizeimage    = VCAM_BUF_SIZE;
	pix->colorspace   = V4L2_COLORSPACE_SRGB;
}

static int vcam_g_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	vcam_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int vcam_try_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	/* 只支持固定分辨率和格式，忽略用户请求直接填回 */
	vcam_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int vcam_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	if (vb2_is_busy(&vcam->queue))
		return -EBUSY;
	vcam_fill_pix_format(&f->fmt.pix);
	return 0;
}

static int vcam_enum_input(struct file *file, void *priv,
			   struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;
	strscpy(inp->name, "test pattern", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int vcam_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vcam_s_input(struct file *file, void *priv, unsigned int i)
{
	return (i == 0) ? 0 : -EINVAL;
}

static const struct v4l2_ioctl_ops vcam_ioctl_ops = {
	.vidioc_querycap         = vcam_querycap,
	.vidioc_enum_fmt_vid_cap = vcam_enum_fmt,
	.vidioc_g_fmt_vid_cap    = vcam_g_fmt,
	.vidioc_try_fmt_vid_cap  = vcam_try_fmt,
	.vidioc_s_fmt_vid_cap    = vcam_s_fmt,
	.vidioc_enum_input       = vcam_enum_input,
	.vidioc_g_input          = vcam_g_input,
	.vidioc_s_input          = vcam_s_input,
	/* vb2 辅助函数直接处理 buffer 管理 ioctl */
	.vidioc_reqbufs          = vb2_ioctl_reqbufs,
	.vidioc_querybuf         = vb2_ioctl_querybuf,
	.vidioc_qbuf             = vb2_ioctl_qbuf,
	.vidioc_dqbuf            = vb2_ioctl_dqbuf,
	.vidioc_streamon         = vb2_ioctl_streamon,
	.vidioc_streamoff        = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations vcam_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.mmap           = vb2_fop_mmap,
	.unlocked_ioctl = video_ioctl2,
};

/* ---- 模块初始化 ---- */

static int __init vcam_init(void)
{
	int ret;

	vcam = kzalloc(sizeof(*vcam), GFP_KERNEL);
	if (!vcam)
		return -ENOMEM;

	mutex_init(&vcam->lock);
	spin_lock_init(&vcam->slock);
	INIT_LIST_HEAD(&vcam->buf_list);

	/* 注册 v4l2_device：无物理设备时需手动设置 name */
	strscpy(vcam->v4l2_dev.name, "vcam", sizeof(vcam->v4l2_dev.name));
	ret = v4l2_device_register(NULL, &vcam->v4l2_dev);
	if (ret) {
		pr_err("vcam: v4l2_device_register failed: %d\n", ret);
		goto err_free;
	}

	/* 初始化 vb2 队列 */
	vcam->queue.type            = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vcam->queue.io_modes        = VB2_MMAP;
	vcam->queue.drv_priv        = vcam;
	vcam->queue.buf_struct_size = sizeof(struct vcam_buf);
	vcam->queue.ops             = &vcam_vb2_ops;
	vcam->queue.mem_ops         = &vb2_vmalloc_memops;
	vcam->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vcam->queue.lock            = &vcam->lock;

	ret = vb2_queue_init(&vcam->queue);
	if (ret) {
		pr_err("vcam: vb2_queue_init failed: %d\n", ret);
		goto err_v4l2;
	}

	/* 注册 video_device（/dev/videoX 节点） */
	strscpy(vcam->vdev.name, "vcam", sizeof(vcam->vdev.name));
	vcam->vdev.v4l2_dev  = &vcam->v4l2_dev;
	vcam->vdev.fops      = &vcam_fops;
	vcam->vdev.ioctl_ops = &vcam_ioctl_ops;
	vcam->vdev.release   = video_device_release_empty;
	vcam->vdev.queue     = &vcam->queue;
	vcam->vdev.lock      = &vcam->lock;
	vcam->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	video_set_drvdata(&vcam->vdev, vcam);

	ret = video_register_device(&vcam->vdev, VFL_TYPE_VIDEO, -1);
	if (ret) {
		pr_err("vcam: video_register_device failed: %d\n", ret);
		goto err_v4l2;
	}

	pr_info("vcam: registered as /dev/video%d\n", vcam->vdev.num);
	return 0;

err_v4l2:
	v4l2_device_unregister(&vcam->v4l2_dev);
err_free:
	kfree(vcam);
	return ret;
}

static void __exit vcam_exit(void)
{
	video_unregister_device(&vcam->vdev);
	v4l2_device_unregister(&vcam->v4l2_dev);
	kfree(vcam);
	pr_info("vcam: unregistered\n");
}

module_init(vcam_init);
module_exit(vcam_exit);
