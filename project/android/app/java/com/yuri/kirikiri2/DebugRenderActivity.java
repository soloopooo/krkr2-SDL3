package com.yuri.kirikiri2;

import org.tvp.kirikiri2.KR2Activity;

/**
 * Debug / RenderDoc-friendly activity.
 *
 * Identical to MainActivity but declared with an intent-filter so that
 * RenderDoc and other GPU debugging tools can discover and launch it.
 *
 * RenderDoc usage:
 *   adb shell am start -n com.yuri.kirikiri2/.DebugRenderActivity \
 *     --es startupPath /sdcard/path/to/game.xp3
 *
 * Once the activity is running, RenderDoc's "Inject into Process" tab
 * will show "com.yuri.kirikiri2.debug_render" — attach and capture.
 *
 * Does NOT appear in the system launcher (no CATEGORY_LAUNCHER).
 */
public class DebugRenderActivity extends KR2Activity {
	@Override
	public int get_res_sd_operate_step() { return R.drawable.sd_operate_step; }
}
