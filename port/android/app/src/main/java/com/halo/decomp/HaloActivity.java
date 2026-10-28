package com.halo.decomp;

import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.view.Display;
import android.view.MotionEvent;
import android.view.WindowManager;
import android.view.ViewGroup;
import android.widget.Toast;

import java.io.ByteArrayOutputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL3's activity, running libmain.so (port/android/host), which
 * loads the game image from the APK's assets.
 */
public class HaloActivity extends SDLActivity implements TouchControls.LayoutTransfer {
    private static final int REQUEST_TOUCH_LAYOUT_EXPORT=0x4841;
    private static final int REQUEST_TOUCH_LAYOUT_IMPORT=0x4842;
    /** lets system link's broadcasts in over Wi-Fi while the game runs */
    private WifiManager.MulticastLock multicastLock;
    private TouchControls touchControls;
    private GamepadSupport gamepads;
    private GyroAim gyro;
    private PvpPublisher pvpPublisher;
    private String pendingTouchLayoutExport;
    private Consumer<String> pendingTouchLayoutImport;

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL3", "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        RunLog.start(this);
        RunLog.line("Game activity creating; native logging will use the same launch file");
        RunLog.line("Network: " + NetworkSettings.describe(this));
        java.io.File gameRoot = getExternalFilesDir(null);
        java.io.File shared = new java.io.File("/sdcard/Documents/HaloCE");
        if (BuildConfig.APPLICATION_ID.endsWith(".vr") && new java.io.File(shared, "maps/ui.map").isFile()) gameRoot = shared;
        // (Test73: the Steam Frame's, as LauncherActivity.baseRoot and host_main.c have it)
        if (Lepton.running() && Lepton.dataRoot() != null) gameRoot = Lepton.dataRoot();
        gameRoot=GameDataLibrary.activeRoot(gameRoot);
        NetworkProfile.gameStarted(gameRoot);
        if (gameRoot != null) { new java.io.File(gameRoot, "coop_status.txt").delete(); new java.io.File(gameRoot, "pvp_status.txt").delete(); }
        super.onCreate(savedInstanceState);
        if (gameRoot != null) { pvpPublisher = new PvpPublisher(this, gameRoot); }
        if (!BuildConfig.APPLICATION_ID.endsWith(".vr") && mLayout != null) {
            touchControls = new TouchControls(this);
            mLayout.addView(touchControls, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            touchControls.requestApplyInsets();
            gamepads = new GamepadSupport(this,touchControls);
            gyro = new GyroAim(this,touchControls);
            touchControls.setGyro(gyro);
            // Test73b: the touch screen's SPRINT while the game's SPRINT is HOLD or TOGGLE (movement.sprint, which
            // Player profile > MOVEMENT writes to this game set's config.toml)
            final java.io.File configRoot = gameRoot;
            touchControls.setSprintSetting(() ->
                TouchControls.sprintMode(ConfigSettings.read(configRoot, "movement", "sprint", "\"off\"")));
            RunLog.line("Phone touch controls initialized: player-one merge; multi-touch; hold fire and drag to aim");
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        preferHighestRefreshRate();
        acquireMulticastLock();
        // a new version looked for while the game starts
        Updater.start(this);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        if(touchControls!=null && touchControls.dispatchMenuTouch(event,mSurface)) return true;
        return super.dispatchTouchEvent(event);
    }

    @Override
    public void exportLayout(String contents) {
        if(contents==null||contents.getBytes(StandardCharsets.UTF_8).length>TouchLayoutCodec.MAX_BYTES) {
            Toast.makeText(this,"Touch layout file is too large to export.",Toast.LENGTH_LONG).show();return;
        }
        pendingTouchLayoutExport=contents;
        Intent request=new Intent(Intent.ACTION_CREATE_DOCUMENT);
        request.addCategory(Intent.CATEGORY_OPENABLE);request.setType("text/plain");
        request.putExtra(Intent.EXTRA_TITLE,"halo-ce-touch-layout.hcecontrols");
        try { startActivityForResult(request,REQUEST_TOUCH_LAYOUT_EXPORT); }
        catch(RuntimeException unavailable) {
            pendingTouchLayoutExport=null;
            Toast.makeText(this,"No document picker is available to export the layout.",Toast.LENGTH_LONG).show();
        }
    }

    @Override
    public void importLayout(Consumer<String> imported) {
        pendingTouchLayoutImport=imported;
        Intent request=new Intent(Intent.ACTION_OPEN_DOCUMENT);
        request.addCategory(Intent.CATEGORY_OPENABLE);request.setType("*/*");
        request.putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"text/plain","application/octet-stream"});
        try { startActivityForResult(request,REQUEST_TOUCH_LAYOUT_IMPORT); }
        catch(RuntimeException unavailable) {
            pendingTouchLayoutImport=null;
            Toast.makeText(this,"No document picker is available to import a layout.",Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode,int resultCode,Intent data) {
        super.onActivityResult(requestCode,resultCode,data);
        if(requestCode==REQUEST_TOUCH_LAYOUT_EXPORT) {
            String contents=pendingTouchLayoutExport;pendingTouchLayoutExport=null;
            if(resultCode!=RESULT_OK||data==null||data.getData()==null||contents==null)return;
            try(OutputStream output=getContentResolver().openOutputStream(data.getData(),"wt")) {
                if(output==null)throw new java.io.IOException("document could not be opened");
                output.write(contents.getBytes(StandardCharsets.UTF_8));output.flush();
                Toast.makeText(this,"Touch layout exported.",Toast.LENGTH_LONG).show();
            } catch(Exception failure) {
                Toast.makeText(this,"Could not export touch layout: "+failure.getMessage(),Toast.LENGTH_LONG).show();
            }
            return;
        }
        if(requestCode==REQUEST_TOUCH_LAYOUT_IMPORT) {
            Consumer<String> callback=pendingTouchLayoutImport;pendingTouchLayoutImport=null;
            if(resultCode!=RESULT_OK||data==null||data.getData()==null||callback==null)return;
            try(java.io.InputStream input=getContentResolver().openInputStream(data.getData());
                ByteArrayOutputStream bytes=new ByteArrayOutputStream()) {
                if(input==null)throw new java.io.IOException("document could not be opened");
                byte[] buffer=new byte[4096];int count;
                while((count=input.read(buffer))!=-1) {
                    if(bytes.size()>TouchLayoutCodec.MAX_BYTES-count)throw new java.io.IOException("layout file exceeds 64 KiB");
                    bytes.write(buffer,0,count);
                }
                String contents=StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes.toByteArray())).toString();
                callback.accept(contents);
            } catch(CharacterCodingException malformed) {
                Toast.makeText(this,"Layout not imported: file is not valid UTF-8.",Toast.LENGTH_LONG).show();
            } catch(Exception failure) {
                Toast.makeText(this,"Could not read touch layout: "+failure.getMessage(),Toast.LENGTH_LONG).show();
            }
        }
    }

    @Override
    protected void onDestroy() {
        if (gamepads != null) gamepads.pause();
        if (gyro != null) gyro.pause();
        if (touchControls != null) touchControls.releaseAll();
        if (pvpPublisher != null) pvpPublisher.close();
        RunLog.line("Game activity destroying");
        if (multicastLock != null && multicastLock.isHeld())
            multicastLock.release();
        multicastLock = null;
        super.onDestroy();
    }

    /** Called once by SDL's native main; ownership of the duplicate passes to C. */
    public int openGameLog() { return RunLog.nativeDescriptor(); }

    // the Omni One treadmill (host_omni.c asks, once vr.omni_treadmill is on or off)
    public void omniStart() { OmniOne.start(this); }
    public void omniStop() { OmniOne.stop(); }

    /** Native normal exit runs on the game thread; give withdrawal a bounded chance before _exit. */
    public void prepareGameExit() {
        if (pvpPublisher != null) pvpPublisher.closeBeforeNativeExit();
        RunLog.line("Native exit cleanup finished");
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (gamepads != null) gamepads.resume();
        if (gyro != null) gyro.resume();
        RunLog.line("Game activity resumed");
    }

    @Override
    protected void onPause() {
        if (gamepads != null) gamepads.pause();
        if (gyro != null) gyro.pause();
        if (touchControls != null) touchControls.releaseAll();
        RunLog.line("Game activity paused / headset focus changed");
        super.onPause();
    }

    /**
     * Test72 (a player: "only the face and trigger buttons work. Dpad and
     * sticks dont"): a controller's sticks and hat (its D-pad on most) reach
     * SDL here, at the activity, as its buttons do (SDLActivity's
     * dispatchKeyEvent). SDL's own path takes them only through its
     * surface's listener - not while another view has the focus - and only
     * when the event's source is exactly SOURCE_JOYSTICK, where some
     * Bluetooth controllers say joystick and gamepad at once, so their
     * motion was taken for a mouse's and dropped.
     */
    private final java.util.HashSet<Integer> motionDevicesLogged = new java.util.HashSet<>();

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & android.view.InputDevice.SOURCE_CLASS_JOYSTICK) != 0
                && org.libsdl.app.SDLControllerManager.isDeviceSDLJoystick(event.getDeviceId())) {
            if (motionDevicesLogged.add(event.getDeviceId())) {
                android.view.InputDevice device = event.getDevice();
                RunLog.line(String.format(java.util.Locale.ROOT,
                        "Controller motion: %s (sources 0x%x, this event's 0x%x) to the game directly",
                        device != null ? device.getName() : "?", device != null ? device.getSources() : 0,
                        event.getSource()));
            }
            try {
                org.libsdl.app.SDLControllerManager.handleJoystickMotionEvent(event);
                return true;
            } catch (RuntimeException notReady) {
                /* (SDL not set up yet: its own path, as before) */
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (gamepads != null) gamepads.focus(hasFocus);
        if (gyro != null) gyro.focus(hasFocus);
        if (!hasFocus && touchControls != null) touchControls.releaseAll();
        super.onWindowFocusChanged(hasFocus);
    }

    /**
     * Many phones drop the Wi-Fi's broadcast and multicast datagrams to
     * save power unless an app holds this: without it they would not see
     * system link games on the local network, nor be seen hosting one.
     */
    private void acquireMulticastLock() {
        try {
            WifiManager wifi = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wifi == null)
                return;
            multicastLock = wifi.createMulticastLock("halo-system-link");
            multicastLock.setReferenceCounted(false);
            multicastLock.acquire();
            RunLog.line("System Link Wi-Fi multicast lock acquired");
        } catch (RuntimeException e) {
            // (no Wi-Fi, or not allowed: the local network may miss games)
            multicastLock = null;
            RunLog.line("System Link multicast unavailable: " + e);
        }
    }

    /**
     * The game draws a frame at every display refresh, between its 30 Hz
     * ticks (port/linux/game/render_interpolation.c); Android otherwise
     * often keeps an app at 60 Hz on a faster display.
     */
    private void preferHighestRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode();
        Display.Mode best = current;

        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == current.getPhysicalWidth() &&
                mode.getPhysicalHeight() == current.getPhysicalHeight() &&
                mode.getRefreshRate() > best.getRefreshRate()) {
                best = mode;
            }
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        attributes.preferredDisplayModeId = best.getModeId();
        getWindow().setAttributes(attributes);
    }
}
