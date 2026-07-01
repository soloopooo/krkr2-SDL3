package com.yuri.kirikiri2;

import android.graphics.Bitmap;
import android.graphics.Bitmap.Config;

import java.io.IOException;

class TLGDecoder {
	static Bitmap decode(byte[] data) throws IOException {
		if (data.length < 11) throw new IOException("Not a TLG file");
		if (matchMagic(data, 0, TLG5_MAGIC)) return decodeTLG5(data, 0);
		if (matchMagic(data, 0, TLG6_MAGIC)) return decodeTLG6(data, 0);
		if (matchMagic(data, 0, TLG0_MAGIC)) return decodeTLG0(data);
		throw new IOException("Unknown TLG format");
	}

	private static boolean matchMagic(byte[] data, int off, byte[] magic) {
		if (off + magic.length > data.length) return false;
		for (int i = 0; i < magic.length; i++)
			if (data[off + i] != magic[i]) return false;
		return true;
	}

	private static final byte[] TLG5_MAGIC = {'T','L','G','5','.','0',0,'r','a','w',0x1A,0};
	private static final byte[] TLG6_MAGIC = {'T','L','G','6','.','0',0,'r','a','w',0x1A,0};
	private static final byte[] TLG0_MAGIC = {'T','L','G','0','.','0',0,'s','d','s',0x1A,0};
	// 12 bytes each: "TLG5.0\0raw\0x1a\0"

	// TLG0.0 SDS wrapper: skip 11-byte magic + 4-byte size, decode embedded TLG5/6
	private static Bitmap decodeTLG0(byte[] data) throws IOException {
		int rawSize = readI32LE(data, 11);
		byte[] inner = new byte[rawSize];
		System.arraycopy(data, 15, inner, 0, Math.min(rawSize, data.length - 15));
		return decode(inner);
	}

	//--------------------------------------------------------------------------
	// TLG5.0: color planes + modified LZSS + differential coding
	//--------------------------------------------------------------------------
	private static Bitmap decodeTLG5(byte[] data, int off) throws IOException {
		int pos = off + 11;
		int colors = data[pos++] & 0xFF;
		int w = readI32LE(data, pos); pos += 4;
		int h = readI32LE(data, pos); pos += 4;
		int blockH = readI32LE(data, pos); pos += 4;
		int blockCount = (h + blockH - 1) / blockH;
		pos += blockCount * 4;

		int[] pixels = new int[w * h];

		// 3c: planes[0]=B [1]=G [2]=R,  4c: [0]=R [1]=G [2]=B [3]=A
		byte[][] planes = new byte[colors][w * blockH];

		for (int by = 0; by < h; by += blockH) {
			int curH = Math.min(blockH, h - by);
			int lineBytes = w * curH;

			for (int c = 0; c < colors; c++) {
				int flag = data[pos++] & 0xFF;
				int size = readI32LE(data, pos); pos += 4;
				byte[] dec = flag == 0
					? tlg5DecompressSlide(data, pos, size, lineBytes, 0)
					: null;
				pos += size;
				System.arraycopy(flag == 0 ? dec : data, pos - size, planes[c], 0, lineBytes);
			}

			// Compose pixels
			int baseOff = by * w;
			for (int y = 0; y < curH; y++) {
				int li = y * w;
				int accB = 0, accG = 0, accR = 0, accA = 0;
				boolean firstBlock = (by == 0);

				for (int x = 0; x < w; x++) {
					int idx = li + x;
					int B, G, R, A = 0xFF;

					if (colors == 3) {
						B = planes[0][idx] & 0xFF;
						G = planes[1][idx] & 0xFF;
						R = planes[2][idx] & 0xFF;
					} else {
						R = planes[0][idx] & 0xFF;
						G = planes[1][idx] & 0xFF;
						B = planes[2][idx] & 0xFF;
						A = planes[3][idx] & 0xFF;
					}

					B += G; R += G;
					accB += B; accG += G; accR += R; accA += A;

					if (!firstBlock) {
						int upper = pixels[baseOff - w + x];
						accB = (accB + ((upper >> 16) & 0xFF)) & 0xFF;
						accG = (accG + ((upper >> 8) & 0xFF)) & 0xFF;
						accR = (accR + (upper & 0xFF)) & 0xFF;
						accA = (accA + ((upper >> 24) & 0xFF)) & 0xFF;
					}
					pixels[baseOff + x] = (accA & 0xFF) << 24
						| (Math.min(accB, 255) & 0xFF) << 16
						| (Math.min(accG, 255) & 0xFF) << 8
						| (Math.min(accR, 255) & 0xFF);
				}
				baseOff += w;
			}
		}

		Bitmap bmp = Bitmap.createBitmap(w, h, Config.ARGB_8888);
		bmp.setPixels(pixels, 0, w, 0, 0, w, h);
		return bmp;
	}

	// Modified LZSS with 4096-byte sliding window
	static byte[] tlg5DecompressSlide(byte[] in, int off, int len, int expect, int r0) {
		byte[] out = new byte[expect];
		byte[] txt = new byte[4096];
		int r = r0 & 0xFFF, ip = off, ep = off + len, op = 0;
		int safety = 0;
		while (op < expect && ip < ep && safety < expect * 2) {
			safety++;
			int fl = in[ip++] & 0xFF;
			for (int b = 0; b < 8 && op < expect && ip < ep; b++) {
				if ((fl & 1) != 0) {
					if (ip + 2 > ep) break;
					int w = (in[ip] & 0xFF) | ((in[ip + 1] & 0xFF) << 8);
					ip += 2;
					int mp = w & 0xFFF, ml = (w >>> 12) + 3;
					if (ml == 18 && ip < ep) ml += in[ip++] & 0xFF;
					ml = Math.min(ml, expect - op);
					for (int i = 0; i < ml && op < expect; i++) {
						byte bv = txt[mp & 0xFFF];
						out[op++] = bv;
						txt[r] = bv;
						r = (r + 1) & 0xFFF;
						mp++;
					}
				} else {
					byte bv = in[ip++];
					out[op++] = bv;
					txt[r] = bv;
					r = (r + 1) & 0xFFF;
				}
				fl >>>= 1;
			}
		}
		return out;
	}

	//--------------------------------------------------------------------------
	// TLG6.0: golomb bitstream + MED/AVG + chroma filters
	//--------------------------------------------------------------------------
	private static Bitmap decodeTLG6(byte[] data, int off) throws IOException {
		// TODO: full TLG6 decoder
		// The format uses per-component golomb bitstream with adaptive state,
		// 8x8 blocks with serpentine reordering, 16+ chroma filter formulas,
		// and MED/AVG prediction across lines.
		throw new IOException("TLG6 decoder not yet implemented");
	}

	private static int readI32LE(byte[] b, int off) {
		return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8)
			| ((b[off + 2] & 0xFF) << 16) | ((b[off + 3] & 0xFF) << 24);
	}
}
