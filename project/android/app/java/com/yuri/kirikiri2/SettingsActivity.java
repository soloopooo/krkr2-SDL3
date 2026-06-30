package com.yuri.kirikiri2;

import android.os.Bundle;
import android.widget.Switch;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.button.MaterialButton;

import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserFactory;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.HashMap;
import java.util.Map;

public class SettingsActivity extends AppCompatActivity {
	private Map<String, String> mPrefs = new HashMap<>();
	private File mPrefFile;

	private Switch mSetShowFps, mSetKeepScreen, mSetHideSysBtn;
	private MaterialButton mBtnFps, mBtnRenderer;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_settings);

		findViewById(R.id.btnBack).setOnClickListener(v -> finish());

		mPrefFile = getPrefFile();

		mSetShowFps = findViewById(R.id.setShowFps);
		mSetKeepScreen = findViewById(R.id.setKeepScreen);
		mSetHideSysBtn = findViewById(R.id.setHideSysBtn);
		mBtnFps = findViewById(R.id.setFpsLimit);
		mBtnRenderer = findViewById(R.id.setRenderer);

		loadPrefs();

		mSetShowFps.setOnCheckedChangeListener((b, v) -> save("showfps", v));
		mSetKeepScreen.setOnCheckedChangeListener((b, v) -> save("keep_screen_alive", v));
		mSetHideSysBtn.setOnCheckedChangeListener((b, v) -> save("hide_android_sys_btn", v));

		mBtnFps.setOnClickListener(v -> {
			String[] items = {"15", "30", "45", "60"};
			new AlertDialog.Builder(this)
				.setTitle("FPS Limit")
				.setItems(items, (d, i) -> {
					String val = items[i];
					mBtnFps.setText(val);
					save("fps_limit", val);
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

		// Apply to UI
		mSetShowFps.setChecked(getBool("showfps", false));
		mSetKeepScreen.setChecked(getBool("keep_screen_alive", true));
		mSetHideSysBtn.setChecked(getBool("hide_android_sys_btn", false));
		mBtnFps.setText(getStr("fps_limit", "60"));
		String ren = getStr("renderer", "software");
		mBtnRenderer.setText(ren.equals("vulkan") ? "Vulkan" : "Software");
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
