package org.tvp.kirikiri2;

import android.Manifest;
import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.app.Activity;
import android.app.ActivityManager;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager.NameNotFoundException;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Debug;
import android.os.Environment;
import android.os.Handler;
import android.os.Message;
import android.os.storage.StorageManager;
import android.preference.PreferenceManager;
import android.provider.BaseColumns;
import android.provider.MediaStore;
import android.util.Log;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AlertDialog;
import androidx.core.app.ActivityCompat;
import androidx.documentfile.provider.DocumentFile;

import com.google.android.material.dialog.MaterialAlertDialogBuilder;
import android.widget.PopupMenu;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Color;


import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Utility class for handling the media store.
 */
@SuppressWarnings("ALL")
abstract class MediaStoreUtil {
    public static Uri getUriFromFile(final String path,Context context) {
        ContentResolver resolver = context.getContentResolver();
        Cursor filecursor = resolver.query(MediaStore.Files.getContentUri("external"),
                new String[] { BaseColumns._ID }, MediaStore.MediaColumns.DATA + " = ?",
                new String[] { path }, MediaStore.MediaColumns.DATE_ADDED + " desc");
        filecursor.moveToFirst();

        if (filecursor.isAfterLast()) {
            filecursor.close();
            ContentValues values = new ContentValues();
            values.put(MediaStore.MediaColumns.DATA, path);
            return resolver.insert(MediaStore.Files.getContentUri("external"), values);
        }
        else {
            int imageId = filecursor.getInt(filecursor.getColumnIndex(BaseColumns._ID));
            Uri uri = MediaStore.Files.getContentUri("external").buildUpon().appendPath(
                    Integer.toString(imageId)).build();
            filecursor.close();
            return uri;
        }
    }

    public static void addFileToMediaStore(final String path, Context context) {
        Intent mediaScanIntent = new Intent(Intent.ACTION_MEDIA_SCANNER_SCAN_FILE);
        File file = new File(path);
        Uri contentUri = Uri.fromFile(file);
        mediaScanIntent.setData(contentUri);
        context.sendBroadcast(mediaScanIntent);
    }

}

@SuppressWarnings("ALL")
public class KR2Activity extends SDLActivity implements ActivityCompat.OnRequestPermissionsResultCallback {

    @Override
    protected String[] getLibraries() {
        return new String[] { "krkr2yuri" };
    }

    public static final int RC_WRITE_EXTERNAL = 1;
    public static final int RC_PHONE_STATE = 2;

	static ActivityManager.MemoryInfo memoryInfo = new ActivityManager.MemoryInfo();
	static ActivityManager mAcitivityManager = null;
	static Debug.MemoryInfo mDbgMemoryInfo = new Debug.MemoryInfo();
	public static void updateMemoryInfo() {
		if(mAcitivityManager == null) {
			mAcitivityManager =(ActivityManager)sInstance.getSystemService(Activity.ACTIVITY_SERVICE);
		}
		mAcitivityManager.getMemoryInfo(memoryInfo);
		Debug.getMemoryInfo(mDbgMemoryInfo);
	}
	
	public static long getAvailMemory() {
		return memoryInfo.availMem;
	}

	public static long getUsedMemory() {
		return mDbgMemoryInfo.getTotalPss(); // in kB
	}

    private static void requestPhoneState() {
        // Permission has not been granted and must be requested.
        if (ActivityCompat.shouldShowRequestPermissionRationale(sInstance,
                Manifest.permission.READ_PHONE_STATE)) {
            // Provide an additional rationale to the user if the permission was not granted
            // and the user would benefit from additional context for the use of the permission.
            // Display a SnackBar with cda button to request the missing permission.
            ActivityCompat.requestPermissions(sInstance,
                    new String[]{Manifest.permission.READ_PHONE_STATE},
                    RC_PHONE_STATE);

        } else {
            // Request the permission. The result will be received in onRequestPermissionResult().
            ActivityCompat.requestPermissions(sInstance,
                    new String[]{Manifest.permission.READ_PHONE_STATE}, RC_PHONE_STATE);
        }
    }

    private static void requestExternalWrite() {
        // Permission has not been granted and must be requested.
        if (ActivityCompat.shouldShowRequestPermissionRationale(sInstance,
                Manifest.permission.WRITE_EXTERNAL_STORAGE)) {
            // Provide an additional rationale to the user if the permission was not granted
            // and the user would benefit from additional context for the use of the permission.
            // Display a SnackBar with cda button to request the missing permission.
            ActivityCompat.requestPermissions(sInstance,
                    new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE},
                    RC_WRITE_EXTERNAL);

        } else {
            // Request the permission. The result will be received in onRequestPermissionResult().
            ActivityCompat.requestPermissions(sInstance,
                    new String[]{Manifest.permission.WRITE_EXTERNAL_STORAGE}, RC_WRITE_EXTERNAL);
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        switch (requestCode) {
            case RC_PHONE_STATE:
                Log.d("Krkr2", "onRequestPermissionsResult: PHONE STATE");
                break;
            case RC_WRITE_EXTERNAL:
                Log.d("Krkr2", "onRequestPermissionsResult: WRITE EXTERNAL");
                break;
        }
    }


    static public String getDeviceId() { // ## fix android.permission.READ_PRIVILEGED_PHONE_STATE
		return "";
	}

	static public KR2Activity sInstance;
	static public KR2Activity GetInstance() {return sInstance;}
    
	@Override
	public void onCreate(Bundle savedInstanceState) {
		sInstance = this;
        Sp = PreferenceManager.getDefaultSharedPreferences(this);
		super.onCreate(savedInstanceState);
		nativeInitJNI();
		doSetSystemUiVisibility();
	
		if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.LOLLIPOP) {
			for(String path : getExtSdCardPaths(this)) {
		        if (!isWritableNormalOrSaf(path)) {
		            guideDialogForLEXA(path);
		        }
			}
		}
		
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            requestExternalWrite();
        }
		initDump(this.getFilesDir().getAbsolutePath() + "/dump");

		// Extract bundled DroidSansFallback.ttf from APK assets to internal storage
		// so the native font system can find it (SDL variant: storage system doesn't
		// handle absolute paths, so we make it available as a regular file).
		try {
			java.io.InputStream is = getAssets().open("DroidSansFallback.ttf");
			if (is != null) {
				java.io.File dst = new java.io.File(getFilesDir(), "DroidSansFallback.ttf");
				if (!dst.exists()) {
					java.io.FileOutputStream os = new java.io.FileOutputStream(dst);
					byte[] buf = new byte[65536];
					int n;
					while ((n = is.read(buf)) > 0) os.write(buf, 0, n);
					os.close();
				}
				is.close();
			}
		} catch (java.io.IOException e) {
			// Font file not found or copy failed; system font fallback will handle it
		}

		// Forward launch intent extras to the engine. Recognized extras:
		//   "startupPath" : String  -> path to a .xp3 archive or a bootable folder
		//   "args"        : String[] -> per-game options, each "-key=value" or "-flag"
		// These mirror Win32 argv[1] (startup) and argv[2..] (options); the native
		// side stashes them and TVPCheckStartupArg consumes them on the cocos thread.
		Intent launchIntent = getIntent();
		if (launchIntent != null) {
			String startupPath = launchIntent.getStringExtra("startupPath");
			String[] args = launchIntent.getStringArrayExtra("args");
			if (startupPath != null || args != null) {
				nativeSetStartupArgs(startupPath, args);
			}
		}

		// Floating game menu overlay (draggable button + popup menu)
		GameMenuOverlay.attach(this);
	}
	
	@Override
	public void onDestroy() {
		// SDLActivity.onDestroy handles cleanup + waits for SDL thread to finish
		super.onDestroy();
	}
	
	static class DialogMessage
	{
		public String Title;
		public String Text;
		public String[] Buttons;
		public EditText TextEditor = null;

		public DialogMessage()
		{
		}
		
		public void Init(final String title, final String text, final String[] buttons)
		{
			this.Title = title;
			this.Text = text;
			this.Buttons = buttons;
		}
		
		void onButtonClick(int n) {
			if(TextEditor != null) {
				onMessageBoxText(TextEditor.getText().toString());
			}
        	onMessageBoxOK(n);
		}
		
		public MaterialAlertDialogBuilder CreateBuilder() {
		/*	TextView showText = new TextView(sInstance);
			showText.setText(Text);
			if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.HONEYCOMB)
				showText.setTextIsSelectable(true);*/
			MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(sInstance).
                setTitle(Title).
                setMessage(Text).
                //setView(showText).
				setCancelable(false);
			if(Buttons.length >= 1) {
                builder = builder.setPositiveButton(Buttons[0], new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                    	onButtonClick(0);
                    }
                });
			}
			if(Buttons.length >= 2) {
                builder = builder.setNeutralButton(Buttons[1], new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                    	onButtonClick(1);
                    }
                });
    		}
			if(Buttons.length >= 3) {
                builder = builder.setNegativeButton(Buttons[2], new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                    	onButtonClick(2);
                    }
                });
    		}
			return builder;
		}
		
		public void ShowMessageBox()
		{
			CreateBuilder().create().show();
		}
		
		public void ShowInputBox(final String text) {
			MaterialAlertDialogBuilder builder = CreateBuilder();
			TextEditor = new EditText(sInstance);  
			LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
			                     LinearLayout.LayoutParams.MATCH_PARENT,
			                     LinearLayout.LayoutParams.MATCH_PARENT);
			TextEditor.setLayoutParams(lp);
			TextEditor.setText(text);
			builder.setView(TextEditor);
			AlertDialog ad = builder.create(); 
			ad.show();
			TextEditor.requestFocus();
            InputMethodManager imm = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            imm.showSoftInput(TextEditor, 0);
		}
	}
	static DialogMessage mDialogMessage = new DialogMessage();

    SharedPreferences Sp;
	
	static Handler msgHandler = new Handler() {
		@Override
		public void handleMessage(Message msg) {
			sInstance.handleMessage(msg);
		}
	};
	
	public void handleMessage(Message msg) {
		
	}
	
	static public void ShowMessageBox(final String title, final String text, final String[] Buttons) {
		mDialogMessage.Init(title, text, Buttons);
		msgHandler.post(new Runnable() {
			@Override
			public void run() {
				mDialogMessage.ShowMessageBox();
			}
		});
	}
	
	static public void ShowInputBox(final String title, final String prompt, final String text, final String[] Buttons) {
		mDialogMessage.Init(title, prompt, Buttons);
		msgHandler.post(new Runnable() {
			@Override
			public void run() {
				mDialogMessage.ShowInputBox(text);
			}
		});
	}

	static public void ShowMenuDialog(final String[] captions, final boolean[] hasSub) {
		msgHandler.post(new Runnable() {
			@Override
			public void run() {
				AlertDialog.Builder builder = new AlertDialog.Builder(sInstance);
				builder.setTitle("Game Menu");
				builder.setItems(captions, new DialogInterface.OnClickListener() {
					@Override
					public void onClick(DialogInterface dialog, int which) {
						dialog.dismiss();
						nativeOnMenuResult(which);
					}
				});
				builder.setOnCancelListener(new DialogInterface.OnCancelListener() {
					@Override
					public void onCancel(DialogInterface dialog) {
						nativeOnMenuResult(-1);
					}
				});
				builder.show();
			}
		});
	}
	
	
	
	static private native void onMessageBoxOK(int nButton);
	static private native void onMessageBoxText(String text);
	static private native void onNativeExit();
	static public native void onNativeInit();
	static public native void onBannerSizeChanged(int w, int h);
	static private native void initDump(String path);
	static private native void nativeShowGameMenu();
	static private native void nativeToggleMouseMode();
	static private native void nativeShowKeyboard();
	static private native void nativeGameMenuExit();
	static private native boolean nativeIsFullscreenStretch();
	static private native void nativeToggleAspectRatio();
	static private native void nativeOnMenuResult(int index);
	
	static public void MessageController(int what, int arg1, int arg2) {
        Message msg = msgHandler.obtainMessage();
        msg.what = what;
        msg.arg1 = arg1;
        msg.arg2 = arg2;
		msgHandler.sendMessage(msg);
	}
	
	static public String GetVersion() {
		String verstr = null;
		try {
			verstr = sInstance.getPackageManager().getPackageInfo(sInstance.getPackageName(), 0).versionName;
		} catch (NameNotFoundException e1) {
		}
		return verstr;
	}
	
	StorageManager mStorageManager = null;
    Method mMethodGetPaths = null;
    Method mGetVolumeState = null;
    
    public String[] getStoragePath() {
    	String[] ret = new String[0];
    	if(mStorageManager == null) {
        	mStorageManager = (StorageManager)getSystemService(STORAGE_SERVICE);
            try {
                mMethodGetPaths = StorageManager.class.getMethod("getVolumePaths");
                mGetVolumeState = StorageManager.class.getMethod("getVolumeState", String.class);
            } catch (NoSuchMethodException e) {
                e.printStackTrace();
            }
    	}
    	if(mMethodGetPaths != null) {
            try {
            	ret = (String[])mMethodGetPaths.invoke(mStorageManager);
            } catch (IllegalArgumentException e) {
     
            } catch (IllegalAccessException e) {
     
            } catch (InvocationTargetException e) {
     
            } catch (Exception e) {
     
            }
    	}
        
        if(mGetVolumeState != null) {
            try {
            	for(int i = 0; i < ret.length; ++i) {
            		String status = (String)mGetVolumeState.invoke(mStorageManager, ret[i]);
            		if(Environment.MEDIA_MOUNTED.equals(status) || Environment.MEDIA_MOUNTED_READ_ONLY.equals(status)) {
            			;
            		} else {
            			ret[i] = null;
            		}
            	}
            } catch (IllegalArgumentException e) {
     
            } catch (IllegalAccessException e) {
     
            } catch (InvocationTargetException e) {
     
            } catch (Exception e) {
     
            }
        }
        
		return ret;
    }
    
    private static native void nativeInitJNI();
    
    public int get_res_sd_operate_step() { return -1; }

    static void requireLEXA(final String path) {
		msgHandler.post(new Runnable() {
			@Override
			public void run() {
				guideDialogForLEXA(path);
			}
		});
    }
    static void guideDialogForLEXA(final String path) {
    	MaterialAlertDialogBuilder builder = new MaterialAlertDialogBuilder(sInstance);
    	ImageView image = new ImageView(sInstance);
    	image.setImageResource(sInstance.get_res_sd_operate_step());
    	builder
    		.setView(image)
    		.setTitle(path)
    		.setPositiveButton("OK", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                    triggerStorageAccessFramework();
                }
            })
            .setNegativeButton("Cancel", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface dialog, int which) {
                	// nothing to do
                }
            })
            .show();
    }
    
    static final boolean isWritable(final File file) {
        if(file==null)
            return false;
        boolean isExisting = file.exists();

        try {
            FileOutputStream output = new FileOutputStream(file, true);
            try {
                output.close();
            }
            catch (IOException e) {
                // do nothing.
            }
        }
        catch (FileNotFoundException e) {
            return false;
        }
        boolean result = file.canWrite();

        // Ensure that file is not created during this process.
        if (!isExisting) {
            file.delete();
        }

        return result;
    }

    static final boolean isWritableNormal(final String path) {
        boolean ret = isWritableNormalOrSaf(path);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            requestExternalWrite();
            return isWritableNormalOrSaf(path);
        }
        return ret;
    }


    static final boolean isWritableNormalOrSaf(final String path) {

        Log.i("kr2activaty","check path " + path + "permision");

    	Context c = sInstance;
    	File folder = new File(path);
    	folder.mkdir();
    	// Log.d("ke2activate", String.format("%b  and  %b", folder.exists(), folder.isDirectory()));
        if (!folder.exists() || !folder.isDirectory()) {
           return false;
        }

        // Find a non-existing file in this directory.
        int i = 0;
        File file;
        do {
            String fileName = "AugendiagnoseDummyFile" + (++i);
            file = new File(folder, fileName);
        }
        while (file.exists());

        // First check regular writability
        Log.d("ke2activate", String.format("%b  ", isWritable(file)));
        if (isWritable(file)) {
            return true;
        }

        // Next check SAF writability.
        DocumentFile document = getDocumentFile(file, false,c);

        if (document == null) {
            return false;
        }

        // This should have created the file - otherwise something is wrong with access URL.
        boolean result = document.canWrite() && file.exists();

        // Ensure that the dummy file is not remaining.
        document.delete();
        DocumentFile.fromFile(folder).delete();

        return result;
    }
    
    @TargetApi(Build.VERSION_CODES.KITKAT)
    private static String[] getExtSdCardPaths(Context context) {
        List<String> paths = new ArrayList<String>();
        for (File file : context.getExternalFilesDirs("external")) {
            if (file != null && !file.equals(context.getExternalFilesDir("external"))) {
                int index = file.getAbsolutePath().lastIndexOf("/Android/data");
                if (index < 0) {
                    Log.w("FileUtils", "Unexpected external file dir: " + file.getAbsolutePath());
                } else {
                    String path = file.getAbsolutePath().substring(0, index);
                    try {
                        path = new File(path).getCanonicalPath();
                    }
                    catch (IOException e) {
                        // Keep non-canonical path.
                    }
                    paths.add(path);
                }
            }
        }
        //if(paths.isEmpty())paths.add("/storage/sdcard1");
        return paths.toArray(new String[0]);
    }
    static String[] _extSdPaths;

    public static String getExtSdCardFolder(final File file,Context context) {
    	if(_extSdPaths == null)
    		_extSdPaths = getExtSdCardPaths(context);
        try {
            for (int i = 0; i < _extSdPaths.length; i++) {
                if (file.getCanonicalPath().startsWith(_extSdPaths[i])) {
                    return _extSdPaths[i];
                }
            }
        }
        catch (IOException e) {
            return null;
        }
        return null;
    }
    
    public static boolean isOnExtSdCard(final File file,Context c) {
        return getExtSdCardFolder(file,c) != null;
    }
    
    public static DocumentFile getDocumentFile(final File file, final boolean isDirectory,Context context) {
        String baseFolder = getExtSdCardFolder(file,context);
        boolean originalDirectory=false;
        if (baseFolder == null) {
            return null;
        }

        String relativePath = null;
        try {
            String fullPath = file.getCanonicalPath();
            if(!baseFolder.equals(fullPath))
            relativePath = fullPath.substring(baseFolder.length() + 1);
            else originalDirectory=true;
        }
        catch (IOException e) {
            return null;
        }
        catch (Exception f){
            originalDirectory=true;
            //continue
        }
        String as=PreferenceManager.getDefaultSharedPreferences(context).getString("URI",null);

        Uri treeUri =null;
        if(as!=null)treeUri=Uri.parse(as);
        if (treeUri == null) {
            return null;
        }

        // start with root of SD card and then parse through document tree.
        DocumentFile document = DocumentFile.fromTreeUri(context, treeUri);
        if(originalDirectory)return document;
        String[] parts = relativePath.split("\\/");
        for (int i = 0; i < parts.length; i++) {
            DocumentFile nextDocument = document.findFile(parts[i]);
            if (nextDocument == null) {
            	try {
	                if ((i < parts.length - 1) || isDirectory) {
	                    nextDocument = document.createDirectory(parts[i]);
	                } else {
	                    nextDocument = document.createFile("image", parts[i]);
	                }
	            } catch (Exception e) {
	            	return null;
	            }
            }
            document = nextDocument;
        }

        return document;
    }
    
    static public boolean RenameFile(String from, String to) {
    	File file = new File(from);
    	File target = new File(to);
    	if(!file.exists())
    		return false;
    	if(target.exists()) {
    		if(!DeleteFile(target.getAbsolutePath())) return false;
    	}
    	
    	File parent = target.getParentFile();
    	if(!parent.exists()) {
    		if(!CreateFolders(parent.getAbsolutePath())) return false;
    	}
    	// Try the normal way
        if(file.renameTo(target)) return true;
        
        // Try with Storage Access Framework.
        if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.LOLLIPOP /*&& isOnExtSdCard(file, sInstance)*/) {
            DocumentFile document = getDocumentFile(file, false, sInstance);
        	if(document.renameTo(to))
        		return true;
        }
        
        // Try Media Store Hack
        if (Build.VERSION.SDK_INT==Build.VERSION_CODES.KITKAT) {
        	try {
				FileInputStream input = new FileInputStream(file);
	        	int filesize = (int) file.length();
				byte []buffer = new byte[filesize];
				input.read(buffer);
				input.close();
            	OutputStream out = MediaStoreHack.getOutputStream(sInstance, target.getAbsolutePath());
                out.write(buffer);
                out.close();
                return MediaStoreHack.delete(sInstance, file);
			} catch (IOException e) {
				// TODO Auto-generated catch block
				return false;
				//e.printStackTrace();
			}
        }
        
    	return false;
    }
    
    public static final boolean deleteFilesInFolder(final File folder,Context context) {
        boolean totalSuccess = true;
        if(folder==null)
            return false;
        if (folder.isDirectory()) {
            for (File child : folder.listFiles()) {
                deleteFilesInFolder(child, context);
            }

            if (!folder.delete())
                totalSuccess = false;
        } else {

            if (!folder.delete())
                totalSuccess = false;
        }
        return totalSuccess;
    }

    static public boolean DeleteFile(String path) {
    	File file = new File(path);
    	// First try the normal deletion.
        boolean fileDelete = deleteFilesInFolder(file, sInstance);
        if (file.delete() || fileDelete)
            return true;

        // Try with Storage Access Framework.
        if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.LOLLIPOP && isOnExtSdCard(file, sInstance)) {

            DocumentFile document = getDocumentFile(file, false,sInstance);
            return document.delete();
        }

        // Try the Kitkat workaround.
        if (Build.VERSION.SDK_INT==Build.VERSION_CODES.KITKAT) {
            ContentResolver resolver = sInstance.getContentResolver();

            try {
                Uri uri = MediaStoreHack.getUriFromFile(file.getAbsolutePath(),sInstance);
                resolver.delete(uri, null, null);
                return !file.exists();
            }
            catch (Exception e) {
                Log.e("FileUtils", "Error when deleting file " + file.getAbsolutePath(), e);
                return false;
            }
        }

        return !file.exists();
    }
    
	public static OutputStream getOutputStream(@NonNull final File target,Context context,long s)throws Exception {
	    OutputStream outStream = null;
	    try {
	        // First try the normal way
			if (isWritable(target)) {
			    // standard way
			    outStream = new FileOutputStream(target);
			} else {
			    if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.LOLLIPOP) {
			        // Storage Access Framework
			    DocumentFile targetDocument = getDocumentFile(target, false,context);
			    outStream = context.getContentResolver().openOutputStream(targetDocument.getUri());
			} else if (Build.VERSION.SDK_INT==Build.VERSION_CODES.KITKAT) {
			    // Workaround for Kitkat ext SD card
		        return MediaStoreHack.getOutputStream(context,target.getPath());
		        }
		    }
		} catch (Exception e) {
		    Log.e("FileUtils",
    			"Error when copying file from " +  target.getAbsolutePath(), e);
	    }
	  return outStream;
    }

    
    static public boolean WriteFile(String path, byte data[]) {
        File target = new File(path);
        if(target.exists()) {
        	DeleteFile(target.getAbsolutePath()); // to avoid number suffix name
        } else {
            File parent = target.getParentFile();
            if(!parent.exists())
            	CreateFolders(parent.getAbsolutePath());
        }
        OutputStream out = null;
        
    	// Try the normal way
    	try {
        	if(isWritable(target)) {
        		OutputStream os = new FileOutputStream(target);
    			os.write(data);
    			os.close();
    			return true;
        	}

            // Try with Storage Access Framework.
            if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.LOLLIPOP /*&& isOnExtSdCard(file, sInstance)*/) {
                DocumentFile document = getDocumentFile(target, false, sInstance);
                try {
                	Uri docUri = document.getUri();
                    out = sInstance.getContentResolver().openOutputStream(docUri);
                } //catch (FileNotFoundException e) {
                    // e.printStackTrace();}
                catch (IOException e) {
                    // e.printStackTrace();
                }
            } else if (Build.VERSION.SDK_INT==Build.VERSION_CODES.KITKAT) {
                // Workaround for Kitkat ext SD card
                Uri uri = MediaStoreHack.getUriFromFile(target.getAbsolutePath(),sInstance);
                out = sInstance.getContentResolver().openOutputStream(uri);
            } else {
                return false;
            }
            
            if (out != null) {
                out.write(data);
                out.close();
                return true;
            }
		} catch (FileNotFoundException e) {
			//return false;
		} catch (IOException e) {
			//return false;
		}

    	return false;
    }
    
    static public boolean CreateFolders(String path) {
    	File file = new File(path);
    	
        // Try the normal way
    	if(file.mkdirs()) {
    		return true;
    	}

        // Try with Storage Access Framework.
        if (Build.VERSION.SDK_INT>=Build.VERSION_CODES.LOLLIPOP /*&& FileUtil.isOnExtSdCard(file, context)*/) {
            DocumentFile document = getDocumentFile(file, true,sInstance);
            // getDocumentFile implicitly creates the directory.

            if (document != null)
                return document.exists();
            else
                return false;
        }
        
        // Try the Kitkat workaround.
        if (Build.VERSION.SDK_INT==Build.VERSION_CODES.KITKAT) {
            try {
            	return MediaStoreHack.mkdir(sInstance,file);
            } catch (IOException e) {
                //return false;
            }
        }
        
    	return false;
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);

        //SDLActivity.mHasFocus = hasFocus;
        if (hasFocus) {
        	hideSystemUI();
        }
    }
    
    @TargetApi(Build.VERSION_CODES.HONEYCOMB)
    void doSetSystemUiVisibility() {
		int uiOpts = View.SYSTEM_UI_FLAG_LAYOUT_STABLE
		        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
		        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
		        | View.SYSTEM_UI_FLAG_FULLSCREEN
		        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
		        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY;
		getWindow().getDecorView().setSystemUiVisibility(uiOpts);
    }

    //--------------------------------------------------------------------------
    // Debug overlay (FPS/memory) — Android native TextView on top of SDL surface
    //--------------------------------------------------------------------------
    private static TextView mDebugOverlay = null;
    private static boolean mDebugOverlayVisible = false;

    public static void showDebugOverlay(final boolean show) {
        if (show == mDebugOverlayVisible) return;
        mDebugOverlayVisible = show;
        msgHandler.post(new Runnable() {
            @Override
            public void run() {
                if (show && mDebugOverlay == null) {
                    mDebugOverlay = new TextView(sInstance);
                    mDebugOverlay.setTextColor(Color.argb(220, 255, 255, 255));
                    mDebugOverlay.setTextSize(12);
                    mDebugOverlay.setShadowLayer(2, 1, 1, Color.argb(200, 0, 0, 0));
                    mDebugOverlay.setPadding(12, 12, 12, 12);
                    mDebugOverlay.setVisibility(View.GONE);
                    FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.WRAP_CONTENT,
                        FrameLayout.LayoutParams.WRAP_CONTENT);
                    lp.gravity = android.view.Gravity.TOP | android.view.Gravity.START;
                    mDebugOverlay.setLayoutParams(lp);
                    sInstance.mLayout.addView(mDebugOverlay);
                }
                if (mDebugOverlay != null) {
                    mDebugOverlay.setVisibility(show ? View.VISIBLE : View.GONE);
                }
            }
        });
    }

    public static void updateDebugOverlay(final String text) {
        if (!mDebugOverlayVisible || mDebugOverlay == null) return;
        msgHandler.post(new Runnable() {
            @Override
            public void run() {
                if (mDebugOverlay != null)
                    mDebugOverlay.setText(text);
            }
        });
    }

    private static native boolean nativeGetHideSystemButton();
    private static native void nativeSetSafTreeUri(String uri);
    private static native String nativeGetSafTreeUri();
    private static native void nativeSetStartupArgs(String startupPath, String[] args);
    void hideSystemUI() {
    	if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.KITKAT) {
    		doSetSystemUiVisibility();
    	}
    }
    
    static public String getExternalStoragePath() {
    	Activity ctx = getContext();
    	if (ctx == null) return "";
    	java.io.File extDir = ctx.getExternalFilesDir(null);
    	return extDir != null ? extDir.getAbsolutePath() : "";
    }

    static public String getInternalStoragePath() {
    	Activity ctx = getContext();
    	if (ctx == null) return "";
    	java.io.File intDir = ctx.getFilesDir();
    	return intDir != null ? intDir.getAbsolutePath() : "";
    }

    static public String getDriverPath() {
    	Activity ctx = getContext();
    	if (!(ctx instanceof KR2Activity)) return "";
    	StringBuilder sb = new StringBuilder();
    	String[] paths = ((KR2Activity)ctx).getStoragePath();
    	if (paths != null) {
    		for (String p : paths) {
    			if (p != null) sb.append(p).append(";");
    		}
    	}
    	return sb.toString();
    }

    static public String getApkStoragePath() {
    	Activity ctx = getContext();
    	return ctx != null ? ctx.getPackageCodePath() : "";
    }

    static public String getPackageVersionString() {
    	Activity ctx = getContext();
    	if (ctx == null) return "";
    	try {
    		return ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0).versionName;
    	} catch (Exception e) {
    		return "";
    	}
    }

    static public boolean createFolders(String path) {
    	java.io.File dir = new java.io.File(path);
    	if (dir.isDirectory()) return true;
    	return dir.mkdirs();
    }

    static public boolean renameFile(String oldPath, String newPath) {
    	java.io.File oldFile = new java.io.File(oldPath);
    	java.io.File newFile = new java.io.File(newPath);
    	return oldFile.renameTo(newFile);
    }

    static public String getLocaleName() {
    	Locale defloc = Locale.getDefault();
    	String lang = defloc.getLanguage();
    	String country = defloc.getCountry();
    	if(!country.isEmpty()) {
    		lang += "_";
    		lang += country.toLowerCase();
    	}
    	return lang;
    }
    
    static public void exit() {
    	msgHandler.post(new Runnable() {
			@Override
			public void run() {
				Activity act = sInstance;
				if (act != null) act.finish();
			}
		});
    }
    
    static final int ORIENT_VERTICAL = 1;
    static final int ORIENT_HORIZONTAL = 2;
    
    static public void setOrientation(int orient) {
    	if(orient == ORIENT_VERTICAL) {
    		sInstance.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_PORTRAIT);
    	} else if(orient == ORIENT_HORIZONTAL) {
    		sInstance.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
    	}
    }
    
    @TargetApi(Build.VERSION_CODES.LOLLIPOP)
	static public void triggerStorageAccessFramework() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        sInstance.startActivityForResult(intent, 3);
    }

    @SuppressLint("WrongConstant")
    @TargetApi(Build.VERSION_CODES.KITKAT)
    protected void onActivityResult(int requestCode, int responseCode, Intent intent) {
        if (requestCode == 3) {
            Uri treeUri = null;
            if (responseCode == Activity.RESULT_OK) {
                // Get Uri from Storage Access Framework.
                treeUri = intent.getData();
                // Persist URI - this is required for verification of writability.
                if (treeUri != null) {
                    String uriStr = treeUri.toString();
                    Sp.edit().putString("URI", uriStr).commit();
                    // Mirror the URI into the engine's GlobalPreference.xml so the
                    // cross-platform config layer is aware of the granted tree.
                    nativeSetSafTreeUri(uriStr);
                }
            }

            // If not confirmed SAF, or if still not writable, then revert settings.
            if (responseCode != Activity.RESULT_OK) {
                return;
            }

            // After confirmation, update stored value of folder.
            // Persist access permissions.
            final int takeFlags = intent.getFlags()
                    & (Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            getContentResolver().takePersistableUriPermission(treeUri, takeFlags);
        }
    }

    //--------------------------------------------------------------------------
    // GameMenuOverlay — draggable floating button + popup menu
    //--------------------------------------------------------------------------
    static class GameMenuOverlay {
        static final int ITEM_GAME_MENU = 0;
        static final int ITEM_WINDOW = 1;
        static final int ITEM_MOUSE_MODE = 2;
        static final int ITEM_KEYBOARD = 3;
        static final int ITEM_EXIT = 4;
        private View mButton;
        private float mOffsetX, mOffsetY;
        private boolean mMouseMode = true;

        static GameMenuOverlay attach(KR2Activity activity) {
            GameMenuOverlay overlay = new GameMenuOverlay();
            overlay.create(activity);
            overlay.show();
            return overlay;
        }

        void create(KR2Activity activity) {
            float density = activity.getResources().getDisplayMetrics().density;
            int btnSize = (int)(48 * density + 0.5f);

            mButton = new View(activity) {
                @Override
                protected void onDraw(Canvas canvas) {
                    super.onDraw(canvas);
                    int cx = getWidth() / 2, cy = getHeight() / 2, r = cx - 4;
                    Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
                    p.setColor(Color.argb(180, 64, 64, 64));
                    canvas.drawCircle(cx, cy, r, p);
                    p.setColor(Color.argb(220, 255, 255, 255));
                    p.setStrokeWidth(3);
                    float bw = r * 0.55f;
                    for (int i = -1; i <= 1; i++)
                        canvas.drawLine(cx - bw, cy + i * 7, cx + bw, cy + i * 7, p);
                }
            };
            mButton.setVisibility(View.GONE);
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(btnSize, btnSize);
            lp.gravity = android.view.Gravity.BOTTOM | android.view.Gravity.START;
            lp.bottomMargin = (int)(100 * density + 0.5f);
            lp.leftMargin = (int)(16 * density + 0.5f);
            mButton.setLayoutParams(lp);

            mButton.setOnTouchListener((v, event) -> {
                switch (event.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN:
                    mOffsetX = event.getRawX() - v.getX();
                    mOffsetY = event.getRawY() - v.getY();
                    return true;
                case android.view.MotionEvent.ACTION_MOVE:
                    v.setX(event.getRawX() - mOffsetX);
                    v.setY(event.getRawY() - mOffsetY);
                    return true;
                case android.view.MotionEvent.ACTION_UP: {
                    float dx = event.getRawX() - (v.getX() + mOffsetX);
                    float dy = event.getRawY() - (v.getY() + mOffsetY);
                    if (Math.sqrt(dx*dx + dy*dy) < 20)
                        showMenu(activity, v);
                    return true;
                }
                }
                return false;
            });

            activity.mLayout.addView(mButton);
        }

		void showMenu(KR2Activity activity, View anchor) {
			// Popup inherits activity theme (DayNight), follows system dark/light mode
            PopupMenu popup = new PopupMenu(activity, anchor);
            popup.getMenu().add(0, ITEM_GAME_MENU, 0, "Game Menu");
            popup.getMenu().add(0, ITEM_WINDOW, 0,
                nativeIsFullscreenStretch() ? "Window (Stretch)" : "Window (Aspect)");
            popup.getMenu().add(0, ITEM_MOUSE_MODE, 0,
                mMouseMode ? "Switch to Touch" : "Switch to Mouse");
            popup.getMenu().add(0, ITEM_KEYBOARD, 0, "Keyboard");
            popup.getMenu().add(0, ITEM_EXIT, 0, "Exit");
            popup.setOnMenuItemClickListener(item -> {
                switch (item.getItemId()) {
                case ITEM_GAME_MENU:
                    nativeShowGameMenu();
                    break;
                case ITEM_WINDOW:
                    nativeToggleAspectRatio();
                    break;
                case ITEM_MOUSE_MODE:
                    mMouseMode = !mMouseMode;
                    nativeToggleMouseMode();
                    break;
                case ITEM_KEYBOARD:
                    nativeShowKeyboard();
                    break;
                case ITEM_EXIT:
                    nativeGameMenuExit();
                    break;
                }
                return true;
            });
            popup.show();
        }

        void show() { if (mButton != null) mButton.setVisibility(View.VISIBLE); }
        void hide() { if (mButton != null) mButton.setVisibility(View.GONE); }
    }
}
