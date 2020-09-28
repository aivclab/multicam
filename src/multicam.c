#include <Python.h>
#include <numpy/arrayobject.h>
#include <structmember.h>
#include <stdio.h>
#include <pthread.h>
#include <linux/videodev2.h>
#include "libyuv.h"
#include "multicam.h"
#include "v4l2.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define STR2FOURCC(s) FOURCC(toupper(s[0]),toupper(s[1]),toupper(s[2]),toupper(s[3]))

static int
v4l2cam_init(v4l2camObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *device = NULL, *tmp;
    static char *kwlist[] = {"device", "size", "format", "fps", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|(ii)sI", kwlist,
                                    &device, &(self->width), &(self->height), &(self->format), &(self->fps)))
        return -1;        
    tmp = self->device;
    Py_INCREF(device);
    self->device = device;
    Py_XDECREF(tmp);
    
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

PyMethodDef v4l2cam_methods[] = {
    {"start", (PyCFunction)v4l2cam_start, METH_NOARGS, ""},
    {"stop",  (PyCFunction)v4l2cam_stop,  METH_NOARGS, ""},
    {"read",  (PyCFunction)v4l2cam_read,  METH_NOARGS, ""},
    {NULL, NULL, 0, NULL}
};

//TODO: expose fd?
static PyMemberDef v4l2cam_members[] = {
    {"device", T_OBJECT_EX, offsetof(v4l2camObject, device), 0, "device path"},
    {"format", T_OBJECT_EX, offsetof(v4l2camObject, format), 0, "format specification"},
    {"width", T_INT, offsetof(v4l2camObject, width), 0, "image width"},
    {"height", T_INT, offsetof(v4l2camObject, height), 0, "image height"},
    {"fd", T_INT, offsetof(v4l2camObject, fd), 0, "fd"},
    {NULL}  /* Sentinel */
};

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
    {"camsys_read",  (PyCFunction)camsys_read, METH_VARARGS, NULL},
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
