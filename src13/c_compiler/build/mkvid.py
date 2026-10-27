import struct, os

def chunk(cid, data):
    pad = b"\x00" if (len(data) & 1) else b""
    return cid + struct.pack("<I", len(data)) + data + pad

def listc(kind, data):
    return chunk(b"LIST", kind + data)

def jpeg(dc_bits):
    # grayscale 8x8. DC huffman: cat 0 = '0', cat 11 = '10'. AC EOB = '0'.
    # dc_bits is the entropy after SOI markers, already padded to bytes.
    dqt = bytes([0]) + bytes([1] * 64)
    sof = struct.pack(">BHHB", 8, 8, 8, 1) + bytes([1, 0x11, 0])
    dht_dc = bytes([0x00]) + bytes([1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) + bytes([0, 11])
    dht_ac = bytes([0x10]) + bytes([1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]) + bytes([0])
    sos = bytes([1, 1, 0x00, 0, 63, 0])
    def seg(mark, body):
        return bytes([0xFF, mark]) + struct.pack(">H", len(body) + 2) + body
    return b"\xFF\xD8" + seg(0xDB, dqt) + seg(0xC0, sof) + seg(0xC4, dht_dc) + seg(0xC4, dht_ac) + seg(0xDA, sos) + dc_bits + b"\xFF\xD9"

# gray (DC 0): bits 0,0 then pad 1s -> 00111111
JPG_GRAY = jpeg(b"\x3F")
# white, coef 1024: 10 + 10000000000 + 0 + pad 11
JPG_WHITE = jpeg(b"\xA0\x03")
# black, coef -1024: 10 + 01111111111 + 0 + pad 11
JPG_BLACK = jpeg(b"\x9F\xFB")

def avi(frames, codec, bpp, w, h, raw_pixels=False):
    us = 100000
    avih = struct.pack("<IIIIIIIIII", us, 0, 0, 0, len(frames), 0, 1, 0, w, h) + b"\x00" * 16
    # strh: vids, handler, flags, pri/lang, init, scale, rate, start, length
    strh = b"vids" + codec + struct.pack("<IHHIIIIII", 0, 0, 0, 0, 1, 10, 0, len(frames), 0)
    strh += b"\x00" * 16
    stride = ((w * bpp + 31) // 32) * 4
    height = h if raw_pixels else h
    strf = struct.pack("<IiiHHI", 40, w, height, 1, bpp, 0 if raw_pixels else int.from_bytes(codec, "little"))
    strf += struct.pack("<IIII", h * stride, 0, 0, 0)
    hdrl = listc(b"hdrl", chunk(b"avih", avih) + listc(b"strl", chunk(b"strh", strh) + chunk(b"strf", strf)))
    movi_body = b""
    kind = b"00db" if raw_pixels else b"00dc"
    for fr in frames:
        movi_body += chunk(kind, fr)
    movi = listc(b"movi", movi_body)
    body = hdrl + movi
    return b"RIFF" + struct.pack("<I", len(body) + 4) + b"AVI " + body

def raw_frame(w, h, b, g, r):
    stride = ((w * 3 + 31) // 32) * 4
    row = bytes([b, g, r]) * w + b"\x00" * (stride - w * 3)
    # bottom-up: same solid color
    return row * h

def box(typ, payload):
    return struct.pack(">I", 8 + len(payload)) + typ + payload

def mp4(jpegs, codec=b"jpeg"):
    # one chunk containing all samples
    samples = b"".join(jpegs)
    ftyp = box(b"ftyp", b"isom" + struct.pack(">I", 0) + b"isommp41")
    mdat = box(b"mdat", samples)
    mdat_off = len(ftyp) + 8
    n = len(jpegs)
    # mvhd v0 timescale 1000 duration n*40
    mvhd = b"\x00" * 12 + struct.pack(">II", 1000, n * 40) + b"\x00" * (4 + 8 + 2 + 2 + 2 + 2 + 36) 
    # mvhd total content should be 100 bytes for v0? We'll build explicitly.
    mvhd = bytearray(100)
    struct.pack_into(">I", mvhd, 12, 1000)
    struct.pack_into(">I", mvhd, 16, n * 40)
    # identity matrix at offset 40? For mvhd:
    # 0 ver, 4 ctime, 8 mtime, 12 scale, 16 dur, 20 rate(4), 24 vol(2)+pad(2)+res(8)=36, 36 matrix 36 -> 72, 72 predef 24, 96 next 4 = 100
    mvhd[20:24] = struct.pack(">I", 0x00010000)  # rate 1.0
    mvhd[24:26] = struct.pack(">H", 0x0100)
    # matrix
    struct.pack_into(">9I", mvhd, 36, 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000)
    tkhd = bytearray(84)
    tkhd[12:16] = struct.pack(">I", 1)  # track id
    tkhd[20:24] = struct.pack(">I", n * 40)
    struct.pack_into(">9I", tkhd, 40, 0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000)
    struct.pack_into(">I", tkhd, 76, 8 << 16)
    struct.pack_into(">I", tkhd, 80, 8 << 16)
    mdhd = bytearray(24)
    struct.pack_into(">I", mdhd, 12, 1000)
    struct.pack_into(">I", mdhd, 16, n * 40)
    hdlr = b"\x00" * 8 + b"vide" + b"\x00" * 12 + b"Video\x00"
    # stsd
    entry = bytearray(86)
    struct.pack_into(">I", entry, 0, 86)
    entry[4:8] = codec
    entry[14:16] = struct.pack(">H", 1)  # data ref
    struct.pack_into(">H", entry, 16 + 2 + 2 + 12, 8)  # width at 32? 
    # SampleEntry is 16 bytes then visual.
    # 0 size, 4 format, 8 reserved6, 14 dataref -> 16
    # visual: 16 pre_defined2, 18 reserved2, 20 pre_defined12 -> 32, width at 32
    struct.pack_into(">HH", entry, 32, 8, 8)
    struct.pack_into(">II", entry, 36, 0x00480000, 0x00480000)
    struct.pack_into(">H", entry, 36 + 8 + 4, 1)  # frame count at 48
    entry[84:86] = struct.pack(">h", -1)
    entry[82:84] = struct.pack(">H", 24)
    stsd = b"\x00" * 4 + struct.pack(">I", 1) + bytes(entry)
    stts = b"\x00" * 4 + struct.pack(">III", 1, n, 40)
    stsc = b"\x00" * 4 + struct.pack(">IIII", 1, 1, n, 1)
    stsz = b"\x00" * 4 + struct.pack(">II", 0, n) + b"".join(struct.pack(">I", len(j)) for j in jpegs)
    stco = b"\x00" * 4 + struct.pack(">II", 1, mdat_off)
    stbl = box(b"stsd", stsd) + box(b"stts", stts) + box(b"stsc", stsc) + box(b"stsz", stsz) + box(b"stco", stco)
    minf = box(b"stbl", stbl)
    mdia = box(b"mdhd", bytes(mdhd)) + box(b"hdlr", hdlr) + box(b"minf", minf)
    trak = box(b"tkhd", bytes(tkhd)) + box(b"mdia", mdia)
    moov = box(b"mvhd", bytes(mvhd)) + box(b"trak", trak)
    return ftyp + mdat + moov

def main():
    base = os.path.dirname(os.path.abspath(__file__))
    root = os.path.normpath(os.path.join(base, "..", ".."))
    frames = [JPG_GRAY, JPG_WHITE, JPG_BLACK]
    avi_mjpg = avi(frames, b"MJPG", 24, 8, 8, False)
    raws = [raw_frame(8, 8, 80, 80, 80), raw_frame(8, 8, 255, 255, 255), raw_frame(8, 8, 0, 0, 0)]
    avi_rgb = avi(raws, b"DIB ", 24, 8, 8, True)
    mov = mp4(frames, b"jpeg")
    avc = mp4([b"\x00\x00\x00\x04\x65\x88\x84\x00"], b"avc1")
    out = os.path.join(root, "content")
    paths = {
        os.path.join(out, "SAMPLE.AVI"): avi_mjpg,
        os.path.join(out, "SAMPLE.MP4"): mov,
        os.path.join(base, "rgb.avi"): avi_rgb,
        os.path.join(base, "avc.mp4"): avc,
        os.path.join(base, "mjpg.avi"): avi_mjpg,
        os.path.join(base, "jpeg.mp4"): mov,
    }
    for p, data in paths.items():
        with open(p, "wb") as f:
            f.write(data)
        print(p, len(data))

if __name__ == "__main__":
    main()
