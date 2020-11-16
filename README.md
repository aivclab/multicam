Synchronized reading from multiple webcams using v4l2 on Linux
--------------------------------------------------------------
Due to buffering, getting synchronized real-time video from a
single or multiple webcams is difficult, if not impossible.
This framework is intended fix just that.

When using multiple cameras, it is a requirement that they support the same configuration.

Installation
------------
`sudo apt install libjpeg-turbo8-dev libjpeg-dev cmake`
`python setup.py install` for system-wide installation
`python setup.py install --user` for user-specific installation

Use
---
Multiple cams:
```
import multicam as mc
with mc.Multicam(['/dev/video0','/dev/video2'], (640,480), 'YUYV', fps=30) as cs:
    try:
        while True: 
            res = cs.read() #RGB images
            print(res.shape)
    except KeyboardInterrupt:
        pass
```

Single cam:
```
import multicam as mc
with mc.Camera(0, (640,480), 'YUYV', fps=30) as c:
    print(c.read().shape)
``` 

Various utils:
```
import multicam as mc
print(mc.list_cams())
print(mc.is_valid_device("/dev/video0"))
print(mc.get_formats("/dev/video0"))
```
