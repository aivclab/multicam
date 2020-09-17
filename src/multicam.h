#ifndef MULTICAM_H
#define MULTICAM_H
struct buffer {
    void * start;
    size_t length;
};

typedef struct CameraObject {
    PyObject_HEAD
    PyObject *device;
    char* format;
    struct buffer* buffers;
    unsigned int n_buffers;
    int width;
    int height;
    unsigned int fps;
    int fd;
    int fourcc;
} CameraObject;

#endif //MULTICAM_H