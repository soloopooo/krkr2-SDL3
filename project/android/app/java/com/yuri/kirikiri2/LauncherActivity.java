package com.yuri.kirikiri2;

import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.Intent;
import android.os.AsyncTask;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.View;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.Toolbar;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.channels.FileChannel;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public class LauncherActivity extends AppCompatActivity {
	private RecyclerView mFileList;
	private FileAdapter mAdapter;
	private RecyclerView mRecentList;
	private RecentAdapter mRecentAdapter;
	private LinearLayout mBreadcrumbContainer;
	private HorizontalScrollView mBreadcrumbScroll;
	private View mRecentCard;
	private File mCurrentDir;
	private List<FileEntry> mFiles = new ArrayList<>();
	private List<String> mRecentPaths = new ArrayList<>();
	private static final int MAX_RECENT = 20;

	// Clipboard for copy/cut
	private static class ClipData {
		File source;
		boolean isCut; // false = copy, true = cut (move)
	}
	private ClipData mClipboard;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_launcher);

		Toolbar toolbar = findViewById(R.id.toolbar);
		toolbar.setSubtitle("Select a game");
		setSupportActionBar(toolbar);

		mBreadcrumbContainer = findViewById(R.id.breadcrumbContainer);
		mBreadcrumbScroll = findViewById(R.id.breadcrumbScroll);
		mRecentCard = findViewById(R.id.recentCard);

		mAdapter = new FileAdapter(mFiles,
			entry -> {
				if (entry.isDirectory) {
					loadDir(new File(entry.fullPath));
			} else if (entry.isGame) {
				addRecent(entry.fullPath);
				startGame(entry.fullPath);
			} else if (entry.isVideo) {
				Intent intent = new Intent(this, VideoPlayerActivity.class);
				intent.putExtra("videoPath", entry.fullPath);
				startActivity(intent);
			}
		},
			(entry, pos) -> showFileMenu(entry, pos)
		);

		mFileList = findViewById(R.id.fileList);
		mFileList.setLayoutManager(new LinearLayoutManager(this));
		mFileList.setAdapter(mAdapter);

		mRecentList = findViewById(R.id.recentList);
		mRecentList.setLayoutManager(new LinearLayoutManager(this));
		mRecentAdapter = new RecentAdapter(mRecentPaths,
			new RecentAdapter.OnRecentClickListener() {
				@Override
				public void onPlay(String path) {
					addRecent(path);
					startGame(path);
				}
				@Override
				public void onDelete(String path) {
					mRecentPaths.remove(path);
					mRecentAdapter.setPaths(mRecentPaths);
					saveRecent();
				}
			});
		mRecentList.setAdapter(mRecentAdapter);

		// Quick-access buttons
		findViewById(R.id.btnDownload).setOnClickListener(v ->
			loadDir(new File("/storage/emulated/0/Download")));
		findViewById(R.id.btnRoot).setOnClickListener(v ->
			loadDir(new File("/storage/emulated/0")));

		findViewById(R.id.btnSettings).setOnClickListener(v ->
			startActivity(new Intent(this, SettingsActivity.class)));
		findViewById(R.id.btnLogcat).setOnClickListener(v ->
			startActivity(new Intent(this, LogViewerActivity.class)));
		findViewById(R.id.btnAbout).setOnClickListener(v -> showAboutDialog());

		loadRecent();
		refreshRecentCard();

		// Determine startup directory
		File startDir = null;
		// 1. Check saved last_path if remember_last_path is enabled
		boolean rem = getBoolFromPref("remember_last_path", true);
		if (rem) {
			String saved = getSharedPreferences("launcher", MODE_PRIVATE)
				.getString("last_path", "");
			if (!saved.isEmpty()) {
				File f = new File(saved);
				if (f.isDirectory()) startDir = f;
			}
		}
		// 2. Fallback to /storage/emulated/0 or internal
		if (startDir == null) {
			startDir = new File("/storage/emulated/0");
			if (!startDir.isDirectory()) startDir = getExternalFilesDir(null);
		}
		if (startDir != null) loadDir(startDir);
	}

	//--------------------------------------------------------------------------
	// Directory navigation
	//--------------------------------------------------------------------------

	private void loadDir(File dir) {
		if (!dir.isDirectory()) return;
		mCurrentDir = dir;
		updateBreadcrumbs(dir);
		new LoadTask().execute(dir);
		// Remember last path
		getSharedPreferences("launcher", MODE_PRIVATE).edit()
			.putString("last_path", dir.getAbsolutePath()).apply();
	}

	private void updateBreadcrumbs(File dir) {
		mBreadcrumbContainer.removeAllViews();

		List<String> segments = new ArrayList<>();
		List<File> dirs = new ArrayList<>();
		File cur = dir;
		while (cur != null) {
			dirs.add(0, cur);
			String name = cur.getName();
			if (TextUtils.isEmpty(name) || "/".equals(name)) {
				segments.add(0, "Root");
			} else {
				segments.add(0, name);
			}
			cur = cur.getParentFile();
		}

		String abs = dir.getAbsolutePath();
		if (abs.startsWith("/storage/emulated/0") && segments.size() >= 1) {
			segments.set(0, abs.equals("/storage/emulated/0") ? "Internal storage" : "Internal");
		}

		int depth = dirs.size();
		for (int i = 0; i < depth; i++) {
			if (i > 0) {
				TextView sep = new TextView(this);
				sep.setText("›");
				sep.setTextSize(14);
				sep.setTextColor(getColor(R.color.on_surface_variant));
				sep.setAlpha(0.4f);
				sep.setPadding(2, 0, 2, 0);
				mBreadcrumbContainer.addView(sep);
			}

			boolean isLast = (i == depth - 1);
			String label = segments.get(i);

			TextView tv = new TextView(this);
			tv.setText(label);
			tv.setTextSize(13);
			tv.setEllipsize(TextUtils.TruncateAt.END);
			tv.setMaxLines(1);
			tv.setPadding(6, 8, 6, 8);

			if (isLast) {
				tv.setTextColor(getColor(R.color.on_surface));
				tv.setTypeface(null, android.graphics.Typeface.BOLD);
			} else {
				tv.setTextColor(getColor(R.color.primary));
				tv.setClickable(true);
				tv.setFocusable(true);
				File target = dirs.get(i);
				tv.setOnClickListener(v -> {
					if (!target.equals(mCurrentDir)) loadDir(target);
				});
			}

			mBreadcrumbContainer.addView(tv);
		}

		mBreadcrumbScroll.post(() -> mBreadcrumbScroll.fullScroll(HorizontalScrollView.FOCUS_RIGHT));
	}

	//--------------------------------------------------------------------------
	// Game launch
	//--------------------------------------------------------------------------

	private void startGame(String path) {
		Intent intent = new Intent(this, MainActivity.class);
		intent.putExtra("startupPath", path);
		startActivity(intent);
	}

	//--------------------------------------------------------------------------
	// Long-press file context menu
	//--------------------------------------------------------------------------

	private void showFileMenu(FileEntry entry, int position) {
		PopupMenu popup = new PopupMenu(this, mFileList.findViewHolderForLayoutPosition(position).itemView);
		popup.getMenu().add(0, 1, 0, "Copy");
		popup.getMenu().add(0, 2, 0, "Cut");
		popup.getMenu().add(0, 3, 0, "Delete");
		popup.getMenu().add(0, 4, 0, "Rename");

		if (entry.isGame || entry.isVideo) {
			popup.getMenu().add(0, 7, 0, "Play");
		}

		String lower = entry.name.toLowerCase();
		if (!entry.isDirectory && !entry.isVideo && (lower.endsWith(".xp3") || lower.endsWith(".xp4"))) {
			popup.getMenu().add(0, 5, 0, "Extract XP3");
		}

		if (mClipboard != null) {
			popup.getMenu().add(0, 6, 0, "Paste");
		}

		popup.setOnMenuItemClickListener(item -> {
			switch (item.getItemId()) {
				case 1: copyFile(new File(entry.fullPath), false); return true;
				case 2: copyFile(new File(entry.fullPath), true); return true;
				case 3: deleteFile(new File(entry.fullPath)); return true;
				case 4: renameFile(new File(entry.fullPath)); return true;
				case 5: extractXP3(new File(entry.fullPath)); return true;
				case 6: pasteFiles(); return true;
				case 7:
					if (entry.isVideo) {
						Intent vIntent = new Intent(this, VideoPlayerActivity.class);
						vIntent.putExtra("videoPath", entry.fullPath);
						startActivity(vIntent);
					} else {
						addRecent(entry.fullPath);
						startGame(entry.fullPath);
					}
					return true;
			}
			return false;
		});
		popup.show();
	}

	//--------------------------------------------------------------------------
	// Clipboard
	//--------------------------------------------------------------------------

	private void copyFile(File file, boolean cut) {
		mClipboard = new ClipData();
		mClipboard.source = file;
		mClipboard.isCut = cut;
		Toast.makeText(this, (cut ? "Cut" : "Copy") + ": " + file.getName(), Toast.LENGTH_SHORT).show();
	}

	private void pasteFiles() {
		if (mClipboard == null || mCurrentDir == null) return;
		final File src = mClipboard.source;
		final File dst = new File(mCurrentDir, src.getName());

		if (src.equals(dst)) {
			Toast.makeText(this, "Same location", Toast.LENGTH_SHORT).show();
			return;
		}

		new AsyncTask<Void, Void, Boolean>() {
			private String mError;
			private ProgressDialog pd;
			@Override
			protected void onPreExecute() {
				pd = new ProgressDialog(LauncherActivity.this);
				pd.setMessage(mClipboard.isCut ? "Moving..." : "Copying...");
				pd.setIndeterminate(true);
				pd.setCancelable(false);
				pd.show();
			}
			@Override
			protected Boolean doInBackground(Void... v) {
				try {
					if (mClipboard.isCut) {
						if (!src.renameTo(dst)) {
							copyFileRecursive(src, dst);
							deleteRecursive(src);
						}
					} else {
						copyFileRecursive(src, dst);
					}
					return true;
				} catch (Exception e) {
					mError = e.getMessage();
					return false;
				}
			}
			@Override
			protected void onPostExecute(Boolean ok) {
				pd.dismiss();
				if (ok) {
					Toast.makeText(LauncherActivity.this, "Done", Toast.LENGTH_SHORT).show();
					if (mClipboard.isCut) mClipboard = null;
					loadDir(mCurrentDir);
				} else {
					Toast.makeText(LauncherActivity.this, "Error: " + mError, Toast.LENGTH_LONG).show();
				}
			}
		}.execute();
	}

	//--------------------------------------------------------------------------
	// Delete
	//--------------------------------------------------------------------------

	private void deleteFile(final File file) {
		new AlertDialog.Builder(this)
			.setTitle("Delete")
			.setMessage("Delete " + file.getName() + "?")
			.setPositiveButton("Delete", (d, w) -> {
				new AsyncTask<Void, Void, Boolean>() {
					private ProgressDialog pd;
					@Override
					protected void onPreExecute() {
						pd = new ProgressDialog(LauncherActivity.this);
						pd.setMessage("Deleting...");
						pd.setIndeterminate(true);
						pd.setCancelable(false);
						pd.show();
					}
					@Override
					protected Boolean doInBackground(Void... v) {
						return deleteRecursive(file);
					}
					@Override
					protected void onPostExecute(Boolean ok) {
						pd.dismiss();
						if (ok) loadDir(mCurrentDir);
						else Toast.makeText(LauncherActivity.this, "Delete failed", Toast.LENGTH_SHORT).show();
					}
				}.execute();
			})
			.setNegativeButton("Cancel", null)
			.show();
	}

	//--------------------------------------------------------------------------
	// Rename
	//--------------------------------------------------------------------------

	private void renameFile(final File file) {
		final EditText input = new EditText(this);
		input.setText(file.getName());
		input.setSelection(0, file.getName().lastIndexOf('.') > 0 ?
			file.getName().lastIndexOf('.') : file.getName().length());

		new AlertDialog.Builder(this)
			.setTitle("Rename")
			.setView(input)
			.setPositiveButton("Rename", (d, w) -> {
				String newName = input.getText().toString().trim();
				if (newName.isEmpty()) return;
				File dst = new File(file.getParent(), newName);
				if (file.renameTo(dst)) {
					loadDir(mCurrentDir);
				} else {
					Toast.makeText(this, "Rename failed", Toast.LENGTH_SHORT).show();
				}
			})
			.setNegativeButton("Cancel", null)
			.show();
	}

	//--------------------------------------------------------------------------
	// XP3 extraction
	//--------------------------------------------------------------------------

	private void extractXP3(final File xp3File) {
		final ProgressDialog pd = new ProgressDialog(this);
		pd.setTitle("Extracting XP3");
		pd.setMessage("Parsing...");
		pd.setIndeterminate(false);
		pd.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
		pd.setCancelable(false);
		pd.setMax(100);
		pd.show();

		new AsyncTask<Void, Integer, Boolean>() {
			private String mError;
			@Override
			protected Boolean doInBackground(Void... v) {
				try {
					android.util.Log.i("XP3Extract", "Starting extraction of " + xp3File.getName());

					List<XP3Extractor.Entry> entries = XP3Extractor.listEntries(xp3File);
					android.util.Log.i("XP3Extract", "listEntries returned " + entries.size());

					if (entries.isEmpty()) {
						mError = "No files found in archive";
						return false;
					}

					File outDir = new File(xp3File.getParent(),
						xp3File.getName().replaceAll("(?i)\\.xp[34]$", "") + "_unpacked");

					XP3Extractor.extract(xp3File, outDir, entries, (fileName, cur, total) -> {
						publishProgress(cur, total);
					});
					return true;
				} catch (Throwable t) {
					mError = t.getMessage();
					if (mError == null) mError = t.toString();
					android.util.Log.e("XP3Extract", "extract failed", t);
					return false;
				}
			}
			@Override
			protected void onProgressUpdate(Integer... v) {
				pd.setProgress(v[0]);
				pd.setMax(v[1]);
			}
			@Override
			protected void onPostExecute(Boolean ok) {
				pd.dismiss();
				if (ok) {
					Toast.makeText(LauncherActivity.this, "Extracted to " +
						xp3File.getName().replaceAll("(?i)\\.xp[34]$", "") + "_unpacked",
						Toast.LENGTH_LONG).show();
					loadDir(mCurrentDir);
				} else {
					Toast.makeText(LauncherActivity.this,
						"Extract failed: " + (mError != null ? mError : "unknown"),
						Toast.LENGTH_LONG).show();
				}
			}
		}.execute();
	}

	//--------------------------------------------------------------------------
	// Read engine config from GlobalPreference.xml
	//--------------------------------------------------------------------------
	private boolean getBoolFromPref(String key, boolean def) {
		try {
			File base = getExternalFilesDir(null);
			if (base == null) return def;
			File f = new File(base, ".preference/GlobalPreference.xml");
			if (!f.exists()) return def;
			FileInputStream is = new FileInputStream(f);
			XmlPullParser parser = XmlPullParserFactory.newInstance().newPullParser();
			parser.setInput(is, "UTF-8");
			int event;
			while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
				if (event == XmlPullParser.START_TAG && "Item".equals(parser.getName())) {
					String k = parser.getAttributeValue(null, "key");
					String v = parser.getAttributeValue(null, "value");
					if (key.equals(k))
						return v.equals("1") || v.equals("true");
				}
			}
			is.close();
		} catch (Exception ignored) {}
		return def;
	}

	//--------------------------------------------------------------------------
	// File I/O utilities
	//--------------------------------------------------------------------------

	private static boolean deleteRecursive(File f) {
		if (f.isDirectory()) {
			File[] children = f.listFiles();
			if (children != null)
				for (File c : children) deleteRecursive(c);
		}
		return f.delete();
	}

	private static void copyFileRecursive(File src, File dst) throws Exception {
		if (src.isDirectory()) {
			dst.mkdirs();
			File[] children = src.listFiles();
			if (children != null)
				for (File c : children)
					copyFileRecursive(c, new File(dst, c.getName()));
		} else {
			dst.getParentFile().mkdirs();
			try (FileInputStream fis = new FileInputStream(src);
				 FileOutputStream fos = new FileOutputStream(dst);
				 FileChannel in = fis.getChannel();
				 FileChannel out = fos.getChannel()) {
				in.transferTo(0, in.size(), out);
			}
		}
	}

	//--------------------------------------------------------------------------
	// About dialog
	//--------------------------------------------------------------------------

	private void showAboutDialog() {
		String ver;
		try {
			ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
		} catch (Exception e) {
			ver = "?";
		}
		new AlertDialog.Builder(this)
			.setTitle("Kirikiroid2-Yuri")
			.setMessage("Version: " + ver + "\n\nSDL3 + Vulkan rendering backend\nNative Android launcher")
			.setPositiveButton("OK", null)
			.show();
	}

	//--------------------------------------------------------------------------
	// Lifecycle
	//--------------------------------------------------------------------------

	@Override
	protected void onResume() {
		super.onResume();
		loadRecent();
		refreshRecentCard();
		mRecentAdapter.setPaths(mRecentPaths);
		if (mCurrentDir != null) loadDir(mCurrentDir);
	}

	@Override
	public void onBackPressed() {
		if (mCurrentDir != null) {
			File parent = mCurrentDir.getParentFile();
			if (parent != null && parent.canRead()) {
				loadDir(parent);
				return;
			}
		}
		super.onBackPressed();
	}

	//--------------------------------------------------------------------------
	// Recent game history
	//--------------------------------------------------------------------------

	private File getRecentFile() {
		File base = getExternalFilesDir(null);
		if (base == null) return null;
		return new File(base, ".preference/recentpath.xml");
	}

	private void loadRecent() {
		mRecentPaths.clear();
		try {
			File f = getRecentFile();
			if (f == null || !f.exists()) return;
			InputStream is = new FileInputStream(f);
			XmlPullParser parser = XmlPullParserFactory.newInstance().newPullParser();
			parser.setInput(is, "UTF-8");
			int event;
			while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
				if (event == XmlPullParser.START_TAG && "Item".equals(parser.getName())) {
					String path = parser.getAttributeValue(null, "Path");
					if (path != null && !path.isEmpty())
						mRecentPaths.add(path);
				}
			}
			is.close();
		} catch (Exception e) {
			mRecentPaths.clear();
		}
	}

	private void saveRecent() {
		try {
			File f = getRecentFile();
			if (f == null) return;
			f.getParentFile().mkdirs();
			FileOutputStream os = new FileOutputStream(f);
			os.write("<?xml version=\"1.0\"?>\n<RecentPathList>\n".getBytes("UTF-8"));
			for (String path : mRecentPaths) {
				String escaped = path.replace("&", "&amp;")
					.replace("<", "&lt;").replace("\"", "&quot;");
				os.write(("  <Item Path=\"" + escaped + "\"/>\n").getBytes("UTF-8"));
			}
			os.write("</RecentPathList>\n".getBytes("UTF-8"));
			os.close();
		} catch (Exception e) {
			// ignore
		}
	}

	private void addRecent(String path) {
		mRecentPaths.remove(path);
		mRecentPaths.add(0, path);
		if (mRecentPaths.size() > MAX_RECENT)
			mRecentPaths.remove(mRecentPaths.size() - 1);
		mRecentAdapter.setPaths(mRecentPaths);
		refreshRecentCard();
		saveRecent();
	}

	private void refreshRecentCard() {
		mRecentCard.setVisibility(mRecentPaths.isEmpty() ? View.GONE : View.VISIBLE);
	}

	//--------------------------------------------------------------------------
	// Directory loading
	//--------------------------------------------------------------------------

	private class LoadTask extends AsyncTask<File, Void, List<FileEntry>> {
		@Override
		protected List<FileEntry> doInBackground(File... dirs) {
			File dir = dirs[0];
			android.util.Log.i("Launcher", "LoadTask start: " + dir.getAbsolutePath());
			long t0 = System.currentTimeMillis();
			List<FileEntry> result = new ArrayList<>();
			File[] children = dir.listFiles();
			if (children == null) {
				android.util.Log.w("Launcher", "LoadTask: listFiles returned null");
				return result;
			}
			android.util.Log.i("Launcher", "LoadTask: " + children.length + " entries");

			int count = 0, gameCount = 0;
			for (File f : children) {
				String name = f.getName();
				if (name.startsWith(".")) continue;

				FileEntry e = new FileEntry();
				e.name = name;
				e.fullPath = f.getAbsolutePath();
				e.isDirectory = f.isDirectory();
				e.lastModified = f.lastModified();

				if (e.isDirectory) {
					e.isGame = f.canRead() && new File(f, "startup.tjs").exists();
				} else if (isVideoFile(name)) {
					e.isVideo = true;
				} else if (isBootableFile(f)) {
					String lower = name.toLowerCase();
					if (lower.endsWith(".xp3") || lower.endsWith(".xp4")) {
						e.isGame = XP3Extractor.hasStartupScript(f);
					} else {
						e.isGame = true; // exe — trust header check
					}
				}
				if (e.isGame) gameCount++;
				result.add(e);
				count++;
			}

			Collections.sort(result, (a, b) -> {
				if (a.isDirectory != b.isDirectory)
					return a.isDirectory ? -1 : 1;
				return a.name.compareToIgnoreCase(b.name);
			});
			long dt = System.currentTimeMillis() - t0;
			android.util.Log.i("Launcher", "LoadTask done: " + count + " items (" + gameCount + " games) in " + dt + "ms");
			return result;
		}

		@Override
		protected void onPostExecute(List<FileEntry> result) {
			mFiles = result;
			mAdapter.setFiles(result);
		}
	}

	/** Check if a file is a bootable archive (XP3, or EXE with embedded XP3). */
	private static final String[] VIDEO_EXTS = {".mp4", ".avi", ".mkv", ".wmv", ".flv", ".mov", ".webm", ".m4v", ".mpg", ".mpeg"};

	private static boolean isVideoFile(String name) {
		String lower = name.toLowerCase();
		for (String ext : VIDEO_EXTS)
			if (lower.endsWith(ext)) return true;
		return false;
	}

	private static boolean isBootableFile(File f) {
		String name = f.getName().toLowerCase();
		if (name.endsWith(".xp3") || name.endsWith(".xp4")) return true;

		// Check magic bytes for EXE with embedded XP3
		try (java.io.RandomAccessFile raf = new java.io.RandomAccessFile(f, "r")) {
			byte[] magic = { 'X', 'P', '3', 0x0D, 0x0A, 0x20, 0x0A, 0x1A, (byte)0x8B, (byte)0x67, 0x01 };
			byte[] header = new byte[11];
			if (raf.read(header) < 11) return false;

			// Check for plain XP3
			boolean match = true;
			for (int i = 0; i < 11; i++)
				if (header[i] != magic[i]) { match = false; break; }
			if (match) return true;

			// Check for MZ (exe) — scan at 16-byte alignment for XP3 magic
			if (header[0] == 'M' && header[1] == 'Z') {
				byte[] buf = new byte[256 * 1024];
				long fileLen = f.length();
				long limit = Math.min(fileLen, 50L * 1024 * 1024);
				for (long base = 16; base < limit; base += buf.length) {
					int remaining = (int) Math.min(buf.length, limit - base);
					raf.seek(base);
					raf.readFully(buf, 0, remaining);
					for (int i = 0; i <= remaining - 11; i += 16) {
						boolean found = true;
						for (int j = 0; j < 11; j++)
							if (buf[i + j] != magic[j]) { found = false; break; }
						if (found) return true;
					}
				}
			}
		} catch (Exception ignored) {}
		return false;
	}
}
