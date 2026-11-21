/* Handheld SDL2 GameController and Arcade Steering Engine for Anbernic RG34XX,
 * RG35XX, TrimUI, Powkiddy, and compatible PortMaster AArch64 handhelds.
 *
 * Features:
 *   - Auto-loading of PortMaster gamecontrollerdb.txt.
 *   - Arcade centring-spring D-pad steering model: digital D-pad smoothly
 *     ramps analog steering with speed-sensitive deflection, snapping back to
 *     center (0x80) on release like the arcade cabinet's torsion spring.
 *   - Discrete menu step-steering for car / transmission / course selection.
 *   - Full analog thumbstick and analog trigger support for dual-stick handhelds.
 *   - Sequential transmission shifting (L1/R1 or D-Up/D-Down) for manual transmission.
 */
#ifndef PLATFORM_ARM64_CONTROLS_H
#define PLATFORM_ARM64_CONTROLS_H

#include <SDL.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct arm64_steering_config {
    float ramp_rate;       /* Steer change per second when key held (default: 3.5) */
    float return_rate;     /* Spring return speed when released (default: 8.0) */
    float max_deflection;  /* Max angle [0..1] (default: 1.0) */
    float analog_deadzone; /* Analog stick deadzone (default: 0.15) */
    int menu_step_mode;    /* 1 = enable discrete menu stepping */
} arm64_steering_config_t;

int arm64_controls_init(const char *gamecontrollerdb_path);
void arm64_controls_shutdown(void);

/* Poll gamepads, update centring-spring physics, and feed Model 2 I/O */
int arm64_controls_poll(float delta_time, int in_race, int in_menu);

/* Reset steering state to center (0x80) */
void arm64_controls_reset_steering(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_ARM64_CONTROLS_H */
