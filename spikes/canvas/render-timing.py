# Summarise QSG_RENDER_TIMING output from one canvas.qml --bench run (spike 05).
# Usage: python3 render-timing.py <stderr log>
import re,sys
gui=[];rt=[];res=[]
for l in open(sys.argv[1],errors='replace'):
    m=re.search(r'Frame prepared, polish=(\d+) ms, lock=\d+ ms, blockedForSync=\d+ ms, animations=(\d+) ms',l)
    if m: gui.append(int(m[1])+int(m[2]))
    m=re.search(r'frame rendered in \d+ms, sync=(\d+), render=(\d+)',l)
    if m: rt.append(int(m[1])+int(m[2]))
    if 'RESULT' in l: res.append(l.split('RESULT ')[1].split(': ')[0][:4]+' '+l.split('→ ')[1].strip())
def st(a):
    a=sorted(a); return f"avg {sum(a)/len(a):.1f} p99 {a[int(len(a)*.99)-1]} max {a[-1]} ms" if a else "-"
print(f"gui(polish+anim): {st(gui)} | render(sync+render): {st(rt)} | {' ; '.join(res)}")
