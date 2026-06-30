#!/usr/bin/env python3
"""Extract/print files from XP3 (Kirikiri) archives."""
import struct, zlib, sys

def xp3_files(path):
    """Yield (name, orgSize, data_offset, original_size, compressed_size, raw_data, segm_flags)."""
    with open(path, 'rb') as f:
        raw = f.read()

    magic = raw[:5]
    assert magic == b'XP3\r\n', f"Bad magic: {magic}"

    # Index chain: first offset at byte 11, then each index's data is followed
    # by the next offset pointer (linked list).
    pos = 11  # first index offset position
    while True:
        if pos + 8 > len(raw):
            break
        idx_off = struct.unpack_from('<Q', raw, pos)[0]
        if idx_off == 0 or idx_off >= len(raw):
            break

        # Seek to index data
        p = idx_off
        flag = raw[p]; p += 1

        if (flag & 1):  # ZLIB
            csize = struct.unpack_from('<Q', raw, p)[0]; p += 8
            isize = struct.unpack_from('<Q', raw, p)[0]; p += 8
            idx_data = zlib.decompress(raw[p:p+csize])
            p += csize
        else:  # RAW
            isize = struct.unpack_from('<Q', raw, p)[0]; p += 8
            idx_data = raw[p:p+isize]
            p += isize

        # Parse all File entries in this index segment
        i = 0
        while i + 12 < len(idx_data):
            if idx_data[i:i+4] != b'File':
                i += 1; continue
            chunk_sz = struct.unpack_from('<Q', idx_data, i+4)[0]
            chunk_end = i + 12 + chunk_sz
            if chunk_end > len(idx_data):
                break

            info_pos = idx_data.find(b'info', i+12, chunk_end)
            if info_pos < 0: i = chunk_end; continue
            ds = info_pos + 12
            org = struct.unpack_from('<Q', idx_data, ds+4)[0]
            nlen = struct.unpack_from('<H', idx_data, ds+20)[0]
            name = idx_data[ds+22:ds+22+nlen*2].decode('utf-16-le', errors='replace')

            segm_pos = idx_data.find(b'segm', i+12, chunk_end)
            foff = forig = farc = sf = 0
            if segm_pos >= 0:
                sd = segm_pos + 12
                sf = struct.unpack_from('<I', idx_data, sd)[0]
                foff = struct.unpack_from('<Q', idx_data, sd+4)[0]
                forig = struct.unpack_from('<Q', idx_data, sd+12)[0]
                farc = struct.unpack_from('<Q', idx_data, sd+20)[0]

            yield name, org, foff, forig, farc, raw, sf

            i = chunk_end

        # Next index position = right after this index data (= p)
        if (flag & 0x80):  # CONTINUE bit
            pos = p
        else:
            break

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <archive.xp3> [filter_name]")
        print(f"  If filter_name given, extracts matching file(s) to stdout.")
        return

    path, filter_ = sys.argv[1], sys.argv[2].lower() if len(sys.argv) > 2 else None

    for name, org, foff, forig, farc, raw, sf in xp3_files(path):
        if filter_:
            if filter_ in name.lower():
                data = raw[foff:foff+farc]
                if sf & 1:
                    data = zlib.decompress(data)
                sys.stdout.buffer.write(data)
            continue
        print(f"{name} ({org} bytes)")

if __name__ == '__main__':
    main()
