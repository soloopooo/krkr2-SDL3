package com.yuri.kirikiri2;

import android.os.Bundle;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.button.MaterialButton;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class SettingsActivity extends AppCompatActivity {
	private Map<String, String> mPrefs = new HashMap<>();
	private File mPrefFile;

	private Switch mSetShowFps, mSetOutputLog, mSetKeepScreen, mSetHideSysBtn, mSetRemLastPath, mSetForceDefFont;
	private MaterialButton mBtnFps, mBtnRenderer, mBtnMemUsage, mBtnDrawThreads, mBtnTexCompress, mBtnDefFont, mBtnFontScale;
	private SeekBar mCursorBar;
	private TextView mCursorVal;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_settings);

		findViewById(R.id.btnBack).setOnClickListener(v -> finish());

		mPrefFile = getPrefFile();

		mSetShowFps = findViewById(R.id.setShowFps);
		mSetOutputLog = findViewById(R.id.setOutputLog);
		mSetKeepScreen = findViewById(R.id.setKeepScreen);
		mSetHideSysBtn = findViewById(R.id.setHideSysBtn);
		mSetRemLastPath = findViewById(R.id.setRemLastPath);
		mSetForceDefFont = findViewById(R.id.setForceDefFont);
		mBtnFps = findViewById(R.id.setFpsLimit);
		mBtnRenderer = findViewById(R.id.setRenderer);
		mBtnMemUsage = findViewById(R.id.setMemUsage);
		mBtnDrawThreads = findViewById(R.id.setDrawThreads);
		mBtnTexCompress = findViewById(R.id.setTexCompress);
		mBtnDefFont = findViewById(R.id.setDefaultFont);
		mBtnFontScale = findViewById(R.id.setFontScale);
		mCursorBar = findViewById(R.id.setCursorScale);
		mCursorVal = findViewById(R.id.setCursorVal);

		loadPrefs();

		mCursorBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
			@Override public void onProgressChanged(SeekBar bar, int p, boolean user) {
				if (!user) return;
				float val = p / 20f;
				mCursorVal.setText(String.format("%.0f%%", val * 100));
				save("vcursor_scale", String.format("%.2f", val));
			}
			@Override public void onStartTrackingTouch(SeekBar bar) {}
			@Override public void onStopTrackingTouch(SeekBar bar) {}
		});

		mSetShowFps.setOnCheckedChangeListener((b, v) -> save("showfps", v));
		mSetOutputLog.setOnCheckedChangeListener((b, v) -> save("outputlog", v));
		mSetKeepScreen.setOnCheckedChangeListener((b, v) -> save("keep_screen_alive", v));
		mSetHideSysBtn.setOnCheckedChangeListener((b, v) -> save("hide_android_sys_btn", v));
		mSetRemLastPath.setOnCheckedChangeListener((b, v) -> save("remember_last_path", v));
		mSetForceDefFont.setOnCheckedChangeListener((b, v) -> save("force_default_font", v));

		mBtnFps.setOnClickListener(v -> {
			String[] items = {"120", "90", "75", "60", "45", "30", "15"};
			new AlertDialog.Builder(this)
				.setTitle("FPS Limit")
				.setItems(items, (d, i) -> {
					mBtnFps.setText(items[i]);
					save("fps_limit", items[i]);
				})
				.show();
		});

		mBtnRenderer.setOnClickListener(v -> {
			String[] items = {"software", "vulkan"};
			String[] labels = {"Software", "Vulkan"};
			new AlertDialog.Builder(this)
				.setTitle("Renderer")
				.setItems(labels, (d, i) -> {
					mBtnRenderer.setText(labels[i]);
					save("renderer", items[i]);
				})
				.show();
		});

		mBtnMemUsage.setOnClickListener(v -> {
			String[] items = {"unlimited", "high", "medium", "low"};
			String[] labels = {"Unlimited", "High", "Medium", "Low"};
			new AlertDialog.Builder(this)
				.setTitle("Memory Usage")
				.setItems(labels, (d, i) -> {
					mBtnMemUsage.setText(labels[i]);
					save("memusage", items[i]);
				})
				.show();
		});

		mBtnDrawThreads.setOnClickListener(v -> {
			String[] items = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};
			String[] labels = {"Auto", "1", "2", "3", "4", "5", "6", "7", "8"};
			new AlertDialog.Builder(this)
				.setTitle("Draw Thread Count")
				.setItems(labels, (d, i) -> {
					mBtnDrawThreads.setText(labels[i]);
					save("software_draw_thread", items[i]);
				})
				.show();
		});

		mBtnTexCompress.setOnClickListener(v -> {
			String[] items = {"none", "halfline", "lz4", "lz4+tlg5"};
			String[] labels = {"None", "Half Line", "LZ4", "LZ4+TLG5"};
			new AlertDialog.Builder(this)
				.setTitle("Texture Compression")
				.setItems(labels, (d, i) -> {
					mBtnTexCompress.setText(labels[i]);
					save("software_compress_tex", items[i]);
				})
				.show();
		});

		mBtnDefFont.setOnClickListener(v -> showFontPicker());

		mBtnFontScale.setOnClickListener(v -> {
			String[] items = {"0.5", "0.75", "1.0", "1.25", "1.5", "1.75", "2.0"};
			String[] labels = {"50%", "75%", "100%", "125%", "150%", "175%", "200%"};
			new AlertDialog.Builder(this)
				.setTitle("Font Scale")
				.setItems(labels, (d, i) -> {
					mBtnFontScale.setText(labels[i]);
					save("font_scale", items[i]);
				})
				.show();
		});
	}

	private void showFontPicker() {
		// Scan system font directories
		List<String> fontPaths = new ArrayList<>();
		List<String> fontNames = new ArrayList<>();
		String[][] fontDirs = {{"/system/fonts"}, {"/system/fonts/"}};
		scanFonts("/system/fonts", fontPaths, fontNames);

		// Also include bundled fallback font
		File internal = getExternalFilesDir(null);
		if (internal != null) {
			File fallback = new File(internal, "DroidSansFallback.ttf");
			if (fallback.exists()) {
				fontPaths.add(fallback.getAbsolutePath());
				fontNames.add("DroidSansFallback (bundled)");
			}
		}

		// Build dialog: "Auto (system default)" + sorted fonts
		final String[] allPaths = new String[fontPaths.size() + 1];
		final String[] allLabels = new String[fontNames.size() + 1];
		allPaths[0] = "";
		allLabels[0] = "Auto (system default)";

		for (int i = 0; i < fontPaths.size(); i++) {
			allPaths[i + 1] = fontPaths.get(i);
			// Use just the filename for display
			String name = fontNames.get(i);
			allLabels[i + 1] = name;
		}

		new AlertDialog.Builder(this)
			.setTitle("Default Font")
			.setItems(allLabels, (d, which) -> {
				String path = allPaths[which];
				String label = allLabels[which];
				mBtnDefFont.setText(path.isEmpty() ? "Auto" : label);
				save("default_font", path);
			})
			.show();
	}

	private void scanFonts(String dirPath, List<String> paths, List<String> names) {
		File dir = new File(dirPath);
		if (!dir.isDirectory()) return;
		File[] files = dir.listFiles();
		if (files == null) return;
		for (File f : files) {
			String n = f.getName().toLowerCase();
			if (f.isFile() && (n.endsWith(".ttf") || n.endsWith(".ttc") || n.endsWith(".otf"))) {
				paths.add(f.getAbsolutePath());
				names.add(f.getName());
			}
		}
		// Sort alphabetically
		ArrayList<Integer> indices = new ArrayList<>();
		for (int i = 0; i < names.size(); i++) indices.add(i);
		Collections.sort(indices, (a, b) -> names.get(a).compareToIgnoreCase(names.get(b)));
		ArrayList<String> sortedPaths = new ArrayList<>();
		ArrayList<String> sortedNames = new ArrayList<>();
		for (int i : indices) {
			sortedPaths.add(paths.get(i));
			sortedNames.add(names.get(i));
		}
		paths.clear(); paths.addAll(sortedPaths);
		names.clear(); names.addAll(sortedNames);
	}

	private File getPrefFile() {
		File base = getExternalFilesDir(null);
		if (base == null) return null;
		return new File(base, ".preference/GlobalPreference.xml");
	}

	private void loadPrefs() {
		mPrefs.clear();
		try {
			File f = mPrefFile;
			if (f == null || !f.exists()) return;
			FileInputStream is = new FileInputStream(f);
			XmlPullParser parser = XmlPullParserFactory.newInstance().newPullParser();
			parser.setInput(is, "UTF-8");
			int event;
			while ((event = parser.next()) != XmlPullParser.END_DOCUMENT) {
				if (event == XmlPullParser.START_TAG && "Item".equals(parser.getName())) {
					String key = parser.getAttributeValue(null, "key");
					String val = parser.getAttributeValue(null, "value");
					if (key != null && val != null) mPrefs.put(key, val);
				}
			}
			is.close();
		} catch (Exception ignored) {}

		mSetShowFps.setChecked(getBool("showfps", false));
		mSetOutputLog.setChecked(getBool("outputlog", true));
		mSetKeepScreen.setChecked(getBool("keep_screen_alive", true));
		mSetHideSysBtn.setChecked(getBool("hide_android_sys_btn", false));
		mSetRemLastPath.setChecked(getBool("remember_last_path", true));
		mSetForceDefFont.setChecked(getBool("force_default_font", false));
		mBtnFps.setText(getStr("fps_limit", "60"));

		String ren = getStr("renderer", "software");
		mBtnRenderer.setText(ren.equals("vulkan") ? "Vulkan" : "Software");

		String[] memNames = {"unlimited", "high", "medium", "low"};
		String[] memLabels = {"Unlimited", "High", "Medium", "Low"};
		String mem = getStr("memusage", "unlimited");
		mBtnMemUsage.setText(pickLabel(mem, memNames, memLabels));

		String[] thrNames = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};
		String[] thrLabels = {"Auto", "1", "2", "3", "4", "5", "6", "7", "8"};
		String thr = getStr("software_draw_thread", "0");
		mBtnDrawThreads.setText(pickLabel(thr, thrNames, thrLabels));

		String[] texNames = {"none", "halfline", "lz4", "lz4+tlg5"};
		String[] texLabels = {"None", "Half Line", "LZ4", "LZ4+TLG5"};
		mBtnTexCompress.setText(pickLabel(getStr("software_compress_tex", "none"), texNames, texLabels));

		String[] fsItems = {"0.5", "0.75", "1.0", "1.25", "1.5", "1.75", "2.0"};
		String[] fsLabels = {"50%", "75%", "100%", "125%", "150%", "175%", "200%"};
		mBtnFontScale.setText(pickLabel(getStr("font_scale", "1.0"), fsItems, fsLabels));

		String font = getStr("default_font", "");
		if (font.isEmpty()) mBtnDefFont.setText("Auto");
		else mBtnDefFont.setText(new File(font).getName());

		float cursor = Float.parseFloat(getStr("vcursor_scale", "0.5"));
		mCursorBar.setProgress(Math.round(cursor * 20));
		mCursorVal.setText(String.format("%.0f%%", cursor * 100));
	}

	private static String pickLabel(String value, String[] values, String[] labels) {
		for (int i = 0; i < values.length; i++)
			if (values[i].equals(value)) return labels[i];
		return labels[0];
	}

	private boolean getBool(String key, boolean def) {
		String v = mPrefs.get(key);
		return v != null ? v.equals("1") || v.equals("true") : def;
	}

	private String getStr(String key, String def) {
		return mPrefs.getOrDefault(key, def);
	}

	private void save(String key, boolean val) {
		save(key, val ? "1" : "0");
	}

	private void save(String key, String val) {
		mPrefs.put(key, val);
		writePrefs();
	}

	private void writePrefs() {
		try {
			File f = mPrefFile;
			if (f == null) return;
			f.getParentFile().mkdirs();
			FileOutputStream os = new FileOutputStream(f);
			os.write("<?xml version=\"1.0\"?>\n<GlobalPreference>\n".getBytes("UTF-8"));
			for (Map.Entry<String, String> e : mPrefs.entrySet()) {
				String k = e.getKey().replace("&", "&amp;").replace("<", "&lt;").replace("\"", "&quot;");
				String v = e.getValue().replace("&", "&amp;").replace("<", "&lt;").replace("\"", "&quot;");
				os.write(("  <Item key=\"" + k + "\" value=\"" + v + "\"/>\n").getBytes("UTF-8"));
			}
			os.write("</GlobalPreference>\n".getBytes("UTF-8"));
			os.close();
		} catch (Exception ignored) {}
	}
}
