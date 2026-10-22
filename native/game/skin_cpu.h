// =====================================================================
// skin_cpu.h —— CPU 蒙皮（MeshRenderer 路线）
//
// 为什么要有它：我们现在用的是 MeshFilter + MeshRenderer（静态网格），
// Unity 不给它做蒙皮，所以骨骼怎么转，网格都不会变形。
// 这里自己做线性混合蒙皮：
//   skin_i  = T(worldPos_i) * R_world_i * T(-bindPos_i)
//   v'      = Σ w_k * skin_k * v
// 然后整块写回 Mesh.vertices。
//
// 输入只有一样：每根 MMD 骨骼的「世界旋转四元数」——翻译层从游戏骨骼抄来的。
// 层级、绑定位、父子关系全部由 PMX 数据自己推。
// =====================================================================
#pragma once
#include <cstdint>

namespace mmd { class Model; }

namespace game {
namespace skin {

// 只在模型加载完成后调一次。meshObj 是 UnityEngine.Mesh 的 Il2CppObject*。
bool init(const mmd::Model& model, void* meshObj);

// 翻译层每帧把游戏骨骼的旋转写进来（四元数 x,y,z,w）。索引是 PMX 骨骼号。
void set_bone_world_rot(int boneIndex, const float q[4]);
void clear_all_rot();          // 全部复位成单位旋转

// 重算所有顶点 + 上传。挂在 tick_body 里，每帧一次。
void update();

// 调试：返回上一次 update 花的时间（微秒）
int  last_update_us();
int  bone_count();
int  vertex_count();
bool ready();

}  // namespace skin
}  // namespace game
