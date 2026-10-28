package com.halo.decomp;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;

/**
 * Virtuix Omni One treadmill: the game's side of Omni Connect's interface.
 *
 * Omni Connect (com.virtuix.omniconnect) runs a bound service whose first call, getSharedFd, returns a
 * 320-byte shared-memory region. The treadmill's movement is written into it, and a game that wants it
 * writes back its name, a heartbeat and the head's yaw (host_omni.c has the layout). This class binds the
 * service and gives the region's descriptor to the host; it does nothing until the game asks for it
 * (OMNI ONE in VR SETTINGS, vr.omni_treadmill), and nothing when Omni Connect is not installed.
 */
final class OmniOne {
    private static final String PACKAGE = "com.virtuix.omniconnect";
    private static final String SERVICE = PACKAGE + ".SharedMemoryService";
    private static final String DESCRIPTOR = PACKAGE + ".ISharedMemoryService";
    private static final int TRANSACTION_GET_SHARED_FD = 1;
    private static final long RETRY_MS = 3000;

    static native void nativeAttach(int fd);
    static native void nativeDetach();

    private static Context context;
    private static Handler handler;
    private static boolean wanted, bound;
    private static IBinder service;

    private static final ServiceConnection connection = new ServiceConnection() {
        @Override public void onServiceConnected(ComponentName name, IBinder binder) {
            synchronized (OmniOne.class) { service = binder; }
            RunLog.line("Omni One: bound to Omni Connect's service");
            handler.post(OmniOne::fetch);
        }

        @Override public void onServiceDisconnected(ComponentName name) {
            synchronized (OmniOne.class) { service = null; }
            nativeDetach();
            RunLog.line("Omni One: Omni Connect's service went away");
        }
    };

    private OmniOne() {}

    static synchronized void start(Context c) {
        if (wanted) return;
        wanted = true;
        context = c.getApplicationContext();
        if (handler == null) {
            HandlerThread thread = new HandlerThread("omni-one");
            thread.start();
            handler = new Handler(thread.getLooper());
        }
        handler.post(OmniOne::connect);
    }

    static synchronized void stop() {
        if (!wanted) return;
        wanted = false;
        if (handler != null) handler.removeCallbacksAndMessages(null);
        if (bound) {
            try { context.unbindService(connection); } catch (RuntimeException ignored) { }
            bound = false;
        }
        service = null;
        nativeDetach();
        RunLog.line("Omni One: stopped");
    }

    private static synchronized void connect() {
        if (!wanted || bound) return;
        Intent intent = new Intent();
        intent.setComponent(new ComponentName(PACKAGE, SERVICE));
        try {
            bound = context.bindService(intent, connection, Context.BIND_AUTO_CREATE);
        } catch (RuntimeException e) {
            RunLog.line("Omni One: could not bind (" + e + ")");
        }
        if (!bound) {
            RunLog.line("Omni One: Omni Connect is not available; trying again");
            handler.postDelayed(OmniOne::connect, RETRY_MS);
        }
    }

    // the region may not be ready yet (Omni Connect makes it as it starts)
    private static void fetch() {
        IBinder binder;
        synchronized (OmniOne.class) {
            if (!wanted) return;
            binder = service;
        }
        if (binder == null) return;
        ParcelFileDescriptor descriptor = null;
        Parcel data = Parcel.obtain(), reply = Parcel.obtain();
        try {
            data.writeInterfaceToken(DESCRIPTOR);
            binder.transact(TRANSACTION_GET_SHARED_FD, data, reply, 0);
            reply.readException();
            if (reply.readInt() != 0) descriptor = ParcelFileDescriptor.CREATOR.createFromParcel(reply);
        } catch (Exception e) {
            RunLog.line("Omni One: getSharedFd failed (" + e + ")");
        } finally {
            data.recycle();
            reply.recycle();
        }
        if (descriptor == null) {
            handler.postDelayed(OmniOne::fetch, RETRY_MS);
            return;
        }
        nativeAttach(descriptor.detachFd());
        RunLog.line("Omni One: shared memory attached");
    }
}
