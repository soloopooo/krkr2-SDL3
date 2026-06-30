# Android Native Launcher

Replace the cocos2d-x `MainFileSelectorForm` with a native Android launcher activity.

## 1. Architecture

```
LauncherActivity (native UI: file list, recent history, settings)
  ↓ startActivity(MainActivity, extra={"startupPath": gamePath})
MainActivity (= KR2Activity, extends SDLActivity, runs engine)
  ↓ game exits → finish()
LauncherActivity (back to launcher)
```

- `LauncherActivity` is the `<intent-filter>` main/launcher entry.
- `MainActivity` (`KR2Activity`) is started only when a game is selected.
- When the game exits, `MainActivity.finish()` returns to LauncherActivity.
- No GL/engine code runs in LauncherActivity — pure Android UI.

## 2. LauncherActivity Layout

```
┌──────────────────────────────────────┐
│ 顶部：工具条                          │
│  [≡ 菜单]  选择游戏  [设置 ⚙]         │
├──────────────────────────────────────┤
│ 最近游戏 (折叠式标题)                  │
│ ┌──────────────────────────────────┐ │
│ │ 游戏标题 A            [▶][✕]     │ │
│ │ 游戏标题 B            [▶][✕]     │ │
│ └──────────────────────────────────┘ │
│──────────────────────────────────────│
│ 文件浏览器                           │
│ 当前路径: /storage/emulated/0/       │
│ ┌──────────────────────────────────┐ │
│ │ 📁 DCIM/                        │ │
│ │ 📁 Download/         ← 有游戏    │ │
│ │ 📁 Android/                      │ │
│ │ 📦 game.xp3         ← 可启动     │ │
│ │ 📦 another.xp3       ← 可启动     │ │
│ └──────────────────────────────────┘ │
│                                      │
│ 底部：快速访问                         │
│ [下载] [内置存储] [外置SD卡]           │
└──────────────────────────────────────┘
```

## 3. Implementation Steps

### Step 3.1: Create `LauncherActivity.java`

**Package:** `com.yuri.kirikiri2` (same as `MainActivity`)

**Key components:**
- `RecyclerView` for file list (with `FileAdapter`)
- `RecyclerView` for recent games (with `RecentAdapter`)
- `TextView` showing current path
- Shortcut buttons at bottom

**Lifecycle:**
```java
onCreate() → initViews(), loadRecentHistory(), listFiles(currentPath)
onResume() → refreshRecentHistory()
```

### Step 3.2: File Browser Logic

**Data model:**
```java
class FileEntry {
    String name, fullPath;
    boolean isDirectory;
    boolean isGame; // .xp3 or contains startup.tjs
    long lastModified;
}
```

**Directory listing:** Use `new File(path).listFiles()` — runs on background thread.

**Game detection:**
- File ends with `.xp3` → `isGame = true`
- Directory contains `startup.tjs` → `isGame = true`
- Directory name matches known game patterns

**Navigation:** Track breadcrumb path stack for back navigation.

**Visual cues:**
- Folders: folder icon
- Game files (`.xp3`/`.xp4`): game icon + "play" indicator
- Non-game files: generic file icon
- Sort: directories first, then alphabetical

### Step 3.3: Recent Game History

**Data source:** `recentpath.xml` from internal storage.

**Format:** Same as cocos2d version:
```xml
<RecentPathList>
    <Item Path="/storage/.../game.xp3"/>
</RecentPathList>
```

**Location:** `context.getFilesDir().getParent() + "/.preference/recentpath.xml"`

**Operations:**
- Load on startup
- Add when game launches
- Remove on swipe or tap ✕
- Save on changes

**Display:** Path → extract game name from filename/dirname. Show path as subtitle.

### Step 3.4: Side Menu / Settings

**Menu options (toolbar overflow or drawer):**
- Refresh file list
- Global Preference → open `PreferenceActivity`
- About → app version dialog
- Exit → finish()

### Step 3.5: Settings (future)

For now, settings button will just show a basic dialog. Full settings rewrite is a separate phase.

For immediate functionality, create a simple `PreferenceActivity` with basic options:
- Show FPS (checkbox)
- Keep screen alive (checkbox)
- Renderer (select: software/vulkan)

### Step 3.6: JNI / Bridge changes

**New C++ entry point for startup:**
```cpp
// Called from LauncherActivity when game selected
JNIEXPORT void JNICALL
Java_com_yuri_kirikiri2_MainActivity_nativeSetGamePath(JNIEnv *env, jclass, jstring path);
```

**Modify `MainActivity` (`KR2Activity`):**
- On `onCreate`, read `getIntent().getStringExtra("startupPath")`
- If set, call `nativeSetGamePath` before starting engine
- On exit (`TVPExitApplication`), call `finish()` instead of `System.exit()`

**Modify `TVPExitApplication` in native code:**
```cpp
void TVPExitApplication() {
    // Instead of exit(0), finish the activity
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jclass cls = env->FindClass("org/tvp/kirikiri2/KR2Activity");
    jmethodID mid = env->GetStaticMethodID(cls, "finishActivity", "()V");
    env->CallStaticVoidMethod(cls, mid);
}
```

### Step 3.7: AndroidManifest changes

```xml
<!-- LauncherActivity as main entry -->
<activity android:name="com.yuri.kirikiri2.LauncherActivity"
    android:exported="true"
    android:configChanges="...">
    <intent-filter>
        <action android:name="android.intent.action.MAIN" />
        <category android:name="android.intent.category.LAUNCHER" />
    </intent-filter>
</activity>

<!-- MainActivity (engine) — no launcher category -->
<activity android:name="com.yuri.kirikiri2.MainActivity"
    android:exported="true"
    android:configChanges="...">
</activity>
```

### Step 3.8: Directory structure

```
project/android/app/java/com/yuri/kirikiri2/
├── LauncherActivity.java      ← NEW (launcher)
├── LauncherFileAdapter.java   ← NEW (RecyclerView adapter)
├── LauncherRecentAdapter.java ← NEW (recent history adapter)
├── MainActivity.java          ← MODIFY
├── KR2Activity.java           ← MODIFY

project/android/app/res/layout/
├── activity_launcher.xml      ← NEW
├── item_file.xml              ← NEW
├── item_recent.xml            ← NEW

project/android/app/res/menu/
├── launcher_menu.xml          ← NEW
```

## 4. Data Flow

```
Game selected in LauncherActivity:
  → Intent: startActivity(MainActivity, "startupPath"=path)
  → MainActivity.onCreate():
    → getIntent().getStringExtra("startupPath")
    → nativeSetStartupArgs(path)
    → engine starts normally
  → Game exits:
    → TVPExitApplication → finishActivity()
    → LauncherActivity.onResume() → refresh
```

## 5. Phasing

| Phase | Deliverable | Status |
|-------|-------------|--------|
| 1 | LauncherActivity with file browser (RecyclerView + directory listing + game detection) | ✓ |
| 2 | Recent history (read/write recentpath.xml, display + delete) | ✓ |
| 3 | Quick access buttons + path navigation (back, breadcrumbs) | ✓ |
| 4 | Settings integration | ✓ |
| 5 | Polish: icons, animations, edge cases | Partial |
