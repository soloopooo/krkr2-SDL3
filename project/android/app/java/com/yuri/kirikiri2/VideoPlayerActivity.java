package com.yuri.kirikiri2;

import android.content.Intent;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Bundle;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;

import java.io.File;

public class VideoPlayerActivity extends AppCompatActivity {
	private MediaPlayer mPlayer;
	private SurfaceView mSurface;
	private SeekBar mSeek;
	private TextView mTime;
	private boolean mDragging;
	private Runnable mTick;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_videoplayer);

		String path = getIntent().getStringExtra("videoPath");
		if (path == null) { finish(); return; }

		mSurface = findViewById(R.id.videoSurface);
		mSeek = findViewById(R.id.videoSeek);
		mTime = findViewById(R.id.videoTime);

		findViewById(R.id.btnPlay).setOnClickListener(v -> togglePlay());
		findViewById(R.id.btnBack).setOnClickListener(v -> { stopPlayer(); finish(); });

		mSeek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
			@Override public void onProgressChanged(SeekBar b, int p, boolean u) {
				if (u && mPlayer != null) mPlayer.seekTo(p);
			}
			@Override public void onStartTrackingTouch(SeekBar b) { mDragging = true; }
			@Override public void onStopTrackingTouch(SeekBar b) { mDragging = false; }
		});

		mSurface.getHolder().addCallback(new SurfaceHolder.Callback() {
			@Override public void surfaceCreated(SurfaceHolder h) { startPlayer(path, h); }
			@Override public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {}
			@Override public void surfaceDestroyed(SurfaceHolder h) { stopPlayer(); }
		});
	}

	private void startPlayer(String path, SurfaceHolder holder) {
		stopPlayer();
		mPlayer = new MediaPlayer();
		try {
			mPlayer.setDataSource(this, Uri.fromFile(new File(path)));
			mPlayer.setDisplay(holder);
			mPlayer.setOnPreparedListener(mp -> {
				mSeek.setMax(mp.getDuration());
				mp.start();
				runTick();
			});
			mPlayer.setOnCompletionListener(mp -> {
				findViewById(R.id.btnPlay).setSelected(false);
			});
			mPlayer.setOnErrorListener((mp, what, extra) -> true);
			mPlayer.prepareAsync();
		} catch (Exception e) {
			stopPlayer();
			finish();
		}
	}

	private void stopPlayer() {
		if (mTick != null) { mSeek.removeCallbacks(mTick); mTick = null; }
		if (mPlayer != null) { try { mPlayer.stop(); mPlayer.release(); } catch (Exception ignored) {} mPlayer = null; }
	}

	private void togglePlay() {
		if (mPlayer == null) return;
		if (mPlayer.isPlaying()) { mPlayer.pause(); findViewById(R.id.btnPlay).setSelected(false); }
		else { mPlayer.start(); findViewById(R.id.btnPlay).setSelected(true); runTick(); }
	}

	private void runTick() {
		if (mTick != null) mSeek.removeCallbacks(mTick);
		mTick = () -> {
			if (mPlayer != null && !mDragging) {
				int pos = mPlayer.getCurrentPosition();
				mSeek.setProgress(pos);
				mTime.setText(String.format("%02d:%02d/%02d:%02d",
					pos / 60000, (pos / 1000) % 60,
					mSeek.getMax() / 60000, (mSeek.getMax() / 1000) % 60));
			}
			mSeek.postDelayed(mTick, 250);
		};
		mSeek.post(mTick);
	}

	@Override protected void onDestroy() { stopPlayer(); super.onDestroy(); }
}
