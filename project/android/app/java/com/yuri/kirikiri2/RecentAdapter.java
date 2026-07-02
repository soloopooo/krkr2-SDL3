package com.yuri.kirikiri2;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import androidx.recyclerview.widget.RecyclerView;

import java.io.File;
import java.util.List;

class RecentAdapter extends RecyclerView.Adapter<RecentAdapter.ViewHolder> {
	private List<String> mPaths;
	private OnRecentClickListener mListener;

	interface OnRecentClickListener {
		void onPlay(String path);
		void onFolder(String path);
		void onDelete(String path);
	}

	RecentAdapter(List<String> paths, OnRecentClickListener listener) {
		mPaths = paths;
		mListener = listener;
	}

	void setPaths(List<String> paths) {
		mPaths = paths;
		notifyDataSetChanged();
	}

	@Override
	public ViewHolder onCreateViewHolder(ViewGroup parent, int viewType) {
		View v = LayoutInflater.from(parent.getContext())
			.inflate(R.layout.item_recent, parent, false);
		return new ViewHolder(v);
	}

	@Override
	public void onBindViewHolder(ViewHolder h, int i) {
		String path = mPaths.get(i);
		String gameName = extractGameName(path);
		h.name.setText(gameName);
		h.path.setText(path);
		h.playBtn.setOnClickListener(v -> mListener.onPlay(path));
		h.folderBtn.setOnClickListener(v -> mListener.onFolder(path));
		h.deleteBtn.setOnClickListener(v -> mListener.onDelete(path));
	}

	@Override
	public int getItemCount() {
		return mPaths != null ? mPaths.size() : 0;
	}

	static String extractGameName(String path) {
		String name = new File(path).getName();
		if (name.equalsIgnoreCase("startup.tjs")) {
			return new File(path).getParentFile().getName();
		}
		if (name.toLowerCase().endsWith(".xp3") || name.toLowerCase().endsWith(".xp4")) {
			return name.substring(0, name.lastIndexOf('.'));
		}
		return name;
	}

	static class ViewHolder extends RecyclerView.ViewHolder {
		TextView name, path, playBtn, folderBtn, deleteBtn;

		ViewHolder(View v) {
			super(v);
			name = v.findViewById(R.id.recentName);
			path = v.findViewById(R.id.recentPath);
			playBtn = v.findViewById(R.id.recentPlay);
			folderBtn = v.findViewById(R.id.recentFolder);
			deleteBtn = v.findViewById(R.id.recentDelete);
		}
	}
}
