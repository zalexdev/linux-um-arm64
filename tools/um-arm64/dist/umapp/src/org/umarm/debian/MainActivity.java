package org.umarm.debian;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.system.Os;
import android.system.OsConstants;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup.LayoutParams;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import android.net.LocalServerSocket;
import android.net.LocalSocket;
import java.io.FileDescriptor;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.zip.GZIPInputStream;

/**
 * A Linux kernel, running inside an ordinary Android app.
 *
 * This exists to move the port out of the adb shell and into the app sandbox,
 * which is a materially harder environment and the one that actually matters
 * for shipping. The differences that shape this class:
 *
 *   - An app runs in the untrusted_app SELinux domain under a W^X policy. It
 *     may not execute anything it wrote itself, so every binary -- the kernel,
 *     the stub, passt, umnet, umusb -- ships as a lib*.so inside the APK and
 *     is run from getApplicationInfo().nativeLibraryDir, the one directory an
 *     app is allowed to execute from.
 *   - UML normally execs its stub from an anonymous memfd. That is refused
 *     here, so the kernel is passed stub_exe=<nativeLibraryDir>/libstub.so.
 *   - There is no /tmp. Guest RAM is backed from the app's cache directory.
 *
 * The UI is deliberately plain: a scrolling log, a line-input box wired to the
 * guest's stdin, and the few settings worth changing while debugging. It is a
 * console, not a terminal emulator -- there is no ANSI or cursor handling,
 * because the point is to see what the kernel says, not to run vim.
 */
public class MainActivity extends Activity {

    private TextView log;
    private ScrollView scroll;
    private EditText input;
    private EditText memField;
    private CheckBox seccompBox;
    private CheckBox netBox;
    private CheckBox usbBox;
    private Button startStop;

    private Process guest;
    private OutputStream guestIn;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private static final int MAX_CHARS = 200000;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(12, 12, 12, 12);

        /* --- settings row --- */
        LinearLayout settings = new LinearLayout(this);
        settings.setOrientation(LinearLayout.HORIZONTAL);
        settings.setGravity(Gravity.CENTER_VERTICAL);

        TextView memLabel = new TextView(this);
        memLabel.setText("RAM MB ");
        settings.addView(memLabel);

        memField = new EditText(this);
        memField.setInputType(InputType.TYPE_CLASS_NUMBER);
        memField.setText("512");
        memField.setWidth(200);
        settings.addView(memField);

        seccompBox = new CheckBox(this);
        seccompBox.setText("seccomp");
        seccompBox.setChecked(true);
        settings.addView(seccompBox);

        netBox = new CheckBox(this);
        netBox.setText("net");
        netBox.setChecked(true);
        settings.addView(netBox);

        /*
         * Off by default: most runs have nothing plugged into the phone, and
         * the USB path costs a permission dialog before the guest starts.
         */
        usbBox = new CheckBox(this);
        usbBox.setText("usb");
        usbBox.setChecked(false);
        settings.addView(usbBox);

        /* Scrolls sideways, because a LinearLayout does not shrink: with this
         * many controls a narrow screen would put the last checkbox past the
         * right edge, where it cannot be tapped at all. */
        HorizontalScrollView settingsScroll = new HorizontalScrollView(this);
        settingsScroll.addView(settings);
        root.addView(settingsScroll, new LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        /* --- start/stop --- */
        startStop = new Button(this);
        startStop.setText("Boot");
        startStop.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (guest == null) boot(); else stop();
            }
        });
        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(startStop, new LinearLayout.LayoutParams(0,
                LayoutParams.WRAP_CONTENT, 1.0f));

        /*
         * The seccomp filter zygote installs on this app is inherited by every
         * process it spawns, including the kernel -- which is why the guest
         * dies with SIGSYS here but not under `run-as`. This button runs a
         * probe under that same filter and prints which syscalls it forbids,
         * because "something was denied" is not something you can act on.
         */
        Button probe = new Button(this);
        probe.setText("Probe");
        probe.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { runProbe(); }
        });
        buttons.addView(probe, new LinearLayout.LayoutParams(0,
                LayoutParams.WRAP_CONTENT, 1.0f));

        root.addView(buttons, new LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        /* --- console --- */
        log = new TextView(this);
        log.setTypeface(Typeface.MONOSPACE);
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 10);
        log.setTextIsSelectable(true);
        log.setBackgroundColor(Color.BLACK);
        log.setTextColor(Color.GREEN);

        scroll = new ScrollView(this);
        scroll.addView(log);
        root.addView(scroll, new LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, 0, 1.0f));

        /* --- input line, wired to the guest's stdin --- */
        input = new EditText(this);
        input.setHint("type a command, Enter to send");
        input.setSingleLine(true);
        input.setImeOptions(EditorInfo.IME_ACTION_SEND);
        input.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override public boolean onEditorAction(TextView v, int id, android.view.KeyEvent e) {
                send(input.getText().toString());
                input.setText("");
                return true;
            }
        });
        root.addView(input, new LinearLayout.LayoutParams(
                LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT));

        setContentView(root);

        append("umarm -- Linux/UML on aarch64, inside an Android app\n");
        append("uid " + android.os.Process.myUid() + "\n");
        append("libdir " + getApplicationInfo().nativeLibraryDir + "\n\n");

        usbAttachNotice(getIntent());
    }

    /*
     * singleTop in the manifest, so plugging the adapter in while the guest is
     * running delivers the attach intent here instead of stacking a second
     * Activity on top of a boot in progress.
     */
    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        setIntent(i);
        usbAttachNotice(i);
    }

    /*
     * Deliberately one level of anonymous class, not two: a Runnable posting
     * another Runnable produces a MainActivity$N$1 class file that d8 8.2
     * rejects with an internal NullPointerException, which reads like a
     * compiler bug rather than anything to do with this code.
     */
    private java.io.FileOutputStream logFile;
    private LocalServerSocket usbServer;

    private final class Appender implements Runnable {
        private final String s;

        Appender(String s) { this.s = s; }

        @Override public void run() {
            log.append(s);
            /* Keep the buffer bounded: a kernel log will happily grow until
             * the TextView becomes unusable. */
            if (log.length() > MAX_CHARS) {
                CharSequence c = log.getText();
                log.setText(c.subSequence(c.length() - MAX_CHARS / 2, c.length()));
            }
            scroll.fullScroll(View.FOCUS_DOWN);
        }
    }

    /**
     * Everything shown in the terminal is also written to files/boot.log.
     *
     * The on-screen view is a bounded TextView and the interesting part of a
     * failed boot is usually the part that has already scrolled away, which
     * leaves screenshots as the only record -- unreadable past a few hundred
     * lines, and impossible to grep. The file is the app's own private
     * storage, so `adb shell run-as org.umarm.debian cat files/boot.log`
     * retrieves it without root.
     */
    private void logToFile(String s) {
        try {
            if (logFile == null) {
                logFile = new java.io.FileOutputStream(
                        new java.io.File(getFilesDir(), "boot.log"), true);
            }
            logFile.write(s.getBytes("UTF-8"));
            logFile.flush();
        } catch (Exception e) {
            /* Never let logging break the boot it is meant to record. */
        }
    }

    private void append(String s) {
        logToFile(s);
        ui.post(new Appender(s));
    }

    /**
     * Unpack a gzipped asset once, into the app's private files directory.
     * Data, not code -- so it may live here; it just may not be executed.
     */
    private File unpackAsset(String asset, String outName) throws IOException {
        File out = new File(getFilesDir(), outName);
        if (out.exists() && out.length() > 0)
            return out;

        append("unpacking " + asset + " ...\n");
        InputStream in = getAssets().open(asset);
        if (asset.endsWith(".gz"))
            in = new GZIPInputStream(in, 65536);
        FileOutputStream fos = new FileOutputStream(out);
        byte[] buf = new byte[1 << 16];
        long total = 0;
        int n;
        while ((n = in.read(buf)) > 0) {
            fos.write(buf, 0, n);
            total += n;
        }
        fos.close();
        in.close();
        append("unpacked " + (total >> 20) + " MB\n");
        return out;
    }

    private void runProbe() {
        new Thread(new ProbeRunner()).start();
    }

    private final class ProbeRunner implements Runnable {
        @Override public void run() {
            try {
                File p = new File(getApplicationInfo().nativeLibraryDir, "libappseccomp.so");
                if (!p.exists()) {
                    append("probe not in this build\n");
                    return;
                }
                /* The probe is a tracer: run the kernel under it, inside the
                 * app's own seccomp filter, so a kill that leaves no output
                 * still yields a signal and a PC. */
                File k = new File(getApplicationInfo().nativeLibraryDir, "libuml.so");
                File probe = new File(getApplicationInfo().nativeLibraryDir,
                        "libprobe.so");
                ProcessBuilder pb = probe.exists()
                        ? new ProcessBuilder(probe.getAbsolutePath())
                        : new ProcessBuilder(p.getAbsolutePath(),
                                k.getAbsolutePath(), "--version");
                pb.redirectErrorStream(true);
                Process pr = pb.start();
                BufferedReader r = new BufferedReader(
                        new InputStreamReader(pr.getInputStream()), 8192);
                String line;
                while ((line = r.readLine()) != null)
                    append(line + "\n");
                append("[probe exited " + pr.waitFor() + "]\n");
            } catch (Exception e) {
                append("probe error: " + e + "\n");
            }
        }
    }

    /* ------------------------------------------------------------------
     * USB passthrough.
     *
     * An app may not open /dev/bus/usb -- the untrusted_app domain has no
     * access to the device nodes at all. What it may do is ask the user for
     * permission to one device and be handed an already-open usbfs descriptor
     * for it, and USBDEVFS_* ioctls work on that descriptor. libusb reaches
     * Android hardware the same way (libusb_wrap_sys_device); umusb uses it to
     * serve USB/IP to the guest's vhci-hcd.
     * ------------------------------------------------------------------ */

    /*
     * The Realtek RTL8811AU that the guest's in-tree rtw88 8821au driver
     * binds. res/xml/device_filter.xml carries the same pair in decimal,
     * 9047/287, and the two have to be changed together.
     */
    private static final int USB_VENDOR = 0x2357;
    private static final int USB_PRODUCT = 0x011f;

    /* Our own action: the permission answer arrives as a broadcast. */
    private static final String ACTION_USB_PERMISSION =
            "org.umarm.debian.USB_PERMISSION";

    /* Written by the boot thread, read by the receiver on the main one. */
    private volatile CountDownLatch usbGrant;
    private ParcelFileDescriptor usbPfd;

    private final class UsbPermission extends BroadcastReceiver {
        @Override public void onReceive(Context c, Intent i) {
            CountDownLatch l = usbGrant;

            if (l != null)
                l.countDown();
        }
    }

    private static String usbId(int vendor, int product) {
        return String.format(Locale.US, "%04x:%04x", vendor, product);
    }

    private static String usbId(UsbDevice d) {
        return usbId(d.getVendorId(), d.getProductId());
    }

    /**
     * Acquire the adapter and return a descriptor umusb can use, or -1.
     *
     * Called from the boot thread and blocks there while the permission dialog
     * is up: the answer is a broadcast, which is delivered on the main thread,
     * so waiting for it on the main thread would deadlock.
     *
     * Every exit prints why. A USB failure that says nothing is
     * indistinguishable from a kernel that never booted.
     */
    private int usbOpen() {
        usbRelease();

        UsbManager um = (UsbManager)getSystemService(Context.USB_SERVICE);
        if (um == null) {
            append("usb: no UsbManager -- this phone has no USB host mode\n");
            return -1;
        }

        UsbDevice dev = null;
        HashMap<String, UsbDevice> devs = um.getDeviceList();
        for (UsbDevice d : devs.values()) {
            append("usb: sees " + usbId(d) + " " + d.getDeviceName() + "\n");
            if (d.getVendorId() == USB_VENDOR && d.getProductId() == USB_PRODUCT)
                dev = d;
        }
        if (dev == null) {
            append("usb: no " + usbId(USB_VENDOR, USB_PRODUCT) + " adapter"
                    + " among " + devs.size() + " attached device(s);"
                    + " booting without USB\n");
            return -1;
        }

        /*
         * Already granted if the adapter is what launched us: the platform
         * gives an app permission to the device that matched its
         * USB_DEVICE_ATTACHED filter, and keeps it until the device is
         * unplugged.
         */
        if (!um.hasPermission(dev)) {
            append("usb: asking for permission to " + usbId(dev) + " ...\n");
            usbGrant = new CountDownLatch(1);

            UsbPermission recv = new UsbPermission();
            IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);

            try {
                /*
                 * targetSdk 34 refuses an unflagged registerReceiver for an
                 * action the system does not own. The grant is sent through
                 * our own PendingIntent, so it arrives under our uid and
                 * NOT_EXPORTED still receives it -- while EXPORTED would let
                 * any other app on the phone forge one.
                 */
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                    registerReceiver(recv, filter,
                            Context.RECEIVER_NOT_EXPORTED);
                else
                    registerReceiver(recv, filter);

                try {
                    Intent i = new Intent(ACTION_USB_PERMISSION)
                            .setPackage(getPackageName());
                    /*
                     * From API 31 getBroadcast() throws unless it is told
                     * IMMUTABLE or MUTABLE. Immutable is the safe one, and the
                     * price is that the system cannot fill
                     * EXTRA_PERMISSION_GRANTED into the intent it sends back:
                     * an immutable PendingIntent ignores the fill-in intent.
                     * So the broadcast is only a wake-up and hasPermission()
                     * below is the answer.
                     */
                    int flags = Build.VERSION.SDK_INT >= Build.VERSION_CODES.S
                            ? PendingIntent.FLAG_IMMUTABLE : 0;

                    um.requestPermission(dev, PendingIntent.getBroadcast(
                            this, 0, i, flags));
                    if (!usbGrant.await(60, TimeUnit.SECONDS))
                        append("usb: the permission dialog went unanswered"
                                + " for 60s\n");
                } finally {
                    unregisterReceiver(recv);
                }
            } catch (Exception e) {
                append("usb: permission request failed: " + e + "\n");
                return -1;
            } finally {
                usbGrant = null;
            }

            if (!um.hasPermission(dev)) {
                append("usb: permission denied; booting without USB\n");
                return -1;
            }
            append("usb: permission granted\n");
        }

        UsbDeviceConnection conn = um.openDevice(dev);
        if (conn == null) {
            append("usb: openDevice(" + usbId(dev) + ") failed"
                    + " -- unplugged since the scan?\n");
            return -1;
        }
        try {
            /*
             * UsbDeviceConnection owns the descriptor and closes it from its
             * finalize(), which would pull it out from under umusb as soon as
             * the object became garbage. Dup it instead: the copy names the
             * same open usbfs file -- same claimed interfaces, same URB queue
             * -- and nothing but this class can close it.
             */
            ParcelFileDescriptor theirs =
                    ParcelFileDescriptor.adoptFd(conn.getFileDescriptor());
            try {
                usbPfd = ParcelFileDescriptor.dup(theirs.getFileDescriptor());
            } finally {
                /* adoptFd() was only a handle to dup from; the descriptor
                 * itself still belongs to the connection. */
                theirs.detachFd();
            }
            /*
             * Descriptors that arrive over binder come with O_CLOEXEC, and
             * ParcelFileDescriptor.dup() puts it back on the copy. Left set,
             * the number on umusb's command line would name a descriptor that
             * exec had already closed.
             */
            Os.fcntlInt(usbPfd.getFileDescriptor(), OsConstants.F_SETFD, 0);
        } catch (Exception e) {
            append("usb: cannot take over the descriptor: " + e + "\n");
            usbRelease();
            conn.close();
            return -1;
        }
        /* The dup holds the usbfs file open; this only drops the framework's
         * own reference to it. */
        conn.close();

        int fd = usbPfd.getFd();

        append("usb: " + usbId(dev) + " " + dev.getDeviceName()
                + " opened, fd " + fd + "\n");
        return fd;
    }

    /*
     * Hand the usbfs descriptor to umusb over an abstract unix socket.
     *
     * Not on the command line: measured on this device, a descriptor this app
     * opens does not survive the spawn -- "a child's /proc/self/fd has no
     * /dev/bus/usb entry" -- because ART's process spawn closes everything
     * above stderr in the child before exec, as OpenJDK's childproc.c does.
     * FD_CLOEXEC is irrelevant to that; the descriptor has to be sent, not
     * named. SCM_RIGHTS installs a fresh one in umusb's table.
     *
     * Abstract namespace: a socket file under the app's own directory is not
     * something an exec'd child of the app is allowed to connect to, and there
     * is nothing to clean up afterwards.
     */
    private void usbServe(final String name) {
        try {
            usbServer = new LocalServerSocket(name);
        } catch (IOException e) {
            append("usb: cannot listen on @" + name + ": " + e + "\n");
            return;
        }
        new Thread(new UsbServer(name)).start();
    }

    private final class UsbServer implements Runnable {
        private final String name;

        UsbServer(String name) { this.name = name; }

        @Override public void run() {
            try {
                LocalSocket s = usbServer.accept();
                FileDescriptor[] fds = { usbPfd.getFileDescriptor() };

                s.setFileDescriptorsForSend(fds);
                s.getOutputStream().write(0);
                s.getOutputStream().flush();
                s.close();
                append("usb: descriptor sent to umusb\n");
            } catch (Exception e) {
                append("usb: handing over the descriptor failed: " + e + "\n");
            }
        }
    }

    private void usbRelease() {
        if (usbPfd == null)
            return;
        try {
            usbPfd.close();
        } catch (IOException e) {
            /* Deliberately dropping this descriptor; a failed close is not
             * something the caller can act on. */
        }
        usbPfd = null;
    }

    /**
     * Ask a child process what it inherited, and say so.
     *
     * ProcessBuilder is not execve(): OpenJDK's child closes every descriptor
     * above stderr before it execs, and libcore's process code descends from
     * it. Measured on this project's build machine (JDK 21): a descriptor with
     * FD_CLOEXEC clear is listed in the JVM's own /proc/self/fd and absent
     * from the child's. Whether ART does the same is a question only the phone
     * can answer -- and if it does, umusb gets EBADF on --usbfd and every URB
     * fails with nothing in the log pointing at the cause. So one child, once,
     * to make the answer part of the boot log.
     */
    /*
     * Does a descriptor we opened reach a child across exec?
     *
     * Clearing FD_CLOEXEC is necessary and may not be sufficient: OpenJDK's
     * childproc.c, which ART's ProcessImpl descends from, walks /proc/self/fd
     * in the child and closes everything above stderr before exec'ing. Whether
     * this particular Android does that is a question about the device, so ask
     * the device: spawn a child the same way the kernel is spawned and see
     * whether the usbfs descriptor is in its table.
     */
    private boolean usbCheckInherit(int fd) {
        try {
            Process p = new ProcessBuilder("/system/bin/ls", "-l",
                    "/proc/self/fd").redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(
                    new InputStreamReader(p.getInputStream()), 4096);
            String seen = null;
            String line;

            while ((line = r.readLine()) != null)
                if (line.contains("/dev/bus/usb"))
                    seen = line.trim();
            p.waitFor();

            if (seen != null) {
                append("usb: descriptor survives exec (" + seen + ")\n");
                return true;
            }
            append("usb: a child's /proc/self/fd has no /dev/bus/usb entry --"
                    + " fd " + fd + " would reach umusb closed\n");
            return false;
        } catch (Exception e) {
            append("usb: inheritance check did not run: " + e + "\n");
            return false;
        }
    }

    /*
     * USB_DEVICE_ATTACHED started or resumed us. Worth a line: it is the one
     * route to the adapter that needs no permission dialog.
     */
    private void usbAttachNotice(Intent i) {
        if (i == null ||
            !UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(i.getAction()))
            return;

        UsbDevice d = (UsbDevice)i.getParcelableExtra(UsbManager.EXTRA_DEVICE);

        append("usb: " + (d != null ? usbId(d) : "a device")
                + " attached -- tick 'usb' and Boot\n");
    }

    private void boot() {
        startStop.setText("Stop");
        new Thread(new Runnable() { @Override public void run() { bootThread(); } }).start();
    }

    private void bootThread() {
        try {
            String libdir = getApplicationInfo().nativeLibraryDir;
            File kernel = new File(libdir, "libuml.so");
            File stub = new File(libdir, "libstub.so");
            File umnet = new File(libdir, "libumnet.so");
            File passt = new File(libdir, "libpasst.so");
            File umusb = new File(libdir, "libumusb.so");

            if (!kernel.exists()) {
                append("ERROR: " + kernel + " missing from the APK\n");
                return;
            }

            File initrd = unpackAsset("initramfs.cpio.gz", "initramfs.cpio.gz");
            File rootfs = null;
            try {
                rootfs = unpackAsset("rootfs.ext4.gz", "rootfs.ext4");
            } catch (IOException e) {
                /* Optional: the smaller build ships only an initramfs. */
            }

            int usbfd = -1;
            if (usbBox.isChecked()) {
                if (!umusb.exists())
                    append("usb: libumusb.so is not in this build;"
                            + " booting without USB\n");
                else
                    usbfd = usbOpen();
            }

            List<String> cmd = new ArrayList<String>();
            boolean net = netBox.isChecked() && umnet.exists() && passt.exists();
            if (net) {
                cmd.add(umnet.getAbsolutePath());
                cmd.add("--passt");
                cmd.add(passt.getAbsolutePath());
                cmd.add("--");
            }
            /*
             * umusb nests inside umnet: each wrapper execs whatever follows
             * its own --, so with both on, the kernel runs under both and ends
             * up with the vec0 argument umnet appends as well as whatever
             * umusb appends. The descriptor is passed by number, which is why
             * usbOpen() had to clear FD_CLOEXEC on it.
             */
            if (usbfd >= 0) {
                String sock = "umarm-usb-" + android.os.Process.myPid();

                usbServe(sock);
                cmd.add(umusb.getAbsolutePath());
                cmd.add("--usbsock");
                cmd.add(sock);
                /*
                 * The guest cannot attach to a socket of ours: vhci's
                 * attach_store() resolves the number it is given in the fd
                 * table of the process writing to sysfs, which is a guest
                 * process. So umusb listens instead, and the guest connects
                 * out through passt -- which is why usb needs net.
                 */
                cmd.add("--listen");
                cmd.add("127.0.0.1:3240");
                cmd.add("--");
            }
            cmd.add(kernel.getAbsolutePath());
            cmd.add("mem=" + memField.getText().toString() + "M");
            cmd.add("panic=-1");
            cmd.add("con=null");
            cmd.add("con0=fd:0,fd:1");
            /* The whole reason this app can work at all: exec the stub from
             * the APK's library directory rather than from a memfd. */
            cmd.add("stub_exe=" + stub.getAbsolutePath());
            cmd.add("seccomp=" + (seccompBox.isChecked() ? "auto" : "off"));
            if (rootfs != null && rootfs.exists()) {
                cmd.add("ubd0=" + rootfs.getAbsolutePath());
                cmd.add("root=/dev/ubda");
                cmd.add("rw");
                cmd.add("init=/umarm-init");
                cmd.add("umarm.share=" + getFilesDir().getAbsolutePath());
            } else {
                cmd.add("initrd=" + initrd.getAbsolutePath());
                cmd.add("init=/init");
            }

            append("$ " + join(cmd) + "\n\n");

            ProcessBuilder pb = new ProcessBuilder(cmd);
            pb.redirectErrorStream(true);
            pb.directory(getFilesDir());
            /* No /tmp in an app sandbox; guest RAM is backed from the cache
             * directory instead. HOME is where UML puts its umid. */
            pb.environment().put("TMPDIR", getCacheDir().getAbsolutePath());
            pb.environment().put("HOME", getFilesDir().getAbsolutePath());

            guest = pb.start();
            guestIn = guest.getOutputStream();

            BufferedReader r = new BufferedReader(
                    new InputStreamReader(guest.getInputStream()), 8192);
            String line;
            while ((line = r.readLine()) != null)
                append(line + "\n");

            int rc = guest.waitFor();
            append("\n[guest exited, status " + rc + "]\n");
        } catch (Exception e) {
            append("\nERROR: " + e + "\n");
        } finally {
            guest = null;
            guestIn = null;
            /* Hand the adapter back. Interfaces claimed through this usbfs
             * file stay claimed for as long as any descriptor on it is open,
             * so holding the dup after the guest is gone would leave the
             * device claimed by nobody in particular. */
            usbRelease();
            ui.post(new Runnable() {
                @Override public void run() { startStop.setText("Boot"); }
            });
        }
    }

    private void send(String s) {
        if (guestIn == null) {
            append("[not running]\n");
            return;
        }
        try {
            guestIn.write((s + "\n").getBytes("UTF-8"));
            guestIn.flush();
        } catch (IOException e) {
            append("[write failed: " + e + "]\n");
        }
    }

    private void stop() {
        if (guest != null)
            guest.destroy();
    }

    private static String join(List<String> l) {
        StringBuilder b = new StringBuilder();
        for (String s : l) { b.append(s); b.append(' '); }
        return b.toString().trim();
    }
}
