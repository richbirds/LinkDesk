#!/usr/bin/env python3
"""Package the approved PNG into platform icons (macOS sips/iconutil)."""
import pathlib, struct, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[1]
ICONS = ROOT / 'assets/icons'
def strip_metadata(path):
    data = path.read_bytes(); result = data[:8]; offset = 8
    allowed = {b'IHDR', b'PLTE', b'IDAT', b'IEND', b'tRNS', b'sRGB', b'gAMA', b'cHRM', b'pHYs'}
    while offset < len(data):
        length = struct.unpack('>I', data[offset:offset+4])[0]; end = offset+length+12
        if data[offset+4:offset+8] in allowed: result += data[offset:end]
        offset = end
    path.write_bytes(result)
if __name__ == '__main__':
    strip_metadata(ICONS / 'linkdesk.png')
    with tempfile.TemporaryDirectory(prefix='linkdesk-icons-') as temp:
        directory = pathlib.Path(temp); iconset = directory / 'LinkDesk.iconset'; iconset.mkdir()
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                output = iconset / f'icon_{size}x{size}{"@2x" if scale == 2 else ""}.png'
                subprocess.run(['sips', '-z', str(size*scale), str(size*scale), str(ICONS/'linkdesk.png'), '--out', str(output)], check=True, stdout=subprocess.DEVNULL)
                strip_metadata(output)
        subprocess.run(['iconutil', '-c', 'icns', str(iconset), '-o', str(ICONS/'LinkDesk.icns')], check=True)
        sizes = (16,32,48,64,128,256); images = []
        for size in sizes:
            output = directory / f'icon-{size}.png'
            subprocess.run(['sips','-z',str(size),str(size),str(ICONS/'linkdesk.png'),'--out',str(output)],check=True,stdout=subprocess.DEVNULL)
            strip_metadata(output); images.append(output.read_bytes())
        table = bytearray(struct.pack('<HHH',0,1,len(sizes))); offset = 6+16*len(sizes)
        for size,data in zip(sizes,images):
            table.extend(struct.pack('<BBBBHHII',size%256,size%256,0,0,1,32,len(data),offset)); offset += len(data)
        (ICONS/'LinkDesk.ico').write_bytes(table+b''.join(images))
