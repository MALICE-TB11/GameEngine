#include "Player.h"

#include "core/Log.h"
#include "platform/Input.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>

namespace eng {

void Player::init(physics::PhysicsWorld& world, const PlayerSettings& settings) {
    m_world = &world;
    m_settings = settings;
    m_distance = settings.cameraDistance;
    m_character = world.createCharacter({}, settings.spawn);
    m_prevPos = m_currPos = settings.spawn;
}

void Player::respawn() {
    if (!m_world) return;
    m_world->teleportCharacter(m_character, m_settings.spawn);
    m_prevPos = m_currPos = m_settings.spawn;
    m_velocity = vec3(0.0f);
}

void Player::updateInput(const Input& input, bool active) {
    if (!active) {
        m_moveInput = vec2(0.0f);
        m_sprint = false;
        return;
    }
    // 鼠标：环绕相机（光标被捕获时 mouseDelta 是原始位移）
    const vec2 md = input.mouseDelta();
    m_yawDeg -= md.x * m_settings.mouseSensitivity;
    m_pitchDeg = std::clamp(m_pitchDeg + md.y * m_settings.mouseSensitivity, m_settings.minPitchDeg, m_settings.maxPitchDeg);
    m_distance = std::clamp(m_distance - input.scroll() * 0.5f, 1.5f, 15.0f);

    m_moveInput = vec2(0.0f);
    if (input.isDown(GLFW_KEY_W)) m_moveInput.y += 1.0f;
    if (input.isDown(GLFW_KEY_S)) m_moveInput.y -= 1.0f;
    if (input.isDown(GLFW_KEY_D)) m_moveInput.x += 1.0f;
    if (input.isDown(GLFW_KEY_A)) m_moveInput.x -= 1.0f;
    if (glm::length(m_moveInput) > 1.0f) m_moveInput = glm::normalize(m_moveInput); // 斜向不更快
    m_sprint = input.isDown(GLFW_KEY_LEFT_SHIFT);
    // 按键在帧里检测，跳跃在固定步里执行：两者频率不同，先锁存，执行后清除，保证不丢键也不连跳
    if (input.wasPressed(GLFW_KEY_SPACE)) m_jumpQueued = true;
    if (input.wasPressed(GLFW_KEY_R)) respawn();
}

void Player::fixedUpdate(float dt) {
    if (!m_world || !m_character.valid()) return;
    m_prevPos = m_currPos;

    // 期望速度：输入方向转到相机的水平朝向下
    const float yaw = glm::radians(m_yawDeg);
    const vec3 forward{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    const vec3 right{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const float speed = m_sprint ? m_settings.sprintSpeed : m_settings.walkSpeed;
    const vec3 wish = (forward * m_moveInput.y + right * m_moveInput.x) * speed;
    // 指数趋近：每步走剩余差距的 1 - e^(-k·dt)，与帧率/步长无关
    m_velocity += (wish - m_velocity) * (1.0f - std::exp(-m_settings.acceleration * dt));

    const float jump = m_jumpQueued && m_state.grounded ? m_settings.jumpSpeed : 0.0f;
    m_jumpQueued = false;
    m_world->moveCharacter(m_character, m_velocity, jump, dt);
    m_state = m_world->character(m_character);
    m_currPos = m_state.position;

    if (m_currPos.y < m_settings.fallLimit) {
        LOG_INFO("玩家掉出世界，回到出生点");
        respawn();
    }
}

void Player::cameraView(float alpha, mat4& outView, vec3& outEye) const {
    const vec3 target = position(alpha) + vec3(0.0f, m_settings.cameraHeight, 0.0f);
    const float yaw = glm::radians(m_yawDeg), pitch = glm::radians(m_pitchDeg);
    // 从注视点指向相机的方向：pitch > 0 时相机在上方往下看
    const vec3 dir{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    float dist = m_distance;
    if (m_world) {
        // 相机碰撞：留 0.25 m 余量，避免近平面切进墙面
        if (auto hit = m_world->castRay(target, dir, m_distance + 0.25f)) dist = std::max(hit->distance - 0.25f, 0.3f);
    }
    outEye = target + dir * dist;
    outView = glm::lookAt(outEye, target, kWorldUp);
}

} // namespace eng
