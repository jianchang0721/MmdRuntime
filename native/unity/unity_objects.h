// =====================================================================
// unity_objects.h —— Unity 侧对象的 C++ 封装
//
// 只干三件事：建 GameObject/Transform 层级、造 Mesh、拿 instanceID。
// 全部走 il2cpp 的 runtime_invoke，不依赖任何 managed 桩（那些被 strip 了）。
//
// 注意（踩过的坑）：
//   - Il2CppArray 不能缓存，GC 不认 C++ 侧的裸指针，几帧后回收再写必崩
//   - Vector3/Quaternion/BoneWeight/Matrix4x4 都是值类型，传参传栈上地址
//   - 建出来的对象要么挂进场景（Transform 被引用），要么马上设给 SMR，
//     否则可能在两次 tick 之间被 GC 收掉
// =====================================================================
#pragma once
#include <cstdint>

namespace uo {

// 依赖 il2cpp 已绑定（g_api 填好）。找到 UnityEngine 的类后返回 true。
bool init();

bool ready();

// ---- 层级 ----
// 建一个空 GameObject（带指定名字），挂到 parent 下（parent 为空则挂根），
// 设置 localPosition。返回它的 Transform 指针（Il2CppObject*）。
void* create_node(const char* name, void* parentTransform, float lx, float ly, float lz);

// 挂父级（内部走 Transform.set_parent(Transform)，worldPositionStays 默认 true，
// 所以调用后必须紧跟 set_local_position 重新写入局部坐标）
bool set_parent(void* transform, void* parentTransform);

// 只设 localPosition（已存在的 Transform）
bool set_local_position(void* transform, float x, float y, float z);
bool set_local_rotation(void* transform, float x, float y, float z, float w);
bool set_local_scale(void* transform, float x, float y, float z);

// ---- 属性 ----
int32_t instance_id(void* obj);          // Object.GetInstanceID()
const char* object_name(void* obj);      // Object.get_name()（返回静态缓冲，别长期持有）

// ---- 通用桥接（给 mesh 构建用）----
// 注意：这里都用 void*，不引 il2cpp_api.h 的类型，
// 免得在 namespace uo 里造出 uo::Il2CppClass 跟全局的打架
void* u_class(const char* ns, const char* name);            // 找类
void* u_method(void* klass, const char* name, int paramCount); // 找方法（按参数个数筛重载）

// 按「名字 + 参数个数 + 第一个参数类名」筛重载。
// Unity 的 .ctor / set_position / SetColor 之类重载极多，只按个数筛会撞错，
// 而撞错的表现是 runtime_invoke 不抛异常、安安静静返回 nullptr。
void* u_method_typed(void* klass, const char* name, int paramCount, const char* p0Class);
void* u_call(void* obj, void* method, void** args);        // runtime_invoke，返回 Il2CppObject*
void* u_new_array(void* elemClass, size_t count);          // il2cpp_array_new
void* u_array_data(void* arr);                             // 数组元素起始地址（跳过 32 字节头）
void* u_array_data_from(void* arr, size_t idx);            // 第 idx 个元素的地址
void* u_new_object(void* klass);                           // il2cpp_object_new

// ---- 诊断 ----
int created_nodes();

}  // namespace uo
