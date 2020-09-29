from .backend import v4l2cam, camsys_read, is_valid_device, get_formats
from pathlib import Path
import numpy as np

__all__ = ["Multicam", "Camera", "list_cams"]

class Camera():
    '''
      Set up a camera.
      
      Parameters
      ----------
       dev : str, Path or int
         Video capture device path or integer, specifing /dev/video<N> device.
       size : tuple (width, height)
       format : str
         FOURCC string (e.g. "MJPG" or YUYV")
       fps : int
      
      Attributes
      ----------
       started : Bool; Is camera started?
      
      Methods
      -------
       start() : Start camera
       stop() : Stop camera
       read(n=None) :
         if `n` is not `None`; read `n` frames.
       get_formats() : Get available formats, resolutions and framerates
         
      Examples
      --------
      #Manual start/stop
      c = Camera("/dev/video0")
      c.start()
      data = c.read()
      c.stop()
      
      #Using a context manager:
      with Camera("/dev/video0", "/dev/video2") as c:
          data = c.read()
    '''
    def __init__(self, dev, size=(640,480), format="MJPG", fps=30):
        self.dev = dev
        self.size = size
        self.format = format
        self.fps = fps
        self._v4l2cam = None
    
    @property
    def width(self): return self.size[0]
    @property
    def height(self): return self.size[1]
        
    def _devpath(self):
        d = self.dev
        if isinstance(d, int): d = f"/dev/video{d}"
        d = Path(d)
        if not d.exists(): raise ValueError(f"No such device '{d}'.")
        return d
   
    def get_formats(self):
        return get_formats(self.dev)
   
    @property
    def started(self):
        return ((self._v4l2cam is not None) and (self._v4l2cam.fd != -1))
    
    def start(self):
        self.stop() #Restart if already started
        try:
            d = self._devpath()
            self._v4l2cam = v4l2cam(d, self.size, self.format, self.fps)
            self._v4l2cam.start()
        except Exception as e:
            self.stop()
            raise e
    
    def stop(self):
        if self.started: self._v4l2cam.stop()
    
    def read(self, n=None):
        if not self.started:
            raise RuntimeError("Camera has not been started")
        if n is not None:
            return np.stack([self._v4l2cam.read() for _ in range(n)])
        else:
            return self._v4l2cam.read()
    
    def __enter__(self):
        self.start()
        return self
        
    def __exit__(self, type, value, traceback): self.stop()
        
    def __del__(self): self.stop()

class Multicam():
    '''
      Set up a system of cameras for synchronized reading.
      Note: The cameras must all support the same settings.
      
      Parameters
      ----------
       devs : list
         Video capture device paths or integers, specifing /dev/video<N> devices.
       size : tuple (width, height)
       format : str
         FOURCC string (e.g. "MJPG" or YUYV")
       fps : int
      
      Attributes
      ----------
       started : Bool; Are cameras started?
      
      Methods
      -------
       start() : Start cameras
       stop() : Stop cameras
       read(n=None, ids=None) :
         if `n` is not `None`; read `n` frames.
         If `ids` is `None`; read from all cameras.
         Else, `ids` should be an iterable containing the camera indices to read from.
         
      Examples
      --------
      #Manual start/stop
      mc = Multicam(["/dev/video0", "/dev/video2"])
      mc.start()
      data = mc.read()
      mc.stop()
      
      #Using a context manager:
      with Multicam(["/dev/video0", "/dev/video2"]) as mc:
          data = mc.read()
    '''
    def __init__(self, devs, size=(640,480), format="MJPG", fps=30):
        self.devs = devs
        self.size = size
        self.format = format
        self.fps = fps
        self.cameras = []
    
    @property
    def width(self): return self.size[0]
    @property
    def height(self): return self.size[1]
    
    @property
    def started(self):
        return all([c.started for c in self.cameras])
       
    def start(self):
        try:
            for dev in self.devs:
                cam = Camera(dev, self.size, self.format, self.fps)
                cam.start()
                self.cameras.append(cam)
        except Exception as e:
            self.stop()
            raise e
               
    def stop(self):
        try:
            for cam in self.cameras: cam.stop()
        finally:
            self.cameras = []     
    
    def read(self, n=None, ids=None):
        if self.started:
            cams = ([self.cameras[i] for i in ids] if ids else self.cameras)
            if n is not None:
                return np.stack([camsys_read(self, cams) for _ in range(n)], axis=1)
            else:
                return camsys_read(self, cams)
        else:
            raise RuntimeError("One or more cameras not started.")
    
    def __enter__(self):
        self.start()
        return self
        
    def __exit__(self, type, value, traceback): self.stop()
        
    def __del__(self): self.stop()
    
def list_cams():
    return sorted([p for p in Path("/dev/").glob("video*") if mc.is_valid_device(p)])

