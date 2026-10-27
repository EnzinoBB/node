# On Windows run.sh puts this folder on PYTHONPATH: the helpers' output is read by bash (mapfile,
# read), which would keep the carriage return of Python's default "\r\n" line ends.
import sys

for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(newline="\n")
    except (AttributeError, ValueError):
        pass
