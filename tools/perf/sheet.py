import glob,os,sys
from PIL import Image, ImageDraw
d=sys.argv[1]; start=float(sys.argv[2]) if len(sys.argv)>2 else 0
fs=[f for f in sorted(glob.glob(d+'/frames/*.ppm')) if int(os.path.basename(f)[1:7])/60>=start]
cols=6; th=[Image.open(f).resize((320,224)) for f in fs]
sh=Image.new('RGB',(cols*320,max(1,(len(th)+cols-1)//cols)*236)); dr=ImageDraw.Draw(sh)
for i,(f,t) in enumerate(zip(fs,th)):
    x,y=(i%cols)*320,(i//cols)*236; sh.paste(t,(x,y+12)); v=int(os.path.basename(f)[1:7]); dr.text((x+2,y),f'{v/60:.1f}s',fill='yellow')
sh.save(d+'/sheet2.png')
