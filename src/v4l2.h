#ifndef V4L2_H
#define V4L2_H
#include "multicam.h"
int v4l2_close_device(v4l2camObject *self);
int v4l2_get_control(int fd, int id, int *value);
int v4l2_init_device(v4l2camObject *self);
int v4l2_init_mmap(v4l2camObject *self);
int v4l2_open_device(v4l2camObject *self);
int v4l2_query_buffer(v4l2camObject *self);
int v4l2_set_control(int fd, int id, int value);
int v4l2_set_pixelformat(int fd, struct v4l2_format *fmt, unsigned long pixelformat);
int v4l2_start_capturing(v4l2camObject *self);
int v4l2_stop_capturing(v4l2camObject *self);
int v4l2_uninit_device(v4l2camObject *self);
int v4l2_xioctl(int fd, int request, void *arg);
#endif //V4L2_H
