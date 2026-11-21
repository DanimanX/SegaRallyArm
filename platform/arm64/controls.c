#include "controls.h"
#include "model2_rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static SDL_GameController *g_controller = NULL;
static arm64_steering_config_t g_cfg = {
    .ramp_rate = 3.8f,
    .return_rate = 8.5f,
    .max_deflection = 1.0f,
    .analog_deadzone = 0.15f,
    .menu_step_mode = 1
};

static float g_steer_pos = 0.0f; /* -1.0 (full left) to +1.0 (full right), 0 = center */
static unsigned g_shifter = 0;   /* 0=N, 1..4 */

int arm64_controls_init(const char *gamecontrollerdb_path)
{
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            fprintf(stderr, "arm64_controls: SDL_InitSubSystem(GAMECONTROLLER) failed: %s\n", SDL_GetError());
        }
    }

    if (gamecontrollerdb_path) {
        int added = SDL_GameControllerAddMappingsFromFile(gamecontrollerdb_path);
        if (added > 0) {
            printf("arm64_controls: loaded %d controller mappings from %s\n", added, gamecontrollerdb_path);
        }
    } else {
        /* Try default paths */
        if (SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt") < 0) {
            SDL_GameControllerAddMappingsFromFile("portmaster/segarallyarm/gamecontrollerdb.txt");
        }
    }

    /* Open first available joystick/controller */
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            g_controller = SDL_GameControllerOpen(i);
            if (g_controller) {
                printf("arm64_controls: using GameController: %s\n", SDL_GameControllerName(g_controller));
                break;
            }
        }
    }

    g_steer_pos = 0.0f;
    g_shifter = 0;
    return 0;
}

void arm64_controls_shutdown(void)
{
    if (g_controller) {
        SDL_GameControllerClose(g_controller);
        g_controller = NULL;
    }
}

void arm64_controls_reset_steering(void)
{
    g_steer_pos = 0.0f;
    model2_io_analog_set(MODEL2_IO_AN_STEER, 0x80u);
}

int arm64_controls_poll(float delta_time, int in_race, int in_menu)
{
    if (delta_time <= 0.0f || delta_time > 0.1f)
        delta_time = 1.0f / 60.0f;

    /* Hotplug check */
    if (!g_controller) {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_IsGameController(i)) {
                g_controller = SDL_GameControllerOpen(i);
                if (g_controller) break;
            }
        }
    }

    int left = 0, right = 0, up = 0, down = 0;
    int btn_a = 0, btn_b = 0, btn_x = 0, btn_y = 0;
    int btn_l1 = 0, btn_r1 = 0, btn_l2 = 0, btn_r2 = 0;
    int btn_start = 0, btn_select = 0, btn_guide = 0;
    float analog_x = 0.0f;

    if (g_controller) {
        left = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        right = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        up = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
        down = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);

        btn_a = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_A);
        btn_b = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_B);
        btn_x = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_X);
        btn_y = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_Y);

        btn_l1 = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        btn_r1 = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        btn_start = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_START);
        btn_select = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_BACK);
        btn_guide = SDL_GameControllerGetButton(g_controller, SDL_CONTROLLER_BUTTON_GUIDE);

        Sint16 axis_lt = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        Sint16 axis_rt = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (axis_lt > 8000) btn_l2 = 1;
        if (axis_rt > 8000) btn_r2 = 1;

        Sint16 lx = SDL_GameControllerGetAxis(g_controller, SDL_CONTROLLER_AXIS_LEFTX);
        if (abs(lx) > (int)(g_cfg.analog_deadzone * 32767.0f)) {
            analog_x = (float)lx / 32767.0f;
        }
    }

    /* Fallback / keyboard input merging */
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    if (ks) {
        if (ks[SDL_SCANCODE_LEFT]) left = 1;
        if (ks[SDL_SCANCODE_RIGHT]) right = 1;
        if (ks[SDL_SCANCODE_UP]) up = 1;
        if (ks[SDL_SCANCODE_DOWN]) down = 1;
        if (ks[SDL_SCANCODE_A] || ks[SDL_SCANCODE_Z]) btn_a = 1; /* Throttle */
        if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_X]) btn_b = 1; /* Brake */
        if (ks[SDL_SCANCODE_V]) btn_x = 1;                      /* Change View */
        if (ks[SDL_SCANCODE_1] || ks[SDL_SCANCODE_RETURN]) btn_start = 1;
        if (ks[SDL_SCANCODE_5] || ks[SDL_SCANCODE_RSHIFT]) btn_select = 1;
        if (ks[SDL_SCANCODE_F2]) btn_guide = 1;
    }

    /* Exit Hotkey Combo: Select + Start */
    if (btn_select && btn_start) {
        return 1; /* Signal quit */
    }

    /* Test menu combo: Select + X, or Guide/Menu button */
    if ((btn_select && btn_x) || btn_guide) {
        model2_io_request_test_menu();
        model2_io_in0_set_mask(MODEL2_IO_IN0_TEST, 1);
    }

    /* Arcade digital Centring-Spring steering model */
    if (fabsf(analog_x) > 0.05f) {
        /* Analog stick overrides D-pad */
        g_steer_pos = analog_x;
    } else {
        if (left && !right) {
            g_steer_pos -= g_cfg.ramp_rate * delta_time;
            if (g_steer_pos < -g_cfg.max_deflection)
                g_steer_pos = -g_cfg.max_deflection;
        } else if (right && !left) {
            g_steer_pos += g_cfg.ramp_rate * delta_time;
            if (g_steer_pos > g_cfg.max_deflection)
                g_steer_pos = g_cfg.max_deflection;
        } else {
            /* Snap back to center with exponential decay */
            if (g_steer_pos > 0.0f) {
                g_steer_pos -= g_cfg.return_rate * delta_time;
                if (g_steer_pos < 0.0f) g_steer_pos = 0.0f;
            } else if (g_steer_pos < 0.0f) {
                g_steer_pos += g_cfg.return_rate * delta_time;
                if (g_steer_pos > 0.0f) g_steer_pos = 0.0f;
            }
        }
    }

    /* Map -1.0..+1.0 to Model 2 0x00..0xFF (0x80 center) */
    float steer_val = 128.0f + g_steer_pos * 127.0f;
    if (steer_val < 0.0f) steer_val = 0.0f;
    if (steer_val > 255.0f) steer_val = 255.0f;
    model2_io_analog_set(MODEL2_IO_AN_STEER, (uint8_t)steer_val);

    /* Throttle & Brake */
    int throttle_held = (btn_a || btn_r2 || btn_r1 || up);
    int brake_held = (btn_b || btn_l2 || btn_l1 || down);
    model2_io_analog_set(MODEL2_IO_AN_ACCEL, throttle_held ? 0xE0u : 0x00u);
    model2_io_analog_set(MODEL2_IO_AN_BRAKE, brake_held ? 0xE0u : 0x00u);

    /* Shifter (Manual transmission sequential shifts via L1/R1 or Up/Down) */
    static int last_shift_up = 0, last_shift_down = 0;
    int shift_up = btn_r1;
    int shift_down = btn_l1;

    if (shift_up && !last_shift_up) {
        if (g_shifter < 4) g_shifter++;
        model2_io_shifter_set(g_shifter);
    }
    if (shift_down && !last_shift_down) {
        if (g_shifter > 0) g_shifter--;
        model2_io_shifter_set(g_shifter);
    }
    last_shift_up = shift_up;
    last_shift_down = shift_down;

    /* Buttons: Coin, Start, VR View */
    model2_io_in0_set_mask(MODEL2_IO_IN0_COIN1, btn_select);
    model2_io_in0_set_mask(MODEL2_IO_IN0_START1, btn_start);
    model2_io_in0_set_mask(MODEL2_IO_IN0_VR, btn_x);

    return 0;
}
