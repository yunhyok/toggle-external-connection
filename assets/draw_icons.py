"""Draw the two tray states from the same geometry. Only needed to change artwork."""
from io import BytesIO
from pathlib import Path
from struct import pack

from PIL import Image, ImageDraw

HERE = Path(__file__).resolve().parent
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def glyph(size, blocked):
    s = 16
    art = Image.new("RGBA", (32*s, 32*s))
    draw = ImageDraw.Draw(art)
    def box(*coords):
        return tuple(round(value*s) for value in coords)
    def line(coords, color, width):
        draw.line(box(*coords), fill=color, width=round(width*s), joint="curve")
    ink, sky, tile = "#ddeeff", "#78baff", "#1e2938"
    draw.rounded_rectangle(box(.6, 1.8, 31.4, 30.2), radius=6*s, fill=tile,
                           outline="#8097af", width=s)
    # Two local endpoints remain joined in both states.
    line((5.8, 8.7, 10.7, 8.7, 10.7, 23.3, 5.8, 23.3), ink, 2)
    for y in (8.7, 23.3):
        draw.rounded_rectangle(box(3.6, y-2, 8.1, y+2), radius=s, fill=ink)
    c = "#efaa89" if blocked else sky
    line((10.2,16,13.5 if blocked else 18.1,16), c, 2.6)
    draw.ellipse(box(17.5,8.6,29.3,23.4), outline=c, width=2*s)
    if size >= 24:
        draw.arc(box(20.6,8.6,26.1,23.4),0,360,fill=c,width=round(1.3*s))
    line((18.1,16,28.5,16),c,1.6)
    if blocked:
        line((20,7.7,12.1,24),tile,4.7)
        line((20,7.7,12.1,24),"#ffad86",2.3)
    return art.resize((size,size),Image.Resampling.LANCZOS)


def ico(blocked, stem):
    frames = []
    for size in SIZES:
        image = glyph(size, blocked)
        data = BytesIO()
        if size >= 128:
            image.save(data, format="PNG")
        else:
            # Uncompressed BGRA + a 1-bit mask for classic Explorer and Setup.
            data.write(pack("<IiiHHIIiiII",40,size,2*size,1,32,0,size*size*4,0,0,0,0))
            data.write(image.transpose(Image.Transpose.FLIP_TOP_BOTTOM).tobytes("raw","BGRA"))
            data.write(bytes(((size+31)//32)*4*size))
        frames.append(data.getvalue())
    out = BytesIO()
    out.write(pack("<HHH",0,1,len(frames)))
    offset = 6 + len(frames)*16
    for size,data in zip(SIZES,frames):
        out.write(pack("<BBBBHHII",size%256,size%256,0,0,1,32,len(data),offset))
        offset += len(data)
    (HERE/f"{stem}.ico").write_bytes(out.getvalue()+b"".join(frames))


if __name__ == "__main__":
    ico(False,"connected")
    ico(True,"blocked")
