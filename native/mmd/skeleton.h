// =====================================================================
// skeleton.h —— PMX 骨骼 → Unity Transform 层级树
//
// 一根 PMX 骨骼对应一个 GameObject（只带 Transform，不加渲染组件）。
// localPosition = 自己绝对坐标 - 父级绝对坐标（PMX 存的是模型空间绝对坐标，
// 上一轮就栽在"沿父链重复累加"上，这次直接取差值）。
// =====================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "pmx.h"

namespace mmd {

struct BoneNode {
    int     pmxIndex  = -1;
    int     parentPmx = -1;
    void*   transform = nullptr;   // Il2CppObject* (UnityEngine.Transform)
    int32_t instanceId = 0;        // 写 m_Bones 用
    float   worldPos[3] = {0, 0, 0};
    bool    ok = false;
};

class Skeleton {
public:
    // rootParent：挂到哪个 Transform 下（游戏角色的节点）。传 nullptr 就是场景根。
    bool build(const Model& model, void* rootParent);

    void destroy();

    int count() const { return (int)nodes_.size(); }
    bool valid(int i) const { return i >= 0 && i < (int)nodes_.size() && nodes_[i].ok; }
    BoneNode& at(int i) { return nodes_[i]; }
    const BoneNode& at(int i) const { return nodes_[i]; }

    void*   transform_at(int i) const { return valid(i) ? nodes_[i].transform : nullptr; }
    int32_t instance_at(int i) const { return valid(i) ? nodes_[i].instanceId : 0; }

    // 统计：成功建的节点数 / 根节点数 / 最大层级深度
    int built() const { return built_; }
    int roots() const { return roots_; }
    int maxDepth() const { return maxDepth_; }

private:
    std::vector<BoneNode> nodes_;
    int built_ = 0;
    int roots_ = 0;
    int maxDepth_ = 0;
};

}  // namespace mmd
