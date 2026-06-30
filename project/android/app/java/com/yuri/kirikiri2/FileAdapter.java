package com.yuri.kirikiri2;

import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.TextView;

import androidx.appcompat.widget.AppCompatImageView;
import androidx.recyclerview.widget.RecyclerView;

import java.util.List;

class FileAdapter extends RecyclerView.Adapter<FileAdapter.ViewHolder> {
	private List<FileEntry> mFiles;
	private OnFileClickListener mListener;
	private OnFileLongClickListener mLongListener;

	interface OnFileClickListener {
		void onFileClick(FileEntry entry);
	}

	interface OnFileLongClickListener {
		void onFileLongClick(FileEntry entry, int position);
	}

	FileAdapter(List<FileEntry> files, OnFileClickListener listener,
			OnFileLongClickListener longListener) {
		mFiles = files;
		mListener = listener;
		mLongListener = longListener;
	}

	void setFiles(List<FileEntry> files) {
		mFiles = files;
		notifyDataSetChanged();
	}

	@Override
	public ViewHolder onCreateViewHolder(ViewGroup parent, int viewType) {
		View v = LayoutInflater.from(parent.getContext())
			.inflate(R.layout.item_file, parent, false);
		return new ViewHolder(v);
	}

	@Override
	public void onBindViewHolder(ViewHolder h, int i) {
		FileEntry e = mFiles.get(i);
		if (e.isDirectory) {
			h.icon.setImageResource(R.drawable.ic_folder);
		} else if (e.isVideo) {
			h.icon.setImageResource(R.drawable.ic_play_arrow);
		} else {
			h.icon.setImageResource(e.isGame ? R.drawable.ic_videogame_asset : R.drawable.ic_insert_drive_file);
		}
		h.name.setText(e.name);
		if (e.isDirectory) {
			h.subtitle.setVisibility(View.VISIBLE);
			h.subtitle.setText(e.isGame ? "Contains startup.tjs" : "Directory");
		} else if (e.isGame) {
			h.subtitle.setVisibility(View.VISIBLE);
			h.subtitle.setText("Game archive");
		} else if (e.isVideo) {
			h.subtitle.setVisibility(View.VISIBLE);
			h.subtitle.setText("Video file");
		} else {
			h.subtitle.setVisibility(View.GONE);
		}
		h.badge.setVisibility(e.isGame ? View.VISIBLE : View.GONE);
		h.itemView.setOnClickListener(v -> mListener.onFileClick(e));
		h.itemView.setOnLongClickListener(v -> {
			if (mLongListener != null) {
				mLongListener.onFileLongClick(e, h.getAdapterPosition());
				return true;
			}
			return false;
		});
	}

	@Override
	public int getItemCount() {
		return mFiles != null ? mFiles.size() : 0;
	}

	static class ViewHolder extends RecyclerView.ViewHolder {
		AppCompatImageView icon;
		TextView name, subtitle;
		FrameLayout badge;

		ViewHolder(View v) {
			super(v);
			icon = v.findViewById(R.id.fileIcon);
			name = v.findViewById(R.id.fileName);
			subtitle = v.findViewById(R.id.fileSubtitle);
			badge = v.findViewById(R.id.fileBadge);
		}
	}
}
