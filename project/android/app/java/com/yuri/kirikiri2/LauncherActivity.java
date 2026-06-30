package com.yuri.kirikiri2;

import android.content.Intent;
import android.os.AsyncTask;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.View;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.TextView;

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

		mAdapter = new FileAdapter(mFiles, entry -> {
			if (entry.isDirectory) {
				loadDir(new File(entry.fullPath));
			} else if (entry.isGame) {
				addRecent(entry.fullPath);
				startGame(entry.fullPath);
			}
		});

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
		findViewById(R.id.btnAbout).setOnClickListener(v -> showAboutDialog());

		loadRecent();
		refreshRecentCard();

		// Start at /storage/emulated/0 or fallback to internal
		File defaultDir = new File("/storage/emulated/0");
		if (!defaultDir.isDirectory()) defaultDir = getExternalFilesDir(null);
		loadDir(defaultDir);
	}

	private void loadDir(File dir) {
		if (!dir.isDirectory()) return;
		mCurrentDir = dir;
		updateBreadcrumbs(dir);
		new LoadTask().execute(dir);
	}

	private void updateBreadcrumbs(File dir) {
		mBreadcrumbContainer.removeAllViews();

		// Build path segments
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

		// Friendly names for known storage roots
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

		// Auto-scroll to end (show deepest level)
		mBreadcrumbScroll.post(() -> mBreadcrumbScroll.fullScroll(HorizontalScrollView.FOCUS_RIGHT));
	}

	private void startGame(String path) {
		Intent intent = new Intent(this, MainActivity.class);
		intent.putExtra("startupPath", path);
		startActivity(intent);
	}

	private void showAboutDialog() {
		String ver;
		try {
			ver = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
		} catch (Exception e) {
			ver = "?";
		}
		new androidx.appcompat.app.AlertDialog.Builder(this)
			.setTitle("Kirikiroid2-Yuri")
			.setMessage("Version: " + ver + "\n\nSDL3 + Vulkan rendering backend\nNative Android launcher")
			.setPositiveButton("OK", null)
			.show();
	}

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
			List<FileEntry> result = new ArrayList<>();
			File[] children = dir.listFiles();
			if (children == null) return result;

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
				} else {
					String lower = name.toLowerCase();
					e.isGame = lower.endsWith(".xp3") || lower.endsWith(".xp4");
				}
				result.add(e);
			}

			Collections.sort(result, (a, b) -> {
				if (a.isDirectory != b.isDirectory)
					return a.isDirectory ? -1 : 1;
				return a.name.compareToIgnoreCase(b.name);
			});
			return result;
		}

		@Override
		protected void onPostExecute(List<FileEntry> result) {
			mFiles = result;
			mAdapter.setFiles(result);
		}
	}
}
