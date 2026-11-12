// Frame rate 120 (menu, Video; ODYSSEY_NATIVE_120=1 / 0 sets it at start): 120 Hz native draw loop with a 60 Hz simulation.
// The hooks are always in; the menu switches between it and the stock 60 FPS loop while the game runs.
// Keep the full framework update and draw path, including graphics housekeeping and the
// HUD, and run GameSystem::movement at most once per two VI ticks.
#include <stdlib.h>
#include <string.h>

#include "../rt.h"
#include "../odyssey_pose.h"
#include "../settings.h"

static bool hooks_ok, trace, extra_draw;
#define enabled odyssey_native_120_enabled()
static GuestFn original_game_movement, original_draw, original_movement, original_stage_draw, original_flight_draw;
static GuestFn original_boot_intro, original_framework_calc, original_controller_calc;
static uint64_t texture_allocator, texture_calc, prepare_graphics;
static uint64_t last_logic_slot, logic_ticks, controller_ticks, draws, trace_ns;
static uint64_t simulation_tick, camera_matrix;
uint64_t vi_vsync_count(void);
uint64_t vi_vsync_time(uint64_t count);
uint64_t vi_wait_vsync(uint64_t target);
void input_native_tick_begin(void);
void input_native_tick_end(void);

bool odyssey_native_120_enabled(void) { return hooks_ok && g_settings.frame_rate == FPS_120; }

bool odyssey_skip_extra_particle_update(void) { return enabled && extra_draw; }

bool odyssey_motion_enabled(void) {
    static int motion = -1;
    if (motion < 0) {
        const char *mode = getenv("ODYSSEY_MOTION_120");
        motion = !mode || !strcmp(mode, "1");
    }
    return motion != 0;
}

void odyssey_camera_draw(Cpu *c) {
    // StageScene::drawMain's private copy of the current LookAtCamera.
    // Rebuild the view with sead's evaluator before the world and DLAA consume it.
    static __thread uint64_t scene, camera, tick;
    static __thread float previous[9];
    static __thread bool half_drawn;
    if (!enabled || !odyssey_motion_enabled() || !camera_matrix) return;
    uint64_t draw_scene = c->x[19], source = c->x[25];
    float *pose = (float *)(uintptr_t)(c->x[23] + 0x38);
    if (extra_draw) {
        half_drawn = draw_scene == scene && source == camera && tick == simulation_tick;
        return;
    }
    bool continuous = draw_scene == scene && source == camera && half_drawn && simulation_tick == tick + 1;
    half_drawn = false;
    scene = draw_scene;
    camera = source;
    tick = simulation_tick;
    float old[9];
    memcpy(old, previous, sizeof old);
    memcpy(previous, pose, sizeof previous);
    if (!continuous) return;
    float travel = 0.0f, turn = 0.0f, alignment = 0.0f, length = 0.0f, next_length = 0.0f;
    for (unsigned i = 0; i < 3; i++) {
        float eye_delta = pose[i] - old[i], target_delta = pose[i + 3] - old[i + 3];
        float roll_delta = pose[i + 6] - old[i + 6];
        travel += eye_delta * eye_delta + target_delta * target_delta;
        turn += roll_delta * roll_delta;
        float direction = pose[i + 3] - pose[i];
        float next_direction = old[i + 3] - old[i];
        alignment += direction * next_direction;
        length += direction * direction;
        next_length += next_direction * next_direction;
    }
    // Skip cuts, abrupt changes in roll, and half steps that would reverse the view.
    if (!(travel <= 80.0f * 80.0f && turn <= 0.08f * 0.08f && (travel > 0.0f || turn > 0.0f))) return;
    if (!(alignment > 0.0f && alignment * alignment >= 0.95f * length * next_length)) return;
    for (unsigned i = 0; i < 9; i++) pose[i] += 0.5f * (old[i] - pose[i]);
    Cpu draw = *c;
    draw.x[0] = c->x[23];
    draw.x[1] = c->x[23] + 8;
    guest_call(&draw, camera_matrix);
}

static void boot_intro(Cpu *c) {
    // HakoniwaStateBootLoadData::exeBootLayout starts controller setup on step zero.
    // Its transition requires both step > 540 and the asynchronous loader to be ready.
    int32_t *step = (int32_t *)(uintptr_t)(c->x[1] + 0x18);  // al::NerveKeeper::mStep
    bool first = *step == 0;
    original_boot_intro(c);
    if (first && *step == 0) {
        *step = 540;  // NerveKeeper::update increments after this callback returns.
        LOG("[boot] skipped the controller instructions timer\n");
    }
}

static void framework_calc(Cpu *c) {
    // The method tree polls controllers before GameSystem::movement. Decide the tick
    // before either runs, so a draw-only iteration cannot consume a button edge.
    // ODYSSEY_NATIVE_120_STRICT=1 (tests): gameplay ticks and extra draws strictly alternate, whatever the time -
    // a frame series saved to disk (seconds per frame) shows each extra draw next to its gameplay frame
    static int strict = -1;
    if (strict < 0) strict = getenv("ODYSSEY_NATIVE_120_STRICT") != NULL;
    // A loop's own work (its time minus what it spent blocked: the present's vsync wait) - what an extra draw costs
    extern __thread uint64_t g_blocked_ns;
    static uint64_t prev_ns, prev_blocked, work_ns;
    uint64_t now = plat_now_ns(), blocked = g_blocked_ns;
    if (prev_ns && now - prev_ns < 100000000ull) {
        uint64_t w = now - prev_ns - (blocked - prev_blocked < now - prev_ns ? blocked - prev_blocked : 0);
        work_ns = work_ns ? (w > work_ns ? w : (work_ns * 7 + w) / 8) : w;  // (rises at once, falls slowly)
    }
    uint64_t slot = vi_vsync_count() / 2;
    extra_draw = enabled && (strict ? !extra_draw : slot == last_logic_slot);
    // An extra draw only when it ends before the next gameplay tick is due: one that runs past it delays that tick's
    // frame, and in busy scenes (4K at the Cascade waterfall: ~13 ms a loop) gameplay frames then came 13 / 26 ms
    // apart - uneven motion. Then the loop waits for the tick instead (steady 60 there, 120 wherever it fits).
    // ODYSSEY_NATIVE_120_ADAPT=1 turns this on.
    static int adapt = -1;
    if (adapt < 0) adapt = getenv("ODYSSEY_NATIVE_120_ADAPT") && atoi(getenv("ODYSSEY_NATIVE_120_ADAPT")) != 0;  // (off unless asked:
    // measured at 4K without it, the Cascade Kingdom held 119-120 FPS; with it the busy parts ran at 95-110)
    if (extra_draw && !strict && adapt && last_logic_slot != UINT64_MAX) {
        uint64_t tick = (last_logic_slot + 1) * 2;
        uint64_t due = vi_vsync_time(tick);
        if (now + work_ns + work_ns / 8 + 500000 > due) {
            static uint64_t waits;
            if (trace && !(waits++ % 120)) LOG("[native-120] gameplay tick due in %.1f ms, a loop takes %.1f: waiting (%llu)\n",
                                                (double)(due > now ? due - now : 0) / 1e6, work_ns / 1e6, (unsigned long long)waits);
            uint64_t t0 = plat_now_ns();
            kernel_block_begin();  // (the core's other threads run meanwhile)
            vi_wait_vsync(tick);
            kernel_gate_reenter();  // (guest code runs next, from this hook)
            g_blocked_ns += plat_now_ns() - t0;
            slot = vi_vsync_count() / 2;
            extra_draw = slot == last_logic_slot;
            blocked = g_blocked_ns;
            now = plat_now_ns();
        }
    }
    prev_ns = now, prev_blocked = blocked;
    last_logic_slot = slot;
    original_framework_calc(c);
}

static void controller_calc(Cpu *c) {
    if (extra_draw) return;
    if (!enabled) { original_controller_calc(c); return; }
    controller_ticks++;
    input_native_tick_begin();
    original_controller_calc(c);
    input_native_tick_end();
}

static void game_movement(Cpu *c) {
    if (enabled && extra_draw) return;
    logic_ticks++;
    simulation_tick++;
    bool sample = enabled && odyssey_pose_enabled();
    if (sample) odyssey_pose_begin();
    original_game_movement(c);
    if (sample) odyssey_pose_end(simulation_tick);
}

// A second draw needs its own graphics preparation even when gameplay has not advanced: without it the dynamic
// texture allocator still holds the last draw's allocations and the depth shadow map's alloc faults (a stage, and
// the Odyssey's flight between kingdoms: DemoChangeWorldScene, DepthShadowMapDirector::drawToMask).
static void prepare_extra_draw(Cpu *c, const char *what, bool *logged) {
    Cpu graphics = *c;
    uint64_t allocator = *(const uint64_t *)(uintptr_t)texture_allocator;
    if (allocator && texture_calc) {
        graphics.x[0] = allocator;
        guest_call(&graphics, texture_calc);
    }
    graphics.x[0] = c->x[0];
    guest_call(&graphics, prepare_graphics);
    if (trace && !*logged) {
        LOG("[native-120] prepared graphics for an extra %s draw\n", what);
        *logged = true;
    }
}

static void stage_draw(Cpu *c) {
    static bool logged;
    static uint64_t last_stage;
    if (extra_draw) {
        if (c->x[0] == last_stage && odyssey_pose_enabled()) odyssey_pose_draw(c);
        prepare_extra_draw(c, "stage", &logged);
    } else last_stage = enabled ? c->x[0] : 0;
    original_stage_draw(c);
}

static void flight_draw(Cpu *c) {
    static bool logged;
    if (extra_draw) prepare_extra_draw(c, "kingdom flight", &logged);
    original_flight_draw(c);
}

static void proc_draw(Cpu *c) {
    original_draw(c);
    draws++;
    if (!trace) return;
    uint64_t now = plat_now_ns();
    if (!trace_ns) trace_ns = now;
    if (now - trace_ns < 2000000000ull) return;
    LOG("[native-120] %.3fs, %llu simulation ticks, %llu native draws, %llu controller updates\n", (now - trace_ns) / 1e9,
        (unsigned long long)logic_ticks, (unsigned long long)draws, (unsigned long long)controller_ticks);
    trace_ns = now;
    draws = logic_ticks = controller_ticks = 0;
}

static void player_movement(Cpu *c) {
    static uint64_t updates;
    uint64_t actor = c->x[0];
    odyssey_pose_player(actor);
    original_movement(c);
    if (!trace || ++updates % 15) return;
    // 1.0.0 LiveActor::mPoseKeeper and al::getTrans; read the simulation position after movement.
    uint64_t pose = *(const uint64_t *)(uintptr_t)(actor + 0x50);
    if (!pose) return;
    const float *pos = (const float *)(uintptr_t)(pose + 8);
    LOG("[native-120] player %.3fs tick %llu position %.3f %.3f %.3f\n", plat_now_ns() / 1e9,
        (unsigned long long)updates, pos[0], pos[1], pos[2]);
}

void odyssey_native_120_boot(void) {
    static bool installed;
    if (installed) {
        last_logic_slot = UINT64_MAX;
        extra_draw = false;
        return;
    }
    // ODYSSEY_NATIVE_120=1 / 0 sets the initial frame rate for tests.
    settings_load();
    const char *mode = getenv("ODYSSEY_NATIVE_120");
    if (mode && *mode) g_settings.frame_rate = !strcmp(mode, "0") ? FPS_60 : FPS_120;
    bool requested = true;  // (the hooks: always, the setting switches them)
    const char *skip = getenv("ODYSSEY_SKIP_CONTROLLER_INTRO");
    bool skip_intro = skip ? !strcmp(skip, "1") : g_settings.skip_intro;  // (the menu's Skip controller tips)
    trace = getenv("ODYSSEY_NATIVE_120_TRACE") != NULL;
    if (!requested && !trace && !skip_intro) return;

    const Module *main = loader_module("main");
    const uint8_t build_id[] = {0x3c, 0xa1, 0x2d, 0xfa, 0xaf, 0x9c, 0x82, 0xda};
    bool v141 = loader_main_is_v141();
    if (!main || (memcmp(main->build_id, build_id, sizeof build_id) && !v141)) {
        LOG("[native-120] requires Odyssey 1.0.0 or 1.4.1\n");
        return;
    }
    if (skip_intro) {
        // virtual Nerve callback for exeBootLayout: slot 0 of (anonymous)::HostTypeNrvBootLayout's vtable (1.4.1: the
        // same two instructions there, tools/dis141.py)
        uint64_t callback = main->base + (v141 ? 0x4b7a30 : 0x5117bc);
        original_boot_intro = dispatch_lookup(callback);
        if (original_boot_intro) hle_override(callback, boot_intro, "Odyssey controller intro timer");
        else LOG("[boot] controller instructions callback unavailable\n");
    }
    if (!requested && !trace) {
        installed = true;
        return;
    }
    uint64_t game_move = loader_find_symbol("_ZN10GameSystem8movementEv");
    uint64_t draw = loader_find_symbol("_ZN2al15GameFrameworkNx9procDraw_Ev");
    uint64_t movement = loader_find_symbol("_ZN19PlayerActorHakoniwa8movementEv");
    uint64_t stage = loader_find_symbol("_ZNK10StageScene8drawMainEv");
    uint64_t flight = loader_find_symbol("_ZNK20DemoChangeWorldScene8drawMainEv");
    uint64_t calc = loader_find_symbol("_ZN4sead15GameFrameworkNx9procCalc_Ev");
    uint64_t controller = loader_find_symbol("_ZN4sead13ControllerMgr4calcEv");
    texture_allocator = loader_find_symbol("_ZN3agl3utl23DynamicTextureAllocator9sInstanceE");
    texture_calc = loader_find_symbol("_ZN3agl3utl23DynamicTextureAllocator4calcEv");
    prepare_graphics = loader_find_symbol("_ZN2al18updateGraphicsPrevEPNS_5SceneE");
    if (odyssey_motion_enabled())
        camera_matrix = loader_find_symbol("_ZNK4sead12LookAtCamera14doUpdateMatrixEPNS_8Matrix34IfEE");
    original_game_movement = dispatch_lookup(game_move);
    original_draw = dispatch_lookup(draw);
    original_movement = dispatch_lookup(movement);
    original_stage_draw = dispatch_lookup(stage);
    original_flight_draw = flight ? dispatch_lookup(flight) : NULL;
    original_framework_calc = dispatch_lookup(calc);
    original_controller_calc = dispatch_lookup(controller);
    // (1.4.1 has no DynamicTextureAllocator::calc - the game never called it and the build dropped it: the extra draws
    // go without it there; prepare_extra_draw skips it)
    if (v141 && !texture_calc) LOG("[native-120] 1.4.1: no DynamicTextureAllocator::calc; extra draws without it\n");
    if (!original_game_movement || !original_draw || (trace && !original_movement) ||
        (requested && (!original_stage_draw || !original_framework_calc || !original_controller_calc ||
                       !texture_allocator || (!texture_calc && !v141) || !prepare_graphics))) {
        LOG("[native-120] required game hooks are unavailable\n");
        return;
    }
    last_logic_slot = UINT64_MAX;
    hooks_ok = true;
    hle_override(game_move, game_movement, "Odyssey simulation cadence");
    {
        hle_override(calc, framework_calc, "Odyssey update cadence");
        hle_override(controller, controller_calc, "Odyssey controller cadence");
        hle_override(stage, stage_draw, "Odyssey extra draw graphics preparation");
        if (original_flight_draw) hle_override(flight, flight_draw, "Odyssey extra flight draw graphics preparation");
        else LOG("[native-120] kingdom flight draw hook unavailable\n");
    }
    if (trace) hle_override(draw, proc_draw, "Odyssey native draw trace");
    if (original_movement && (trace || odyssey_pose_enabled()))
        hle_override(movement, player_movement, "Odyssey player movement observation");
    installed = true;
    LOG("[native-120] %s\n", enabled ? "120 Hz draws with 60 Hz gameplay" : "stock 60 FPS (switchable in the menu)");
}
