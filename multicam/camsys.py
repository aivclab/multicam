from .backend import Camera, camsys_read

class Camsys():
    def __init__(self, devs, size=(640,480), format="MJPG", fps=30):
        self.devs = devs
        self.width, self.height = size
        self.format = format
        self.fps = fps
        self.cameras = []
        for dev in self.devs:
            self.cameras.append(Camera(dev, (self.width, self.height), self.fps, self.format))
    
    def __enter__(self):
        self.start()
        return self
        
    def start(self):
        try:
            for cam in self.cameras:
                cam.start()
        except Exception as e:
            self.stop()
            raise e
            
    def __exit__(self, type, value, traceback):
        self.stop()
    
    def stop(self):
        for cam in self.cameras:
            if cam.fd != -1: cam.stop()
    
    def read(self, ids=None):
        if ids:
            cams = [self.cameras[i] for i in ids]
        else:
            cams = [c for c in self.cameras if c.fd != -1]
        return camsys_read(self)
    
    def __del__(self):
        self.stop()
