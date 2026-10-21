// =====================================================================
// bone_driver.h —— 骨骼驱动
// =====================================================================
#pragma once

#include "../mmd/pmx.h"
#include "../mmd/skeleton.h"

namespace game {

// 初始化（在加载模型后调用）
bool bone_driver_init(const mmd::Model& model, const mmd::Skeleton& skeleton);

// 绑定游戏骨骼（Transform 数组）
void bone_driver_bind_game_bones(void** gameBoneTransforms, int count);

// 每帧更新（在渲染循环里调用）
void bone_driver_update();

// 重置
void bone_driver_reset();

}  // namespace game
