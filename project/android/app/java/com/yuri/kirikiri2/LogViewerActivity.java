package com.yuri.kirikiri2;

import android.os.Bundle;
import android.widget.TextView;

import androidx.appcompat.app.AppCompatActivity;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.tvp.kirikiri2.KR2Activity;

import java.util.ArrayList;

public class LogViewerActivity extends AppCompatActivity {
	private ArrayList<String> mLines = new ArrayList<>();
	private LogAdapter mAdapter;

	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);
		setContentView(R.layout.activity_logviewer);

		findViewById(R.id.btnBack).setOnClickListener(v -> finish());
		findViewById(R.id.btnRefresh).setOnClickListener(v -> loadLogs());
		findViewById(R.id.btnClear).setOnClickListener(v -> {
			mLines.clear();
			mAdapter.notifyDataSetChanged();
		});

		mAdapter = new LogAdapter(mLines);
		RecyclerView list = findViewById(R.id.logList);
		list.setLayoutManager(new LinearLayoutManager(this));
		list.setAdapter(mAdapter);

		loadLogs();
	}

	private void loadLogs() {
		String[] raw = KR2Activity.getEngineLogs();
		mLines.clear();
		for (String s : raw) mLines.add(s);
		mAdapter.notifyDataSetChanged();
		if (!mLines.isEmpty())
			findViewById(R.id.logList).post(() ->
				((RecyclerView)findViewById(R.id.logList)).scrollToPosition(mLines.size() - 1));
	}

	static class LogAdapter extends RecyclerView.Adapter<LogAdapter.VH> {
		private ArrayList<String> mLines;
		LogAdapter(ArrayList<String> lines) { mLines = lines; }
		@Override public VH onCreateViewHolder(android.view.ViewGroup parent, int type) {
			TextView tv = new TextView(parent.getContext());
			tv.setTextSize(10);
			tv.setTypeface(android.graphics.Typeface.MONOSPACE);
			tv.setPadding(8, 2, 8, 2);
			tv.setTextColor(0xFFCCCCCC);
			return new VH(tv);
		}
		@Override public void onBindViewHolder(VH h, int i) {
			h.tv.setText(mLines.get(i));
		}
		@Override public int getItemCount() { return mLines.size(); }
		static class VH extends RecyclerView.ViewHolder {
			TextView tv;
			VH(TextView v) { super(v); tv = v; }
		}
	}
}
