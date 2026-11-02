#!/usr/bin/env python3
"""The report's diagrams, in the diagram-design system with the report's palette."""
import subprocess, html, pathlib
PAPER,INK,MUTED,SOFT,ACCENT,TINT,RULE="#ffffff","#1f3a70","#4f5f80","#6c86b8","#b5651d","#f7ecdf","#dce2f0"
HERE=pathlib.Path(__file__).parent
PAGE="""<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0"><title>{title}</title>
<link href="https://fonts.googleapis.com/css2?family=Geist:wght@400;500;600&family=Geist+Mono:wght@400;500;600&display=swap" rel="stylesheet">
<style>*{{box-sizing:border-box;margin:0;padding:0}}body{{background:{paper}}}.diagram-container{{width:100%;overflow-x:auto}}svg{{width:100%;min-width:{w}px;display:block}}@media print{{.diagram-container{{overflow-x:visible}}svg{{min-width:0}}}}</style></head>
<body><div class="diagram-container"><svg viewBox="0 0 {w} {h}" xmlns="http://www.w3.org/2000/svg" role="img" aria-labelledby="{slug}-title {slug}-desc"><title id="{slug}-title">{title}</title><desc id="{slug}-desc">{desc}</desc><defs>
<marker id="arrow" markerWidth="8" markerHeight="6" refX="7" refY="3" orient="auto"><polygon points="0 0, 8 3, 0 6" fill="{muted}"/></marker>
<marker id="arrow-start" markerWidth="8" markerHeight="6" refX="1" refY="3" orient="auto"><polygon points="8 0, 0 3, 8 6" fill="{muted}"/></marker>
<marker id="arrow-accent" markerWidth="8" markerHeight="6" refX="7" refY="3" orient="auto"><polygon points="0 0, 8 3, 0 6" fill="{accent}"/></marker>
<marker id="arrow-accent-start" markerWidth="8" markerHeight="6" refX="1" refY="3" orient="auto"><polygon points="8 0, 0 3, 8 6" fill="{accent}"/></marker>
</defs><rect width="100%" height="100%" fill="{paper}"/>{body}</svg></div></body></html>"""
def path(pts,r=8):
    d=f"M{pts[0][0]} {pts[0][1]}"
    for i in range(1,len(pts)-1):
        (x0,y0),(x1,y1),(x2,y2)=pts[i-1],pts[i],pts[i+1]
        dx0,dy0=(x1>x0)-(x1<x0),(y1>y0)-(y1<y0); dx1,dy1=(x2>x1)-(x2<x1),(y2>y1)-(y2<y1)
        d+=f" L{x1-dx0*r} {y1-dy0*r} Q{x1} {y1} {x1+dx1*r} {y1+dy1*r}"
    return d+f" L{pts[-1][0]} {pts[-1][1]}"
def arrow(pts,accent=False,dash=False,both=False):
    c=ACCENT if accent else MUTED; m="arrow-accent" if accent else "arrow"
    s=f'<path d="{path(pts)}" fill="none" stroke="{c}" stroke-width="{1.8 if accent else 1.4}"'
    if dash: s+=' stroke-dasharray="5,4"'
    if both: s+=f' marker-start="url(#{m}-start)"'
    return s+f' marker-end="url(#{m})"/>'
def label(x,y,t,anchor="middle"):
    w=len(t)*6.2+8; x0=x-w/2 if anchor=="middle" else x-4 if anchor=="start" else x-w+4
    return f'<rect x="{x0}" y="{y-10}" width="{w}" height="14" fill="{PAPER}"/><text x="{x}" y="{y}" text-anchor="{anchor}" font-family="Geist Mono,monospace" font-size="10" letter-spacing="0.04em" fill="{MUTED}">{html.escape(t)}</text>'
def node(x,y,w,h,name,sub="",focal=False,store=False):
    f,s=(TINT,ACCENT) if focal else ("#eef1f8",INK) if store else (PAPER,INK)
    o=f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="6" fill="{f}" stroke="{s}" stroke-width="1.2"/>'
    ty=y+h/2+(-2 if sub else 5)
    o+=f'<text x="{x+w/2}" y="{ty}" text-anchor="middle" font-family="Geist,sans-serif" font-size="14" font-weight="600" fill="{INK}">{html.escape(name)}</text>'
    if sub: o+=f'<text x="{x+w/2}" y="{ty+16}" text-anchor="middle" font-family="Geist Mono,monospace" font-size="10" fill="{MUTED}">{html.escape(sub)}</text>'
    return o
def zone(x,y,w,h,t):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="#f6f7fb" stroke="{RULE}"/><text x="{x+16}" y="{y+22}" font-family="Geist Mono,monospace" font-size="10" letter-spacing="0.14em" fill="{MUTED}">{t}</text>'
def diamond(cx,cy,w,h,t):
    return f'<polygon points="{cx},{cy-h/2} {cx+w/2},{cy} {cx},{cy+h/2} {cx-w/2},{cy}" fill="#eef1f8" stroke="{INK}" stroke-width="1.2"/><text x="{cx}" y="{cy+5}" text-anchor="middle" font-family="Geist,sans-serif" font-size="13" font-weight="600" fill="{INK}">{t}</text>'
def legend(y,items,w):
    o=f'<line x1="40" y1="{y}" x2="{w-40}" y2="{y}" stroke="{RULE}"/>'; x=40
    for dash,accent,t in items:
        c=ACCENT if accent else MUTED
        o+=f'<line x1="{x}" y1="{y+20}" x2="{x+28}" y2="{y+20}" stroke="{c}" stroke-width="1.6"'+(' stroke-dasharray="5,4"' if dash else '')+f'/><text x="{x+36}" y="{y+24}" font-family="Geist Mono,monospace" font-size="10" fill="{MUTED}">{t}</text>'
        x+=56+len(t)*6.4
    return o
def save(slug,title,desc,w,h,body):
    p=HERE/f"{slug}.html"
    p.write_text(PAGE.format(title=title,desc=desc,slug=slug,w=w,h=h,body=body,paper=PAPER,muted=MUTED,accent=ACCENT))
    subprocess.run(["chromium-browser","--headless","--disable-gpu","--no-sandbox",f"--screenshot={HERE}/{slug}.png",f"--window-size={w},{h}","--force-device-scale-factor=2","--hide-scrollbars",f"file://{p}"],capture_output=True,timeout=60)

# ---- architecture
b=zone(40,40,400,352,"LAPTOP A")+zone(560,40,400,352,"LAPTOP B")
b+=arrow([(408,108),(592,108)],accent=True,both=True)+arrow([(408,228),(592,228)],both=True)+arrow([(408,348),(592,348)])
b+=arrow([(256,348),(208,348)])+arrow([(140,320),(140,256)])+arrow([(240,200),(240,136)],dash=True)+arrow([(760,200),(760,136)],dash=True)
b+=label(500,98,"PAGES, TASKS")+label(500,218,"CONTROL")+label(500,338,"PROGRAM COPY")+label(148,292,"ASKS","start")+label(248,172,"STARTS","start")+label(768,172,"STARTS","start")
b+=node(72,80,336,56,"Launcher","main(), task queue, locks",focal=True)+node(72,200,336,56,"pmd","peers, admission, ledger")+node(72,320,136,56,"pmrun","run N nodes")+node(256,320,152,56,"Website","playground")
b+=node(592,80,336,56,"Worker","same program, tasks",focal=True)+node(592,200,336,56,"pmd","peers, slots, ledger")+node(592,320,152,56,"Website","keeps the copy")
b+=legend(412,[(0,1,"TCP mesh of the job"),(0,0,"frames or calls"),(1,0,"starts a process")],1000)
save("architecture","ParaMesh on two laptops","Each laptop runs pmd and a website, and a job is one launcher and one worker joined by a TCP mesh.",1000,452,b)

# ---- job start
X=[100,300,500,700,900]; N=["pmrun","pmd A","Launcher","Worker","pmd B"]; b=""
for x in X: b+=f'<line x1="{x}" y1="76" x2="{x}" y2="520" stroke="{SOFT}" stroke-width="1" stroke-dasharray="4,4"/>'
M=[(0,1,"L_RUN_REQ",0,0),(1,0,"L_RUN_OK",1,0),(0,2,"START PROCESS",2,0),(2,1,"L_ADMIT_REQ",0,0),(1,4,"SPAWN_REQ, POOL TAG",0,1),(4,3,"START PROCESS",2,0),(3,4,"L_REGISTER",0,0),(4,1,"SPAWN_OK",1,0),(1,2,"L_ADMIT_OK",1,0),(2,3,"JOIN_JOB",0,0),(2,3,"SEG_MAP EPOCH 1",0,0)]
y=112
for a,c,t,k,acc in M:
    b+=arrow([(X[a],y),(X[c],y)],accent=bool(acc),dash=k>0); b+=label((X[a]+X[c])/2,y-8,t); y+=38
for x,n in zip(X,N): b+=node(x-72,24,144,48,n)
b+=legend(532,[(0,1,"signed start request"),(0,0,"call"),(1,0,"reply or start a process")],1000)
save("job-start","Starting a job","The launcher starts first and asks its pmd for admission; workers start only after the capacity check and a signed request.",1000,572,b)

# ---- page states
b=arrow([(190,242),(190,150),(280,150)])+arrow([(400,150),(500,150),(500,242)])+arrow([(540,242),(540,150),(640,150)])+arrow([(760,150),(850,150),(850,242)])
b+=arrow([(130,242),(130,60),(460,60)])+arrow([(580,60),(910,60),(910,242)],accent=True)+arrow([(700,122),(700,105),(540,105),(540,88)])
b+=arrow([(460,270),(220,270)])+arrow([(820,270),(580,270)])+arrow([(880,298),(880,346),(160,346),(160,298)])
b+=label(235,140,"READ FAULT")+label(450,140,"DATA")+label(590,140,"WRITE FAULT")+label(800,140,"GRANT")+label(300,50,"WRITE FAULT")+label(740,50,"GRANT + DATA",)+label(620,98,"INVALIDATED")+label(340,262,"INVALIDATE/EVICT")+label(700,262,"FETCH, KEEP READ")+label(520,338,"FETCH AND GIVE UP, OR EVICT")
b+=node(100,242,120,56,"I","not mapped")+node(460,242,120,56,"S","read-only")+node(820,242,120,56,"M","writable",focal=True)
b+=node(280,122,120,56,"reading","in flight",store=True)+node(640,122,120,56,"upgrading","in flight",store=True)+node(460,32,120,56,"writing","in flight",store=True)
b+=legend(368,[(0,1,"a write grant that brings the page")],1000)
save("page-states","States of a page on a node","A page moves between invalid, shared and modified through in-flight states while a request waits for its reply.",1000,404,b)

# ---- translator
b=arrow([(220,108),(260,108)])+arrow([(420,108),(460,108)])+arrow([(640,108),(680,108)])+arrow([(840,108),(868,108)],accent=True)
b+=arrow([(340,148),(340,212)])+arrow([(760,148),(760,212)])+arrow([(240,240),(130,240),(130,136)])
b+=label(440,98,"YES")+label(348,184,"NO","start")+label(854,98,"YES")+label(768,184,"NO","start")+label(160,232,"INNER LOOPS","start")
b+=node(40,80,180,56,"Each for loop","in main()")+diamond(340,108,160,80,"Shown safe?")+node(460,80,180,56,"PM_TASK","arrays to pm_malloc")+diamond(760,108,160,80,"Compiles?")+node(868,80,112,56,"Pool","runs here",focal=True)
b+=node(240,212,200,56,"Stays on this laptop","try what is inside")+node(660,212,200,56,"Original program","one laptop")
save("translator","How plain C is split","Each loop in main is split only if it can be shown safe, and the original program runs if the result does not compile.",1020,300,b)
