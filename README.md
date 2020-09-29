Synchronized reading from multiple webcams using v4l2 on Linux
--------------------------------------------------------------

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
