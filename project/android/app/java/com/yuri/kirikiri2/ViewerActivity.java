package com.yuri.kirikiri2;

import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.graphics.PointF;
import android.os.Bundle;
import android.view.MotionEvent;
import android.view.View;
import android.widget.ImageView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.appbar.MaterialToolbar;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.nio.charset.Charset;

public class ViewerActivity extends AppCompatActivity {
	private static final String TAG = "Viewer";

	private MaterialToolbar mToolbar;
	private ScrollView mTextScroll;
	private TextView mTextView;
	private ImageView mImageView;

	private String mFilePath;
	private String mFileName;

	// Pinch-to-zoom state
	private Matrix mMatrix = new Matrix();
	private Matrix mSavedMatrix = new Matrix();
	private PointF mStart = new PointF();
	private float mOldDist;
	private static final int NONE = 0, DRAG = 1, ZOOM = 2;
	private int mMode = NONE;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_viewer);

		mFilePath = getIntent().getStringExtra("filePath");
		if (mFilePath == null) { finish(); return; }
		mFileName = new File(mFilePath).getName();

		mToolbar = findViewById(R.id.viewerToolbar);
		mToolbar.setTitle(mFileName);
		mToolbar.setNavigationOnClickListener(v -> finish());

		mTextScroll = findViewById(R.id.textScroll);
		mTextView = findViewById(R.id.viewerText);
		mImageView = findViewById(R.id.viewerImage);

		String lower = mFileName.toLowerCase();
		if (isImageFile(lower)) {
			showImage();
		} else if (isTextFile(lower)) {
			showText();
		} else {
			Toast.makeText(this, "Unsupported file type", Toast.LENGTH_SHORT).show();
			finish();
		}
	}

	private boolean isImageFile(String lower) {
		return lower.endsWith(".png") || lower.endsWith(".jpg") || lower.endsWith(".jpeg")
			|| lower.endsWith(".gif") || lower.endsWith(".bmp") || lower.endsWith(".webp")
			|| lower.endsWith(".tlg");
	}

	private boolean isTextFile(String lower) {
		return lower.endsWith(".txt") || lower.endsWith(".tjs") || lower.endsWith(".json")
			|| lower.endsWith(".xml") || lower.endsWith(".log") || lower.endsWith(".csv")
			|| lower.endsWith(".ini") || lower.endsWith(".cfg") || lower.endsWith(".conf")
			|| lower.endsWith(".ks") || lower.endsWith(".tjs") || lower.endsWith(".bat")
			|| lower.endsWith(".sh") || lower.endsWith(".yml") || lower.endsWith(".yaml")
			|| lower.endsWith(".md") || lower.endsWith(".html") || lower.endsWith(".css")
			|| lower.endsWith(".js") || lower.endsWith(".py") || lower.endsWith(".lua")
			|| lower.endsWith(".asd") || lower.endsWith(".scn");
	}

	private void showImage() {
		mImageView.setVisibility(View.VISIBLE);
		Bitmap bmp = null;
		try {
			if (mFileName.toLowerCase().endsWith(".tlg")) {
				byte[] raw = readAllBytes(mFilePath);
				try {
					bmp = TLGDecoder.decode(raw);
				} catch (Exception e) {
					android.util.Log.w("Viewer", "TLGDecoder failed, trying BitmapFactory", e);
					bmp = BitmapFactory.decodeByteArray(raw, 0, raw.length);
				}
			} else {
				// Decode with subsampling for large images
				BitmapFactory.Options bounds = new BitmapFactory.Options();
				bounds.inJustDecodeBounds = true;
				BitmapFactory.decodeFile(mFilePath, bounds);
				int imgW = bounds.outWidth, imgH = bounds.outHeight;
				if (imgW <= 0 || imgH <= 0) throw new Exception("Can't decode image");

				int reqW = getResources().getDisplayMetrics().widthPixels;
				int reqH = getResources().getDisplayMetrics().heightPixels;
				int sample = 1;
				while (imgW / sample > reqW * 2 || imgH / sample > reqH * 2) sample *= 2;
				BitmapFactory.Options opts = new BitmapFactory.Options();
				opts.inSampleSize = sample;
				opts.inPreferredConfig = Bitmap.Config.RGB_565;
				bmp = BitmapFactory.decodeFile(mFilePath, opts);
			}
			if (bmp == null) throw new Exception("Failed to decode image");
			mImageView.setImageBitmap(bmp);
			mImageView.setScaleType(ImageView.ScaleType.FIT_CENTER);

			// Pinch-to-zoom
			mImageView.setOnTouchListener((v, event) -> {
				switch (event.getAction() & MotionEvent.ACTION_MASK) {
					case MotionEvent.ACTION_DOWN:
						mMatrix.set(mImageView.getImageMatrix());
						mSavedMatrix.set(mMatrix);
						mStart.set(event.getX(), event.getY());
						mMode = DRAG;
						break;
					case MotionEvent.ACTION_POINTER_DOWN:
						mOldDist = spacing(event);
						if (mOldDist > 10f) {
							mSavedMatrix.set(mMatrix);
							mMode = ZOOM;
						}
						break;
					case MotionEvent.ACTION_MOVE:
						if (mMode == DRAG) {
							mMatrix.set(mSavedMatrix);
							mMatrix.postTranslate(event.getX() - mStart.x, event.getY() - mStart.y);
						} else if (mMode == ZOOM) {
							float newDist = spacing(event);
							if (newDist > 10f) {
								mMatrix.set(mSavedMatrix);
								float scale = newDist / mOldDist;
								mMatrix.postScale(scale, scale, midX(event), midY(event));
							}
						}
						break;
					case MotionEvent.ACTION_UP:
					case MotionEvent.ACTION_POINTER_UP:
						mMode = NONE;
						break;
				}
				mImageView.setImageMatrix(mMatrix);
				return true;
			});
		} catch (Exception e) {
			android.util.Log.e(TAG, "showImage failed", e);
			Toast.makeText(this, "Can't open image: " + e.getMessage(), Toast.LENGTH_LONG).show();
			finish();
		}
	}

	private void showText() {
		mTextScroll.setVisibility(View.VISIBLE);
		try {
			// Detect encoding by checking BOM or byte patterns
			File f = new File(mFilePath);
			long fileSize = f.length();
			if (fileSize > 10 * 1024 * 1024) {
				Toast.makeText(this, "File too large for text viewer", Toast.LENGTH_LONG).show();
				finish();
				return;
			}

			byte[] raw = new byte[(int) fileSize];
			try (InputStream is = new FileInputStream(f)) {
				int n = 0;
				while (n < raw.length) {
					int r = is.read(raw, n, raw.length - n);
					if (r < 0) break;
					n += r;
				}
			}

			String text;
			if (raw.length >= 3 && raw[0] == (byte)0xEF && raw[1] == (byte)0xBB && raw[2] == (byte)0xBF) {
				text = new String(raw, 3, raw.length - 3, Charset.forName("UTF-8"));
			} else if (raw.length >= 2 && raw[0] == (byte)0xFF && raw[1] == (byte)0xFE) {
				text = new String(raw, 2, raw.length - 2, Charset.forName("UTF-16LE"));
			} else if (raw.length >= 2 && raw[0] == (byte)0xFE && raw[1] == (byte)0xFF) {
				text = new String(raw, 2, raw.length - 2, Charset.forName("UTF-16BE"));
			} else {
				// Try UTF-8, fall back to Shift-JIS for legacy Japanese
				text = new String(raw, Charset.forName("UTF-8"));
			}
			mTextView.setText(text);
		} catch (Exception e) {
			android.util.Log.e(TAG, "showText failed", e);
			Toast.makeText(this, "Can't open file: " + e.getMessage(), Toast.LENGTH_LONG).show();
			finish();
		}
	}

	// Pinch-to-zoom helpers
	private float spacing(MotionEvent event) {
		float x = event.getX(0) - event.getX(1);
		float y = event.getY(0) - event.getY(1);
		return (float) Math.sqrt(x * x + y * y);
	}

	private float midX(MotionEvent event) {
		return (event.getX(0) + event.getX(1)) / 2;
	}

	private float midY(MotionEvent event) {
		return (event.getY(0) + event.getY(1)) / 2;
	}

	private byte[] readAllBytes(String path) throws Exception {
		java.io.File f = new java.io.File(path);
		byte[] raw = new byte[(int) f.length()];
		try (java.io.InputStream is = new java.io.FileInputStream(f)) {
			int n = 0;
			while (n < raw.length) {
				int r = is.read(raw, n, raw.length - n);
				if (r < 0) break;
				n += r;
			}
		}
		return raw;
	}
}
