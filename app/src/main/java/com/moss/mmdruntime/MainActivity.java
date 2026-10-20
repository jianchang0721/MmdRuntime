package com.moss.mmdruntime;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;

/**
 * MmdRuntime 桌面端
 *
 * 三个功能：
 *   1. 开 ImGui 悬浮窗（App 自己进程里画，不注入游戏，游戏崩不了）
 *   2. 导入 MMD 模型文件夹（自动递归拷贝到游戏私有目录）
 *   3. root 授权自检
 */
public class MainActivity extends Activity {

    static final String GAME_PKG    = "com.hermes.h1game";
    static final String GAME_FILES  = "/data/data/com.hermes.h1game/files";
    static final String MODELS_DIR  = GAME_FILES + "/models";
    static final String ACTIVE_FILE = GAME_FILES + "/mmdrt_active.txt";
    static final String SO_DIR = GAME_FILES + "/so";
    static final int REQ_TREE = 1001;
    static final int REQ_SO   = 1003;

    TextView logView;
    ScrollView scroller;
    Button pickBtn;
    Button overlayBtn;
    Button soBtn;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFF101018);
        int pad = dp(18);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("MmdRuntime");
        title.setTextColor(0xFFB388FF);
        title.setTextSize(22);
        title.setGravity(Gravity.CENTER);
        root.addView(title, lp(-1, -2, 0, 0, 0, dp(2)));

        TextView sub = new TextView(this);
        sub.setText("MMD 运行时 · ImGui 悬浮菜单");
        sub.setTextColor(0xFF8890A8);
        sub.setTextSize(11);
        sub.setGravity(Gravity.CENTER);
        root.addView(sub, lp(-1, -2, 0, 0, 0, dp(16)));

        overlayBtn = mkButton("① 菜单在游戏里（点我看说明）");
        overlayBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { toggleOverlay(); }
        });
        root.addView(overlayBtn, lp(-1, -2, 0, 0, 0, dp(8)));

        pickBtn = mkButton("② 导入模型文件夹");
        pickBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pickFolder(); }
        });
        root.addView(pickBtn, lp(-1, -2, 0, 0, 0, dp(8)));

        soBtn = mkButton("③ 加载自定义 so");
        soBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pickSoFile(); }
        });
        root.addView(soBtn, lp(-1, -2, 0, 0, 0, dp(8)));

        Button authBtn = mkButton("④ 权限审查");
        authBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { checkPerms(); }
        });
        root.addView(authBtn, lp(-1, -2, 0, 0, 0, dp(14)));

        TextView tip = new TextView(this);
        tip.setText("菜单现在直接画在游戏画面里（hook eglSwapBuffers），启动游戏就出现，不用先开这个 App。\n"
                  + "面板挡着游戏就点右上角 X 收成小条，点小条再展开。\n"
                  + "权限审查查四项：root / LSPosed 启用 / 作用域 / so 是否就位。");
        tip.setTextColor(0xFF667088);
        tip.setTextSize(10);
        root.addView(tip, lp(-1, -2, 0, 0, 0, dp(12)));

        logView = new TextView(this);
        logView.setTextColor(0xFF7FD4FF);
        logView.setTextSize(10);
        logView.setTextIsSelectable(true);
        logView.setText("就绪。");

        scroller = new ScrollView(this);
        scroller.addView(logView);
        root.addView(scroller, lp(-1, 0, 0, 0, 0, 0));

        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
    }

    // ---------------- UI 小工具 ----------------

    int dp(int v) { return (int)(v * getResources().getDisplayMetrics().density + 0.5f); }

    LinearLayout.LayoutParams lp(int w, int h, int l, int t, int r, int bo) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(w, h);
        p.leftMargin = l; p.topMargin = t; p.rightMargin = r; p.bottomMargin = bo;
        if (h == 0) p.weight = 1;
        return p;
    }

    Button mkButton(String text) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextColor(0xFFD8CCFF);
        b.setTextSize(14);
        b.setAllCaps(false);
        GradientDrawable g = new GradientDrawable();
        g.setColor(0xFF2A1F4A);
        g.setCornerRadius(dp(14));
        g.setStroke(dp(1), 0xFF7C4DFF);
        b.setBackground(g);
        return b;
    }

    void log(String s) {
        logView.append("\n" + s);
        scroller.post(new Runnable() {
            @Override public void run() { scroller.fullScroll(View.FOCUS_DOWN); }
        });
    }

    // ---------------- 菜单说明 ----------------

    void toggleOverlay() {
        log("菜单在游戏里：LSPosed 勾上作用域后直接开游戏，面板自动画在游戏画面上。\n"
          + "面板右上角 X = 收成小条；点小条再展开。\n"
          + "这个 App 只负责导入模型，不用提前启动。");
    }

    // ---------------- 选文件夹 ----------------

    // ---------------- 加载自定义 so ----------------

    /**
     * 让玩家选一个 .so 文件。
     *
     * 落点：游戏私有目录 files/so/<原名>，之后 HookEntry 的
     * doLoad() 会按顺序 System.load 它（现在是最后一个加载，
     * 排在 libmmdruntime 和 libimgui_overlay 之后）。
     *
     * 走 ACTION_OPEN_DOCUMENT 而不是 ACTION_GET_CONTENT：
     * 前者返回的是持久的 tree 授权，能反复读同一个文件。
     */
    void pickSoFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/octet-stream");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(i, REQ_SO);
        } catch (Throwable t) {
            // 某些国产 ROM 的文档选择器对 octet-stream 支持不好，退回任意类型
            i.setType("*/*");
            try {
                startActivityForResult(i, REQ_SO);
            } catch (Throwable t2) {
                log("打不开文件选择器: " + t2);
            }
        }
    }

    void importSo(Uri soUri) {
        final String name = querySoName(soUri);
        if (name == null || name.isEmpty()) {
            log("读不到文件名");
            return;
        }
        // 安全：只收 .so，且挡掉路径穿越
        final String safe = sanitize(name);
        if (!safe.toLowerCase().endsWith(".so")) {
            log("只收 .so 文件，拿到的是: " + safe);
            return;
        }

        log("选中: " + safe);
        soBtn.setEnabled(false);
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    File stage = new File(getCacheDir(), "so");
                    deleteRec(stage);
                    if (!stage.mkdirs()) throw new Exception("建不了临时目录");
                    File local = new File(stage, safe);
                    InputStream in = getContentResolver().openInputStream(soUri);
                    if (in == null) throw new Exception("打不开选中的文件");
                    copyStream(in, local);

                    long sz = local.length();
                    if (sz < 4096) throw new Exception("文件太小 (" + sz + " 字节)，不像 so");
                    if (sz > 64 * 1024 * 1024) throw new Exception("文件太大 (" + sz / 1048576 + "MB)");

                    int uid = gameUid();
                    String dst = SO_DIR + "/" + safe;
                    StringBuilder cmd = new StringBuilder();
                    cmd.append("set -e; ");
                    cmd.append("mkdir -p '").append(SO_DIR).append("'; ");
                    cmd.append("rm -f '").append(dst).append("'; ");
                    cmd.append("cp '").append(local.getAbsolutePath()).append("' '").append(dst).append("'; ");
                    cmd.append("chown ").append(uid).append(":").append(uid).append(" '").append(dst).append("'; ");
                    cmd.append("chmod 755 '").append(dst).append("'; ");
                    cmd.append("ls -l '").append(dst).append("'");

                    String out = su(cmd.toString());
                    ui("已放到 " + dst);
                    if (out != null && !out.trim().isEmpty()) ui(out.trim());
                    ui("完成。重启游戏后 HookEntry 会加载它。");
                } catch (Throwable t) {
                    ui("失败: " + t);
                } finally {
                    ui2(new Runnable() { @Override public void run() { soBtn.setEnabled(true); } });
                }
            }
        }).start();
    }

    String querySoName(Uri uri) {
        android.database.Cursor c = null;
        try {
            c = getContentResolver().query(uri,
                    new String[]{android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null);
            if (c != null && c.moveToFirst()) return c.getString(0);
        } catch (Throwable ignored) {
        } finally {
            if (c != null) try { c.close(); } catch (Throwable ignored) { }
        }
        // 兜底：从路径尾巴猜
        String p = uri.getPath();
        if (p != null) {
            int i = p.lastIndexOf('/');
            if (i >= 0 && i + 1 < p.length()) return p.substring(i + 1);
        }
        return null;
    }

    void ui2(Runnable r) {
        runOnUiThread(r);
    }

    // ---------------- 选文件夹 ----------------

    void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            startActivityForResult(i, REQ_TREE);
        } catch (Throwable t) {
            log("打不开文件选择器: " + t);
        }
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);

        if (req == REQ_SO) {
            if (res != RESULT_OK || data == null || data.getData() == null) return;
            importSo(data.getData());
            return;
        }

        if (req != REQ_TREE || res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri tree = data.getData();
        log("选中: " + tree);
        pickBtn.setEnabled(false);
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    doImport(tree);
                } catch (Throwable t) {
                    ui("导入失败: " + t);
                } finally {
                    runOnUiThread(new Runnable() {
                        @Override public void run() { pickBtn.setEnabled(true); }
                    });
                }
            }
        }).start();
    }

    // ---------------- 导入 ----------------

    void doImport(Uri treeUri) throws Exception {
        String rootDocId = DocumentsContract.getTreeDocumentId(treeUri);
        String modelName = queryDisplayName(treeUri, rootDocId);
        if (modelName == null || modelName.isEmpty()) modelName = "model_" + System.currentTimeMillis();
        modelName = sanitize(modelName);
        ui("模型名: " + modelName);

        File cache = new File(getCacheDir(), "import");
        deleteRec(cache);
        File stage = new File(cache, modelName);
        if (!stage.mkdirs()) throw new Exception("无法创建临时目录");

        int[] cnt = new int[1];
        walk(treeUri, rootDocId, stage, cnt);
        ui("已读取 " + cnt[0] + " 个文件");

        if (!hasPmx(stage)) throw new Exception("这个文件夹里没有 .pmx 文件");

        int uid = gameUid();
        ui("游戏 uid = " + uid);

        String dst = MODELS_DIR + "/" + modelName;
        StringBuilder cmd = new StringBuilder();
        cmd.append("set -e; ");
        cmd.append("mkdir -p '").append(MODELS_DIR).append("'; ");
        cmd.append("rm -rf '").append(dst).append("'; ");
        cmd.append("cp -r '").append(stage.getAbsolutePath()).append("' '").append(MODELS_DIR).append("/'; ");
        cmd.append("chown -R ").append(uid).append(":").append(uid).append(" '").append(dst).append("'; ");
        cmd.append("chmod -R 755 '").append(dst).append("'; ");
        cmd.append("echo -n '").append(modelName).append("' > '").append(ACTIVE_FILE).append("'; ");
        cmd.append("chown ").append(uid).append(":").append(uid).append(" '").append(ACTIVE_FILE).append("'; ");
        cmd.append("chmod 644 '").append(ACTIVE_FILE).append("'; ");
        cmd.append("du -sh '").append(dst).append("'");

        String out = su(cmd.toString());
        ui("已写入游戏目录");
        if (out != null && !out.trim().isEmpty()) ui(out.trim());
        ui("完成。进游戏后悬浮菜单里能看到这个模型。");
    }

    void walk(Uri treeUri, String parentDocId, File dest, int[] cnt) throws Exception {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId);
        android.database.Cursor c = getContentResolver().query(children, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE
        }, null, null, null);
        if (c == null) return;
        try {
            while (c.moveToNext()) {
                String id   = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                if (name == null) continue;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    File sub = new File(dest, sanitize(name));
                    sub.mkdirs();
                    walk(treeUri, id, sub, cnt);
                } else {
                    Uri fu = DocumentsContract.buildDocumentUriUsingTree(treeUri, id);
                    InputStream in = getContentResolver().openInputStream(fu);
                    if (in == null) continue;
                    File out = new File(dest, sanitize(name));
                    copyStream(in, out);
                    cnt[0]++;
                }
            }
        } finally {
            c.close();
        }
    }

    String queryDisplayName(Uri treeUri, String docId) {
        Uri docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
        android.database.Cursor c = getContentResolver().query(docUri,
                new String[]{DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null, null);
        if (c == null) return null;
        try {
            if (c.moveToFirst()) return c.getString(0);
        } finally { c.close(); }
        return null;
    }

    void copyStream(InputStream in, File out) throws Exception {
        OutputStream os = new FileOutputStream(out);
        try {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
        } finally {
            try { in.close(); } catch (Throwable ignored) { }
            try { os.close(); } catch (Throwable ignored) { }
        }
    }

    boolean hasPmx(File dir) {
        File[] fs = dir.listFiles();
        if (fs == null) return false;
        for (File f : fs) {
            if (f.isDirectory()) { if (hasPmx(f)) return true; }
            else if (f.getName().toLowerCase().endsWith(".pmx")) return true;
        }
        return false;
    }

    static String sanitize(String s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '/' || c == '\\' || c == '\'' || c == '"' || c == '\n' || c == '\r' || c == 0) sb.append('_');
            else sb.append(c);
        }
        return sb.toString().trim();
    }

    void deleteRec(File f) {
        if (f == null || !f.exists()) return;
        File[] fs = f.listFiles();
        if (fs != null) for (File c : fs) deleteRec(c);
        f.delete();
    }

    int gameUid() {
        try {
            return getPackageManager().getApplicationInfo(GAME_PKG, 0).uid;
        } catch (Throwable t) {
            return 10314;
        }
    }

    String su(String cmd) throws Exception {
        Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
        StringBuilder sb = new StringBuilder();
        BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
        String line;
        while ((line = r.readLine()) != null) sb.append(line).append('\n');
        BufferedReader er = new BufferedReader(new InputStreamReader(p.getErrorStream()));
        while ((line = er.readLine()) != null) sb.append("[err] ").append(line).append('\n');
        p.waitFor();
        return sb.toString();
    }

    // ---------------- 权限审查 ----------------

    // 逐项查：root、LSPosed 模块启用、作用域、so 是否就位
    void checkPerms() {
        log("------ 权限审查 ------");
        checkRoot();
        checkLsposed();
        checkSoFiles();
    }

    void checkRoot() {
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    String out = su("id -u");
                    if (out != null && out.trim().startsWith("0")) {
                        ui("✓ root：已授权");
                    } else {
                        ui("✗ root：su 可用但不是 root (" + out.trim() + ")");
                        ui("  去 KernelSU 管理器 → MmdRuntime → 允许");
                    }
                } catch (Throwable t) {
                    ui("✗ root：su 不可用");
                    ui("  打开 KernelSU 管理器 → 应用 → MmdRuntime → 开 root");
                }
            }
        }).start();
    }

    // 走 lspd 的 cli（vector-cli）。root 已授权，直接问它最准。
    void checkLsposed() {
        new Thread(new Runnable() {
            @Override public void run() {
                String pkg = getPackageName();

                // 1) 模块启用状态
                try {
                    String out = su("/data/adb/lspd/cli --json modules ls");
                    if (out == null || out.trim().isEmpty()) {
                        ui("? LSPosed：cli 没输出，可能不是 LSPosed");
                        return;
                    }
                    // 找到我们自己那段："PACKAGE": "<pkg>" ... "STATUS": "enabled"
                    int i = out.indexOf("\"" + pkg + "\"");
                    if (i < 0) {
                        ui("✗ LSPosed：模块列表里没有 " + pkg);
                        ui("  装的是 LSPosed 吗？管理器里能看到 MmdRuntime 吗？");
                        return;
                    }
                    int j = out.indexOf("STATUS", i);
                    String status = "";
                    if (j > 0) {
                        int q1 = out.indexOf('"', out.indexOf(':', j));
                        int q2 = q1 > 0 ? out.indexOf('"', q1 + 1) : -1;
                        if (q2 > q1) status = out.substring(q1 + 1, q2);
                    }
                    if ("enabled".equals(status)) ui("✓ LSPosed：模块已启用");
                    else ui("✗ LSPosed：模块 " + (status.isEmpty() ? "状态未知" : status));

                    // 2) 作用域
                    String sc = su("/data/adb/lspd/cli scope ls " + pkg);
                    if (sc != null && sc.contains(GAME_PKG)) {
                        ui("✓ 作用域：已包含 " + GAME_PKG);
                    } else {
                        ui("✗ 作用域：没有 " + GAME_PKG);
                        ui("  LSPosed → 模块 → MmdRuntime → 作用域 → 勾选游戏");
                    }
                } catch (Throwable t) {
                    ui("✗ LSPosed 查询失败: " + t);
                }
            }
        }).start();
    }

    // 游戏私有目录里躺着的 so 对不对
    void checkSoFiles() {
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    String out = su("ls -l " + GAME_FILES + "/libmmdruntime.so " + GAME_FILES + "/*.so 2>/dev/null");
                    if (out == null || out.trim().isEmpty()) {
                        ui("✗ so：游戏目录里没有 libmmdruntime.so");
                        ui("  " + GAME_FILES + "/");
                        return;
                    }
                    boolean hasMain = out.contains("libmmdruntime.so");
                    if (hasMain) ui("✓ so：libmmdruntime.so 在");
                    else {
                        ui("✗ so：缺 libmmdruntime.so");
                        ui(out.trim());
                    }
                    // 历史残留提醒
                    int stale = 0;
                    for (String line : out.split("\n")) {
                        if (line.contains("libil2cpp_ext_") || line.contains("libmmdruntime_")
                                || line.contains("libmmdbg_")) stale++;
                    }
                    if (stale > 0) ui("! so：有 " + stale + " 个历史残留文件，建议清");
                } catch (Throwable t) {
                    ui("✗ so 查询失败: " + t);
                }
            }
        }).start();
    }

    void ui(final String s) {
        runOnUiThread(new Runnable() { @Override public void run() { log(s); } });
    }
}
