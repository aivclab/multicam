Synchronized reading of multiple webcams

`python setup.py install` for system-wide installation
`python setup.py install --user` for user-specific installation

```
import numpy as np
import multicam as mc
from streamserver import StreamServer
with StreamServer("localhost", fmt='RGB') as ss, \
     mc.Camsys(['/dev/video0','/dev/video2'], (640,480), 'YUYV', fps=30) as cs:
    try:
        while True: 
            res = cs.read()
            ss.set_frame(np.hstack(res[:,::4,::4]))
    except KeyboardInterrupt:
        pass
``` 
