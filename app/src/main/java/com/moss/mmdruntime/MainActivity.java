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
    static final int REQ_TREE = 1001;

    TextView logView;
    ScrollView scroller;
    Button pickBtn;
    Button overlayBtn;

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

        Button authBtn = mkButton("③ root 授权检查");
        authBtn.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { checkRoot(); }
        });
        root.addView(authBtn, lp(-1, -2, 0, 0, 0, dp(14)));

        TextView tip = new TextView(this);
        tip.setText("菜单现在直接画在游戏画面里（hook eglSwapBuffers），启动游戏就出现，不用先开这个 App。\n"
                  + "面板挡着游戏就点右上角 X 收成小条，点小条再展开。\n"
                  + "导入模型时 KernelSU 会弹 root 授权，点允许。");
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

    // ---------------- root 检查 ----------------

    void checkRoot() {
        log("------ root 授权检查 ------");
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    String out = su("id -u");
                    if (out != null && out.contains("0")) {
                        ui("✓ 已有 root，KSU 已授权本应用");
                    } else {
                        ui("su 执行了但不是 root: " + out);
                        ui("去 KernelSU 管理器 → MmdRuntime → 允许");
                    }
                } catch (Throwable t) {
                    ui("✗ su 不可用: " + t.getClass().getSimpleName());
                    ui("打开 KernelSU 管理器 → 应用列表 → MmdRuntime → 开 root。");
                }
            }
        }).start();
    }

    void ui(final String s) {
        runOnUiThread(new Runnable() { @Override public void run() { log(s); } });
    }
}
