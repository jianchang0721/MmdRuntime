package com.moss.mmdruntime;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        TextView tv = new TextView(this);
        tv.setText("MmdRuntime 0.1\n\n"
                + "独立 MMD 运行时，跟游戏骨骼完全解耦。\n\n"
                + "当前阶段：PMX 解析器验证。\n\n"
                + "用法：\n"
                + "1. 把模型拷到\n"
                + "   /data/data/com.hermes.h1game/files/nangongyu.pmx\n"
                + "2. 把 libmmdruntime.so 拷到同目录\n"
                + "3. 启动游戏，日志写在\n"
                + "   /data/data/com.hermes.h1game/files/mmdruntime.log\n");
        tv.setTextSize(14);
        tv.setPadding(40, 80, 40, 40);
        setContentView(tv);
    }
}
