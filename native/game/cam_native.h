// =====================================================================
// cam_native.h —— 直接读写 Transform 的 native 内存来搬相机
//
// 为什么不用 managed getter/setter：
//   build-155 实测，il2cpp_runtime_invoke 调 Transform.get_position
//   拿回来的 Vector3 是 (0,0,0)（相机明明在 (73,13,-494)）。
//   值类型装箱 + 调用约定在这条路上有坑，与其逐个数试，
//   不如直接读写 native 对象 —— managed Object+0x10 就是 m_CachedPtr。
//
// Unity Transform native 布局（2018.4，运行时探测确认）：
//   +0x10  localPosition (float3)
//   +0x20  localScale     (float3)
//   +0x30  localRotation  (float4)
//   +0x90  worldPosition 之类（cached）
// 这些偏移靠「读到的值是否合理」动态验证，不写死。
// =====================================================================
#pragma once

namespace game {
namespace cam_native {

// 给出相机 Transform（managed 对象）。返回 native 指针（= m_CachedPtr）。
void* attach(void* camTransform);

// 世界坐标读写。成功返回 true。
bool get_world_pos(void* tr, float out[3]);
bool set_world_pos(void* tr, const float p[3]);
bool get_world_rot(void* tr, float out[4]);
bool set_world_rot(void* tr, const float q[4]);

// 探测到的偏移（日志用）
int  off_pos();
int  off_rot();

}  // namespace cam_native
}  // namespace game
