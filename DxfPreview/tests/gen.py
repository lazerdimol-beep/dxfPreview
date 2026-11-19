# Generates test DXF files without external libraries.
import math, random
def w(pairs): return "".join(f"{c}\n{v}\n" for c, v in pairs)

def header(ver="AC1015", cp="ANSI_1251"):
    return [(0,"SECTION"),(2,"HEADER"),(9,"$ACADVER"),(1,ver),(9,"$DWGCODEPAGE"),(3,cp),(0,"ENDSEC")]

def tables(layers):
    t=[(0,"SECTION"),(2,"TABLES"),(0,"TABLE"),(2,"LAYER"),(70,len(layers))]
    for name,color,flags in layers:
        t+=[(0,"LAYER"),(2,name),(70,flags),(62,color),(6,"CONTINUOUS")]
    return t+[(0,"ENDTAB"),(0,"ENDSEC")]

def block(name, base, ents):
    b=[(0,"BLOCK"),(8,"0"),(2,name),(70,0),(10,base[0]),(20,base[1]),(30,0),(3,name)]
    for e in ents: b+=e
    return b+[(0,"ENDBLK"),(8,"0")]

def line(a,b,layer="0",color=None):
    e=[(0,"LINE"),(8,layer)]
    if color is not None: e.append((62,color))
    return e+[(10,a[0]),(20,a[1]),(30,0),(11,b[0]),(21,b[1]),(31,0)]
def circle(c,r,layer="0",extr=None,color=None):
    e=[(0,"CIRCLE"),(8,layer)]+([(62,color)] if color is not None else [])+[(10,c[0]),(20,c[1]),(30,0),(40,r)]
    if extr: e+=[(210,extr[0]),(220,extr[1]),(230,extr[2])]
    return e
def arc(c,r,a0,a1,layer="0",extr=None):
    e=[(0,"ARC"),(8,layer),(10,c[0]),(20,c[1]),(30,0),(40,r),(50,a0),(51,a1)]
    if extr: e+=[(210,extr[0]),(220,extr[1]),(230,extr[2])]
    return e
def lwpoly(pts,closed=False,layer="0",color=None):
    e=[(0,"LWPOLYLINE"),(8,layer)]+([(62,color)] if color is not None else [])+[(90,len(pts)),(70,1 if closed else 0)]
    for p in pts:
        e+=[(10,p[0]),(20,p[1])]
        if len(p)>2: e.append((42,p[2]))
    return e
def text(s,p,h,rot=0,hj=0,vj=0,p2=None,layer="0",color=None):
    e=[(0,"TEXT"),(8,layer)]+([(62,color)] if color is not None else [])+[(10,p[0]),(20,p[1]),(30,0),(40,h),(1,s),(50,rot),(72,hj),(73,vj)]
    if p2: e+=[(11,p2[0]),(21,p2[1]),(31,0)]
    return e
def mtext(s,p,h,attach=1,rot=0,layer="0"):
    return [(0,"MTEXT"),(8,layer),(10,p[0]),(20,p[1]),(30,0),(40,h),(41,100),(71,attach),(50,rot),(1,s)]
def insert(name,p,sx=1,sy=1,rot=0,layer="0",color=None,cols=1,rows=1,csp=0,rsp=0):
    e=[(0,"INSERT"),(8,layer)]+([(62,color)] if color is not None else [])+[(2,name),(10,p[0]),(20,p[1]),(30,0),(41,sx),(42,sy),(43,1),(50,rot)]
    if cols>1 or rows>1: e+=[(70,cols),(71,rows),(44,csp),(45,rsp)]
    return e

def section(name, ents):
    s=[(0,"SECTION"),(2,name)]
    for e in ents: s+=e
    return s+[(0,"ENDSEC")]

def test1():
    ents=[]
    # frame
    ents.append(lwpoly([(0,0),(420,0),(420,297),(0,297)],True,"FRAME"))
    # lines & circles colored
    for i in range(1,8):
        ents.append(line((20,20+i*10),(120,20+i*10),color=i))
    ents.append(circle((170,60),30,"GEOM"))
    ents.append(arc((170,60),40,0,135,"GEOM"))
    # mirrored-extrusion arc: OCS x is flipped. Center (-250,60) in OCS -> (250,60) WCS, angles 0..90 in OCS -> 180..90 in WCS (upper-left quarter)
    ents.append(arc((-250,60),30,0,90,"GEOM",extr=(0,0,-1)))
    ents.append(circle((-250,60),5,"GEOM",extr=(0,0,-1)))
    # bulge polyline: rounded slot
    ents.append(lwpoly([(300,30,0),(360,30,1),(360,70,0),(300,70,1)],True,"GEOM",color=3))
    # 2D POLYLINE with VERTEX
    pl=[(0,"POLYLINE"),(8,"GEOM"),(66,1),(10,0),(20,0),(30,0),(70,0),(62,5)]
    for (x,y,b) in [(20,120,0),(60,120,-0.5),(100,120,0),(100,160,0)]:
        pl+= [(0,"VERTEX"),(8,"GEOM"),(10,x),(20,y),(30,0),(42,b)]
    pl+=[(0,"SEQEND"),(8,"GEOM")]
    ents.append(pl)
    # ellipse
    ents.append([(0,"ELLIPSE"),(8,"GEOM"),(62,6),(10,170),(20,140),(30,0),(11,40),(21,10),(31,0),(40,0.4),(41,0),(42,2*math.pi)])
    ents.append([(0,"ELLIPSE"),(8,"GEOM"),(62,4),(10,170),(20,140),(30,0),(11,20),(21,0),(31,0),(40,1.5),(41,0),(42,math.pi)])
    # spline (clamped cubic)
    cps=[(220,120),(240,170),(270,110),(300,170),(330,120)]
    k=[0,0,0,0,1,2,2,2,2]
    sp=[(0,"SPLINE"),(8,"GEOM"),(62,2),(70,8),(71,3),(72,len(k)),(73,len(cps)),(74,0)]
    for v in k: sp.append((40,v))
    for x,y in cps: sp+=[(10,x),(20,y),(30,0)]
    ents.append(sp)
    # texts: cp1251 cyrillic
    ents.append(text("Креслення ЇЖак Ґанок є і ї", (20,260), 7, layer="TEXT"))
    ents.append(text("Center/Middle %%c20 %%d %%p0.1", (210,240), 5, hj=1, vj=2, p2=(210,240), layer="TEXT", color=1))
    ents.append(text("Right-top 30°".replace("°","%%d"), (400,230), 5, rot=30, hj=2, vj=3, p2=(400,230), layer="TEXT", color=2))
    ents.append(mtext("{\\fArial|b1;Багаторядковий}\\PДругий \\H2x;рядок\\Pdia \\U+2205 frac \\S1/2;", (300,290), 4, attach=1, layer="TEXT"))
    # inserts: block with entity on layer 0 BYBLOCK color + text
    ents.append(insert("BOLT",(40,200),1,1,0,"BLOCKS",color=1))
    ents.append(insert("BOLT",(80,200),1.5,1.5,45,"BLOCKS",color=3))
    ents.append(insert("BOLT",(130,190),1,1,0,"BLOCKS",color=4,cols=3,rows=2,csp=15,rsp=15))
    ents.append(insert("BOLT",(200,200),-1,1,0,"BLOCKS",color=6))  # mirrored
    ents.append(insert("NEST",(250,190),1,1,0,"BLOCKS"))
    # hidden layer entity must not appear
    ents.append(line((0,0),(420,297),"HIDDEN",color=1))
    ents.append(line((0,297),(420,0),"FROZEN",color=1))
    # paper space entity must not appear
    ents.append([(0,"LINE"),(67,1),(8,"0"),(10,-1000),(20,-1000),(30,0),(11,-900),(21,-900),(31,0)])
    # solid hatch with hole (polyline path + circle edge path)
    h=[(0,"HATCH"),(8,"HATCH"),(62,1),(10,0),(20,0),(30,0),(210,0),(220,0),(230,1),(2,"SOLID"),(70,1),(71,0),(91,2),
       (92,7),(72,0),(73,1),(93,4),(10,320),(20,200),(10,400),(20,200),(10,400),(20,260),(10,320),(20,260),(97,0),
       (92,16),(93,1),(72,2),(10,360),(20,230),(40,15),(50,0),(51,360),(73,1),(97,0),
       (75,0),(76,1),(98,0)]
    ents.append(h)
    # pattern hatch ANSI31 in edge-defined boundary (lines + arc)
    h2=[(0,"HATCH"),(8,"HATCH"),(62,4),(10,0),(20,0),(30,0),(210,0),(220,0),(230,1),(2,"ANSI31"),(70,0),(71,0),(91,1),
        (92,1),(93,3),
        (72,1),(10,230),(20,200),(11,290),(21,200),
        (72,2),(10,260),(20,200),(40,30),(50,0),(51,180),(73,1),
        (72,1),(10,230),(20,200),(11,230),(21,200),
        (97,0),(75,0),(76,1),(52,45),(41,1),(77,0),(78,1),(53,45),(43,0),(44,0),(45,-2.2),(46,2.2),(79,0),(98,0)]
    ents.append(h2)
    # dashed pattern hatch (ANSI33-ish: dashes)
    h3=[(0,"HATCH"),(8,"HATCH"),(62,2),(10,0),(20,0),(30,0),(210,0),(220,0),(230,1),(2,"DASH"),(70,0),(71,0),(91,1),
        (92,2),(72,0),(73,1),(93,4),(10,20),(20,60),(10,120),(20,60),(10,120),(20,100),(10,20),(20,100),(97,0),
        (75,0),(76,1),(52,0),(41,1),(77,0),(78,1),(53,0),(43,0),(44,0),(45,0),(46,4),(79,2),(49,6),(49,-3),(98,0)]
    ents.append(h3)
    # dimension referencing *D1 block
    ents.append([(0,"DIMENSION"),(8,"DIM"),(2,"*D1"),(10,20),(20,10),(30,0),(70,0)])
    blocks=[
      block("BOLT",(0,0),[circle((0,0),5,"0",color=0), line((-7,0),(7,0),"0"), line((0,-7),(0,7),"0"), text("B",(3,3),3,layer="0")]),
      block("NEST",(0,0),[insert("BOLT",(0,0),1,1,0,"0"), insert("BOLT",(20,0),2,2,0,"0",color=2), lwpoly([(-10,-10),(40,-10),(40,10),(-10,10)],True,"0",color=0)]),
      block("*D1",(0,0),[line((20,10),(120,10),"0"), line((20,5),(20,15),"0"), line((120,5),(120,15),"0"), mtext("100",(70,12),3,attach=8)]),
      block("*Model_Space",(0,0),[]),
    ]
    layers=[("0",7,0),("FRAME",8,0),("GEOM",7,0),("TEXT",3,0),("BLOCKS",5,0),("HIDDEN",-1,0),("FROZEN",1,1),("HATCH",1,0),("DIM",2,0)]
    out=header()+tables(layers)+section("BLOCKS",blocks)+section("ENTITIES",ents)+[(0,"EOF")]
    data=w(out).replace("\n","\r\n").encode("cp1251")
    open("tests/test1.dxf","wb").write(data)

def test2():  # R12 minimal, only ENTITIES, unix newlines, leading spaces in codes
    ents=[line((0,0),(10,10)), circle((5,5),3), text("R12 test",(0,12),1)]
    s="".join(f"  {c}\n{v}\n" for c,v in section("ENTITIES",ents)+[(0,"EOF")])
    open("tests/test2.dxf","w").write(s)

def test3():  # stress
    random.seed(1)
    ents=[]
    for i in range(60000):
        x,y=random.uniform(0,1000),random.uniform(0,1000)
        ents.append(line((x,y),(x+random.uniform(-5,5),y+random.uniform(-5,5)),color=random.randint(1,9)))
    for i in range(20000):
        ents.append(circle((random.uniform(0,1000),random.uniform(0,1000)),random.uniform(0.5,4)))
    for i in range(3000):
        ents.append(text("T%d"%i,(random.uniform(0,1000),random.uniform(0,1000)),2))
    ents.append(line((1e7,1e7),(1e7+1,1e7+1)))  # stray outlier far away
    out=header("AC1027","ANSI_1252")+section("ENTITIES",ents)+[(0,"EOF")]
    open("tests/test3.dxf","w").write(w(out))

def test4():  # R2013 UTF-8, paper-space only
    ents=[[(0,"LINE"),(67,1),(8,"0"),(10,0),(20,0),(30,0),(11,50),(21,50),(31,0)],
          [(0,"TEXT"),(67,1),(8,"0"),(10,0),(20,55),(30,0),(40,4),(1,"Тільки аркуш (UTF-8)")]]
    out=header("AC1027","ANSI_1251")+section("ENTITIES",ents)+[(0,"EOF")]
    open("tests/test4.dxf","w",encoding="utf-8").write(w(out))

test1(); test2(); test3(); test4()

def test5():  # self-referencing / exponential blocks must not hang
    blocks=[block("SELF",(0,0),[line((0,0),(1,1)), insert("SELF",(2,0)), insert("SELF",(0,2))]),
            block("A",(0,0),[insert("B",(0,0)),insert("B",(1,0)),insert("B",(2,0))]+[line((0,0),(0.1,0.1))]),
            block("B",(0,0),[insert("A",(0,0))])]
    ents=[insert("SELF",(0,0)), insert("A",(10,0))]
    out=header()+section("BLOCKS",blocks)+section("ENTITIES",ents)+[(0,"EOF")]
    open("tests/test5.dxf","w").write(w(out))
test5()
