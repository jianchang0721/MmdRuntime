// =====================================================================
// render_probe.h —— 渲染诊断
// =====================================================================
#pragma once

#include "self_smr.h"

namespace game {

// 初始化 hook
bool probe_init();

// 打印自建 SMR 状态
void probe_self_smr_state(SelfSmr& smr, void* meshObj);

// 每帧调用
void probe_frame();

}  // namespace game
