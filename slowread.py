
from urllib.request import urlopen
import sys
import socket
import time

f = urlopen("http://localhost:4321")
while True:
    try:
        l = f.read(1)
    except socket.error:
        break
    sys.stdout.write(l.decode("utf8"))
    sys.stdout.flush()

    time.sleep(0.3)

f.close()
