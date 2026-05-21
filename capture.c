/*
 * capture.c — V4L2 用户态捕获程序
 *
 * 用法：gcc -o capture capture.c && sudo ./capture [/dev/video0] [帧数]
 *
 * 完整 ioctl 序列：
 *   open → QUERYCAP → S_FMT → REQBUFS → QUERYBUF/mmap → QBUF → STREAMON
 *   → 循环(DQBUF → 处理帧 → QBUF) → STREAMOFF → 释放资源
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define BUF_COUNT  4
#define DEFAULT_DEV "/dev/video0"
#define DEFAULT_FRAMES 90  /* 3 秒 @ 30fps */

/* ---- 工具函数 ---- */

static int xioctl(int fd, unsigned long req, void *arg)
{
	int ret;
	do {
		ret = ioctl(fd, req, arg);
	} while (ret == -1 && errno == EINTR);
	return ret;
}

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---- 主程序 ---- */

int main(int argc, char *argv[])
{
	const char *dev    = argc > 1 ? argv[1] : DEFAULT_DEV;
	int total_frames   = argc > 2 ? atoi(argv[2]) : DEFAULT_FRAMES;
	int fd;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_buffer buf;
	void  *mem[BUF_COUNT];
	size_t mem_len[BUF_COUNT];
	enum v4l2_buf_type type;

	/* 1. 打开设备 */
	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* 2. 查询设备能力 */
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("QUERYCAP");
		return 1;
	}
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
	    !(cap.capabilities & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "device does not support capture/streaming\n");
		return 1;
	}
	printf("device: %s  driver: %s\n", cap.card, cap.driver);

	/* 3. 设置格式：640x480 YUYV */
	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = 640;
	fmt.fmt.pix.height      = 480;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field       = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT");
		return 1;
	}
	printf("format: %dx%d  bytes/line: %d  size: %d\n",
	       fmt.fmt.pix.width, fmt.fmt.pix.height,
	       fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);

	/* 4. 申请 MMAP 缓冲区 */
	memset(&req, 0, sizeof(req));
	req.count  = BUF_COUNT;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS");
		return 1;
	}
	printf("buffers allocated: %d\n", req.count);

	/* 5. 查询每个 buffer 并 mmap */
	for (int i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			perror("QUERYBUF");
			return 1;
		}
		mem_len[i] = buf.length;
		mem[i] = mmap(NULL, buf.length,
			      PROT_READ | PROT_WRITE, MAP_SHARED,
			      fd, buf.m.offset);
		if (mem[i] == MAP_FAILED) {
			perror("mmap");
			return 1;
		}
	}

	/* 6. 所有 buffer 入队 */
	for (int i = 0; i < (int)req.count; i++) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = i;
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF");
			return 1;
		}
	}

	/* 7. 启动流 */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		perror("STREAMON");
		return 1;
	}
	printf("streaming started, capturing %d frames...\n", total_frames);

	/* 8. 捕获循环 */
	int    frames    = 0;
	double t_start   = now_ms();
	double t_last    = t_start;
	size_t total_bytes = 0;

	while (frames < total_frames) {
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		/* DQBUF: 阻塞等待驱动填好一帧 */
		if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			perror("DQBUF");
			break;
		}

		frames++;
		total_bytes += buf.bytesused;

		/* 每 30 帧打印一次 fps */
		if (frames % 30 == 0) {
			double t_now = now_ms();
			double fps   = 30000.0 / (t_now - t_last);
			t_last = t_now;
			printf("frame %4d  seq=%d  size=%d  fps=%.1f\n",
			       frames, buf.sequence, buf.bytesused, fps);
		}

		/* 将 buffer 重新入队供驱动复用 */
		if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			perror("QBUF (requeue)");
			break;
		}
	}

	/* 9. 停止流 */
	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	xioctl(fd, VIDIOC_STREAMOFF, &type);

	double elapsed = (now_ms() - t_start) / 1000.0;
	printf("\n--- summary ---\n");
	printf("frames: %d  time: %.2fs  avg_fps: %.1f  total: %.1f MB\n",
	       frames, elapsed, frames / elapsed,
	       total_bytes / 1024.0 / 1024.0);

	/* 10. 释放资源 */
	for (int i = 0; i < (int)req.count; i++)
		munmap(mem[i], mem_len[i]);
	close(fd);
	return 0;
}
