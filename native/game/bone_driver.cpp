// =====================================================================
// bone_driver.cpp —— 骨骼驱动：每帧读游戏骨骼，驱动 MMD 骨骼
//
// 原理（Delta 局部重定向）：
//   Delta_local = (游戏骨骼 bind 局部矩阵)⁻¹ × (游戏骨骼当前局部矩阵)
//   L_ours_now  = L_ours_bind × Delta_local
//   M_ours      = M_ours_parent × L_ours_now
//   skin        = M_ours × L_ours_bind⁻¹
//
// 游戏骨骼只读，不写。
// =====================================================================
#include "bone_driver.h"

#include <cstring>
#include <cmath>

#include "../logsink.h"
#include "../unity/il2cpp_api.h"
#include "../unity/unity_objects.h"

#define DTAG "MmdDriver"
#define DLOG(...) logsink::Write(__VA_ARGS__)

extern Il2CppApi g_api;

namespace game {

namespace {

// 游戏骨骼信息
struct GameBone {
    void*   transform = nullptr;   // 游戏骨骼 Transform
    int32_t instanceId = 0;
    float   bindPos[3] = {0};      // bind 位置（局部）
    float   bindRot[4] = {0,0,0,1}; // bind 旋转（四元数）
    float   bindScale[3] = {1,1,1}; // bind 缩放
    bool    valid = false;
};

// MMD 骨骼信息
struct MmdBone {
    int     pmxIndex = -1;
    int     parentPmx = -1;
    void*   transform = nullptr;
    int32_t instanceId = 0;
    float   bindPos[3] = {0};      // PMX 绝对坐标
    float   bindLocalPos[3] = {0}; // 相对父级的局部坐标
    bool    ok = false;
};

// 驱动器状态
struct BoneDriver {
    bool    initialized = false;
    
    // 游戏侧
    std::vector<GameBone> gameBones;
    
    // MMD 侧
    std::vector<MmdBone> mmdBones;
    int mmdBoneCount = 0;
    
    // 映射表：pmxIndex -> gameBoneIndex（-1 表示无映射）
    std::vector<int> boneMap;
    
    // 统计
    int frameCount = 0;
    int updatedBones = 0;
};

BoneDriver g_driver;

// 矩阵操作
void mat_identity(float m[16]) {
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void mat_mul(float r[16], const float a[16], const float b[16]) {
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r[i*4+j] = a[i*4+0]*b[0*4+j] + a[i*4+1]*b[1*4+j] + a[i*4+2]*b[2*4+j] + a[i*4+3]*b[3*4+j];
        }
    }
}

void mat_inverse(float inv[16], const float m[16]) {
    // 简化：只处理旋转+平移（正交矩阵）
    // 完整实现见 https://stackoverflow.com/a/1148405
    // 这里假设是刚体变换
    float t[16];
    // 转置旋转部分
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t[i*4+j] = m[j*4+i];
    t[3] = t[7] = t[11] = 0;
    t[12] = t[13] = t[14] = 0;
    t[15] = 1;
    // 平移取负
    float tx = -(m[12] * t[0] + m[13] * t[4] + m[14] * t[8]);
    float ty = -(m[12] * t[1] + m[13] * t[5] + m[14] * t[9]);
    float tz = -(m[12] * t[2] + m[13] * t[6] + m[14] * t[10]);
    t[12] = tx; t[13] = ty; t[14] = tz;
    memcpy(inv, t, 64);
}

// 从 Transform 读局部变换
bool read_transform(void* transform, float pos[3], float rot[4], float scale[3]) {
    if (!transform) return false;
    
    // 调 get_localPosition
    static void* mGetLocalPos = nullptr;
    static void* mGetLocalRot = nullptr;
    static void* mGetLocalScale = nullptr;
    
    if (!mGetLocalPos) {
        void* trCls = uo::u_class("UnityEngine", "Transform");
        if (!trCls) return false;
        mGetLocalPos = uo::u_method(trCls, "get_localPosition", 0);
        mGetLocalRot = uo::u_method(trCls, "get_localRotation", 0);
        mGetLocalScale = uo::u_method(trCls, "get_localScale", 0);
    }
    
    if (mGetLocalPos) {
        void* r = uo::u_call(transform, mGetLocalPos, nullptr);
        if (r) {
            float* v = (float*)((char*)r + 16);
            pos[0] = v[0]; pos[1] = v[1]; pos[2] = v[2];
        }
    }
    if (mGetLocalRot) {
        void* r = uo::u_call(transform, mGetLocalRot, nullptr);
        if (r) {
            float* q = (float*)((char*)r + 16);
            rot[0] = q[0]; rot[1] = q[1]; rot[2] = q[2]; rot[3] = q[3];
        }
    }
    if (mGetLocalScale) {
        void* r = uo::u_call(transform, mGetLocalScale, nullptr);
        if (r) {
            float* s = (float*)((char*)r + 16);
            scale[0] = s[0]; scale[1] = s[1]; scale[2] = s[2];
        }
    }
    return true;
}

// 写局部位置到 Transform
bool write_local_pos(void* transform, float x, float y, float z) {
    if (!transform) return false;
    static void* mSetLocalPos = nullptr;
    if (!mSetLocalPos) {
        void* trCls = uo::u_class("UnityEngine", "Transform");
        if (!trCls) return false;
        mSetLocalPos = uo::u_method(trCls, "set_localPosition", 1);
    }
    if (!mSetLocalPos) return false;
    
    float v[3] = {x, y, z};
    void* a[1] = { v };
    return uo::u_call_void(transform, mSetLocalPos, a);
}

// 四元数转矩阵
void quat_to_mat(float m[16], const float q[4]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float x2 = x+x, y2 = y+y, z2 = z+z;
    float xx = x*x2, xy = x*y2, xz = x*z2;
    float yy = y*y2, yz = y*z2, zz = z*z2;
    float wx = w*x2, wy = w*y2, wz = w*z2;
    
    m[0] = 1 - (yy + zz); m[1] = xy - wz;     m[2] = xz + wy;      m[3] = 0;
    m[4] = xy + wz;       m[5] = 1 - (xx+zz); m[6] = yz - wx;      m[7] = 0;
    m[8] = xz - wy;       m[9] = yz + wx;     m[10] = 1 - (xx+yy); m[11] = 0;
    m[12] = 0;            m[13] = 0;          m[14] = 0;           m[15] = 1;
}

// 矩阵转四元数（从旋转矩阵）
void mat_to_quat(float q[4], const float m[16]) {
    float tr = m[0] + m[5] + m[10];
    if (tr > 0) {
        float s = sqrtf(tr + 1.0f) * 2;
        q[3] = 0.25f * s;
        q[0] = (m[9] - m[6]) / s;
        q[1] = (m[2] - m[8]) / s;
        q[2] = (m[4] - m[1]) / s;
    } else if (m[0] > m[5] && m[0] > m[10]) {
        float s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2;
        q[3] = (m[9] - m[6]) / s;
        q[0] = 0.25f * s;
        q[1] = (m[1] + m[4]) / s;
        q[2] = (m[2] + m[8]) / s;
    } else if (m[5] > m[10]) {
        float s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2;
        q[3] = (m[2] - m[8]) / s;
        q[0] = (m[1] + m[4]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[6] + m[9]) / s;
    } else {
        float s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2;
        q[3] = (m[4] - m[1]) / s;
        q[0] = (m[2] + m[8]) / s;
        q[1] = (m[6] + m[9]) / s;
        q[2] = 0.25f * s;
    }
}

}  // namespace

bool bone_driver_init(const mmd::Model& model, const mmd::Skeleton& skeleton) {
    if (g_driver.initialized) return true;
    
    DLOG("[DRIVER] init: %zu mmd bones", model.bones.size());
    
    // 初始化 MMD 骨骼信息
    g_driver.mmdBones.resize(model.bones.size());
    g_driver.boneMap.resize(model.bones.size(), -1);
    
    for (size_t i = 0; i < model.bones.size(); i++) {
        const auto& b = model.bones[i];
        MmdBone& mb = g_driver.mmdBones[i];
        mb.pmxIndex = (int)i;
        mb.parentPmx = b.parent;
        mb.bindPos[0] = b.pos[0];
        mb.bindPos[1] = b.pos[1];
        mb.bindPos[2] = b.pos[2];
        mb.transform = skeleton.transform_at((int)i);
        mb.instanceId = skeleton.instance_at((int)i);
        mb.ok = (mb.transform != nullptr);
        
        // 计算局部坐标（相对父级）
        if (b.parent >= 0 && (size_t)b.parent < model.bones.size()) {
            const auto& p = model.bones[b.parent];
            mb.bindLocalPos[0] = b.pos[0] - p.pos[0];
            mb.bindLocalPos[1] = b.pos[1] - p.pos[1];
            mb.bindLocalPos[2] = b.pos[2] - p.pos[2];
        } else {
            mb.bindLocalPos[0] = b.pos[0];
            mb.bindLocalPos[1] = b.pos[1];
            mb.bindLocalPos[2] = b.pos[2];
        }
    }
    g_driver.mmdBoneCount = (int)model.bones.size();
    
    DLOG("[DRIVER] mmd bones initialized: %d", g_driver.mmdBoneCount);
    g_driver.initialized = true;
    return true;
}

void bone_driver_bind_game_bones(void** gameBoneTransforms, int count) {
    if (!g_driver.initialized) return;
    
    DLOG("[DRIVER] binding %d game bones (ptr)", count);
    g_driver.gameBones.resize(count);
    
    for (int i = 0; i < count; i++) {
        GameBone& gb = g_driver.gameBones[i];
        gb.transform = gameBoneTransforms[i];
        gb.valid = (gb.transform != nullptr);
        
        if (gb.valid) {
            // 读 bind 变换
            float pos[3], rot[4], scale[3];
            if (read_transform(gb.transform, pos, rot, scale)) {
                memcpy(gb.bindPos, pos, 12);
                memcpy(gb.bindRot, rot, 16);
                memcpy(gb.bindScale, scale, 12);
            }
        }
    }
    
    // 简单映射：按索引一一对应（假设游戏骨骼和 PMX 骨骼顺序一致）
    // 实际应该用名字匹配，这里先简化
    for (int i = 0; i < g_driver.mmdBoneCount && i < count; i++) {
        g_driver.boneMap[i] = i;
    }
    
    DLOG("[DRIVER] mapped %d bones", g_driver.mmdBoneCount);
}

void bone_driver_bind_game_bones_by_id(int32_t* gameBoneIds, int count) {
    if (!g_driver.initialized) return;
    
    DLOG("[DRIVER] binding %d game bones by id", count);
    g_driver.gameBones.resize(count);
    
    // 用 il2cpp API 把 instanceID 转成 Transform 指针
    // il2cpp_object_new 不行，得用 Object.FindObjectFromInstanceID
    static void* mFindObject = nullptr;
    if (!mFindObject) {
        void* objCls = uo::u_class("UnityEngine", "Object");
        if (objCls) {
            mFindObject = uo::u_method(objCls, "FindObjectFromInstanceID", 1);
        }
    }
    
    for (int i = 0; i < count; i++) {
        GameBone& gb = g_driver.gameBones[i];
        gb.instanceId = gameBoneIds[i];
        gb.valid = false;
        
        if (mFindObject && gb.instanceId != 0) {
            int32_t id = gb.instanceId;
            void* a[1] = { &id };
            void* obj = uo::u_call(nullptr, mFindObject, a);
            if (obj) {
                gb.transform = obj;
                gb.valid = true;
                
                // 读 bind 变换
                float pos[3], rot[4], scale[3];
                if (read_transform(gb.transform, pos, rot, scale)) {
                    memcpy(gb.bindPos, pos, 12);
                    memcpy(gb.bindRot, rot, 16);
                    memcpy(gb.bindScale, scale, 12);
                }
            }
        }
    }
    
    // 简单映射：按索引一一对应
    for (int i = 0; i < g_driver.mmdBoneCount && i < count; i++) {
        g_driver.boneMap[i] = i;
    }
    
    int validCount = 0;
    for (auto& gb : g_driver.gameBones) if (gb.valid) validCount++;
    DLOG("[DRIVER] mapped %d bones, %d valid", g_driver.mmdBoneCount, validCount);
}

void bone_driver_update() {
    if (!g_driver.initialized || g_driver.gameBones.empty()) return;
    
    g_driver.frameCount++;
    g_driver.updatedBones = 0;
    
    // 每帧更新
    for (int i = 0; i < g_driver.mmdBoneCount; i++) {
        MmdBone& mb = g_driver.mmdBones[i];
        if (!mb.ok) continue;
        
        int gameIdx = g_driver.boneMap[i];
        if (gameIdx < 0 || gameIdx >= (int)g_driver.gameBones.size()) continue;
        
        GameBone& gb = g_driver.gameBones[gameIdx];
        if (!gb.valid) continue;
        
        // 读游戏骨骼当前变换
        float curPos[3], curRot[4], curScale[3];
        if (!read_transform(gb.transform, curPos, curRot, curScale)) continue;
        
        // 计算 Delta（简化：只算位置差，旋转先忽略）
        // 完整实现应该算完整矩阵
        float deltaPos[3] = {
            curPos[0] - gb.bindPos[0],
            curPos[1] - gb.bindPos[1],
            curPos[2] - gb.bindPos[2]
        };
        
        // 应用到 MMD 骨骼
        float newPos[3] = {
            mb.bindLocalPos[0] + deltaPos[0],
            mb.bindLocalPos[1] + deltaPos[1],
            mb.bindLocalPos[2] + deltaPos[2]
        };
        
        write_local_pos(mb.transform, newPos[0], newPos[1], newPos[2]);
        g_driver.updatedBones++;
    }
    
    // 每 60 帧打一次日志
    if (g_driver.frameCount % 60 == 1) {
        DLOG("[DRIVER] frame %d: updated %d/%d bones", 
             g_driver.frameCount, g_driver.updatedBones, g_driver.mmdBoneCount);
    }
}

void bone_driver_reset() {
    g_driver = BoneDriver{};
}

}  // namespace game
