package com.yuri.kirikiri2;

import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.DataFormatException;
import java.util.zip.Inflater;

class XP3Extractor {
	private static final String TAG = "XP3Extract";

	static class Entry {
		String fileName;
		long orgSize;
		List<Segment> segments = new ArrayList<>();
	}

	static class Segment {
		boolean compressed;
		long offset;
		long orgSize;
		long arcSize;
	}

	interface ProgressCallback {
		void onProgress(String fileName, int current, int total);
	}

	static List<Entry> listEntries(File xp3File) throws IOException {
		Log.i(TAG, "listEntries: " + xp3File.getAbsolutePath());
		return parseIndex(xp3File);
	}

	/** Quick check if an XP3 file contains startup.tjs (case-insensitive). */
	static boolean hasStartupScript(File xp3File) {
		try {
			List<Entry> entries = parseIndex(xp3File);
			for (Entry e : entries) {
				if (e.fileName.equalsIgnoreCase("startup.tjs"))
					return true;
			}
		} catch (Exception ignored) {}
		return false;
	}

	static void extract(File xp3File, File outputDir, List<Entry> entries,
			ProgressCallback cb) throws IOException {
		Log.i(TAG, "extract: " + entries.size() + " files to " + outputDir);
		outputDir.mkdirs();
		try (RandomAccessFile raf = new RandomAccessFile(xp3File, "r")) {
			for (int i = 0; i < entries.size(); i++) {
				Entry e = entries.get(i);
				cb.onProgress(e.fileName, i + 1, entries.size());

				File outFile = new File(outputDir, e.fileName.replace('\\', File.separatorChar));
				outFile.getParentFile().mkdirs();

				try (FileOutputStream fos = new FileOutputStream(outFile)) {
					for (Segment seg : e.segments) {
						raf.seek(seg.offset);
						byte[] raw = new byte[(int) seg.arcSize];
						raf.readFully(raw);
						byte[] data;
						if (seg.compressed) {
							data = zlibDecompress(raw, (int) seg.orgSize);
						} else {
							data = raw;
						}
						fos.write(data);
					}
				}
				Log.d(TAG, "  [" + (i + 1) + "/" + entries.size() + "] " + e.fileName);
			}
		}
	}

	//--------------------------------------------------------------------------
	// Index parsing
	//--------------------------------------------------------------------------

	private static List<Entry> parseIndex(File xp3File) throws IOException {
		try (RandomAccessFile raf = new RandomAccessFile(xp3File, "r")) {
			long archiveOffset = findMagic(raf);
			if (archiveOffset < 0)
				throw new IOException("Not an XP3 archive");

			Log.d(TAG, "archiveOffset=" + archiveOffset + " fileLen=" + raf.length());

			// XP3 index is a linked list. First 8-byte pointer at byte 11.
			long pointerFilePos = archiveOffset + 11;
			List<Entry> entries = new ArrayList<>();

			while (true) {
				raf.seek(pointerFilePos);
				long idxOff = readI64LE(raf);
				if (idxOff == 0 || idxOff >= raf.length()) break;

				// Seek to index block
				raf.seek(archiveOffset + idxOff);
				int flag = raf.readUnsignedByte() & 0xFF;
				Log.d(TAG, "index at " + (archiveOffset + idxOff) + " flag=" + flag);

				byte[] indexData;
				if ((flag & 0x07) == 0) { // RAW
					long size = readI64LE(raf);
					Log.d(TAG, "  RAW size=" + size);
					indexData = new byte[(int) size];
					raf.readFully(indexData);
				} else if ((flag & 0x07) == 1) { // ZLIB
					long csize = readI64LE(raf);
					long usize = readI64LE(raf);
					Log.d(TAG, "  ZLIB comp=" + csize + " uncomp=" + usize);
					byte[] comp = new byte[(int) csize];
					raf.readFully(comp);
					indexData = zlibDecompress(comp, (int) usize);
				} else {
					throw new IOException("Unknown index encoding: " + (flag & 0x07));
				}

				parseChunks(indexData, 0, indexData.length, entries, archiveOffset);

				// Next pointer is right after current index data
				pointerFilePos = raf.getFilePointer();

				if ((flag & 0x80) == 0) break; // no continue bit
			}

			Log.i(TAG, "parseIndex done: " + entries.size() + " entries @ " + pointerFilePos);
			return entries;
		}
	}

	private static long findMagic(RandomAccessFile raf) throws IOException {
		raf.seek(0);
		byte[] magic = new byte[]{
			(byte) 'X', (byte) 'P', (byte) '3', 0x0D, 0x0A, 0x20, 0x0A, 0x1A,
			(byte) 0x8B, (byte) 0x67, 0x01
		};

		byte[] header = new byte[11];
		if (raf.read(header) == 11 && bytesEqual(header, 0, magic, 0, 11))
			return 0;

		raf.seek(0);
		byte[] mz = new byte[2];
		raf.readFully(mz);
		if (mz[0] == (byte) 'M' && mz[1] == (byte) 'Z') {
			byte[] buf = new byte[256 * 1024];
			long fileLen = raf.length();
			for (long base = 16; base < fileLen; base += 16) {
				int remaining = (int) Math.min(buf.length, fileLen - base);
				raf.seek(base);
				raf.readFully(buf, 0, remaining);
				for (int i = 0; i <= remaining - 11; i += 16) {
					if (bytesEqual(buf, i, magic, 0, 11))
						return base + i;
				}
			}
		}
		return -1;
	}

	private static void parseChunks(byte[] data, int start, int len,
			List<Entry> entries, long archiveOffset) throws IOException {
		int pos = start;
		int end = start + len;
		while (pos + 12 <= end) {
			String name = new String(data, pos, 4, "ASCII");
			long chunkSize = readI64LE(data, pos + 4);
			int bodyStart = pos + 12;
			int bodyEnd = bodyStart + (int) chunkSize;
			if (bodyEnd > end || bodyEnd < bodyStart) break;

			if ("File".equals(name)) {
				Entry entry = parseFileEntry(data, bodyStart, (int) chunkSize, archiveOffset);
				if (entry != null) entries.add(entry);
			}
			pos = bodyEnd;
		}
	}

	private static Entry parseFileEntry(byte[] data, int start, int size,
			long archiveOffset) throws IOException {
		Entry entry = new Entry();
		int end = start + size;
		int pos = start;

		while (pos + 12 <= end) {
			String name = new String(data, pos, 4, "ASCII");
			long chunkSize = readI64LE(data, pos + 4);
			int body = pos + 12;

			if ("info".equals(name) && body + 22 <= end) {
				int bodyLen = (int) chunkSize;
				if (bodyLen < 22) { pos = body + (int) chunkSize; continue; }
				int nameLen = readI16LE(data, body + 20);
				if (body + 22 + nameLen * 2 > end) { pos = body + (int) chunkSize; continue; }
				entry.fileName = decodeUTF16LE(data, body + 22, nameLen);
			} else if ("segm".equals(name)) {
				int segCount = (int) chunkSize / 28;
				for (int i = 0; i < segCount; i++) {
					int base = body + i * 28;
					if (base + 28 > end) break;
					Segment seg = new Segment();
					int segFlags = readI32LE(data, base);
					seg.offset = readI64LE(data, base + 4) + archiveOffset;
					seg.orgSize = readI64LE(data, base + 12);
					seg.arcSize = readI64LE(data, base + 20);
					seg.compressed = (segFlags & 0x07) == 1;
					entry.segments.add(seg);
				}
			}

			pos = body + (int) chunkSize;
		}

		if (entry.fileName == null || entry.fileName.isEmpty()) return null;
		return entry;
	}

	//--------------------------------------------------------------------------
	// Utilities
	//--------------------------------------------------------------------------

	private static byte[] zlibDecompress(byte[] compressed, int expectedSize) throws IOException {
		Inflater inflater = new Inflater();
		inflater.setInput(compressed);
		byte[] result = new byte[expectedSize];
		try {
			int n = inflater.inflate(result);
			inflater.end();
			if (n != expectedSize)
				throw new IOException("Zlib mismatch: got " + n + " expected " + expectedSize);
		} catch (DataFormatException e) {
			inflater.end();
			throw new IOException("Zlib decompress error", e);
		}
		return result;
	}

	private static String decodeUTF16LE(byte[] data, int offset, int charCount) {
		char[] chars = new char[charCount];
		for (int i = 0; i < charCount; i++)
			chars[i] = (char) ((data[offset + i * 2] & 0xFF) | ((data[offset + i * 2 + 1] & 0xFF) << 8));
		return new String(chars);
	}

	private static boolean bytesEqual(byte[] a, int aOff, byte[] b, int bOff, int n) {
		for (int i = 0; i < n; i++)
			if (a[aOff + i] != b[bOff + i]) return false;
		return true;
	}

	private static long readI64LE(RandomAccessFile raf) throws IOException {
		byte[] b = new byte[8];
		raf.readFully(b);
		return readI64LE(b, 0);
	}

	private static long readI64LE(byte[] b, int off) {
		return (b[off] & 0xFFL) | ((b[off + 1] & 0xFFL) << 8)
			| ((b[off + 2] & 0xFFL) << 16) | ((b[off + 3] & 0xFFL) << 24)
			| ((b[off + 4] & 0xFFL) << 32) | ((b[off + 5] & 0xFFL) << 40)
			| ((b[off + 6] & 0xFFL) << 48) | ((b[off + 7] & 0xFFL) << 56);
	}

	private static int readI32LE(byte[] b, int off) {
		return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8)
			| ((b[off + 2] & 0xFF) << 16) | ((b[off + 3] & 0xFF) << 24);
	}

	private static int readI16LE(byte[] b, int off) {
		return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8);
	}
}
