#include "App.h"

#include "core/FileSystem.h"
#include "core/Log.h"
#include "world/SceneDesc.h"

#include <imgui.h>

#include <thread>

namespace eng {

// 参数结构体 ↔ JSON。必须写在结构体所在的命名空间里（ADL 才能找到）。
// WITH_DEFAULT：JSON 缺字段时用结构体里的默认值。
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WindowSettings, title, width, height)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(RendererSettings, vsync, clearColor, debugView, lodThresholdPx, fxaa)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ShadowSettings, enabled, maxDistance, splitLambda, atlasSize, depthBias, normalBias,
                                                pcfRadius, casterExtent)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LightingSettings, sunElevationDeg, sunAzimuthDeg, sunIntensity, sunColor,
                                                sunAngularRadiusDeg, sunHalo, skyZenith, skyHorizon, groundColor, skyIntensity,
                                                ambientIntensity, exposure, tonemapper)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FixedStepSettings, fixedHz, maxStepsPerFrame, maxFrameTime)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CameraSettings, fovDeg, nearPlane, moveSpeed, sprintMultiplier, lookSensitivity,
                                                startPosition, startYawDeg, startPitchDeg)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DebugSettings, renderdoc)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SceneSettings, file, spinSpeedDeg)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PlayerSettings, walkSpeed, sprintSpeed, jumpSpeed, acceleration, cameraDistance,
                                                cameraHeight, mouseSensitivity, minPitchDeg, maxPitchDeg, spawn, fallLimit)

namespace terrain {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TerrainSettings, resolution, worldSize, seed, islandCenter, islandRadius, coastNoise, maxHeight,
                                                seabedDepth, hillScale, plateauRadius, plateauHeight, lodDistanceFactor, skirtDepth,
                                                sandMaxHeight, reefMaxHeight, rockSlope, mossMinHeight, layerTiling, layerRoughness,
                                                blendSharpness)
} // namespace terrain
namespace physics {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PhysicsSettings, gravity, maxBodies, collisionSteps, threads, tempMemoryMB)
} // namespace physics

int App::run(const LaunchOptions& options) {
    if (!init(options)) {
        shutdown();
        return 1;
    }

    double last = m_clock.seconds();
    const double startTime = last;
    int frame = 0;
    bool resizedDown = false, resizedUp = false, assetsSettled = false;
    while (!m_window.shouldClose()) {
        m_window.pollEvents();
        if (m_window.isMinimized()) {
            // 最小化：不渲染、不推进模拟，阻塞等待恢复；恢复后重置计时，避免一次补很多步
            m_window.waitWhileMinimized();
            last = m_clock.seconds();
            continue;
        }

        const double now = m_clock.seconds();
        const double dt = now - last;
        last = now;
        m_input.update(m_window);

        // 外部编辑器修改了 config/engine.json → 热加载
        if (m_config.pollChanged()) loadSettings();
        // 资源层：完成后台加载的 GPU 上传、检查源文件变化并热重载
        m_assets.update(now);
        // 拖进窗口的文件 → 导入资产并放进场景
        if (auto dropped = m_window.consumeDroppedFiles(); !dropped.empty()) m_editor.onFilesDropped(dropped);

        // ---- 固定步长模拟（受编辑器的暂停 / 单步 / 时间缩放控制） ----
        int steps = m_loop.advance(m_sim.paused ? 0.0 : dt * m_sim.timeScale);
        if (m_sim.paused && m_sim.pendingSteps > 0) {
            steps = 1;
            --m_sim.pendingSteps;
        }
        for (int i = 0; i < steps; ++i) fixedUpdate(m_loop.fixedDt());

        // ---- 相机：输入是否交给相机由编辑器决定（鼠标在视口里才响应） ----
        updateCamera(dt);

        // ---- 界面：编辑器在这里决定渲染输出到视口贴图还是全屏 ----
        m_imgui.beginFrame();
        m_editor.draw(float(dt));
        m_imgui.endFrame();

        // 投影矩阵的宽高比取自本帧实际的场景分辨率（视口面板或整个窗口）
        const VkExtent2D se = m_renderer.sceneExtent();
        m_view.proj = m_camera.projection(float(se.width) / float(std::max(se.height, 1u)));
        // 级联阴影要按相机视锥切分，需要知道 fov 与近平面
        m_view.fovY = glm::radians(m_cameraSettings.fovDeg);
        m_view.nearPlane = m_cameraSettings.nearPlane;

        // 冒烟测试：先等资源全部加载完（首次运行要烘焙），再开始计帧，保证截图里是最终画面
        if (m_options.frames > 0 && !assetsSettled) {
            assetsSettled = m_assets.pendingCount() == 0;
            if (!assetsSettled && now - startTime > 120.0) {
                LOG_ERROR("冒烟测试：资源加载超过 120 秒仍未完成");
                assetsSettled = true;
            }
        }
        const bool counting = m_options.frames <= 0 || assetsSettled;

        // 冒烟测试顺带测窗口缩放：1/3 处缩小、2/3 处恢复，覆盖交换链与视口贴图重建路径
        if (m_options.frames >= 30 && counting) {
            if (frame == m_options.frames / 3 && !resizedDown) {
                m_window.setSize(m_windowSettings.width * 3 / 4, m_windowSettings.height * 3 / 4);
                resizedDown = true;
            } else if (frame == m_options.frames * 2 / 3 && !resizedUp) {
                m_window.setSize(m_windowSettings.width, m_windowSettings.height);
                resizedUp = true;
            }
        }

        const bool lastFrame = counting && m_options.frames > 0 && frame + 1 >= m_options.frames;
        if (lastFrame && !m_options.capturePath.empty()) m_renderer.requestCapture(m_options.capturePath);

        // 渲染插值 + LOD 选择 + 选中高亮
        LodContext lod;
        lod.cameraPos = m_camera.position();
        lod.pixelsPerRadian = float(se.height) / (2.0f * std::tan(glm::radians(m_cameraSettings.fovDeg) * 0.5f));
        lod.thresholdPx = m_rendererSettings.lodThresholdPx;
        m_scene.buildDrawList(m_drawList, float(m_loop.alpha()), lod, m_editor.selection(), m_editor.highlightStrength(now));
        addPlayerDraw(float(m_loop.alpha()));
        if (m_renderer.drawFrame(m_view, m_drawList) && counting) ++frame;

        if (m_options.frames > 0 && frame >= m_options.frames) break;
    }

    const uint32_t errors = log::errorCount();
    shutdown();
    if (errors > 0) {
        LOG_ERROR("运行期间出现 {} 条错误日志，返回码 2", errors);
        return 2;
    }
    return 0;
}

bool App::init(const LaunchOptions& options) {
    m_options = options;
    m_config = ConfigFile("config/engine.json");
    loadSettings();
    if (options.camera) {
        // 只影响这次运行的初始相机，不写回配置
        CameraSettings cs = m_cameraSettings;
        const auto& c = *options.camera;
        cs.startPosition = {c[0], c[1], c[2]};
        cs.startYawDeg = c[3];
        cs.startPitchDeg = c[4];
        m_camera.reset(cs);
    } else {
        m_camera.reset(m_cameraSettings);
    }
    m_jobs.init();

    // RenderDoc 必须在创建 Vulkan 实例之前加载
    const bool wantRenderDoc = options.renderdoc.value_or(m_debugSettings.renderdoc && options.frames <= 0);
    m_renderdoc.init(wantRenderDoc);

    if (!m_window.create(m_windowSettings)) return false;

    rhi::DeviceSettings ds;
#ifdef ENGINE_DEBUG
    ds.validation = true;
#else
    ds.validation = false;
#endif
    if (options.validation) ds.validation = *options.validation;
    if (!m_device.init(ds, m_window.handle())) return false;
    if (!m_renderer.init(m_device, m_window, m_rendererSettings)) return false;
    m_renderer.setLighting(m_lighting);
    if (!m_imgui.init(m_device, m_window, m_renderer.swapchain().format(), m_renderer.swapchain().imageCount())) return false;
    m_renderer.setOverlay([this](VkCommandBuffer cmd) { m_imgui.render(cmd); });

    // ---- 资源层：扫描 assets/（补齐 .meta），烘焙缓存放在 build/cooked/ ----
    m_assetDb.init(fs::resolve("assets"), fs::shaderBinaryDir().parent_path() / "cooked");
    m_assetDb.scan();
    m_assets.init(m_device, m_assetDb, m_jobs, m_renderer.defaults().error());
    m_scene.init(m_assets);

    // ---- 物理 + 地形 + 玩家（场景之前：演示场景要按地形高度摆放物体） ----
    m_physics.init(m_physicsSettings);
    m_scenePhysics.init(m_physics, m_assets);
    m_terrain.init(m_device, m_renderer.pipelines(), m_assets, Renderer::kSceneColorFormat, Renderer::kDepthFormat);
    m_terrain.setLayerTextures(loadTerrainLayers());
    rebuildTerrain();
    m_terrain.showLod = options.terrainLod;
    m_renderer.addFeature(&m_terrain);
    m_player.init(m_physics, m_playerSettings);
    applyPlayerSettings();
    m_player.respawn();
    m_playerMaterial.baseColor = {0.06f, 0.22f, 0.30f, 1.0f}; // 深青色斗篷
    m_playerMaterial.pbr = {0.0f, 0.6f, 1.0f, 0.5f};

    // ---- 场景：优先打开场景文件；不存在时生成演示场景并保存 ----
    const std::filesystem::path scene = options.scene.empty() ? m_sceneSettings.file : options.scene;
    if (!openScene(scene)) {
        LOG_INFO("场景文件 {} 不存在，生成演示场景", scene.string());
        m_scenePath = scene;
        createDemoScene();
        saveScene();
    }

    EditorContext ctx;
    ctx.device = &m_device;
    ctx.renderer = &m_renderer;
    ctx.scene = &m_scene;
    ctx.assets = &m_assets;
    ctx.scenePath = &m_scenePath;
    ctx.camera = &m_camera;
    ctx.loop = &m_loop;
    ctx.renderdoc = &m_renderdoc;
    ctx.config = &m_config;
    ctx.rendererSettings = &m_rendererSettings;
    ctx.lighting = &m_lighting;
    ctx.loopSettings = &m_loopSettings;
    ctx.cameraSettings = &m_cameraSettings;
    ctx.renderdocAutoLoad = &m_debugSettings.renderdoc;
    ctx.sim = &m_sim;
    ctx.saveConfig = [this] { saveSettings(); };
    ctx.reloadConfig = [this] {
        m_config.load();
        loadSettings();
    };
    ctx.requestQuit = [this] { m_window.requestClose(); };
    ctx.saveScene = [this] { saveScene(); };
    ctx.openScene = [this](const std::filesystem::path& p) {
        if (openScene(p)) m_editor.onSceneReplaced();
    };
    ctx.newScene = [this] {
        m_scene.clear();
        m_scenePath = "scenes/untitled.scene.json";
        m_editor.onSceneReplaced();
    };
    ctx.projectSettingsExtra = [this] {
        ImGui::SliderFloat("立方体转速", &m_sceneSettings.spinSpeedDeg, 0.0f, 360.0f, "%.0f°/秒");
        ImGui::TextDisabled("启动场景: %s", m_sceneSettings.file.c_str());
        if (ImGui::SmallButton("把当前场景设为启动场景")) m_sceneSettings.file = m_scenePath.generic_string();
    };
    ctx.shadowSettings = &m_shadowSettings;
    ctx.terrainSettings = &m_terrainSettings;
    ctx.terrain = &m_terrain;
    ctx.physics = &m_physics;
    ctx.scenePhysics = &m_scenePhysics;
    ctx.regenerateTerrain = [this] { rebuildTerrain(); };
    ctx.worldPanelExtra = [this] { drawPlayerPanel(); };
    m_editor.init(ctx, options.hideUI);

    // --model：额外放入一个模型（外部路径不会复制进工程，只做临时登记）
    if (!options.model.empty()) {
        if (auto guid = m_assetDb.registerFile(std::filesystem::absolute(options.model))) {
            const ModelHandle h = m_assets.loadModel(*guid);
            waitForModel(h);
            Transform t;
            t.position = {6.0f, 0.0f, -2.0f};
            m_scene.instantiate(h, t, std::filesystem::path(options.model).stem().string());
        } else {
            LOG_WARN("--model {} 不是支持的模型文件", options.model);
        }
    }
    if (!options.select.empty()) m_editor.selectByName(options.select);
    return true;
}

void App::shutdown() {
    // 先停 Job 线程：后台任务可能还引用着资源管理器
    m_jobs.shutdown();
    if (m_device.device()) vkDeviceWaitIdle(m_device.device());
    m_editor.shutdown(); // 必须在 ImGui 后端关闭之前：要释放视口贴图的 ImGui 描述符
    m_renderer.removeFeature(&m_terrain);
    m_scenePhysics.shutdown(); // 先移除场景刚体，再销毁物理世界
    m_physics.shutdown();
    m_scene.clear();
    m_terrain.shutdown();
    m_assets.shutdown();
    m_imgui.shutdown();
    m_renderer.shutdown();
    m_device.shutdown();
    m_window.destroy();
}

void App::loadSettings() {
    m_windowSettings = m_config.get<WindowSettings>("window");
    m_rendererSettings = m_config.get<RendererSettings>("renderer");
    m_shadowSettings = m_config.get<ShadowSettings>("shadows");
    m_lighting = m_config.get<LightingSettings>("lighting");
    m_loopSettings = m_config.get<FixedStepSettings>("loop");
    m_cameraSettings = m_config.get<CameraSettings>("camera");
    m_debugSettings = m_config.get<DebugSettings>("debug");
    m_sceneSettings = m_config.get<SceneSettings>("scene");
    m_terrainSettings = m_config.get<terrain::TerrainSettings>("terrain");
    m_physicsSettings = m_config.get<physics::PhysicsSettings>("physics");
    m_playerSettings = m_config.get<PlayerSettings>("player");
    // 热加载时只更新「改了立刻能生效」的参数；地形形状要点「重新生成」，物理容量要重启
    m_terrain.setSettings(m_terrainSettings);
    if (m_physics.initialized()) {
        m_physics.setGravity(m_physicsSettings.gravity);
        applyPlayerSettings();
    }
    m_loop.setSettings(m_loopSettings);
    m_renderer.setSettings(m_rendererSettings);
    m_renderer.setShadowSettings(m_shadowSettings);
    m_renderer.setLighting(m_lighting);
    m_camera.setSettings(m_cameraSettings);
}

void App::saveSettings() {
    m_cameraSettings.moveSpeed = m_camera.speed();
    m_config.set("window", m_windowSettings);
    m_config.set("renderer", m_rendererSettings);
    m_config.set("shadows", m_shadowSettings);
    m_config.set("lighting", m_lighting);
    m_config.set("loop", m_loopSettings);
    m_config.set("camera", m_cameraSettings);
    m_config.set("debug", m_debugSettings);
    m_config.set("scene", m_sceneSettings);
    m_config.set("terrain", m_terrainSettings);
    m_config.set("physics", m_physicsSettings);
    m_config.set("player", m_playerSettings);
    m_config.save();
}

bool App::openScene(const std::filesystem::path& projectRelative) {
    auto desc = loadSceneFile(fs::resolve(projectRelative));
    if (!desc) return false;
    m_scene.fromDesc(*desc);
    m_scenePath = projectRelative;
    LOG_INFO("已打开场景 {}（{} 个实体）", projectRelative.generic_string(), m_scene.entities().size());
    return true;
}

void App::saveScene() {
    saveSceneFile(fs::resolve(m_scenePath), m_scene.toDesc());
}

void App::waitForModel(ModelHandle h) {
    // 只在启动阶段使用：同步等待后台烘焙/加载完成（期间继续执行主线程上传任务）
    while (m_assets.status(h) == asset::AssetStatus::Loading) {
        m_assets.update(m_clock.seconds());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void App::createDemoScene() {
    // 岛屿演示场景：所有物体摆在原点附近的平台上（地形在那里整平成 plateauHeight）
    using asset::BuiltinModel;
    const float ground = m_heightfield.heightAt(0.0f, 0.0f);
    auto add = [&](const std::string& name, BuiltinModel model, const Transform& t, const MaterialParams& params, PhysicsMode physics) {
        Entity e;
        e.name = name;
        e.current = t;
        e.model = m_assets.builtin(model);
        e.materialOverride = params;
        e.physics = physics;
        m_scene.add(e);
    };

    // ---- 材质球阵列：横向粗糙度 0→1；前排电介质（红漆），后排金属（金）。静态碰撞体，角色会被挡住 ----
    const int kColumns = 7;
    for (int row = 0; row < 2; ++row) {
        for (int i = 0; i < kColumns; ++i) {
            MaterialParams p;
            p.baseColor = row == 0 ? vec4(0.55f, 0.06f, 0.05f, 1.0f) : vec4(1.0f, 0.77f, 0.34f, 1.0f);
            p.pbr = {row == 0 ? 0.0f : 1.0f, float(i) / float(kColumns - 1), 1.0f, 0.5f};
            Transform t;
            t.position = {-4.5f + 1.5f * float(i), ground + 0.45f, 2.0f + 1.4f * float(row)};
            t.scale = vec3(0.9f);
            add(std::string("材质球/") + (row == 0 ? "Dielectric_r" : "Gold_r") + std::to_string(i), BuiltinModel::Sphere, t, p,
                PhysicsMode::Static);
        }
    }

    // ---- 旋转立方体：固定步长更新 + 渲染插值的演示（不参与物理） ----
    MaterialParams painted;
    painted.baseColor = {0.05f, 0.25f, 0.6f, 1.0f};
    painted.pbr = {0.0f, 0.35f, 1.0f, 0.5f};
    Transform cube;
    cube.position = {-4.0f, ground + 1.2f, -1.0f};
    add("Spinner", BuiltinModel::Cube, cube, painted, PhysicsMode::None);

    // ---- 木箱金字塔 + 石球：动态刚体，可以被角色推倒 ----
    MaterialParams wood;
    wood.baseColor = {0.33f, 0.20f, 0.10f, 1.0f};
    wood.pbr = {0.0f, 0.75f, 1.0f, 0.5f};
    const float kCrate = 0.7f;
    int crate = 0;
    for (int row = 0; row < 3; ++row)
        for (int i = 0; i < 3 - row; ++i) {
            Transform t;
            t.scale = vec3(kCrate);
            // 每层之间留 1 cm 空隙：刚体一开始就互相穿插的话，第一步会被「弹开」
            t.position = {4.5f + (float(i) - float(2 - row) * 0.5f) * (kCrate + 0.02f), ground + kCrate * (0.5f + float(row)) + 0.01f * float(row + 1),
                          -1.0f};
            add("木箱/" + std::to_string(crate++), BuiltinModel::Cube, t, wood, PhysicsMode::Dynamic);
        }
    MaterialParams stone;
    stone.baseColor = {0.35f, 0.34f, 0.32f, 1.0f};
    stone.pbr = {0.0f, 0.85f, 1.0f, 0.5f};
    Transform ball;
    ball.position = {-2.5f, ground + 4.0f, -0.5f};
    ball.scale = vec3(0.8f);
    add("石球", BuiltinModel::Sphere, ball, stone, PhysicsMode::Dynamic);

    // ---- glTF 模型（遗迹）：通过资产数据库引用，第一次会在后台烘焙；三角网格静态碰撞体 ----
    if (auto guid = m_assetDb.findByPath(fs::resolve("assets/models/ruins.glb"))) {
        const ModelHandle ruins = m_assets.loadModel(*guid);
        waitForModel(ruins); // 实例化前需要知道模型有几个实例（节点）
        Transform root;
        root.position = {0.0f, ground, -3.0f};
        const EntityId first = m_scene.instantiate(ruins, root, "ruins");
        for (EntityId id = first; id < m_scene.entities().size(); ++id) m_scene.entity(id).physics = PhysicsMode::Static;
    } else {
        LOG_WARN("找不到 assets/models/ruins.glb；运行 python tools/make_test_assets.py 生成测试资源");
    }
}

void App::fixedUpdate(double dt) {
    // 模拟状态只在这里以固定步长改变。先保存上一步，渲染时在两步之间插值。
    m_simTime += dt;
    m_scene.beginFixedStep();
    // 按名字找旋转立方体：场景可能被重新加载、实体可能被删除，所以不缓存下标
    if (auto spinner = m_scene.findByName("Spinner")) {
        Entity& e = m_scene.entity(*spinner);
        const quat step = glm::angleAxis(glm::radians(m_sceneSettings.spinSpeedDeg) * float(dt), glm::normalize(vec3(0.3f, 1.0f, 0.2f)));
        e.current.rotation = glm::normalize(step * e.current.rotation);
    }

    // 物理：实体 → 刚体（新建/瞬移/删除）→ 角色 → 刚体模拟 → 动态刚体位置写回实体
    m_scenePhysics.syncBeforeStep(m_scene);
    m_player.fixedUpdate(float(dt));
    m_physics.step(float(dt));
    m_scenePhysics.syncAfterStep(m_scene);
}

void App::rebuildTerrain() {
    const double t0 = m_clock.seconds();
    m_heightfield.generate(m_terrainSettings, &m_jobs);
    m_terrain.setSettings(m_terrainSettings);
    m_terrain.setHeightfield(m_heightfield);

    // 碰撞体用同一份采样：看到的地面 = 站立的地面
    if (m_terrainBody.valid()) m_physics.removeBody(m_terrainBody);
    if (m_terrainShape.valid()) m_physics.releaseShape(m_terrainShape);
    m_terrainBody = {};
    m_terrainShape = m_physics.createHeightfield(m_heightfield.samples(), m_heightfield.resolution(), m_heightfield.origin(),
                                                 m_heightfield.spacing());
    if (m_terrainShape.valid()) {
        physics::BodyDesc d;
        d.shape = m_terrainShape;
        d.motion = physics::Motion::Static;
        d.friction = 0.8f;
        m_terrainBody = m_physics.addBody(d);
    }
    LOG_INFO("地形生成完成，用时 {:.0f} ms", (m_clock.seconds() - t0) * 1000.0);
    if (m_physics.initialized()) applyPlayerSettings(); // 地形变了，出生点高度跟着变
}

terrain::TerrainLayerTextures App::loadTerrainLayers() {
    terrain::TerrainLayerTextures t;
    const char* names[4] = {"sand", "rock", "reef", "moss"};
    for (int i = 0; i < 4; ++i) {
        const std::string base = std::string("assets/terrain/") + names[i];
        if (auto g = m_assetDb.findByPath(fs::resolve(base + ".png"))) t.albedo[i] = m_assets.loadTexture(*g);
        if (auto g = m_assetDb.findByPath(fs::resolve(base + "_n.png"))) t.normal[i] = m_assets.loadTexture(*g);
        if (!t.albedo[i].valid() || !t.normal[i].valid())
            LOG_WARN("缺少地形贴图 {}(.png/_n.png)，该层用默认贴图；运行 python tools/make_terrain_textures.py 生成", base);
    }
    return t;
}

void App::applyPlayerSettings() {
    // 出生点不能埋在地下：至少放在地面以上 5 cm（配置里的 y 只是下限）
    PlayerSettings s = m_playerSettings;
    s.spawn.y = std::max(s.spawn.y, m_heightfield.heightAt(s.spawn.x, s.spawn.z) + 0.05f);
    m_player.setSettings(s);
}

void App::addPlayerDraw(float alpha) {
    // 玩家不是场景实体（不保存进场景文件），每帧直接追加一个绘制项
    const Model& capsule = m_assets.model(m_assets.builtin(asset::BuiltinModel::Capsule));
    if (capsule.meshes.empty()) return;
    DrawItem item;
    item.mesh = &capsule.meshes[0];
    item.model = glm::translate(mat4(1.0f), m_player.position(alpha)) *
                 glm::rotate(mat4(1.0f), glm::radians(m_player.yawDeg()), kWorldUp);
    item.material = capsule.materials.empty() ? nullptr : &capsule.materials[0];
    item.paramsOverride = &m_playerMaterial;
    m_drawList.opaque.push_back(item);
}

void App::drawPlayerPanel() {
    const physics::CharacterState& s = m_player.state();
    ImGui::Text("位置 (%.2f, %.2f, %.2f)", s.position.x, s.position.y, s.position.z);
    ImGui::TextDisabled("%s · 速度 %.2f m/s", s.grounded ? "着地" : (s.onSteepGround ? "陡坡上（滑落）" : "空中"),
                        glm::length(vec2(s.velocity.x, s.velocity.z)));
    bool c = false;
    c |= ImGui::SliderFloat("步行速度", &m_playerSettings.walkSpeed, 0.5f, 10.0f, "%.1f m/s");
    c |= ImGui::SliderFloat("冲刺速度", &m_playerSettings.sprintSpeed, 1.0f, 20.0f, "%.1f m/s");
    c |= ImGui::SliderFloat("起跳速度", &m_playerSettings.jumpSpeed, 0.0f, 15.0f, "%.1f m/s");
    c |= ImGui::SliderFloat("加速度", &m_playerSettings.acceleration, 1.0f, 40.0f, "%.1f /s");
    c |= ImGui::SliderFloat("相机高度", &m_playerSettings.cameraHeight, 0.5f, 3.0f, "%.2f m");
    if (c) applyPlayerSettings();
    if (ImGui::SmallButton("回到出生点")) m_player.respawn();
    ImGui::TextDisabled("游戏模式（F1）：WASD 移动 · Shift 冲刺 · 空格跳 · 鼠标转视角 · 滚轮远近 · R 重生");
}

void App::updateCamera(double dt) {
    const bool game = m_editor.gameMode();
    m_player.updateInput(m_input, game);
    if (game) {
        // 游戏模式：第三人称相机跟随玩家，鼠标一直用来转视角
        m_window.setCursorCaptured(true);
        m_editor.setCameraLooking(false);
        m_player.cameraView(float(m_loop.alpha()), m_view.view, m_view.position);
        m_view.time = m_clock.seconds();
        m_view.dt = float(dt);
        return;
    }
    const bool looking = m_camera.update(m_input, float(dt), m_editor.cameraMouseAllowed(), m_editor.cameraKeyboardAllowed());
    m_window.setCursorCaptured(looking);
    m_editor.setCameraLooking(looking);
    m_view.view = m_camera.view();
    m_view.position = m_camera.position();
    m_view.time = m_clock.seconds();
    m_view.dt = float(dt);
}

} // namespace eng
