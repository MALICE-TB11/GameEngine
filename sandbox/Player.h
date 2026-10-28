#pragma once
// 玩家：胶囊角色 + 第三人称环绕相机。属于「玩法胶水」，所以放在 sandbox 而不是引擎模块里。
//
//   每帧      updateInput：读键鼠 → 相机转向、期望移动方向；跳跃按键锁存到下一个固定步
//   每固定步  fixedUpdate：期望速度平滑加速 → PhysicsWorld::moveCharacter（重力、台阶、贴地都在物理里处理）
//   渲染时    position(alpha) / cameraView(alpha)：在两个固定步之间插值，画面不随物理步长抖动
//
// 相机碰撞：从角色头顶向相机位置打一条射线，被地形或建筑挡住时把相机拉近，不会钻进山体里。
// EXTEND: M8 战斗时换成状态机（翻滚、攻击、受击），锁定目标时相机改为看向目标。

#include "math/MathTypes.h"
#include "physics/PhysicsWorld.h"

namespace eng {

class Input;

struct PlayerSettings {
    float walkSpeed = 4.0f;    // 米/秒
    float sprintSpeed = 7.5f;  // 按住 Shift
    float jumpSpeed = 5.0f;    // 起跳竖直速度（米/秒）：h = v²/2g ≈ 1.27 m
    float acceleration = 10.0f; // 速度趋近期望值的快慢（1/秒）：越大越「跟手」，越小越有惯性
    float cameraDistance = 5.0f;
    float cameraHeight = 1.5f; // 相机注视点（角色头部附近）离脚底的高度
    float mouseSensitivity = 0.15f; // 度/像素
    float minPitchDeg = -60.0f, maxPitchDeg = 70.0f;
    vec3 spawn{0.0f, 2.2f, 7.0f}; // 出生点（脚底）；掉出世界时回到这里
    float fallLimit = -80.0f;     // 低于这个高度视为掉出世界
};

class Player {
public:
    void init(physics::PhysicsWorld& world, const PlayerSettings& settings);
    void setSettings(const PlayerSettings& s) { m_settings = s; }

    // active = 游戏模式（接收输入）；编辑器模式下角色照常受重力，但不响应键鼠
    void updateInput(const Input& input, bool active);
    void fixedUpdate(float dt);
    void respawn();

    vec3 position(float alpha) const { return glm::mix(m_prevPos, m_currPos, alpha); }
    // 第三人称相机：返回视图矩阵与相机位置
    void cameraView(float alpha, mat4& outView, vec3& outEye) const;
    const physics::CharacterState& state() const { return m_state; }
    float yawDeg() const { return m_yawDeg; }

private:
    physics::PhysicsWorld* m_world = nullptr;
    physics::CharacterId m_character;
    PlayerSettings m_settings;
    physics::CharacterState m_state;
    vec3 m_prevPos{0.0f}, m_currPos{0.0f};
    vec3 m_velocity{0.0f};   // 平滑后的水平速度
    vec2 m_moveInput{0.0f};  // x = 右，y = 前（相机朝向下）
    bool m_sprint = false;
    bool m_jumpQueued = false;
    float m_yawDeg = 0.0f;   // 0 = 相机在角色 +Z 一侧、看向 -Z
    float m_pitchDeg = 20.0f;
    float m_distance = 5.0f;
};

} // namespace eng
