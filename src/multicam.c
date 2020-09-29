#include <Python.h>
#include <numpy/arrayobject.h>
#include <structmember.h>
#include <stdio.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "libyuv.h"
#include "multicam.h"
#include "v4l2.h"
#include <fcntl.h>   

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define STR2FOURCC(s) FOURCC(toupper(s[0]),toupper(s[1]),toupper(s[2]),toupper(s[3]))

static int
v4l2cam_init(v4l2camObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *device = NULL;//, *tmp;
    static char *kwlist[] = {"device", "size", "format", "fps", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|(ii)sf", kwlist,
                                    &device, &(self->width), &(self->height), &(self->format), &(self->fps)))
        return -1;        
    PyObject *fspath = PyOS_FSPath(device);
    self->device = (char *) PyUnicode_AsUTF8(fspath);
    //Format
    if (self->format) {
        if (strlen(self->format) != 4) {
           PyErr_Format(PyExc_ValueError, "`%s` is not a valid FOURCC", self->format);
            return -1;
        }
        self->fourcc = STR2FOURCC(self->format);
    }
    else
        self->fourcc = 0;
    
    self->buffers = NULL;
    self->n_buffers = 0;
    self->fd = -1;
    return 0;
}

static void
v4l2cam_dealloc(v4l2camObject *self)
{
    Py_XDECREF(self->device);
    //Py_XDECREF(self->format);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

PyObject *
v4l2cam_start(v4l2camObject *self, PyObject *args)
{
    if (v4l2_open_device(self) == 0) {
        v4l2_close_device(self);
        return NULL;
    }
    else {
        int initres = v4l2_init_device(self);
        if (initres == 0) {
            v4l2_close_device(self);
            return NULL;
        }
        if (v4l2_start_capturing(self) == 0) {
            v4l2_close_device(self);
            return NULL;
        }
    }
    Py_RETURN_NONE;
}

PyObject *
v4l2cam_stop(v4l2camObject *self, PyObject *args)
{
    if (v4l2_stop_capturing(self) == 0)
        return NULL;
    if (v4l2_uninit_device(self) == 0)
        return NULL;
    if (v4l2_close_device(self) == 0)
        return NULL;
    Py_RETURN_NONE;
}

typedef struct CamReadWorkerArgStruct {
    v4l2camObject *cam;
    uint8_t *dst;
    int res;
} CamReadWorkerArgStruct;

void *
cam_read_worker(void *argp)
{
    CamReadWorkerArgStruct *args = argp;
    v4l2camObject *cam = args->cam;
    uint8_t *dst = args->dst;
    int libyuv_res;
    
    uint8_t argb[cam->height * cam->width * 4];
    
    //Prepare buffer
    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    //Dequeue buffer
    if (-1 == v4l2_xioctl(cam->fd, VIDIOC_DQBUF, &buf)) {
        fprintf(stderr, "ioctl(VIDIOC_DQBUF) failure : %d, %s", errno, strerror(errno));
        args->res = 1;
        return NULL;
    }
    
    //Convert to ARGB
    libyuv_res = ConvertToARGB(
                   (uint8_t *) cam->buffers[buf.index].start, //sample
                   cam->buffers[buf.index].length, //sample_size
                   argb, cam->width*4, //dst, dst_stride
                   0, 0, //crop_x, crop_y
                   cam->width, cam->height,
                   cam->width, cam->height,
                   kRotate0, //RotationMode
                   cam->fourcc); //FOURCC
    
    if (libyuv_res != 0) {
        fprintf(stderr, "libyuv ConvertToARGB failed: %i\n", libyuv_res);
        args->res = 2;
        return NULL;
    }
    //Re-queue buffer
    if (-1 == v4l2_xioctl(cam->fd, VIDIOC_QBUF, &buf)) {
        fprintf(stderr, "v4l2 ioctl(VIDIOC_QBOF) failed:  %d, %s", errno, strerror(errno));
        args->res = 3;
        return NULL;
    }
    //Convert to RGB, put in dst
    libyuv_res = ARGBToRAW(argb, cam->width*4, dst, cam->width*3, cam->width, cam->height);
    if (libyuv_res != 0) {
        fprintf(stderr, "libyuv ARGBtoRAW failed: %i\n", libyuv_res);
        args->res = 4;
        return NULL;
    }
    
    args->res = 0;
    return NULL;
}

PyObject *
v4l2cam_read(v4l2camObject *self)
{
    pthread_t thread;
    CamReadWorkerArgStruct cam_args;
    PyObject *res;
    uint8_t *dst = PyDataMem_NEW(self->width * self->height * 3);
    
    //Prepare thread args
    cam_args = (CamReadWorkerArgStruct){self, dst, 0};
    //Run thread
    pthread_create(&thread, NULL, cam_read_worker, (void *)(&cam_args));
    pthread_join(thread, NULL);
    //Check for errors
    if (cam_args.res) {
        PyErr_Format(PyExc_RuntimeError, "Reading image failed: %i\n", cam_args.res);
        return NULL;
    }
    //To Numpy array
    npy_intp dims[3] = {self->height, self->width, 3};
    res = PyArray_New(&PyArray_Type, 3, dims, NPY_UINT8, NULL, dst, 1, NPY_ARRAY_OWNDATA, NULL);
    if (!res) {
        PyErr_SetString(PyExc_RuntimeError, "PyArray_NEW failed\n");
        return NULL;
    }
    
    return res;
}

static PyObject *
camsys_read(PyObject *self, PyObject *args)
{
    pthread_t *threads = NULL;
    CamReadWorkerArgStruct *cam_args = NULL;
    PyObject *res = NULL;
    PyObject *camsys, *cams, *pywidth=NULL, *pyheight=NULL, *camobj, *cam=NULL;
    if (!PyArg_ParseTuple(args, "OO", &camsys, &cams)) return NULL;
        
//    cams = PyObject_GetAttrString(camsys, "cameras"); //INCREF!
    if (!cams) return NULL;

    int N = (int) PySequence_Length(cams);
    if (N <= 0) {
        PyErr_SetString(PyExc_ValueError, "camsys contains no cameras.");
        goto RETURN;
    }

    pywidth = PyObject_GetAttrString(camsys, "width"); //INCREF!
    if (!pywidth) goto RETURN;
    pyheight = PyObject_GetAttrString(camsys, "height"); //INCREF!
    if (!pyheight) goto RETURN;
    int width = (int) PyLong_AsLong(pywidth);
    int height = (int) PyLong_AsLong(pyheight);
    int cam_dst_sz = width * height * 3;


    threads = (pthread_t *) malloc(N*sizeof(pthread_t));
    cam_args = (CamReadWorkerArgStruct *) malloc(N*sizeof(CamReadWorkerArgStruct));

    npy_intp dims[4] = {N,height, width, 3};
    PyObject *arr = PyArray_SimpleNew(4, dims, NPY_UINT8); //INCREF!
    if (!arr)
        goto RETURN;
    uint8_t *dst = (uint8_t *) PyArray_DATA((PyArrayObject *) arr);

    for (int i=0; i<N; i++) { //Prepare thread args
        camobj = PySequence_GetItem(cams, i);
        if (!camobj) goto RETURN;
        cam = PyObject_GetAttrString(camobj, "_v4l2cam");
        cam_args[i] = (CamReadWorkerArgStruct){(v4l2camObject *) cam, &dst[i * cam_dst_sz], 0};
        Py_XDECREF(cam);
    }
    for (int i=0; i<N; i++) //Run threads
        pthread_create(&(threads[i]), NULL, cam_read_worker, (void *)(&cam_args[i]));
    for (int i=0; i<N; i++) 
        pthread_join(threads[i], NULL);
    for (int i=0; i<N; i++) { //Check for errors
        if (cam_args[i].res) {
            PyErr_Format(PyExc_RuntimeError, "Reading image from camera %i failed: %i\n", i, cam_args[i].res);
            goto RETURN;
        }
    }

    res = arr;
    RETURN:
    free(threads);
    free(cam_args);
    Py_XDECREF(pywidth);
    Py_XDECREF(pyheight);
    return res;
}

static PyObject *
is_valid_device(PyObject *module, PyObject *device)
{
    PyObject *fspath = PyOS_FSPath(device);
    char *devicestr = (char *) PyUnicode_AsUTF8(fspath);
    int fd = open(devicestr, O_RDONLY, 0);
    
    int res = v4l2_test_valid_device(fd, devicestr);
    close(fd);
    if (res == 0) {
        PyErr_Clear();
        Py_RETURN_FALSE;
    }
    else
        Py_RETURN_TRUE;
        
}

PyMethodDef v4l2cam_methods[] = {
    {"start",    (PyCFunction)v4l2cam_start,    METH_NOARGS, ""},
    {"stop",     (PyCFunction)v4l2cam_stop,     METH_NOARGS, ""},
    {"read",     (PyCFunction)v4l2cam_read,     METH_NOARGS, ""},
    {NULL, NULL, 0, NULL}
};

static PyMemberDef v4l2cam_members[] = {
    {"device", T_OBJECT_EX, offsetof(v4l2camObject, device), 0, "device path"},
    {"format", T_OBJECT_EX, offsetof(v4l2camObject, format), 0, "format specification"},
    {"width", T_INT, offsetof(v4l2camObject, width), 0, "image width"},
    {"height", T_INT, offsetof(v4l2camObject, height), 0, "image height"},
    {"fd", T_INT, offsetof(v4l2camObject, fd), 0, "fd"},
    {NULL}  /* Sentinel */
};





static PyObject *
get_framerates(int fd, __u32 pixel_format, __u32 width, __u32 height) {
    PyObject *fpslist = NULL;
    struct v4l2_frmivalenum fps;
    memset(&fps,0,sizeof(fps));
    fps.index = 0;
    fps.pixel_format = pixel_format;
    fps.width = width;
    fps.height = height;
    float fmin, fmax, fstep;
    
    fpslist = PyList_New(0);
    
    while (0 == v4l2_xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fps)) {
        if (fps.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            PyList_Append(fpslist, Py_BuildValue("f", 1.0*fps.discrete.denominator/fps.discrete.numerator));
            fps.index++;
        }
        else {
            fmin = fps.stepwise.min.denominator/fps.stepwise.min.numerator;
            fmax = fps.stepwise.max.denominator/fps.stepwise.max.numerator;
            fstep = fps.stepwise.step.denominator/fps.stepwise.step.numerator;
            for (unsigned int f=fmin; f<=fmax; f+=fstep)
                PyList_Append(fpslist, Py_BuildValue("i", f));
            break;
        }
    }
    return fpslist;
}


static PyObject *
get_framesizes(int fd, struct v4l2_fmtdesc *fmt) {
    PyObject *fszdict = NULL;
    struct v4l2_frmsizeenum fsz;
    memset(&fsz,0,sizeof(fsz));
    fsz.index = 0;
    fsz.pixel_format = fmt->pixelformat;
    fszdict = PyDict_New();
    PyObject *key = NULL;
    __u32 h,w;
    
    while (0 == v4l2_xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fsz)) {
        if (fsz.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            w = fsz.discrete.width;
            h = fsz.discrete.height;
            key = Py_BuildValue("ii", w, h);
            PyDict_SetItem(fszdict, key, get_framerates(fd, fsz.pixel_format, w,h));
            fsz.index++;
        }
        else {
            for (w=fsz.stepwise.min_width; w<=fsz.stepwise.max_width; w+=fsz.stepwise.step_width)
                for (h=fsz.stepwise.min_height; h<=fsz.stepwise.max_height; h+=fsz.stepwise.step_height) {
                    key = Py_BuildValue("ii", w, h);
                    PyDict_SetItem(fszdict, key, get_framerates(fd, fsz.pixel_format, w,h));
                 }
            break;
        }
    }
    return fszdict;
}

static PyObject *
get_formats(PyObject *module, PyObject *device)
{
    struct v4l2_fmtdesc fmt;

    PyObject *fmtdict = NULL, *details = NULL;
    char fourcc[] = "xxxx";

    
    PyObject *fspath = PyOS_FSPath(device);
    char *devicestr = (char *) PyUnicode_AsUTF8(fspath);
    int fd = open(devicestr, O_RDONLY, 0);
    int valid = v4l2_test_valid_device(fd, devicestr);
    if (!valid) goto return_err;

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;   
    fmt.index = 0;
    fmtdict = PyDict_New();
    //Loop formats
    while (0 == v4l2_xioctl(fd, VIDIOC_ENUM_FMT, &fmt)) {
        fmt.index++;
        memcpy(&fourcc, &fmt.pixelformat, 4);
        details = PyDict_New();
        
        PyDict_SetItemString(details, "description", PyUnicode_FromString((char *) fmt.description));
        PyDict_SetItemString(details, "compressed", PyBool_FromLong((long) (fmt.flags & V4L2_FMT_FLAG_COMPRESSED)));
        PyDict_SetItemString(details, "emulated", PyBool_FromLong((long) (fmt.flags & V4L2_FMT_FLAG_EMULATED)));
        
        PyDict_SetItemString(details, "framesizes", get_framesizes(fd, &fmt));
        PyDict_SetItemString(fmtdict, fourcc, details);
    }
    if (!PyErr_Occurred())
        return fmtdict;
    
    
    return_err:
    close(fd);
    Py_XDECREF(fmtdict);
    return NULL;    
}


static PyTypeObject v4l2camType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "multicam.v4l2cam",
    .tp_basicsize = sizeof(v4l2camObject),
    .tp_itemsize = 0,
    .tp_dealloc = (destructor) v4l2cam_dealloc,
    .tp_methods = v4l2cam_methods,
    .tp_members = v4l2cam_members,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_init = (initproc) v4l2cam_init,
    .tp_new = PyType_GenericNew,
};

static PyMethodDef v4l2camMethods[] = {
    {"camsys_read",     (PyCFunction)camsys_read,     METH_VARARGS, NULL},
    {"is_valid_device", (PyCFunction)is_valid_device, METH_O,       NULL},
    {"get_formats",     (PyCFunction)get_formats,     METH_O,       NULL},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

static PyModuleDef multicammodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "multicam",
    .m_doc = "v4l2 camera interface",
    .m_size = -1,
    .m_methods = v4l2camMethods
};

PyMODINIT_FUNC
PyInit_backend(void)
{
    Py_Initialize();
    import_array();
    PyObject *m;
    if (PyType_Ready(&v4l2camType) < 0)
        return NULL;

    m = PyModule_Create(&multicammodule);
    if (m == NULL)
        return NULL;

    Py_INCREF(&v4l2camType);
    if (PyModule_AddObject(m, "v4l2cam", (PyObject *) &v4l2camType) < 0) {
        Py_DECREF(&v4l2camType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
