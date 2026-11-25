"""List the actions of a RenderDoc capture, with their event ids and names.

    RDC=<capture.rdc> OUT=<file> QT_QPA_PLATFORM=offscreen \
        qrenderdoc --python runtime/tools/rdc_actions.py

qrenderdoc's scripting shell does not show a script's standard output, so the
list goes to the file OUT. A labeled draw of the Switch 2 graphics layer reads
`draw set <n> cmd <i> (<NVN call>) vs <key> fs <key>`; see
docs/guides/debug-renderdoc.md.
"""
import os
import sys

import renderdoc as rd

sys.stdout = open(os.environ["OUT"], "w")

path = os.environ["RDC"]
cap = rd.OpenCaptureFile()
res = cap.OpenFile(path, "", None)
if res != rd.ResultCode.Succeeded:
    print("open failed", res)
    os._exit(1)
res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if res != rd.ResultCode.Succeeded:
    print("replay failed", res)
    os._exit(1)
sd = ctrl.GetStructuredFile()


# docgen:region walk
def walk(a, depth):
    print("  " * depth + "%d %s" % (a.eventId, a.GetName(sd)))
    for c in a.children:
        walk(c, depth + 1)


for a in ctrl.GetRootActions():
    walk(a, 0)
# docgen:end
ctrl.Shutdown()
cap.Shutdown()
sys.stdout.flush()
os._exit(0)
