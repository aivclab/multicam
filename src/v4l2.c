#include <Python.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>              /* low-level i/o */

#include <linux/videodev2.h>

#include "v4l2.h"
#include "libyuv.h" //TODO: remove? Used for FOURCC


#define CLEAR(x) memset(&(x), 0, sizeof(x))

/*
 * Functions for v4l2 cameras.
 * This code is based partly on pyvideograb by Laurent Pointal at
 * http://laurent.pointal.org/python/projets/pyvideograb
*/
int v4l2_xioctl (int fd, int request, void *arg)
{
    int r;

    do {
        r = ioctl(fd, request, arg);
    }
    while (r == -1 && errno == EINTR);

    return r;
}


/* A wrapper around a VIDIOC_S_FMT ioctl to check for format compatibility */

int
v4l2_set_pixelformat(v4l2camObject *self, struct v4l2_format *fmt, unsigned long pixelformat)
{
    fmt->fmt.pix.pixelformat = pixelformat;

    if (-1 == v4l2_xioctl(self->fd, VIDIOC_S_FMT, fmt)) {
        PyErr_Format(PyExc_SystemError, "%s: set_pixelformat failed (ioctl(VIDIOC_S_FMT))", self->device);
        return 0;
    }

    if (fmt->fmt.pix.pixelformat == pixelformat) {
        return 1;
    }
    else {
        PyErr_Format(PyExc_SystemError, "%s: set_pixelformat failed (ioctl(VIDIOC_S_FMT))", self->device);
        return 0;
    }
}


/* gets the value of a specific camera control if available */
int
v4l2_get_control(int fd, int id, int *value)
{
    struct v4l2_control control;
    CLEAR(control);

    control.id = id;

    if (-1 == v4l2_xioctl(fd, VIDIOC_G_CTRL, &control)) {
        return 0;
    }

    *value = control.value;
    return 1;
}

/* sets a control if supported. the camera may round the value */
int
v4l2_set_control(int fd, int id, int value)
{
    struct v4l2_control control;
    CLEAR(control);

    control.id = id;
    control.value = value;

    if (-1 == v4l2_xioctl(fd, VIDIOC_S_CTRL, &control)) {
        return 0;
    }

    return 1;
}



int
v4l2_query_buffer(v4l2camObject *self)
{
    unsigned int i;

    for (i = 0; i < self->n_buffers; ++i) {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == v4l2_xioctl(self->fd, VIDIOC_QUERYBUF, &buf)) {
            PyErr_Format(PyExc_MemoryError, "%s: ioctl(VIDIOC_QUERYBUF) failure : %d, %s", self->device, errno, strerror(errno));
            return 0;
        }

        /*  is there a buffer on outgoing queue ready for us to take? */
        if (buf.flags & V4L2_BUF_FLAG_DONE)
            return 1;
    }

    /* no buffer ready to take */
    return 0;
}

int
v4l2_stop_capturing(v4l2camObject *self)
{
    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == v4l2_xioctl(self->fd, VIDIOC_STREAMOFF, &type)) {
        PyErr_Format(PyExc_SystemError, "%s: ioctl(VIDIOC_STREAMOFF) failure : %d, %s", self->device, errno, strerror(errno));
        return 0;
    }

    return 1;
}

int
v4l2_start_capturing(v4l2camObject *self)
{
    unsigned int i;
    enum v4l2_buf_type type;

    for (i = 0; i < self->n_buffers; ++i) {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == v4l2_xioctl(self->fd, VIDIOC_QBUF, &buf)) {
            PyErr_Format(PyExc_EnvironmentError, "%s: ioctl(VIDIOC_QBUF) failure : %d, %s", self->device, errno, strerror(errno));
            return 0;
        }
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == v4l2_xioctl(self->fd, VIDIOC_STREAMON, &type)) {
        PyErr_Format(PyExc_EnvironmentError, "%s: ioctl(VIDIOC_STREAMON) failure : %d, %s", self->device, errno, strerror(errno));
        return 0;
    }

    return 1;
}

int
v4l2_uninit_device(v4l2camObject *self)
{
    unsigned int i;

    for (i = 0; i < self->n_buffers; ++i) {
        if (-1 == munmap(self->buffers[i].start, self->buffers[i].length)) {
            PyErr_Format(PyExc_MemoryError, "%s: munmap failure: %d, %s", self->device, errno, strerror(errno));
            return 0;
        }
    }

    free(self->buffers);

    return 1;
}

int
v4l2_init_mmap(v4l2camObject *self)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);

    /* 2 is the minimum possible, and some drivers will force a higher count.
       It will likely result in buffer overruns, but for purposes of gaming,
       it is probably better to drop frames than get old frames. */
    req.count = 5;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (-1 == v4l2_xioctl(self->fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            PyErr_Format(PyExc_MemoryError, "%s does not support memory mapping",self->device);
            return 0;
        }
        else {
            PyErr_Format(PyExc_MemoryError, "%s: ioctl(VIDIOC_REQBUFS) failure : %d, %s", self->device, errno, strerror(errno));
            return 0;
        }
    }

    if (req.count < 2) {
        PyErr_Format(PyExc_MemoryError, "%s: Insufficient buffer memory\n", self->device);
        return 0;
    }

    self->buffers = calloc(req.count, sizeof(*self->buffers));

    if (!self->buffers) {
        PyErr_Format(PyExc_MemoryError, "Out of memory");
        return 0;
    }

    for (self->n_buffers = 0; self->n_buffers < req.count; ++self->n_buffers) {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = self->n_buffers;

        if (-1 == v4l2_xioctl(self->fd, VIDIOC_QUERYBUF, &buf)) {
            PyErr_Format(PyExc_MemoryError, "%s: ioctl(VIDIOC_QUERYBUF) failure : %d, %s", self->device, errno, strerror(errno));
            // free(self->buffers);
            return 0;
        }

        self->buffers[self->n_buffers].length = buf.length;
        self->buffers[self->n_buffers].start =
            mmap(NULL /* start anywhere */, buf.length,
                 PROT_READ | PROT_WRITE /* required */,
                 MAP_SHARED /* recommended */, self->fd, buf.m.offset);

        if (MAP_FAILED == self->buffers[self->n_buffers].start) {
            PyErr_Format(PyExc_MemoryError, "%s: mmap failure : %d, %s", self->device, errno, strerror(errno));
            return 0;
        }
    }

    return 1;
}


int v4l2_test_valid_device(int fd, char* device)
{
    struct v4l2_capability cap;
    if (-1 == v4l2_xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            PyErr_Format(PyExc_SystemError, "%s is not a V4L2 device", device);
            return 0;
        }
        else {
            PyErr_Format(PyExc_SystemError, "%s: ioctl(VIDIOC_QUERYCAP) failure : %d, %s", device, errno, strerror(errno));
            return 0;
        }
    }

    if (!(cap.device_caps & V4L2_CAP_VIDEO_CAPTURE)) {
        PyErr_Format(PyExc_SystemError, "%s is not a video capture device", device);
        return 0;
    }

    if (!(cap.device_caps & V4L2_CAP_STREAMING)) {
        PyErr_Format(PyExc_SystemError, "%s does not support streaming i/o", device);
        return 0;
    }
    return 1;
}

struct v4l2_fract float2fract(float fps) {
    float k=1, r;
    r = fmod(fps,1);
    if (r > 0) k = 1.0f/r;
    
    printf("%f %f %i %i\n", fps, r, (unsigned int) k, (unsigned int) (k*fps));
    struct v4l2_fract res = {(unsigned int) k, (unsigned int) (k*fps)};
    return res;
}

int v4l2_init_device(v4l2camObject *self)
{

    struct v4l2_format fmt;

    if (!v4l2_test_valid_device(self->fd, self->device)) return 0;

    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = self->width;
    fmt.fmt.pix.height = self->height;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (!v4l2_set_pixelformat(self, &fmt, self->fourcc)) {
        return 0;
    }

    /* Note VIDIOC_S_FMT may change width and height. */
    if (((unsigned int) self->width != fmt.fmt.pix.width) || ( (unsigned int) self->height != fmt.fmt.pix.height)) {
        PyErr_Format(PyExc_SystemError, "%s: Failed while setting size=(%d,%d). Got (%d,%d).", self->device, self->width, self->height, fmt.fmt.pix.width, fmt.fmt.pix.height);
        return 0;  
    }   

    struct v4l2_streamparm parm;
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    //v4l2_xioctl(self->fd, VIDIOC_G_PARM, &parm);
    parm.parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
    
     
    //parm.parm.capture.timeperframe.numerator = 1;
    //parm.parm.capture.timeperframe.denominator = self->fps;
    struct v4l2_fract targetfps = float2fract(self->fps);
    parm.parm.capture.timeperframe = targetfps;
    
    if (-1 == v4l2_xioctl(self->fd, VIDIOC_S_PARM, &parm)) {
        PyErr_Format(PyExc_SystemError, "%s: Failed while setting fps=%S", self->device, PyFloat_FromDouble((double) self->fps));
        return 0;
    }
    
    if ((parm.parm.capture.timeperframe.numerator != targetfps.numerator) || (parm.parm.capture.timeperframe.denominator != targetfps.denominator)) {
        float actualfps = 1.0*parm.parm.capture.timeperframe.denominator/parm.parm.capture.timeperframe.numerator;
        PyErr_Format(PyExc_SystemError, "%s: Failed while setting fps=%S. Got %S.", self->device, PyFloat_FromDouble((double) self->fps), PyFloat_FromDouble((double) actualfps));
        return 0;
    }

    if (!v4l2_init_mmap(self)) {
        return 0;
    }
       
    return 1;
}

int
v4l2_close_device(v4l2camObject *self)
{
    if (self->fd == -1)
        return 1;

    if (-1 == close(self->fd)) {
        PyErr_Format(PyExc_SystemError, "Cannot close '%s': %d, %s", self->device, errno, strerror(errno));
        return 0;
    }
    self->fd = -1;

    return 1;
}

int
v4l2_open_device(v4l2camObject *self)
{
    struct stat st;

    if (-1 == stat(self->device, &st)) {
        PyErr_Format(PyExc_SystemError, "Cannot stat '%s': %d, %s", self->device, errno, strerror(errno));
        goto return_err;
    }

    if (!S_ISCHR(st.st_mode)) {
        PyErr_Format(PyExc_SystemError, "%s is not a device", self->device);
        goto return_err;
    }

    self->fd = open(self->device, O_RDWR, 0);

    if (-1 == self->fd) {
        PyErr_Format(PyExc_SystemError, "Cannot open '%s': %d, %s", self->device, errno, strerror(errno));
        goto return_err;
    }
    return 1;
    
    return_err: //LABEL return_err
    return 0;

}
