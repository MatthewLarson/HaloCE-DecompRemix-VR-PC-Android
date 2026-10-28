/*
PORT_CONFIG.C

The native ports' settings (port_config.h), parsed with tomlc17
(port/third_party/tomlc17). Every setting is in the table below with its
type, default, the HALO_* environment variable that overrides it and the
comment written into a new file. The file is read once, on the first
question; unknown keys and values of the wrong type are reported in the log
and the defaults used instead, and the file itself is never rewritten once
it exists, so that the player's edits and comments stay.
*/

#include "platform.h"
#include "port_config.h"
#include "halo_port_limits.h"
#include "tomlc17.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

/* ---------- the settings */

enum config_type
{
	_config_boolean,
	_config_integer,
	_config_real,
	_config_string,
};

/* how the setting's environment variable sets it */
enum config_environment
{
	/* the variable's text is the value ("0", "false", "no" and "off" are
	false for a boolean) */
	_environment_value,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it true */
	_environment_set_is_true,
	/* the variable being set (not empty, "0", "false", "no" or "off") makes it false */
	_environment_set_is_false,
};

/* the builds a setting means something in, and is written for */
enum
{
	_platform_linux = 1,
	_platform_android = 2,
	_platform_windows = 4,
	_platform_desktop = _platform_linux | _platform_windows,
	_platform_all = _platform_desktop | _platform_android,
	/* the Android build for VR headsets (HALO_VR) only */
	_platform_vr = 8,
	/* Test72: every build without a headset (Android, Windows, Linux, web):
	the flat game's own settings, which the VR build never writes */
	_platform_flat = 16,
};

struct config_setting
{
	const char *name;
	enum config_type type;
	/* as it is written in the file */
	const char *default_value;
	/* NULL: none, for a setting the Android app reads from the file itself */
	const char *environment;
	enum config_environment environment_style;
	unsigned platforms;
	const char *comment;
};

/* crouching's keys (in a browser C alone: a page cannot keep Ctrl+W, Ctrl+S or
Ctrl+D, which moving and crouching would press, from the browser) */
#ifdef HALO_WEB
#define CROUCH_KEYS "\"C\""
#else
#define CROUCH_KEYS "\"Left Ctrl, C\""
#endif

static const struct config_setting config_settings[] =
{
	{ "browser.show_empty", _config_boolean, "true", "HALO_BROWSER_SHOW_EMPTY", _environment_value, _platform_all,
		"Include empty games in the in-game server browser." },
	{ "browser.show_full", _config_boolean, "true", "HALO_BROWSER_SHOW_FULL", _environment_value, _platform_all,
		"Include full games in the in-game server browser." },
	{ "browser.engine", _config_integer, "-1", "HALO_BROWSER_ENGINE", _environment_value, _platform_all,
		"Browser game type: -1 all, 0 co-op, 1 CTF, 2 Slayer, 3 Oddball, 4 King, 5 Race." },
	{ "browser.teams", _config_string, "\"all\"", "HALO_BROWSER_TEAMS", _environment_value, _platform_all,
		"Browser teams: all, teams, or ffa." },
	{ "browser.passwords", _config_string, "\"all\"", "HALO_BROWSER_PASSWORDS", _environment_value, _platform_all,
		"Browser password filter: all, locked, or unlocked." },
	{ "browser.maps", _config_string, "\"all\"", "HALO_BROWSER_MAPS", _environment_value, _platform_all,
		"Browser map filter: all or known (catalog only; joining still checks actual game files)." },
	{ "display.mode", _config_string, "\"\"", "HALO_DISPLAY_MODE", _environment_value, _platform_desktop,
		"\"fullscreen\" takes the display (at display.resolution's mode),\n"
		"\"borderless\" is a window over the whole desktop, \"windowed\" a window\n"
		"(display.window_size). Empty: display.fullscreen's (true: borderless).\n"
		"F11 switches to the window and back." },

	{ "display.resolution", _config_string, "\"native\"", "HALO_RESOLUTION", _environment_value, _platform_desktop,
		"What fullscreen and borderless draw at: \"native\", the display's own, or\n"
		"\"<width>x<height>\" (\"1920x1080\"), 640x480 or more. Fullscreen sets the\n"
		"display to it; borderless draws it scaled to the display." },

	{ "display.resolution_scaling", _config_string, "\"native\"", "HALO_RESOLUTION_SCALING", _environment_value,
		_platform_desktop,
		"\"native\" draws at the window's resolution (fullscreen, the display's or\n"
		"display.resolution); \"original\" draws the Xbox's 640x480 and scales it\n"
		"up." },

	{ "display.window_size", _config_string, "\"\"", "HALO_WINDOW_SIZE", _environment_value, _platform_desktop,
		"The window's size, \"<width>x<height>\" (\"1920x1080\"), 640x480 or more (it\n"
		"can be resized). Empty: display.window_scale's." },

	{ "display.max_fps", _config_integer, "0", "HALO_MAX_FPS", _environment_value, _platform_desktop,
		"With vsync off, the most frames a second: 0 for twice the display's\n"
		"refresh rate, -1 for no limit (which can hang some Intel graphics)." },

	{ "display.anti_aliasing", _config_string, "\"off\"", "HALO_ANTI_ALIASING", _environment_value, _platform_all,
		"Smoothing of jagged edges, which the Xbox did not have: \"off\"; \"fxaa\"\n"
		"or \"smaa\" smooth the 3D view once it is drawn (the HUD and menus stay\n"
		"sharp); \"ssaa2x\" draws at twice the resolution each way (four times\n"
		"the work); \"msaa2x\", \"msaa4x\" or \"msaa8x\" draw with that many samples\n"
		"a pixel. Android has \"fxaa\" for \"smaa\", and no \"ssaa2x\"." },

	{ "display.hud_button_style", _config_string, "\"classic\"", "HALO_HUD_BUTTON_STYLE", _environment_value, _platform_all,
		"High-res gameplay controller buttons: classic uses plain Xbox faces;\n"
		"gems uses shaded Xbox faces; menu_gems uses the menu's button art.\n"
		"Requires display.high_res_hud." },
	{ "display.shadow_resolution", _config_integer, "128", "HALO_SHADOW_RESOLUTION", _environment_value,
		_platform_all,
		"The size the objects' shadows are drawn at, in pixels each way: 128 as\n"
		"on the Xbox, or 256, 512 or 1024 for smoother edges, as soft." },

	{ "display.menus", _config_string, "\"pc\"", "HALO_MENUS", _environment_value, _platform_all,
		"The menus: \"pc\" for the PC version's main menu (port/assets/menus,\n"
		"and a menus folder here for your own), \"xbox\" for the Xbox's." },
	{ "display.menu_language", _config_string, "\"maps\"", "HALO_MENU_LANGUAGE", _environment_value, _platform_all,
		"The PC menus' language: \"maps\" for the language of the Custom Edition\n"
		"whose loc.map is in custom_maps (as that edition shows them; English\n"
		"without one), \"english\" for English. The port's own lines are English." },

	{ "display.per_pixel_lighting", _config_boolean, "false", "HALO_PER_PIXEL_LIGHTING", _environment_value,
		_platform_all,
		"Light the models (characters, weapons, vehicles, scenery) for each\n"
		"pixel by the lights the game gives them, without the facets the light\n"
		"of each vertex shows across curved surfaces; false lights each vertex,\n"
		"as the Xbox does." },
	{ "display.structure_culling", _config_integer, "31", "HALO_STRUCTURE_CULLING", _environment_value,
		_platform_all,
		"Which tests skip the level's triangles the camera cannot see, as bits\n"
		"added together: 1 outside the view, 2 groups facing away, 4 triangles\n"
		"facing away, 8 smaller groups, 16 nearer rooms first; 0 walks them as\n"
		"the Xbox does. The console's structure_culling changes it until the\n"
		"game quits." },
	{ "display.model_detail", _config_string, "\"stock\"", "HALO_MODEL_DETAIL", _environment_value, _platform_all,
		"Which of their levels of detail models are drawn at: \"stock\" chooses by\n"
		"their size on the Xbox's 480 lines, at any resolution; \"native\" by their\n"
		"size on the display's, so that more detail is kept at a distance at high\n"
		"resolutions; \"highest\" draws the models \"native\" does, each at its most\n"
		"detailed level." },
	{ "display.hdr", _config_boolean, "false", "HALO_HDR", _environment_value, _platform_all,
		"Draw the 3D view into a floating-point back buffer, which keeps what\n"
		"the game's additive effects (explosions, plasma, lights, the sun's\n"
		"glare) add past white, and tone map it before the HUD is drawn:\n"
		"display.hdr_tone_mapping brings it back to the display's range\n"
		"instead of clipping it. false draws into 8 bits a channel, as the\n"
		"Xbox does. In VR (Test73) the eyes are drawn into 16-bit floating-point\n"
		"targets: on the Quest that is twice the memory traffic, a frame-time cost." },
	{ "display.hdr_exposure", _config_real, "1.0", "HALO_HDR_EXPOSURE", _environment_value, _platform_all,
		"With display.hdr, the 3D view's brightness in linear light before it\n"
		"is tone mapped, 0.25 to 4.0." },
	{ "display.hdr_tone_mapping", _config_string, "\"neutral\"", "HALO_HDR_TONE_MAPPING", _environment_value,
		_platform_all,
		"With display.hdr, the curve that brings what is past white into the\n"
		"display's range: \"neutral\" (the picture as drawn, with only its\n"
		"brightest parts rolled off), \"aces\" (filmic, more contrast) or\n"
		"\"reinhard\" (softer, and darker throughout)." },
	{ "display.hdr_output", _config_boolean, "false", "HALO_HDR_OUTPUT", _environment_value, _platform_desktop,
		"With display.hdr, ask for a floating-point window (scRGB), so that an\n"
		"HDR display shows what is past white brighter than the desktop's\n"
		"white rather than tone mapped down to it; the operating system's HDR\n"
		"must be on. Taken up when the game starts." },

	{ "audio.music_volume", _config_real, "1.0", "HALO_MUSIC_VOLUME", _environment_value, _platform_all,
		"The music's volume, 0.0 to 1.0 (of audio.volume)." },

	{ "audio.effects_volume", _config_real, "1.0", "HALO_EFFECTS_VOLUME", _environment_value, _platform_all,
		"The volume of every other sound (effects and speech), 0.0 to 1.0 (of\n"
		"audio.volume)." },
	{ "audio.buffer_frames", _config_integer, "512", "HALO_AUDIO_BUFFER_FRAMES", _environment_value,
		_platform_desktop,
		"The audio device's buffer, in sample frames at 48 kHz (64 to 8192): larger\n"
		"rides out stalls that cut the sound out, smaller has less delay (512\n"
		"is 11 ms, 2048 is 43 ms)." },
	{ "audio.channels", _config_string, "\"stereo\"", "HALO_AUDIO_CHANNELS", _environment_value, _platform_desktop,
		"\"stereo\", \"5.1\" (surround, as the Xbox's Dolby Digital), or \"auto\"\n"
		"for 5.1 when the sound device has six or more channels." },
	{ "audio.reverb", _config_boolean, "false", "HALO_REVERB", _environment_value, _platform_all,
		"Reverberate the world's sounds as the place the player is in does (the\n"
		"maps' sound environments, as the Xbox's I3DL2 reverb did); false keeps\n"
		"them dry." },
	{ "audio.resampler", _config_string, "\"sinc\"", "HALO_AUDIO_RESAMPLER", _environment_value, _platform_all,
		"How sounds are converted to the output's rate (OpenCE 232's Sound\n"
		"Processing): \"sinc\" this port's smooth curve (Catmull-Rom: the Quest\n"
		"cannot afford upstream's band-limited filter); \"linear\" between\n"
		"neighbouring samples, brighter for the aliasing it lets through." },
	{ "audio.voice_chat", _config_string, "\"off\"", "HALO_VOICE_CHAT", _environment_value, _platform_all,
		"Voice chat microphone: \"off\" (default), \"push_to_talk\" (hold\n"
		"controls.push_to_talk; the microphone opens only while held), or\n"
		"\"open_mic\" (transmit while speech is detected). Receiving is controlled\n"
		"separately by audio.voice_volume." },
	{ "audio.voice_volume", _config_real, "1.0", "HALO_VOICE_VOLUME", _environment_value, _platform_all,
		"The volume of other players' voices, from 0 (muted) to 2." },
	{ "audio.input_device", _config_string, "\"default\"", "HALO_AUDIO_INPUT_DEVICE", _environment_value,
		_platform_desktop,
		"The microphone voice chat listens to: a device's name as Settings >\n"
		"Audio lists it, or \"default\" for the system's (also if it is not found)." },
	{ "audio.output_device", _config_string, "\"default\"", "HALO_AUDIO_OUTPUT_DEVICE", _environment_value,
		_platform_desktop,
		"Where game and voice audio plays: an SDL device name or \"default\" for\n"
		"the system's default." },
	{ "input.playstation_kernel_driver", _config_boolean, "false", "HALO_PLAYSTATION_KERNEL_DRIVER",
		_environment_value, _platform_desktop,
		"Linux: a PlayStation controller (DualSense, DualShock 4) through the\n"
		"kernel's driver of it (hid-playstation, hid-sony), which needs no setup;\n"
		"false: through SDL's own, which needs Steam's udev rules to open it. Read\n"
		"at start-up." },

	{ "input.mouse_vertical_sensitivity", _config_real, "0.0", "HALO_MOUSE_VERTICAL_SENSITIVITY", _environment_value,
		_platform_desktop,
		"How far the view turns up and down for the mouse's movement; 0 for the\n"
		"same as input.mouse_sensitivity." },

	{ "controls.move_forward", _config_string, "\"W\"", "HALO_KEY_MOVE_FORWARD", _environment_value, _platform_all,
		"The keyboard and mouse's controls, which Settings > Controls Setup\n"
		"changes: up to two keys or buttons each, separated by a comma. Keys by\n"
		"their names on a US keyboard (\"W\", \"Space\", \"Left Ctrl\", \"F1\"): a key\n"
		"is the one in that place on any keyboard, which the menus show by its\n"
		"own label. Buttons: \"Mouse Left\", \"Mouse Right\", \"Mouse Middle\",\n"
		"\"Mouse 4\", \"Mouse 5\", \"Wheel\" (either way), \"Wheel Up\" and \"Wheel\n"
		"Down\"; empty for none. Moving forward:" },
	{ "controls.move_backward", _config_string, "\"S\"", "HALO_KEY_MOVE_BACKWARD", _environment_value, _platform_all,
		"Moving backward." },

	{ "controls.strafe_left", _config_string, "\"A\"", "HALO_KEY_STRAFE_LEFT", _environment_value, _platform_all,
		"Moving left." },

	{ "controls.strafe_right", _config_string, "\"D\"", "HALO_KEY_STRAFE_RIGHT", _environment_value, _platform_all,
		"Moving right." },

	{ "controls.jump", _config_string, "\"Space\"", "HALO_KEY_JUMP", _environment_value, _platform_all,
		"Jumping (and skipping cutscenes)." },

	{ "controls.crouch", _config_string, "\"Left Ctrl, C\"", "HALO_KEY_CROUCH", _environment_value, _platform_all,
		"Crouching." },

	{ "controls.fire", _config_string, "\"Mouse Left\"", "HALO_KEY_FIRE", _environment_value, _platform_all,
		"Firing." },

	{ "controls.throw_grenade", _config_string, "\"Mouse Right, G\"", "HALO_KEY_THROW_GRENADE", _environment_value,
		_platform_all,
		"Throwing a grenade." },

	{ "controls.melee", _config_string, "\"F, Mouse 4\"", "HALO_KEY_MELEE", _environment_value, _platform_all,
		"Melee attack." },

	{ "controls.reload", _config_string, "\"R\"", "HALO_KEY_RELOAD", _environment_value, _platform_all,
		"Reloading." },

	{ "controls.zoom", _config_string, "\"Z, Mouse Middle\"", "HALO_KEY_ZOOM", _environment_value, _platform_all,
		"Zooming the scope." },

	{ "controls.switch_weapon", _config_string, "\"Wheel, 1\"", "HALO_KEY_SWITCH_WEAPON", _environment_value,
		_platform_all,
		"Switching weapons." },

	{ "controls.switch_grenade", _config_string, "\"X\"", "HALO_KEY_SWITCH_GRENADE", _environment_value, _platform_all,
		"Switching grenades." },

	{ "controls.action", _config_string, "\"E\"", "HALO_KEY_ACTION", _environment_value, _platform_all,
		"The action: picking up (held: swapping weapons), entering and leaving\n"
		"vehicles, pressing switches; never reloading (the controller's X does\n"
		"when there is nothing to act on)." },

	{ "controls.flashlight", _config_string, "\"Q\"", "HALO_KEY_FLASHLIGHT", _environment_value, _platform_all,
		"The flashlight." },

	{ "controls.scoreboard", _config_string, "\"Tab\"", "HALO_KEY_SCOREBOARD", _environment_value, _platform_all,
		"Showing the scores (the controller's Back)." },

	{ "controls.pause", _config_string, "\"Escape\"", "HALO_KEY_PAUSE", _environment_value, _platform_all,
		"The pause menu (the controller's Start)." },
	{ "controls.screenshot", _config_string, "\"F10\"", "HALO_KEY_SCREENSHOT", _environment_value, _platform_all,
		"Save a PNG screenshot in the screenshots folder beside maps/ (one a\n"
		"press; the desktop builds)." },
	{ "controls.push_to_talk", _config_string, "\"V\"", "HALO_KEY_PUSH_TO_TALK", _environment_value, _platform_all,
		"Hold to transmit voice when audio.voice_chat is \"push_to_talk\". In VR,\n"
		"the BUTTONS page also lets you bind a Quest controller button." },
	{ "controls.third_person", _config_string, "\"Keypad Enter\"", "HALO_KEY_THIRD_PERSON", _environment_value,
		_platform_all,
		"The third-person camera on foot, on and off, in a campaign and co-op\n"
		"(controls.free_look held: free look; the wheel: nearer and further)." },
	{ "controls.free_look", _config_string, "\"Left Alt, Right Alt\"", "HALO_KEY_FREE_LOOK", _environment_value,
		_platform_all,
		"Held in the third-person camera: the camera turns alone, the player\n"
		"keeps on its way." },
	{ "controls.chat", _config_string, "\"T\"", "HALO_KEY_CHAT", _environment_value, _platform_all,
		"Network games' text chat: type a line to everyone (Enter sends it,\n"
		"Escape lets it go)." },
	{ "controls.team_chat", _config_string, "\"Y\"", "HALO_KEY_TEAM_CHAT", _environment_value, _platform_all,
		"Network games' text chat: type a line to your team." },
	{ "controls.sprint", _config_string, "\"Left Shift\"", "HALO_KEY_SPRINT", _environment_value, _platform_flat,
		"Test72: SPRINT, held or pressed to toggle (movement.sprint). A gamepad or a touch\n"
		"stick sprints with movement.sprint \"auto\" instead (a full push forward)." },

	/* Test72: stereoscopic 3D on the flat builds (flat_stereo.c) */
	{ "display.stereo", _config_string, "\"off\"", "HALO_STEREO", _environment_value, _platform_flat,
		"STEREO 3D: \"off\"; \"sbs_half\" (side by side, each eye squeezed: 3D televisions);\n"
		"\"sbs_full\" (side by side, each eye its own shape); \"tab_half\", \"tab_full\" (over and\n"
		"under, the left eye on top); \"anaglyph\" (red and cyan glasses). Each eye is drawn from its\n"
		"own place, converging at stereo_convergence." },
	{ "display.stereo_separation", _config_real, "64", "HALO_STEREO_SEPARATION", _environment_value, _platform_flat,
		"EYE SEPARATION in millimetres, 0 to 200 (64: an average adult's)." },
	{ "display.stereo_convergence", _config_real, "2.5", "HALO_STEREO_CONVERGENCE", _environment_value, _platform_flat,
		"CONVERGENCE in metres, 0.25 to 100: the distance that appears at the screen; nearer comes out\n"
		"of it, further goes in." },
	{ "display.stereo_swap", _config_boolean, "false", "HALO_STEREO_SWAP", _environment_value, _platform_flat,
		"EYE SWAP: the eyes exchanged (a display or glasses that expect the other order)." },
	{ "display.stereo_hud_depth", _config_real, "0", "HALO_STEREO_HUD_DEPTH", _environment_value, _platform_flat,
		"HUD DEPTH in metres, 0 to 100: how far away the HUD appears; 0 puts it at the screen\n"
		"(the convergence distance)." },
	{ "display.stereo_reticle_depth", _config_real, "0", "HALO_STEREO_RETICLE_DEPTH", _environment_value, _platform_flat,
		"RETICLE DEPTH in metres, 0 to 100: how far away the crosshair appears; 0 puts it at the\n"
		"screen (the convergence distance)." },

	/* Test72: the flat builds' movement extras (flat_movement.c), each off
	until turned on; the VR build has its own (vr.slide, vr.parkour_*). */
	{ "movement.sprint", _config_string, "\"off\"", "HALO_MOVEMENT_SPRINT", _environment_value, _platform_flat,
		"SPRINT, offline: \"off\"; \"hold\" (controls.sprint held); \"toggle\" (pressed to start,\n"
		"again to stop; entering a vehicle stops it); \"auto\" (the stick held fully forward a\n"
		"moment, for gamepads and touch). Movement and acceleration times movement.sprint_speed." },
	{ "movement.sprint_speed", _config_real, "1.5", "HALO_MOVEMENT_SPRINT_SPEED", _environment_value, _platform_flat,
		"SPRINT SPEED, 1.0x to 3.0x; default 1.5x (the VR build's)." },
	{ "movement.slide", _config_boolean, "false", "HALO_MOVEMENT_SLIDE", _environment_value, _platform_flat,
		"SLIDE, offline: sprinting on the ground, a crouch begun slides on with the run's momentum,\n"
		"giving it up to friction and slope, steered a little by the stick; standing, jumping, a wall\n"
		"or a walk's pace ends it. Needs SPRINT." },
	{ "movement.slide_momentum", _config_real, "1", "HALO_MOVEMENT_SLIDE_MOMENTUM", _environment_value, _platform_flat,
		"SLIDE DISTANCE, 0.5x to 2x: the lift a slide starts with and how long it may last." },
	{ "movement.slide_friction", _config_real, "1", "HALO_MOVEMENT_SLIDE_FRICTION", _environment_value, _platform_flat,
		"SLIDE FRICTION, 0.25x to 3x: how quickly a slide gives up its speed." },
	{ "movement.slide_damage", _config_real, "1", "HALO_MOVEMENT_SLIDE_DAMAGE", _environment_value, _platform_flat,
		"SLIDE IMPACT, 0 to 3: sliding into an enemy staggers it or knocks it down (Grunts,\n"
		"Jackals; not Elites or Hunters) with up to half a melee at 1; 0: no impact." },
	{ "movement.slide_jump_cancel", _config_boolean, "true", "HALO_MOVEMENT_SLIDE_JUMP_CANCEL", _environment_value,
		_platform_flat, "JUMP CANCELS: Jump ends a slide at once, its momentum carried into the jump." },
	{ "movement.parkour_wallrun", _config_boolean, "false", "HALO_MOVEMENT_WALLRUN", _environment_value, _platform_flat,
		"WALL RUN, offline or a co-op host: jump at a wall while moving along it to run on it\n"
		"briefly; native collision stays in charge." },
	{ "movement.parkour_walljump", _config_boolean, "false", "HALO_MOVEMENT_WALLJUMP", _environment_value, _platform_flat,
		"WALL JUMP: Jump again in the air beside a wall to leap off it, the way you look." },
	{ "movement.wallrun_speed", _config_real, "1.5", "HALO_MOVEMENT_WALLRUN_SPEED", _environment_value, _platform_flat,
		"WALL SPEED, 1x to 2x." },
	{ "movement.wallrun_duration", _config_real, "2.0", "HALO_MOVEMENT_WALLRUN_DURATION", _environment_value,
		_platform_flat, "WALL TIME, 0.5 to 3 seconds." },
	{ "movement.walljump_scale", _config_real, "1.5", "HALO_MOVEMENT_WALLJUMP_SCALE", _environment_value, _platform_flat,
		"WALL JUMP POWER, 0.5x to 1.5x." },

	{ "launcher.show", _config_boolean, "true", "HALO_LAUNCHER", _environment_value, _platform_desktop,
		"Test72: the launcher first (play, the game data, the custom campaigns, settings, PC VR, What's New,\n"
		"updates): false starts the game at once, as before." },

	{ "debug.menu_open", _config_string, "\"\"", "HALO_MENU_OPEN", _environment_value, _platform_all,
		"Start on this screen of the menus (port/assets/menus) instead of the main\n"
		"menu, a player profile being edited; empty for the main menu." },

	{ "pvp.time_limit", _config_integer, "0", "HALO_PVP_TIME_LIMIT", _environment_value, _platform_all,
		"Match time limit in minutes: 0 (unlimited) to 1440." },

	{ "pvp.friendly_fire", _config_integer, "0", "HALO_PVP_FRIENDLY_FIRE", _environment_value, _platform_all,
		"Team damage: 0 on, 1 off, 2 shields only, 3 explosives only." },

	{ "pvp.vehicle_respawn_time", _config_integer, "0", "HALO_PVP_VEHICLE_RESPAWN_TIME", _environment_value, _platform_all,
		"Empty/destroyed vehicle respawn seconds: 0 never, up to 3600." },

	{ "pvp.auto_team_balance", _config_boolean, "false", "HALO_PVP_AUTO_TEAM_BALANCE", _environment_value, _platform_all,
		"Balance teams in the lobby and on death; host authority." },

	{ "pvp.radar_players", _config_integer, "0", "HALO_PVP_RADAR_PLAYERS", _environment_value, _platform_all,
		"Motion tracker: 0 all, 1 friends only, 2 none." },

	{ "pvp.custom_loadout", _config_boolean, "false", "HALO_PVP_CUSTOM_LOADOUT", _environment_value, _platform_all,
		"Use selected primary/secondary weapons instead of variant defaults." },

	{ "pvp.primary_weapon", _config_integer, "2", "HALO_PVP_PRIMARY_WEAPON", _environment_value, _platform_all,
		"Custom loadout: 0 none, 1 random, 2 AR, 3 pistol, 4 shotgun, 5 sniper, 6 rocket, 7 plasma pistol, 8 plasma rifle, 9 needler." },

	{ "pvp.secondary_weapon", _config_integer, "3", "HALO_PVP_SECONDARY_WEAPON", _environment_value, _platform_all,
		"Same custom-loadout weapon choices as primary_weapon." },

	{ "pvp.infinite_grenades", _config_boolean, "false", "HALO_PVP_INFINITE_GRENADES", _environment_value, _platform_all,
		"Unlimited grenades in launcher-hosted PvP, including expanded player counts." },

	{ "display.fullscreen", _config_boolean, "true", "HALO_FULLSCREEN", _environment_value, _platform_desktop,
		"Start fullscreen, drawing at the display's resolution and shape; false\n"
		"starts in a window, which draws the Xbox's 640x480. F11 switches." },
	{ "display.window_scale", _config_integer, "2", "HALO_WINDOW_SCALE", _environment_value, _platform_desktop,
		"Where display.window_size is empty: the window's size as a multiple of\n"
		"640x480." },
	{ "display.screen_width", _config_integer, "0", "HALO_SCREEN_WIDTH", _environment_value, _platform_android,
		"Columns of the 480-line picture: 0 for the display's shape, 640 for the\n"
		"Xbox's 4:3." },
	{ "display.vsync", _config_boolean, "true", "HALO_NO_VSYNC", _environment_set_is_false, _platform_all,
		"Wait for the display between frames; false draws as fast as possible." },
	{ "display.interpolation", _config_boolean, "true", "HALO_INTERPOLATION", _environment_value, _platform_all,
		"Draw a frame for every display refresh, blending between the game's 30\n"
		"ticks a second; false keeps the original 30 frames a second." },
	{ "display.direct_camera", _config_boolean, "true", "HALO_DIRECT_CAMERA", _environment_value, _platform_desktop,
		"In first person, point the view where the player aims now instead of\n"
		"where the last tick left it: the view turns the frame the mouse moves,\n"
		"not up to two ticks (66 ms) later." },
	/* OpenCE Build 201: the display options (FOV, the first-person weapon, your
	legs, the shield's color and glow). In VR the headset's projection, the
	gun and hands and the full body stay VR's own (view_fov.c,
	first_person_legs.c); the shield options apply to both. */
	{ "display.fov", _config_real, "0.0", "HALO_FOV", _environment_value, _platform_all,
		"The first-person view's field of view on foot, in degrees across at\n"
		"16:9 (20 to 150); 0 keeps the stock view. Vehicles, cinematics and\n"
		"scripted cameras keep their own." },
	{ "display.viewmodel_fov", _config_real, "0.0", "HALO_VIEWMODEL_FOV", _environment_value, _platform_all,
		"The first-person weapon's and hands' field of view, in degrees across\n"
		"at 16:9 (20 to 150); 0 keeps the weapon's stock view, also when\n"
		"display.fov widens the world." },
	{ "display.viewmodel_visible", _config_boolean, "true", "HALO_VIEWMODEL_VISIBLE", _environment_value, _platform_all,
		"Draw the first-person weapon, hands and what is attached to them.\n"
		"Off, they are not drawn; firing, animation, sound and lights go on." },
	{ "display.passenger_third_person", _config_boolean, "false", "HALO_PASSENGER_THIRD_PERSON", _environment_value, _platform_all,
		"Third-person Warthog and Scorpion passengers. Weapon zoom is disabled." },
	{ "display.viewmodel_shield", _config_boolean, "false", "HALO_VIEWMODEL_SHIELD", _environment_value, _platform_all,
		"The energy shield's flare on the first-person arms too, as on the body\n"
		"(the stock game draws it on the body only)." },
	/* (the OpenCE fork jcompto95-tech's third-person camera, third_person.c:
	this port's setting for it, so that a touchscreen without a keyboard can
	turn it on; controls.third_person still toggles it while playing) */
	{ "display.third_person", _config_boolean, "false", "HALO_THIRD_PERSON", _environment_value, _platform_all,
		"The third-person camera on foot in a campaign and co-op, over the\n"
		"shoulder (the flat game's; not in VR). controls.third_person toggles it." },
	/* (the OpenCE fork jcompto95-tech's stencil shadows, stencil_shadows.c: the
	fork keeps them to desktops; this port draws them on Android and Quest too,
	one face a draw, off by default) */
	{ "display.stencil_shadows", _config_boolean, "false", "HALO_STENCIL_SHADOWS", _environment_value, _platform_all,
		"Hard stencil shadows from characters and vehicles besides the game's\n"
		"own, as F.E.A.R. and Doom 3 cast them: on the level, on other objects\n"
		"and on themselves, your own body's and weapon's in first person too\n"
		"(in VR the body others see, the gun in your hand not; Test73b).\n"
		"Costly on a Quest." },
	{ "game.shadow_objects_stop", _config_boolean, "true", "HALO_SHADOW_OBJECTS_STOP", _environment_value, _platform_all,
		"(Test72, HaloFreak95's) Stencil shadows end at a machine or scenery behind what casts them\n"
		"(a cryotube's wall, a crate), as at the level, not through it; false: the level's alone." },
	{ "display.holstered_weapons", _config_boolean, "false", "HALO_HOLSTERED_WEAPONS", _environment_value, _platform_all,
		"(Test72, HaloFreak95's) Every character wears the weapons it carries but does not hold: a\n"
		"long gun slung across its back, a pistol or needler on its right thigh. false: they are\n"
		"unseen, as stock. Only the drawing changes." },
	{ "display.first_person_legs", _config_boolean, "false", "HALO_FIRST_PERSON_LEGS", _environment_value, _platform_all,
		"Your own legs in first person, seen looking down: your body as others\n"
		"see it, drawn from the waist down, moving as it moves." },
	{ "display.big_multiplayer_guns", _config_string, "\"classic\"", "HALO_BIG_MULTIPLAYER_GUNS", _environment_value, _platform_all,
		"Weapons and equipment lying about in multiplayer: \"classic\" (the\n"
		"game's, drawn larger than in the campaign) or \"enhanced\" (their real\n"
		"size, as in the campaign). Only how they are drawn changes." },
	{ "display.overshield_effect", _config_boolean, "false", "HALO_OVERSHIELD_EFFECT", _environment_value, _platform_all,
		"The overshield as this machine draws it: its flare rising up the body as\n"
		"it charges (2 seconds, 3 from a shield that was down) and staying lit,\n"
		"its energy slower, in its own color (the powerup's), on the first-person\n"
		"arms too with viewmodel_shield; a hit on it plays what the projectile\n"
		"plays on an Elite's shield. Only what is drawn and heard changes." },
#ifdef HALO_ANDROID
	{ "game.blood_layers", _config_integer, "6", "HALO_BLOOD_LAYERS", _environment_value, _platform_all,
		"BLOOD LAYERS (Test72, HaloFreak95's): how many splats of blood one place carries at once\n"
		"(0: as many as land there, the game's own). Blood on blood is solid after a few layers and\n"
		"no redder for the rest, and each layer is its pixels drawn again: 6 on Android and the Quest." },
#else
	{ "game.blood_layers", _config_integer, "0", "HALO_BLOOD_LAYERS", _environment_value, _platform_all,
		"BLOOD LAYERS (Test72, HaloFreak95's): how many splats of blood one place carries at once\n"
		"(0: as many as land there, the game's own). Blood on blood is solid after a few layers and\n"
		"no redder for the rest, and each layer is its pixels drawn again." },
#endif
	{ "game.dynamic_blood", _config_boolean, "false", "HALO_DYNAMIC_BLOOD", _environment_value, _platform_all,
		"Blood thrown onto the wall behind a body hit, running down walls in\n"
		"drops, and landing on trees and fences (port/linux/game/blood.c).\n"
		"This machine's alone: nothing is sent. false: the game's own blood." },
	{ "game.vehicle_blood", _config_boolean, "false", "HALO_VEHICLE_BLOOD", _environment_value, _platform_all,
		"Blood that stains vehicles and objects (rocks, crates, machines,\n"
		"dropped weapons), painted on their drawn models and riding with them,\n"
		"and runs down their sides (port/linux/game/blood.c). This machine's\n"
		"alone: nothing is sent. false: the game's own blood." },
	{ "game.projectile_effects", _config_boolean, "false", "HALO_PROJECTILE_EFFECTS", _environment_value, _platform_all,
		"Bullet holes and plasma burns painted onto vehicles, objects, Spartans'\n"
		"bare armor and two-sided surfaces (trees, fences), moving with what\n"
		"they are on, and plasma's sparks and smoke lingering where a bolt\n"
		"struck a vehicle or bare armor (blood.c, plasma_embers.c); plasma\n"
		"bolts lighting the level and what they pass in their own color, needles\n"
		"more dimly (projectile_lights.c). This machine's alone: nothing is\n"
		"sent. false: the game's own." },
#ifdef HALO_ANDROID
	{ "display.decals_on_gpu", _config_boolean, "false", "HALO_GPU_DECALS", _environment_value, _platform_all,
		"FAST DECALS (Test72, HaloFreak95's): decals (bullet holes, scorch marks, blood) drawn from the\n"
		"vertices they were made with, a draw each, as the Xbox drew them, instead of their vertices\n"
		"written again every frame. The picture is the same. Off by default on Android and the Quest,\n"
		"whose frames are bound by their draws, until measured there ([render-perf] shows the draws)." },
#else
	{ "display.decals_on_gpu", _config_boolean, "true", "HALO_GPU_DECALS", _environment_value, _platform_all,
		"FAST DECALS (Test72, HaloFreak95's): decals (bullet holes, scorch marks, blood) drawn from the\n"
		"vertices they were made with, a draw each, as the Xbox drew them, instead of their vertices\n"
		"written again every frame. The picture is the same; many decals cost a fraction. Off: as before." },
#endif
	{ "display.shield_glow", _config_boolean, "false", "HALO_SHIELD_GLOW", _environment_value, _platform_all,
		"Energy shields light what is around them as they flare, as plasma\n"
		"does, in the shield's own color." },
	{ "display.shield_glow_intensity", _config_string, "\"default\"", "HALO_SHIELD_GLOW_INTENSITY", _environment_value, _platform_all,
		"How far the shield glow reaches: \"default\" or \"light_show\" (three\n"
		"times as far, and at full strength from a third of the flare)." },
	{ "display.shield_color", _config_string, "\"default\"", "HALO_SHIELD_COLOR", _environment_value, _platform_all,
		"Your energy shield's color: \"default\" (the shield's own) or a\n"
		"multiplayer armor color: white, black, red, blue, gray, yellow, green,\n"
		"pink, purple, cyan, cobalt, orange, teal, sage, brown, tan, maroon or\n"
		"salmon. In a network game the other players see it if they show\n"
		"players' custom armor (display.others_custom_armor)." },
	{ "display.spartan_armor_color", _config_string, "\"default\"", "HALO_SPARTAN_ARMOR_COLOR", _environment_value,
		_platform_all,
		"Your Spartan's armor in a multiplayer armor color (\"white\", \"black\",\n"
		"\"red\"...) or \"blackout\" (deeper than black); \"default\": the game's\n"
		"own (in multiplayer, the profile's color). A team game keeps the team's." },
	{ "display.spartan_lights_color", _config_string, "\"default\"", "HALO_SPARTAN_LIGHTS_COLOR", _environment_value,
		_platform_all,
		"Your Spartan's armor lights (the glow above the knees) in a multiplayer\n"
		"armor color; \"default\": the game's own." },
	{ "display.visor_color", _config_string, "\"default\"", "HALO_VISOR_COLOR", _environment_value, _platform_all,
		"Your Spartan's visor in a multiplayer armor color (\"white\", \"black\",\n"
		"\"red\"... as the profile's colors; black a deep, glossy black), or\n"
		"\"default\", its gold." },
	{ "display.ammo_counter_color", _config_string, "\"default\"", "HALO_AMMO_COUNTER_COLOR", _environment_value,
		_platform_all,
		"(Test72, HaloFreak95's) The assault rifle's display in your hands - its ammo counter,\n"
		"compass and icons: \"default\" (its own blue) or a multiplayer armor color (\"white\",\n"
		"\"red\", \"green\"...). Only how it is drawn changes." },
	{ "display.flashlight_color", _config_string, "\"default\"", "HALO_FLASHLIGHT_COLOR", _environment_value,
		_platform_all,
		"Your Spartan's flashlight in a multiplayer armor color; \"default\": the\n"
		"game's own." },
	{ "display.others_custom_armor", _config_boolean, "false", "HALO_OTHERS_CUSTOM_ARMOR", _environment_value,
		_platform_all,
		"Other players' Spartans as they chose them (Pause > Armor): their armor,\n"
		"lights, visor and shield (and flashlight, display.others_custom_flashlights);\n"
		"false: every one as stock." },
	{ "display.others_custom_flashlights", _config_boolean, "false", "HALO_OTHERS_CUSTOM_FLASHLIGHTS",
		_environment_value, _platform_all,
		"Other players' flashlights in the colors they chose; false: as stock." },
	{ "display.high_res_hud", _config_boolean, "true", "HALO_HIGH_RES_HUD", _environment_value, _platform_all,
		"Draw the HUD (meters, counters, panels, motion sensor, reticles,\n"
		"waypoints, scopes) from the high-res assets (8x the maps' bitmaps);\n"
		"false draws the maps' own bitmaps." },
	{ "display.high_res_text", _config_boolean, "true", "HALO_HIGH_RES_TEXT", _environment_value, _platform_all,
		"Draw the menus' and HUD's text with the fonts in port/assets/fonts\n"
		"(Overpass) at the resolution the game draws at, and the menus' titles\n"
		"from port/assets/titles; false draws the maps' bitmap fonts and titles." },
	{ "display.player_names", _config_string, "\"all\"", "HALO_PLAYER_NAMES", _environment_value, _platform_all,
		"In multiplayer, whose names are drawn above their heads: \"all\",\n"
		"\"allies\", \"enemies\" or \"none\". An enemy's shows only within the\n"
		"motion sensor's reach, in sight and not camouflaged; none show if the\n"
		"gametype's motion tracker shows no players, only allies' if it shows\n"
		"only friends." },
	{ "display.player_name_scale", _config_real, "1.0", "HALO_PLAYER_NAME_SCALE", _environment_value, _platform_all,
		"How large the players' names are drawn: 1.0 three quarters of the size of\n"
		"the HUD's text, 0.25 to 4." },
	{ "display.match_timer", _config_boolean, "false", "HALO_MATCH_TIMER", _environment_value, _platform_all,
		"Draw the match clock at the bottom right in multiplayer. It counts up\n"
		"from the game's first tick, which the items' respawn periods count from\n"
		"too: a weapon is back on the map at every multiple of its period on the\n"
		"clock." },
	{ "display.scoreboard_team_layout", _config_string, "\"teams\"", "HALO_SCOREBOARD_TEAM_LAYOUT", _environment_value,
		_platform_all,
		"How the scoreboard lists a team game's players: \"teams\" in a column for\n"
		"each team (red on the left, blue on the right), \"score\" all in order of\n"
		"score." },
	{ "display.scoreboard_background", _config_boolean, "true", "HALO_SCOREBOARD_BACKGROUND", _environment_value,
		_platform_all,
		"Draw a panel behind the multiplayer scoreboard, for clearer text." },
	{ "display.scoreboard_background_color", _config_string, "\"16, 16, 16, 150\"", "HALO_SCOREBOARD_BACKGROUND_COLOR",
		_environment_value, _platform_all,
		"The scoreboard panel's colour: \"red, green, blue, alpha\", each 0 to 255\n"
		"(alpha 0 is see-through, 255 solid)." },
	{ "display.anisotropic_filtering", _config_integer, "0", "HALO_ANISOTROPIC_FILTERING", _environment_value,
		_platform_all,
		"Sharpen textures seen at a grazing angle (floors, walls, terrain), which\n"
		"the Xbox did not: 2, 4, 8 or 16 times; 0 filters as the Xbox did. 4 keeps\n"
		"most of 16's sharpness at less cost on slower GPUs." },
	{ "display.latency_meter", _config_boolean, "false", "HALO_LATENCY_METER", _environment_value, _platform_all,
		"In a network game, your round trip to the host in milliseconds at the\n"
		"screen's top right, with bars (the host: its slowest player's), and\n"
		"\"Connection problem\" when the host has sent nothing for two seconds;\n"
		"the scoreboard's pings in the same colours, as this machine times them." },

	{ "audio.enabled", _config_boolean, "true", "HALO_NO_AUDIO", _environment_set_is_false, _platform_all,
		"Play sound." },
	{ "audio.volume", _config_real, "1.0", "HALO_VOLUME", _environment_value, _platform_all,
		"The volume of everything, 0.0 to 1.0." },

	{ "input.mouse_sensitivity", _config_real, "1.0", "HALO_MOUSE_SENSITIVITY", _environment_value, _platform_desktop,
		"How far the view turns for the mouse's movement." },
	{ "input.invert_mouse", _config_boolean, "false", "HALO_MOUSE_INVERT", _environment_set_is_true, _platform_desktop,
		"Moving the mouse forward looks down." },
	{ "network.block_mouse_aim_assist", _config_boolean, "false", "HALO_NET_BLOCK_MOUSE_AIM_ASSIST", _environment_value,
		_platform_all, "Host: disable optional mouse magnetism for mouse/keyboard players. Controllers and touch keep their own assist." },
	{ "input.mouse_aim_assist", _config_boolean, "false", "HALO_MOUSE_AIM_ASSIST", _environment_value, _platform_desktop,
		"Magnetism while aiming with the mouse, as with a controller: the view\n"
		"slowed and dragged along by a target. The last of the mouse and the\n"
		"right stick to move decides. The bullets' autoaim (bent toward the\n"
		"target) stays either way. On Android it is the touch controls' too:\n"
		"swipe and gyro aim (HaloFreak95's input.touch_aim_assist)." },

	{ "game.console_log", _config_string, "\"important\"", "HALO_CONSOLE_LOG", _environment_value, _platform_all,
		"What the game's console shows on screen of what it logs: \"important\"\n"
		"(bans, players dropped for cheating, what refuses a command, and the\n"
		"asserts that stop the game), \"all\" (every line, the game's own\n"
		"chatter too), or \"none\" (the asserts that stop the game only). What\n"
		"a command prints shows whatever this is, and debug.txt has every line." },

	{ "game.language", _config_string, "\"\"", "HALO_LANGUAGE", _environment_value, _platform_all,
		"The language the game asks the Xbox for: \"ja\", \"de\", \"fr\", \"es\" or \"it\";\n"
		"empty for English. The game data decides what is translated." },
	{ "game.custom_edition", _config_boolean, "true", "HALO_CUSTOM_EDITION", _environment_set_is_true, _platform_all,
		"Load and run Halo Custom Edition and OpenSauce (.yelo) maps, and download\n"
		"them from a network game's host (network.map_download); false refuses them.\n"
		"Experimental: docs/custom_edition_caches.md in the source says what works.\n"
		"On by default, as OpenCE's (Test73). On Android the launcher's mods turn it on while\n"
		"one is installed." },
	{ "game.custom_edition_revision", _config_integer, "1", "HALO_CUSTOM_EDITION_REVISION", _environment_value,
		_platform_all,
		"Internal one-time migration (Test73): a config saved with Custom Edition\n"
		"maps off, the default before, has them on. Later saved choices are kept." },
	{ "bots.count", _config_integer, "0", "HALO_BOTS", _environment_value, _platform_all,
		"Computer players (bots) in a local (split screen) Slayer game: 0 to 15\n"
		"join its lobby with the players, as far as the game has room. Not in\n"
		"system link or internet games. 0: none, as in the stock game." },
	{ "bots.skill", _config_string, "\"normal\"", "HALO_BOT_SKILL", _environment_value, _platform_all,
		"How well the bots play: \"easy\", \"normal\", \"heroic\" or \"legendary\"\n"
		"(how soon they react, how well they aim, how far they see)." },
	{ "bots.teams", _config_string, "\"even\"", "HALO_BOT_TEAMS", _environment_value, _platform_all,
		"In team games: \"even\" puts the bots on both teams, evening them with\n"
		"the players; \"against\" puts them all on the team the players are not on." },
	/* OpenCE Build 201 */
#ifdef HALO_PROFILE
	{ "debug.profile_record", _config_boolean, "false", "HALO_PROFILE_RECORD", _environment_value, _platform_all,
		"Record a profile with no command (configure.py --profile builds): from\n"
		"when profile_record_when says until profile_stop, a map change or the\n"
		"end, into numbered part files in the data folder's profiles folder\n"
		"(tools/net_report.py reads it)." },
	{ "debug.profile_record_when", _config_string, "\"start\"", "HALO_PROFILE_RECORD_WHEN", _environment_value,
		_platform_all,
		"\"start\": from the first frame until the first map change; \"game\":\n"
		"each game that is not the main menu, a recording each, until its map\n"
		"goes." },
	{ "debug.profile_memory", _config_integer, "256", "HALO_PROFILE_MEMORY", _environment_value, _platform_all,
		"Megabytes a recording keeps in memory, 4 to 1024: two halves, each\n"
		"written out as a part when it fills." },
#endif
	/* (the OpenCE fork jcompto95-tech's item physics, item_physics.c: the fork
	keeps it to desktops; this port runs it on Android and Quest too, Jolt in
	the host as the ragdolls) */
	{ "game.item_physics", _config_string, "\"classic\"", "HALO_ITEM_PHYSICS", _environment_value, _platform_all,
		"How weapons and grenades lying about move: \"classic\" (the game's own:\n"
		"they bounce, then lie flat) or \"enhanced\" (rigid bodies, Jolt Physics:\n"
		"they tumble, lean against what they meet and fall off ledges; pushed by\n"
		"shots and blasts as the game pushes them). An item a hand holds is the\n"
		"hand's. In a network game the host's moves them." },
	{ "game.death_physics", _config_string, "\"ragdoll\"", "HALO_DEATH_PHYSICS", _environment_value, _platform_all,
		"How bodies fall when they die: \"classic\" (the game's own death\n"
		"animations) or \"ragdoll\" (limp, thrown by what killed them, tumbling\n"
		"down stairs and over ledges: OpenCE's dynamic ragdolls, Jolt Physics,\n"
		"port/third_party/JoltPhysics; on Android the host runs Jolt). How this\n"
		"machine draws its dead: nothing sent between machines changes." },
	{ "display.text_chat", _config_boolean, "true", "HALO_TEXT_CHAT", _environment_value, _platform_all,
		"Show the text chat of network games: false hides its lines and keeps\n"
		"its keys (and a controller's D-pad Up) from opening it, as for purists\n"
		"and streamers." },
	{ "display.chat_style", _config_string, "\"colors\"", "HALO_CHAT_STYLE", _environment_value, _platform_all,
		"The text chat's colours: \"colors\" (names in their team's colour, the\n"
		"text a pale blue) or \"white\" (all white, as Custom Edition)." },
	{ "game.enhanced_animations", _config_boolean, "true", "HALO_ENHANCED_ANIMATIONS", _environment_value, _platform_all,
		"The player bipeds' grenade throws keep their legs moving (crouched,\n"
		"in the air and in a vehicle's seat too), riders' hands leave the grips\n"
		"to throw and reload, and a player turns with the aim while throwing;\n"
		"false: the original animations, which freeze the legs and stand a\n"
		"rider up." },
	/* (the OpenCE fork jcompto95-tech's corpse and decal limits) */
	{ "game.corpse_duration", _config_string, "\"default\"", "HALO_CORPSE_DURATION", _environment_value, _platform_all,
		"How long the dead lie in multiplayer: \"default\" (the game's own,\n"
		"30 seconds still) or \"extended\" (until the game's limit on its active\n"
		"garbage clears the oldest no one sees). In a network game the host's\n"
		"setting decides." },
	{ "game.corpse_limit", _config_string, "\"default\"", "HALO_CORPSE_LIMIT", _environment_value, _platform_all,
		"How many of the dead (and dropped weapons) stay about before those no\n"
		"player sees are cleared: \"default\" (the game's own, 50 for 16 players)\n"
		"or \"insane\" (eight times as many). In a network game the host's\n"
		"setting decides." },
	{ "game.decal_duration", _config_string, "\"default\"", "HALO_DECAL_DURATION", _environment_value, _platform_all,
		"How long marks the game keeps for minutes (bullet holes, scorch marks,\n"
		"blood) last: \"default\" (the game's own, about three minutes, then they\n"
		"fade) or \"extended\" (until the decal limit retires the oldest)." },
	{ "game.decals", _config_string, "\"default\"", "HALO_DECALS", _environment_value, _platform_desktop,
		"How many decals (bullet holes, scorch marks, blood) stay before the\n"
		"oldest give way: \"default\" (the game's own limit, 2048), \"4x\", \"8x\"\n"
		"or \"insane\" (twelve times). Takes effect as the next map begins." },

	/* (true, unlike OpenCE's false since 329d311c1: this port's releases have
	always opened every level to a new profile, and the owner keeps it so) */
	{ "game.unlock_all_levels", _config_boolean, "true", "HALO_UNLOCK_ALL_LEVELS", _environment_value, _platform_all,
		"true (default): a profile made from now on starts with every campaign\n"
		"level finished on every difficulty, so any level can be played. false: a\n"
		"new profile starts at The Pillar of Autumn and opens each level as the\n"
		"one before it is finished, as on the Xbox. Profiles already made keep\n"
		"their progress; the console's profile_reset_campaign starts one over." },

	{ "game.downloaded_maps", _config_string, "\"\"", "HALO_DOWNLOADED_MAPS", _environment_value, _platform_all,
		"Maps played as downloaded ones, besides those map sharing downloaded:\n"
		"their names (bloodgulch, hugeass), separated by commas, or \"*\" for\n"
		"every map. A downloaded map's scripts may not change the player's\n"
		"settings or other players' games (debug.txt names what they were\n"
		"refused)." },

	{ "paths.data", _config_string, "\"\"", "HALO_DATA_ROOT", _environment_value, _platform_desktop,
		"The folder holding the game data's maps folder; empty looks in the\n"
		"working directory and its assets folder. Windows paths are easiest in\n"
		"single quotes: 'C:\\Games\\Halo'." },
	{ "paths.saves", _config_string, "\"\"", "HALO_SAVE_ROOT", _environment_value, _platform_desktop,
		"Where saved games and profiles go; empty for the usual place\n"
		"(~/.local/share/halo-linux, or %APPDATA%\\halo on Windows)." },

	{ "renderer.safe_geometry", _config_boolean, "true", "HALO_SAFE_GEOMETRY", _environment_value, _platform_all,
		"Geometry compatibility mode (restart required). Streams every vertex each\n"
		"frame (the fenced stream ring on Android), with no static mirrors, and CPU index rebasing.\n"
		"Default on everywhere: Quest VR, flat Android, Windows and Linux, flat and PC VR (Test72:\n"
		"Normal's vertex mirror drew stale effect and grass quads on PC). Not a data revision selector." },
    { "renderer.vr_geometry_revision", _config_integer, "1", "HALO_VR_GEOMETRY_REVISION", _environment_value, _platform_vr,
        "Internal VR geometry-default migration revision. Keep at 1 after choosing Safe or Normal." },
    { "renderer.android_geometry_revision", _config_integer, "1", "HALO_ANDROID_GEOMETRY_REVISION", _environment_value, _platform_android,
        "Internal flat Android geometry-default migration revision. Keep at 1 after choosing Safe or Normal. VR uses vr_geometry_revision." },
    { "renderer.desktop_geometry_revision", _config_integer, "1", "HALO_DESKTOP_GEOMETRY_REVISION", _environment_value, _platform_desktop,
        "Internal Windows/Linux (flat and PC VR) geometry-default migration revision. Keep at 1 after choosing Safe or Normal." },

	{ "network.address", _config_string, "\"\"", "HALO_NET_ADDRESS", _environment_value, _platform_all,
		"This machine's IPv4 address for system link, for a machine on several\n"
		"networks; empty chooses one." },
	{ "network.broadcast", _config_string, "\"\"", "HALO_NET_BROADCAST", _environment_value, _platform_all,
		"Comma-separated IPv4 addresses system link sends its announcements to\n"
		"instead of the local network's broadcast address (for VPNs); empty for\n"
		"the local network." },
	{ "network.online", _config_boolean, "true", "HALO_NET_ONLINE", _environment_value, _platform_all,
		"Internet play: hosting makes an invite link (logged, and put on the\n"
		"clipboard) that lets whoever has it join over the internet; opening a\n"
		"link (or copying one before switching to the game) joins. Only people\n"
		"with the invite can join. Off keeps system link to the local network." },
	{ "network.voice_lobby", _config_boolean, "true", "HALO_NET_VOICE_LOBBY", _environment_value, _platform_all,
		"Hosting: allow voice chat in the lobby and post-game." },
	{ "network.voice_mode", _config_string, "\"team_global_enemy_proximity\"", "HALO_NET_VOICE_MODE",
		_environment_value, _platform_all,
		"Hosting: in-game voice routing: off, team_proximity, team_enemy_proximity,\n"
		"team_global, or team_global_enemy_proximity. The Server Setup menu names\n"
		"these choices." },
	{ "network.voice_kbps", _config_integer, "24", "HALO_NET_VOICE_KBPS", _environment_value, _platform_all,
		"Hosting: voice quality in kilobits per second, 8 to 64." },
	{ "network.voice_proximity", _config_real, "15.0", "HALO_NET_VOICE_PROXIMITY", _environment_value,
		_platform_all,
		"Hosting: proximity voice range in world units (5 to 100)." },
    { "network.legacy_coop", _config_boolean, "false", "HALO_NET_LEGACY_COOP", _environment_value, _platform_all,
        "Opt in to legacy network12-14 campaign clients. Old hosts omit scripted team allegiance; marine reticles/team behavior may be wrong.\n"
        "Uses old native presentation behavior without invented allegiance. Prefer network15-32. Hosting stays32." },
	{ "network.protocol_version", _config_integer, "0", "HALO_NETWORK_PROTOCOL_VERSION", _environment_value,
		_platform_all,
		"Client protocol target: 0 (the default) lists/joins all compatible PvP\n"
		"versions 11-32; 11-32 selects one exact server version. This build always\n"
		"hosts as 32. Campaign needs15+, or consented legacy12-14; Custom Edition\n"
		"needs23+. This is read once when the game starts; restart after changing\n"
		"it. Invalid values fall back to the build's own version, 32." },
	{ "network.join_from_clipboard", _config_boolean, "true", "HALO_NET_JOIN_FROM_CLIPBOARD", _environment_value,
		_platform_all,
		"Join the game of an invite link found on the clipboard when the game\n"
		"comes to the front." },
	{ "network.tunnel_port", _config_integer, "0", "HALO_NET_TUNNEL_PORT", _environment_value, _platform_all,
		"The UDP port internet play uses; 0 picks one. A fixed one can be\n"
		"forwarded on the router, for networks whose NAT stops connections." },
	{ "network.allow_upnp", _config_boolean, "true", "HALO_NET_ALLOW_UPNP", _environment_value, _platform_all,
		"Let internet play ask the router (UPnP) to forward its port, for\n"
		"networks whose NAT stops connections: when a player joins this\n"
		"machine's game, and when joining a game takes too long. False never\n"
		"asks." },
    { "network.public_lobby", _config_boolean, "true", "HALO_NET_PUBLIC_LOBBY", _environment_value, _platform_all,
        "Discover signed public OpenCE games in the in-game System Link list." },
    { "network.host_public", _config_boolean, "false", "HALO_NET_HOST_PUBLIC", _environment_value, _platform_all,
        "List native PvP hosts publicly. Launcher PUBLIC overrides this per session. Campaign remains separate." },
	{ "network.signalling_brokers", _config_string,
		"\"opence.milenko.org:1883,broker.emqx.io:1883,broker.hivemq.com:1883,test.mosquitto.org:1883\"",
		"HALO_NET_BROKERS", _environment_value, _platform_all,
		"Public MQTT brokers through which the machines of an invite find each\n"
		"other and the server browser's listings travel (its messages are\n"
		"encrypted); comma-separated host:port, up to 4. The first is upstream's\n"
		"own broker (its brokers.txt); the earlier default list moves to this one." },
	/* HaloCE-Quest-VR: the OpenCE fork jcompto95-tech's network extras
	(network_extras.c): their own message kinds, after OpenCE's own (87-90),
	which OpenCE builds drop as unknown; no network version change */
	{ "network.community_extras", _config_boolean, "true", "HALO_NET_COMMUNITY_EXTRAS", _environment_value,
		_platform_all,
		"Community extras in network games: the players' Armor choices shared\n"
		"and votes to kick or ban with a reason. Players without them (OpenCE's\n"
		"own builds) play as ever; false sends and reads none. Text chat is\n"
		"OpenCE's own (network.text_chat). A player on a Test67-72 build of\n"
		"this port gets none of these, nor text chat, until they update." },
    { "net.physical_interactions", _config_boolean, "true", "HALO_NET_PHYSICAL_INTERACTIONS", _environment_value, _platform_all,
        "VR INTERACTIONS (Server Setup > co-op options): the co-op host's one switch for every player's VR play: grabs and\n"
        "throws, physical melee and fist combat, physical grenades, manual reload, parkour and slides (Test73: HOST VR\n"
        "ACTIONS is part of it). Requires every peer to negotiate this port's optional extension. On by default (Test73);\n"
        "unsupported or mixed peers retain ordinary campaign behavior. PvP keeps stock movement." },
    { "net.vr_interactions_applied", _config_boolean, "false", "HALO_NET_VR_INTERACTIONS_APPLIED", _environment_value, _platform_all,
        "Internal: Test73 turned VR INTERACTIONS on once in older configs (config_vr_interactions_default)." },
	/* OpenCE build 232: the host's text chat (network_chat.c) */
	{ "network.text_chat", _config_string, "\"on\"", "HALO_NET_TEXT_CHAT", _environment_value, _platform_all,
		"Hosting: the text chat: \"on\" (links in lines are refused), \"links\"\n"
		"(links allowed) or \"off\", in either case. Always off in a game listed\n"
		"in the server browser (a public internet game)." },
	/* OpenCE Build 201: votes to kick and map sharing (no network version
	change: network 28 carries them, and 29, which this build hosts, since) */
	{ "network.votekick", _config_boolean, "true", "HALO_NET_VOTEKICK", _environment_value, _platform_all,
		"Hosting: let the players vote to kick a player (the scoreboard's\n"
		"right-click, or the console's votekick). More than half of the players\n"
		"must vote, counted once per address." },
	{ "network.votekick_minutes", _config_integer, "5", "HALO_NET_VOTEKICK_MINUTES", _environment_value,
		_platform_all,
		"Hosting: the minutes a player must have played on this server to start\n"
		"a vote to kick (0 to 60); to vote, 2 minutes or this, the less." },
	{ "network.votekick_ban_minutes", _config_integer, "30", "HALO_NET_VOTEKICK_BAN_MINUTES", _environment_value,
		_platform_all,
		"Hosting: the minutes a player kicked by a vote cannot join again (1 to\n"
		"1440)." },
	{ "network.map_share", _config_boolean, "true", "HALO_NET_MAP_SHARE", _environment_value, _platform_all,
		"Hosting: send a player who joins without the game's Custom Edition map\n"
		"the map (from the active game set's maps folder), and the resource maps\n"
		"it needs that the player lacks (bitmaps.map, sounds.map, loc.map). The\n"
		"game's other players help (network.map_seed)." },
	/* (Test73b, the owner: "raise the download limit to max ... by default so
	custom map downloads via the server are faster": both limits at their
	most, OpenCE's 1024 and 256 before; network.map_share_revision) */
	{ "network.map_share_upload", _config_integer, "20480", "HALO_NET_MAP_SHARE_UPLOAD", _environment_value,
		_platform_all,
		"Hosting: the most kilobytes a second the host sends of maps (16 to\n"
		"20480: 20 MB/s, the default), so that the game's own traffic keeps the\n"
		"rest of the upload: lower it if the game lags while a player\n"
		"downloads a map." },
	{ "network.map_seed", _config_boolean, "true", "HALO_NET_MAP_SEED", _environment_value, _platform_all,
		"Playing in another's game: help send its map to players who join\n"
		"without it, as a torrent's peers do (the pieces this machine has, at\n"
		"most network.map_seed_upload)." },
	{ "network.map_seed_upload", _config_integer, "10240", "HALO_NET_MAP_SEED_UPLOAD", _environment_value,
		_platform_all,
		"Playing in another's game: the most kilobytes a second this machine\n"
		"sends of maps to other players (16 to 10240: 10 MB/s, the default)." },
	{ "network.map_download", _config_boolean, "true", "HALO_NET_MAP_DOWNLOAD", _environment_value, _platform_all,
		"Joining a game on a Custom Edition map this machine does not have:\n"
		"download it (and the resource maps it needs) from the game's players\n"
		"into the active game set's maps folder, if its host shares it\n"
		"(network.map_share); false leaves the game, saying which map is\n"
		"missing." },
	{ "network.map_share_revision", _config_integer, "1", "HALO_NET_MAP_SHARE_REVISION", _environment_value,
		_platform_all,
		"Internal one-time migration (Test73b): a config saved with the map\n"
		"upload limits at their defaults before (1024 and 256) has them at their\n"
		"most. Later saved choices are kept." },
	{ "network.coop_public", _config_boolean, "false", "HALO_NET_COOP_PUBLIC", _environment_value, _platform_all,
		"Whether an online co-op game (Create Game > Internet, a SINGLEPLAYER\n"
		"map) starts as PUBLIC or, false, PRIVATE: Server Setup's LISTING in\n"
		"co-op, which writes its choice here." },
	{ "network.coop_friendly_fire", _config_string, "\"on\"", "HALO_NET_COOP_FRIENDLY_FIRE", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game hurt each other: \"off\",\n"
		"\"on\", \"shields_only\" or \"explosives_only\" (FRIENDLY FIRE in\n"
		"co-op's Server Setup > Co-op Options writes its choice here). Their AI\n"
		"allies they hurt as network.coop_ally_friendly_fire says." },
	{ "network.coop_ally_friendly_fire", _config_boolean, "true", "HALO_NET_COOP_ALLY_FRIENDLY_FIRE",
		_environment_value, _platform_all,
		"Whether the players of an online co-op game hurt their AI allies (the\n"
		"Marines, Captain Keyes, the Sentinels while they are allies), as in the\n"
		"campaign; false, the players' weapons, grenades and vehicles do them\n"
		"no harm. ALLY FRIENDLY FIRE in co-op's Server Setup > Co-op Options\n"
		"writes its choice here." },
	{ "network.coop_own_friendly_fire", _config_boolean, "true", "HALO_NET_COOP_OWN_FRIENDLY_FIRE",
		_environment_value, _platform_all,
		"Whether this machine's players hurt, and are hurt by, the other players\n"
		"of an online co-op game: two players hurt each other only if both have\n"
		"it true. FRIENDLY FIRE in that game's pause menu writes its choice\n"
		"here. A player away from the controls for over 70 seconds is hurt by\n"
		"every player, whatever either chose." },
	{ "network.coop_player_collisions", _config_boolean, "true", "HALO_NET_COOP_PLAYER_COLLISIONS", _environment_value,
		_platform_all,
		"Whether the players of an online co-op game bump into each other;\n"
		"false, they walk through each other (the AI's characters they still\n"
		"bump into). PLAYER COLLISIONS in co-op's Server Setup > Co-op Options\n"
		"writes its choice here." },
	{ "network.coop_enemies_mode", _config_string, "\"per_player\"", "HALO_NET_COOP_ENEMIES_MODE", _environment_value,
		_platform_all,
		"Online co-op's extra enemies: \"none\", \"per_player\" (each squad of\n"
		"enemies grows by coop_enemies for each player past the first) or\n"
		"\"multiplier\" (each is coop_enemies_multiplier times as large, for any\n"
		"number of players). EXTRA ENEMIES in co-op's Server Setup > Co-op\n"
		"Options writes its choice here." },
	{ "network.coop_pickups", _config_string, "\"none\"", "HALO_NET_COOP_PICKUPS", _environment_value, _platform_all,
		"Extra campaign weapons, grenades, health kits, overshields and camouflage.\n"
		"Modes: \"none\", \"per_player\" (coop_enemies percent per extra player),\n"
		"or \"multiplier\" (coop_enemies_multiplier times as many), up to 8 times\n"
		"the original pickups. Only safe nearby places receive copies." },
	{ "network.coop_vehicles", _config_string, "\"none\"", "HALO_NET_COOP_VEHICLES", _environment_value, _platform_all,
		"Extra campaign vehicles: \"none\", \"per_player\" or \"multiplier\".\n"
		"After a player clears its unloaded starting place by both bounding\n"
		"radii plus 15 percent, make one more there, up to\n"
		"floor((players - 1) * coop_enemies * 2 / 300) extras, or floor((coop_enemies_multiplier - 1) * 2 / 3) in static mode.\n"
		"The host alone creates them, and waits while the starting place is blocked." },
	{ "network.coop_enemies", _config_integer, "50", "HALO_NET_COOP_ENEMIES", _environment_value, _platform_all,
		"Online co-op's extra enemies per player, a percentage: for each player\n"
		"past the first, each squad of enemies a level places gets this much of\n"
		"itself more (100: as many again; 25 to 200). PER PLAYER in co-op's\n"
		"Server Setup > Co-op Options writes its choice here." },
	{ "network.coop_enemies_multiplier", _config_integer, "2", "HALO_NET_COOP_ENEMIES_MULTIPLIER", _environment_value,
		_platform_all,
		"Online co-op's shared static multiplier (2 to 32). Pickups are capped\n"
		"at 8 times their original count; extra vehicle growth uses two thirds\n"
		"of (multiplier - 1), rounded down. MULTIPLIER in\n"
		"co-op's Server Setup > Co-op Options writes its choice here." },
	{ "network.stun_servers", _config_string, "\"stun.l.google.com:19302,stun.cloudflare.com:3478\"",
		"HALO_NET_STUN", _environment_value, _platform_all,
		"Public STUN servers that tell this machine its internet address;\n"
		"comma-separated host:port." },
	{ "discord.application_id", _config_string, "\"1553978809840050229\"", "HALO_DISCORD_APPLICATION",
		_environment_value, _platform_desktop,
		"The Discord application internet play invites go through while the\n"
		"Discord desktop client runs; empty for none." },

	{ "update.auto", _config_boolean, "true", "HALO_UPDATE_AUTO", _environment_value, _platform_all,
		"Look for a new version when the game starts, and offer to update to it;\n"
		"false never looks (the game's \"Do not ask again\" writes false here)." },
	{ "streamer.enabled", _config_boolean, "false", "HALO_STREAMER_MODE", _environment_value, _platform_all,
		"Hide offensive words in names and text from other players, maps and mods\n"
		"as stars, at the levels below (Settings > Network Setup > Streamer Mode)." },
	{ "streamer.slurs", _config_string, "\"mild\"", "HALO_STREAMER_SLURS", _environment_value, _platform_all,
		"With streamer.enabled, the slurs (race, sexuality, gender, disability)\n"
		"hidden: \"off\", \"severe\", \"strong\" (and severe) or \"mild\" (all)." },
	{ "streamer.swearing", _config_string, "\"strong\"", "HALO_STREAMER_SWEARING", _environment_value, _platform_all,
		"With streamer.enabled, the swearing and sexual words hidden: \"off\",\n"
		"\"severe\", \"strong\" (and severe) or \"mild\" (all)." },
	{ "streamer.insults", _config_string, "\"off\"", "HALO_STREAMER_INSULTS", _environment_value, _platform_all,
		"With streamer.enabled, the insults that are not slurs hidden: \"off\",\n"
		"\"severe\", \"strong\" (and severe) or \"mild\" (all)." },


	{ "debug.network_test", _config_string, "\"\"", "HALO_NETWORK_TEST", _environment_value, _platform_all,
		"Automated system link sessions for testing (port/linux/game/network_test.c):\n"
		"\"host:<map>\" hosts a game on that map, \"local:<map>\" plays a local (split\n"
		"screen) game on it, \"join\" joins the first game found,\n"
		"\"browse\" the one with the most players the server browser lists (not\n"
		"co-op, if it can), \"log\" logs the game that runs (and runs its cues);\n"
		"empty for none." },
	{ "debug.network_test_start", _config_real, "15.0", "HALO_NETWORK_TEST_START", _environment_value, _platform_all,
		"Seconds after hosting that an automated test game starts." },
	{ "debug.network_test_kill", _config_real, "0.0", "HALO_NETWORK_TEST_KILL", _environment_value, _platform_all,
		"Every this many seconds an automated test host kills its last player; 0 never." },
	{ "debug.network_test_score", _config_integer, "0", "HALO_NETWORK_TEST_SCORE", _environment_value, _platform_all,
		"The score an automated test host's game type plays to (a short game, to\n"
		"test the next); 0 the game type's own." },
	{ "debug.network_test_shoot", _config_real, "0.0", "HALO_NETWORK_TEST_SHOOT", _environment_value, _platform_all,
		"Every this many seconds each automated test player hits the next with\n"
		"their weapon, within its reach (the host brings far players near the\n"
		"first a second before); 0 never." },
	{ "debug.network_test_vehicle", _config_real, "0.0", "HALO_NETWORK_TEST_VEHICLE", _environment_value, _platform_all,
		"This many seconds into an automated test game the host seats its last\n"
		"player as a vehicle's driver (and out 15 seconds on); 0 never." },
	{ "debug.network_test_pickup", _config_real, "0.0", "HALO_NETWORK_TEST_PICKUP", _environment_value, _platform_all,
		"This many seconds into an automated test game the host stands its last\n"
		"player on a weapon, which a joining player then picks up; 0 never." },
	{ "debug.network_test_chat", _config_real, "0.0", "HALO_NETWORK_TEST_CHAT", _environment_value, _platform_all,
		"This many seconds into an automated test game each machine's player\n"
		"says a chat line to everyone, then to the team and to the vehicle (the\n"
		"host seats the last two players in one first), then the host mutes and\n"
		"unmutes a player, refuses a link and turns the chat off, and the\n"
		"clients type on the on-screen keyboard (port/linux/NETCODE.md); 0 never." },
	{ "debug.network_test_name", _config_string, "\"\"", "HALO_NETWORK_TEST_NAME", _environment_value, _platform_all,
		"The name this machine's player takes in an automated test game (ASCII);\n"
		"empty keeps its profile's." },
	{ "debug.network_test_pickup_weapon", _config_string, "\"\"", "HALO_NETWORK_TEST_PICKUP_WEAPON", _environment_value,
		_platform_all,
		"The weapon network_test_pickup stands the player on, and the second weapon\n"
		"the host gives its last player: the first whose tag name has this in it\n"
		"(\"sniper\", \"assault\", say); empty any." },
	{ "debug.network_test_cues", _config_string, "\"\"", "HALO_NETWORK_TEST_CUES", _environment_value, _platform_all,
		"Console lines an automated test runs at a game second, from the first tick\n"
		"every player of debug.network_test_cue_players has a unit:\n"
		"\"<second> <line>|<second> <line>...\" (the host's, or a game of one machine;\n"
		"not on the web)." },
	{ "debug.network_test_cue_players", _config_integer, "1", "HALO_NETWORK_TEST_CUE_PLAYERS", _environment_value,
		_platform_all,
		"The players debug.network_test_cues waits for." },
	{ "debug.network_test_public", _config_boolean, "false", "HALO_NETWORK_TEST_PUBLIC", _environment_value,
		_platform_all,
		"The network test's host lists its game in the server browser, as Create\n"
		"Game > Internet's PUBLIC does (debug.network_test \"browse\" joins it)." },
	{ "debug.telnet_console", _config_boolean, "false", "HALO_TELNET_CONSOLE", _environment_set_is_true, _platform_all,
		"Listen on 127.0.0.1 (port telnet_console_port) for a script console that\n"
		"runs what it is sent as the game's console does, with no password; false\n"
		"none." },
	{ "debug.telnet_console_port", _config_integer, "2323", "HALO_TELNET_CONSOLE_PORT", _environment_value,
		_platform_all,
		"The port of the script console (telnet_console); the Xbox's was 23, which\n"
		"only the administrator can listen on." },
	{ "debug.touch_targets", _config_boolean, "false", "HALO_TOUCH_TARGETS", _environment_set_is_true, _platform_all,
		"Outline the menus' tap targets (item green, value blue, list slot yellow,\n"
		"legend button red, the band beside a list's slots orange; the virtual\n"
		"keyboard's keys white), mark where the last finger went down and the\n"
		"last tap landed for 3 seconds, and log each tap with the target it hit\n"
		"(and a value's split); to judge touch accuracy." },
	{ "debug.log_addresses", _config_boolean, "false", "HALO_LOG_ADDRESSES", _environment_set_is_true, _platform_all,
		"Write other machines' internet addresses whole in debug.txt and the\n"
		"console, for debugging your own network. False writes each public\n"
		"address as a tag (addr#3f2a9c) that only matches up within one run;\n"
		"local network addresses are always whole. Do not post a debug.txt\n"
		"written with this on: it has the address of everyone you played with." },
	{ "debug.network_latency", _config_real, "0.0", "HALO_NETWORK_LATENCY", _environment_value, _platform_all,
		"Milliseconds everything received is held back (a round trip between two\n"
		"machines of twice it), to test the netcode as over the internet; 0 none." },
	{ "debug.network_loss", _config_real, "0.0", "HALO_NETWORK_LOSS", _environment_value, _platform_all,
		"Percent of datagrams received that are dropped, for the same; 0 none." },
	{ "debug.network_corrupt", _config_real, "0.0", "HALO_NETWORK_CORRUPT", _environment_value, _platform_all,
		"Percent of the datagrams received that are damaged at random, to test\n"
		"that nothing a machine sends can crash the game; 0 none." },
	{ "debug.network_corrupt_stream", _config_real, "0.0", "HALO_NETWORK_CORRUPT_STREAM", _environment_value,
		_platform_all,
		"Percent of the reads of streams that are damaged at random, for the\n"
		"same (a damaged stream is closed, so a little goes a long way); 0 none." },
	{ "debug.network_corrupt_after", _config_real, "0.0", "HALO_NETWORK_CORRUPT_AFTER", _environment_value,
		_platform_all,
		"Seconds after the start before anything is damaged, so that a game can\n"
		"be set up and started first (a host's messages to its own client are\n"
		"damaged too)." },
	{ "debug.test_input", _config_string, "\"\"", "HALO_TEST_INPUT", _environment_value, _platform_all,
		"\"bot:<seed>\" plays controller 1 with a scripted pattern (automated\n"
		"network tests); \"look:<seed>\" stands still, only turning and looking\n"
		"up and down; \"menu:<buttons>\" presses menu buttons one a second (a,\n"
		"b, x, y, up, down, left, right, start, back or wait, separated by\n"
		"spaces or commas), for testing the menus; empty for none." },
	{ "debug.bot_trace", _config_integer, "0", "HALO_BOT_TRACE", _environment_value, _platform_all,
		"Log each bot's state (bots.count) every this many seconds: where it is\n"
		"and goes, its path, target and weapon; 0 never." },
	{ "debug.update_answer", _config_string, "\"\"", "HALO_UPDATE_ANSWER", _environment_value, _platform_desktop,
		"The answer to the new version question, for automated tests: \"yes\",\n"
		"\"no\" or \"never\" (do not ask again, confirmed); empty asks." },
	{ "debug.fixed_step", _config_boolean, "false", "HALO_FIXED_STEP", _environment_set_is_true, _platform_all,
		"A game of this machine alone runs one tick a frame, whatever time has\n"
		"passed, so that it plays the same every time (tests); game_speed has no\n"
		"effect." },
	{ "debug.exit_after", _config_real, "0.0", "HALO_EXIT_AFTER", _environment_value, _platform_all,
		"Quit this many seconds after the window opens (without a window, after\n"
		"the first frame); 0 never." },
	{ "debug.hidden_window", _config_boolean, "false", "HALO_HIDDEN_WINDOW", _environment_set_is_true, _platform_desktop,
		"Keep the window hidden (and never fullscreen)." },
	{ "debug.voice_test", _config_boolean, "false", "HALO_VOICE_TEST", _environment_set_is_true, _platform_all,
		"Voice chat's automated tests: a tone instead of the microphone, and\n"
		"each voice heard logged once a second." },
	{ "debug.null_renderer", _config_boolean, "false", "HALO_NULL_RENDERER", _environment_set_is_true, _platform_all,
		"Run without a window, drawing nothing." },
	{ "debug.gl_debug", _config_boolean, "false", "HALO_GL_DEBUG", _environment_set_is_true, _platform_all,
		"Report OpenGL errors in the log." },
	{ "debug.gpu_stats", _config_boolean, "false", "HALO_GPU_STATS", _environment_set_is_true, _platform_all,
		"Log the renderer's draw counts once a second." },
	{ "debug.gpu_trace_frame", _config_integer, "-1", "HALO_GPU_TRACE", _environment_value, _platform_all,
		"Log every draw of this frame; -1 none (VR: -2, vr.dump_frame's)." },
	{ "debug.gpu_trace_constants", _config_boolean, "false", "HALO_GPU_TRACE_CONSTANTS", _environment_set_is_true, _platform_all,
		"With gpu_trace_frame, also the vertex shader constants." },
	{ "debug.gpu_skip_vertex_shaders", _config_string, "\"\"", "HALO_GPU_SKIP_VS", _environment_value, _platform_all,
		"Comma-separated ids of vertex shaders not to draw with." },
	{ "debug.gpu_dump_shaders", _config_string, "\"\"", "HALO_GPU_DUMP_SHADERS", _environment_value, _platform_all,
		"A folder to write the generated GLSL to; empty none." },
	{ "debug.gpu_debug_expression", _config_string, "\"\"", "HALO_GPU_DEBUG_EXPR", _environment_value, _platform_all,
		"A GLSL expression every pixel shader shows instead of its result." },
	{ "debug.gpu_debug_texture0", _config_boolean, "false", "HALO_GPU_DEBUG_T0", _environment_set_is_true, _platform_all,
		"Pixel shaders show their first texture." },
	{ "debug.gpu_debug_flat", _config_boolean, "false", "HALO_GPU_DEBUG_FLAT", _environment_set_is_true, _platform_all,
		"Pixel shaders show their vertex colour." },
	{ "debug.gpu_draw_scissor", _config_boolean, "true", "HALO_GPU_DRAW_SCISSOR", _environment_value, _platform_all,
		"Rasterize an object's shadow and a dynamic light's pass only inside the\n"
		"rectangle of the screen they can change (halo-ce-universal's); false\n"
		"rasterizes their whole triangles, as before, to compare: the pictures are\n"
		"the same." },
	{ "debug.shadows_one_at_a_time", _config_boolean, "false", "HALO_SHADOWS_ONE_AT_A_TIME", _environment_set_is_true, _platform_all,
		"Cast each object's shadow as soon as its map is drawn, as the game did,\n"
		"instead of all of them after the last map (rasterizer_xbox_shadows.c): for\n"
		"comparing the two, whose pictures are the same." },
	{ "debug.screenshot_directory", _config_string, "\"\"", "HALO_SCREENSHOT_DIR", _environment_value, _platform_all,
		"A folder to save frames to (with screenshot_every); empty none." },
	{ "debug.screenshot_every", _config_integer, "0", "HALO_SCREENSHOT_EVERY", _environment_value, _platform_all,
		"Save every this many frames to screenshot_directory; 0 none." },
	{ "debug.texture_dump_directory", _config_string, "\"\"", "HALO_TEXTURE_DUMP", _environment_value, _platform_all,
		"A folder to write every texture to as it is uploaded; empty none." },
	{ "debug.texture_log", _config_boolean, "false", "HALO_TEXTURE_LOG", _environment_set_is_true, _platform_all,
		"Log texture uploads." },
	{ "debug.texture_no_cache", _config_boolean, "false", "HALO_TEXTURE_NO_CACHE", _environment_set_is_true, _platform_all,
		"Upload textures again every time they are used." },
	{ "debug.memory_watch", _config_boolean, "true", NULL, _environment_value, _platform_android,
		"Track guest writes by page protection; false compares page contents\n"
		"once per frame. ARM-translated guests always compare page contents." },
	{ "debug.sample_seconds", _config_real, "0.0", "HALO_SAMPLE", _environment_value,
		_platform_android | _platform_windows,
		"Log where the game is this often, in seconds; 0 never. On Android every\n"
		"game thread (read by the app, port/android/host/host_debug.c), on\n"
		"Windows the main thread (port/windows/src/win32_memory_watch.c)." },

	{ "vr.enabled", _config_boolean, "true", "HALO_VR", _environment_value, _platform_vr,
		"Play in the headset (OpenXR); false shows the game on the flat screen\n"
		"as the phone build does." },
	{ "vr.resolution_scale", _config_real, "0.0", "HALO_VR_RESOLUTION_SCALE", _environment_value, _platform_vr,
		"The game's picture in the headset as a fraction of the runtime's\n"
		"recommended eye resolution (about 1680x1760 on the Quest 3, 1728x1728 on\n"
		"the Steam Frame), 0.5 to 2.0; 0 chooses for the headset: 1.0 on the\n"
		"Quest 3, 3S and Pro and the Steam Frame, 0.85 on the Quest 2, 0.7 on\n"
		"the first Quest. The eye images the compositor gets are this size too,\n"
		"so above 1.0 the extra detail reaches the display; 1.25 is about the\n"
		"Quest 3's own panels (2064x2208) and the Steam Frame's (2160x2160),\n"
		"above that supersampling." },
	{ "vr.fov_mode", _config_string, "\"full\"", "HALO_VR_FOV_MODE", _environment_value, _platform_vr,
		"How much of the view the game draws: \"full\" (all the headset shows)\n"
		"or \"glasses\" (a window of vr.glasses_fov_h by vr.glasses_fov_v\n"
		"degrees ahead of each eye, black round it), which previews the field\n"
		"of view of upcoming VR glasses. The window draws far fewer pixels\n"
		"(about 40% as many on the Quest 3) and fewer objects, which leaves\n"
		"room for a higher frame rate or vr.resolution_scale." },
	{ "vr.glasses_fov_h", _config_real, "70.0", "HALO_VR_GLASSES_FOV_H", _environment_value, _platform_vr,
		"The glasses window's width, in degrees (vr.fov_mode), 20 to 160." },
	{ "vr.glasses_fov_v", _config_real, "66.0", "HALO_VR_GLASSES_FOV_V", _environment_value, _platform_vr,
		"The glasses window's height, in degrees (vr.fov_mode), 20 to 160." },
	{ "vr.screen_distance", _config_real, "2.5", "HALO_VR_SCREEN_DISTANCE", _environment_value, _platform_vr,
		"How far ahead the flat screen (menus, cutscenes) floats, in metres." },
	{ "vr.screen_width", _config_real, "2.4", "HALO_VR_SCREEN_WIDTH", _environment_value, _platform_vr,
		"The flat screen's width, in metres (its height is three quarters)." },
	{ "vr.stereo", _config_boolean, "true", "HALO_VR_STEREO", _environment_value, _platform_vr,
		"Play in 3D around you; false keeps gameplay on the flat screen too." },
	{ "vr.world_scale", _config_real, "0.328084", "HALO_VR_WORLD_SCALE", _environment_value, _platform_vr,
		"Game units per metre of head movement and eye separation: the game's\n"
		"unit is 10 feet (0.328084 per metre). Larger makes the world feel\n"
		"smaller. WORLD SCALE (vr.world_size) divides it." },
	{ "vr.world_size", _config_real, "1.0", "HALO_VR_WORLD_SIZE", _environment_value, _platform_vr,
		"Test73b: WORLD SCALE (VR Settings > COMFORT): how big the world is around you, 1.0 its\n"
		"true size; 1.2 makes it a fifth bigger (you a fifth smaller), 0.8 smaller (0.5 to 2.0).\n"
		"Your eyes' separation and your head's and hands' movement take it, at once." },
	{ "vr.hud_distance", _config_real, "15.0", "HALO_VR_HUD_DISTANCE", _environment_value, _platform_vr,
		"How far ahead of the eyes the HUD floats, in metres: far, so the eyes\n"
		"need not refocus between it and the world." },
	{ "vr.menu_anchor", _config_string, "\"head\"", "HALO_VR_MENU_ANCHOR", _environment_value, _platform_vr,
		"Test72: MENU POSITION: \"head\" (the menus ride ahead of the head, as before); \"world\" (placed in\n"
		"front of you as a menu opens and fixed there: lean in to read small text, look around it);\n"
		"\"follow\" (fixed, brought back in front when you have looked well away for a moment). A stick\n"
		"click, or holding VIEW to recentre, brings it back in front." },
	{ "vr.menu_distance", _config_real, "1.6", "HALO_VR_MENU_DISTANCE", _environment_value, _platform_vr,
		"Test72: MENU DISTANCE: how far ahead a fixed menu is placed, in metres (0.5 to 5)." },
	{ "vr.menu_scale", _config_real, "1.0", "HALO_VR_MENU_SCALE", _environment_value, _platform_vr,
		"Test72: MENU SIZE: a fixed menu's size, 0.5 to 2 (1: as large in view as the head-locked menu)." },
	{ "vr.menu_height", _config_real, "0.0", "HALO_VR_MENU_HEIGHT", _environment_value, _platform_vr,
		"Test72: MENU HEIGHT: a fixed menu's height above (or below, negative) the eyes, in metres (-1 to 1)." },
	{ "vr.hud_width", _config_real, "10.0", "HALO_VR_HUD_WIDTH", _environment_value, _platform_vr,
		"The HUD's width, in metres (its height is three quarters); with\n"
		"vr.hud_distance, how much of the view it spans (10 at 15: 37 degrees)." },
	{ "vr.refresh_rate", _config_real, "72.0", "HALO_VR_REFRESH_RATE", _environment_value, _platform_vr,
		"The headset's display rate to ask for, in hertz: the highest the\n"
		"runtime offers at or below it; 0 leaves the runtime's choice. The Quest 3\n"
		"offers 72, 80, 90 and 120; SteamVR on the Steam Frame offers only the\n"
		"rate its own display setting is on." },
	{ "vr.eye_order", _config_string, "\"left\"", "HALO_VR_EYE_ORDER", _environment_value, _platform_vr,
		"Which eye is drawn first each frame: \"left\" (as always) or \"right\".\n"
		"The first eye's pass also moves the frame's effects on (the gun posed,\n"
		"rain, glows), so both eyes still show the same moment. A test for a\n"
		"picture spoilt in one eye only: if it moves to the other eye with this,\n"
		"it belongs to the first pass, not to that eye." },
	{ "vr.cutscenes", _config_string, "\"immersive\"", "HALO_VR_CUTSCENES", _environment_value, _platform_vr,
		"\"immersive\": cutscenes around you, seen from their camera as you look\n"
		"about (a moment of black at each cut); \"screen\": on a large screen\n"
		"ahead (in 3D, vr.cinema_3d); \"flat\": on the flat screen." },
	{ "vr.script_messages", _config_boolean, "false", "HALO_VR_SCRIPT_MESSAGES", _environment_value, _platform_vr,
		"Show script print messages in the green terminal overlay while in VR.\n"
		"False keeps them in the run log only; the flat build is unchanged." },
	{ "vr.cinema_3d", _config_boolean, "true", "HALO_VR_CINEMA_3D", _environment_value, _platform_vr,
		"With vr.cutscenes \"screen\": the cutscene screen in 3D (each eye its\n"
		"own picture); false shows them flat." },
	{ "vr.cinema_separation", _config_real, "0.064", "HALO_VR_CINEMA_SEPARATION", _environment_value, _platform_vr,
		"The cutscene screen's eye separation, in metres of the game's world:\n"
		"more for deeper 3D." },
	{ "vr.cinema_convergence", _config_real, "2.0", "HALO_VR_CINEMA_CONVERGENCE", _environment_value, _platform_vr,
		"How far from the cutscene's camera, in game units (10 feet), things\n"
		"show at the screen's depth; nearer comes out of it." },
	{ "vr.cinema_distance", _config_real, "3.0", "HALO_VR_CINEMA_DISTANCE", _environment_value, _platform_vr,
		"How far ahead the cutscene screen floats, in metres." },
	{ "vr.cinema_width", _config_real, "3.6", "HALO_VR_CINEMA_WIDTH", _environment_value, _platform_vr,
		"The cutscene screen's width, in metres." },
	{ "vr.controls", _config_string, "\"vr\"", "HALO_VR_CONTROLS", _environment_value, _platform_vr,
		"The controllers' layout. \"vr\": right trigger fire, left trigger zoom, A\n"
		"jump, B reload and action, X switch grenades, Y switch weapons, right\n"
		"bumper grenade, left bumper flashlight, right stick click melee, left\n"
		"stick click crouch, menu pause, view back (held a second, recentre).\n"
		"Quest Touch controllers take their buttons from the vr.button_* settings\n"
		"(VR Settings > BUTTONS). \"pad\": as an Xbox controller." },
	{ "vr.move_relative", _config_string, "\"head\"", "HALO_VR_MOVE_RELATIVE", _environment_value, _platform_vr,
		"What the move stick moves you relative to: \"head\", or where the\n"
		"\"left\" or \"right\" controller points." },
	{ "vr.omni_treadmill", _config_boolean, "false", "HALO_VR_OMNI_TREADMILL", _environment_value, _platform_vr,
		"Virtuix Omni One treadmill: its walking (Omni Connect presents it as the left\n"
		"controller's move stick) is shaped by vr.omni_deadzone and vr.omni_speed, and walking\n"
		"about the room (vr.roomscale) and the arms' run are left off. On foot only: vehicles,\n"
		"seats and cutscenes are never changed by it." },
	{ "vr.omni_yaw", _config_string, "\"standard\"", "HALO_VR_OMNI_YAW", _environment_value, _platform_vr,
		"Omni One treadmill: which way the head's yaw is given to Omni Connect, \"standard\" or \"reversed\" (try\n"
		"it if the treadmill's forward turns the wrong way when you turn your head)." },
	{ "vr.omni_deadzone", _config_real, "0.1", "HALO_VR_OMNI_DEADZONE", _environment_value, _platform_vr,
		"Omni One treadmill: the move stick's dead zone, 0 to 0.5 (steps below it are ignored)." },
	{ "vr.omni_speed", _config_real, "1.0", "HALO_VR_OMNI_SPEED", _environment_value, _platform_vr,
		"Omni One treadmill: on-foot movement speed scale, 0.25 to 2 (1 as the stick gives it)." },
	{ "vr.third_person", _config_boolean, "false", "HALO_VR_THIRD_PERSON", _environment_value, _platform_vr,
		"Test72: THIRD PERSON: on foot in a campaign or co-op (not multiplayer), the view from behind you\n"
		"over your shoulder, turning with the stick; the head aims at what it looks at, the buttons do the\n"
		"hands' physical work (those features are held off meanwhile and come back as they were). Not\n"
		"in vehicles, which have VIEW." },
	{ "vr.aim", _config_string, "\"hand\"", "HALO_VR_AIM", _environment_value, _platform_vr,
		"What aims on foot: \"head\" (where you look, with a reticle ahead) or\n"
		"\"hand\" (the right controller, holding the weapon). In vehicles the\n"
		"head aims either way." },
	{ "vr.crosshair", _config_string, "\"native\"", "HALO_VR_CROSSHAIR", _environment_value, _platform_vr,
		"Gameplay crosshair: \"native\" uses the weapon's own animated artwork,\n"
		"or \"off\" hides it. The menu pointer is independent." },
	{ "vr.crosshair_size", _config_real, "1.0", "HALO_VR_CROSSHAIR_SIZE", _environment_value, _platform_vr,
		"Native crosshair size multiplier (0.25 to 3)." },
    { "vr.aim_steadying", _config_real, "0", "HALO_VR_AIM_STEADYING", _environment_value, _platform_vr,
        "Extra shared gun/shot/scope aim steadying, 0..0.8. Zero preserves stock scope smoothing." },
    { "vr.assisted_combat", _config_boolean, "true", "HALO_VR_ASSISTED_COMBAT", _environment_value, _platform_vr,
        "Optional enemy-only gaze/punch combo assist, in a local game or co-op whose host's VR INTERACTIONS is on (Test73: every co-op player's; its host deals each blow). Native collision and damage remain in use." },
    { "vr.combat_moves", _config_boolean, "true", "HALO_VR_COMBAT_MOVES", _environment_value, _platform_vr,
        "COMBAT MOVES: optional enemy-specific attack poses, physical duck/sidestep dodges, arm-control and jump-pummel openings within FIST COMBAT. Enemy-only; real contacts and native collision remain required." },
    { "vr.projectile_parry", _config_boolean, "true", "HALO_VR_PROJECTILE_PARRY", _environment_value, _platform_vr,
        "DEFLECT SHOTS: projectiles met by your gun or your fist turn away. A gun held in a shot's path blocks it\n"
        "(it glances off, no swing needed); a gun or a closed hand (the grip) swung at 1.2 m/s bats it along\n"
        "the swing. By kind: plasma, needles and vehicles' bolts by any of them; bullets only off a gun;\n"
        "grenades batted or bounced; rockets and fuel rods only by a hard swing (2.5 m/s); tank shells,\n"
        "mortars and flame never. A deflected shot is yours (its damage and kills). Local solo or an enabled\n"
        "co-op host only; nothing on a client. Test72: on by default." },
    { "vr.deflect_assist", _config_real, "0.03", "HALO_VR_DEFLECT_ASSIST", _environment_value, _platform_vr,
        "DEFLECT AID: metres added round the gun and the fist where a shot is met: 0 (as they are),\n"
        "0.03 (default) or 0.06." },
    { "vr.deflect_strength", _config_real, "1.0", "HALO_VR_DEFLECT_STRENGTH", _environment_value, _platform_vr,
        "DEFLECT FORCE: the deflected shot's speed against what it had (a block keeps a little less):\n"
        "0.75, 1.0 (default) or 1.25." },
    { "vr.combat_highlights", _config_boolean, "true", "HALO_VR_COMBAT_HIGHLIGHTS", _environment_value, _platform_vr,
        "Highlight a locked enemy head/chest and show stun/impact feedback with assisted combat." },
    { "vr.combat_combo_display", _config_boolean, "true", "HALO_VR_COMBAT_COMBO_DISPLAY", _environment_value, _platform_vr,
        "Test72: COMBO DISPLAY (FIST COMBAT): the combo (blows, switches to another enemy, parries, blocks, grabs,\n"
        "knockdowns in a row) as a small holographic counter in the HUD's blue, low and left of the view, gold\n"
        "when a takedown is ready. Off: only the lights over the enemy (HIT HIGHLIGHTS)." },
    { "vr.weapon_pull", _config_boolean, "true", "HALO_VR_WEAPON_PULL", _environment_value, _platform_vr,
        "WEAPON PULL (Half-Life 2 VR's and Alyx's gravity gloves): point a free hand at a weapon lying within PULL REACH, squeeze to lock on, flick the hand back (or keep squeezing, PULL STYLE GRIP) and it flies to the hand; a hand closed as it arrives catches it. Local play or the co-op host." },
    { "vr.weapon_pull_reach", _config_real, "3.0", "HALO_VR_WEAPON_PULL_REACH", _environment_value, _platform_vr,
        "PULL REACH: how far from the hand a weapon can be pulled, in metres (1 to 5)." },
    { "vr.weapon_pull_style", _config_string, "\"flick\"", "HALO_VR_WEAPON_PULL_STYLE", _environment_value, _platform_vr,
        "PULL STYLE: \"flick\" (squeeze on the weapon, then snap the hand back, as Alyx's gloves) or \"grip\" (squeeze on it for about half a second)." },
    { "vr.weapon_pull_glow", _config_boolean, "true", "HALO_VR_WEAPON_PULL_GLOW", _environment_value, _platform_vr,
        "PULL GLOW: the weapon a hand can pull is outlined (pulsing; brighter while it flies to the hand)." },
    { "vr.combat_lunge_reach", _config_real, "12.0", "HALO_VR_COMBAT_LUNGE_REACH", _environment_value, _platform_vr,
        "LUNGE (FIST COMBAT): how far a punch carries you to the enemy you look at or throw it at, in metres, 0 to 15 (12 since Test72; Arkham Shadow's punch across the room: a leaping dash up to 14 m/s, the blow landing on arrival); 0 keeps FIST REACH's short step only." },
    { "vr.combat_enemy_health", _config_real, "3.0", "HALO_VR_COMBAT_ENEMY_HEALTH", _environment_value, _platform_vr,
        "ENEMY HEALTH (FIST COMBAT): enemies take this many times more punches, so fights flow as combos (each hit in a row adds 8%, up to +80%); takedowns, guns and grenades are unchanged." },
    { "vr.combat_counters", _config_boolean, "true", "HALO_VR_COMBAT_COUNTERS", _environment_value, _platform_vr,
        "COUNTERS (FIST COMBAT): an enemy swinging at you shows a blue warning over its head and taps both hands; a punch toward it counters (its blow undone, it is knocked down, the combo goes on)." },
    { "vr.combat_hit_assist", _config_real, "0.50", "HALO_VR_COMBAT_HIT_ASSIST", _environment_value, _platform_vr,
        "Test70: HIT ASSIST (FIST COMBAT): a punch thrown at an enemy already in striking range lands when\n"
        "the fist's stroke passes within this many metres of its skeleton (head, chest, arms, legs; 0 to 0.7;\n"
        "0.50 since Test72), Halo's body bubble keeping the hand off the skin. 0: the fist's own sweep must\n"
        "meet the body, as before." },
    { "vr.combat_lunge_hop", _config_real, "0.30", "HALO_VR_COMBAT_LUNGE_HOP", _environment_value, _platform_vr,
        "Test70: LUNGE HOP (FIST COMBAT): a lunge on the ground begins with a leap of about this height in\n"
        "metres (0 to 0.45; 0.30 since Test72; lower for a short lunge, higher to an enemy standing above),\n"
        "a Spartan's combat leap rather than a slide; 0 slides as before." },
    { "vr.combat_reach", _config_real, "3.0", "HALO_VR_COMBAT_REACH", _environment_value, _platform_vr,
        "How far from the eyes a punch picks an enemy (metres, 0.75..3); short of striking range a short step closes the gap." },
    { "vr.physical_grenades", _config_boolean, "true", "HALO_VR_PHYSICAL_GRENADES", _environment_value, _platform_vr,
        "Grab a waist grenade with a free hand and release Grip to throw. A local game, or (Test73) co-op whose host's VR INTERACTIONS allows it (the host throws a co-op player's); buttons remain available." },
    { "vr.grenade_chest_forward", _config_real, "0.12", "HALO_VR_GRENADE_CHEST_FORWARD", _environment_value, _platform_vr,
        "GRENADE FWD: the waist grenades in front of the body (from the neck), metres 0.08..0.4 (12 cm)." },
    { "vr.grenade_chest_down", _config_real, "0.60", "HALO_VR_GRENADE_CHEST_DOWN", _environment_value, _platform_vr,
        "GRENADE DOWN: the waist grenades below the neck's height with the head level, metres 0.15..0.8 (60 cm: the waist)." },
    { "vr.grenade_chest_spread", _config_real, "0.18", "HALO_VR_GRENADE_CHEST_SPREAD", _environment_value, _platform_vr,
        "GRENADE GAP: each waist grenade to the side of the middle, metres 0.06..0.3 (18 cm). Frag left, plasma right." },
    { "vr.grenade_throw_scale", _config_real, "1.5", "HALO_VR_GRENADE_THROW_SCALE", _environment_value, _platform_vr,
        "Physical grenade hand momentum multiplier, 0.5..2.0; final speed capped at 15 m/s." },
    { "vr.grenade_scale", _config_real, "1.0", "HALO_VR_GRENADE_SCALE", _environment_value, _platform_vr,
        "Test73, GRENADE SIZE: the waist grenades and the one held, as a share of the game's own grenade (1: as it is thrown; 0.5..1.5)." },
    { "vr.diag_grenade_hand", _config_boolean, "false", "HALO_VR_DIAG_GRENADE_HAND", _environment_value, _platform_vr,
        "Test73, for testing: draw a frag grenade held in the free hand (vr.dump_frame's images show it), with or without PHYS GRENADES." },
    { "vr.diag_weapon", _config_string, "\"\"", "HALO_VR_DIAG_WEAPON", _environment_value, _platform_vr,
        "Test73, for testing: a weapon tag's name (weapons\\\\sniper rifle\\\\sniper rifle): put in the hand two seconds into a\n"
        "local game, for eye dumps of each zoom weapon's scope. Empty: none." },
    { "net.host_vr_actions", _config_boolean, "false", "HALO_HOST_VR_ACTIONS", _environment_value, _platform_all,
        "Before Test73, the co-op host's fist assist and physical grenades on their own switch; now part of VR INTERACTIONS\n"
        "(net.physical_interactions). Kept for older configs; no longer read." },
	{ "vr.crosshair_opacity", _config_real, "1.0", "HALO_VR_CROSSHAIR_OPACITY", _environment_value, _platform_vr,
		"Native crosshair opacity (0 to 1). Does not change the menu pointer." },
    { "vr.vehicle_defaults_applied", _config_boolean, "false", "HALO_VR_VEHICLE_DEFAULTS_APPLIED", _environment_value, _platform_vr,
        "Internal one-time third-person/right-controller default migration. Later saved choices are preserved." },
	{ "vr.vehicle_view", _config_string, "\"chase\"", "HALO_VR_VEHICLE_VIEW", _environment_value, _platform_vr,
		"Vehicles seen from the seat, turning with the vehicle and the horizon\n"
		"kept level (\"first_person\"), or from the game's chase camera (\"chase\")." },
	{ "vr.vehicle_tilt", _config_real, "0.0", "HALO_VR_VEHICLE_TILT", _environment_value, _platform_vr,
		"First-person vehicles, a driver's seat: how much the view tilts with the vehicle (0, the\n"
		"default: the horizon stays level and the cockpit tilts; 1: the cockpit stays put; 0.5 half)." },
	{ "vr.vehicle_steering", _config_string, "\"right\"", "HALO_VR_VEHICLE_STEERING", _environment_value, _platform_vr,
        "Driver steering: \"right\" (default), \"left\", \"head\", or \"stick\".\n"
        "Right/left use that physical controller, independent of weapon grip.\n"
#ifdef HALO_VR
        "Legacy \"hand\" means right. Gunners use vr.turret_aim. Left stick drives." },
#else
        "Legacy \"hand\" means right. Gunners retain head aim. Left stick drives." },
#endif
#ifdef HALO_VR
	{ "vr.turret_aim", _config_string, "\"right\"", "HALO_VR_TURRET_AIM", _environment_value, _platform_vr,
		"Mounted turret/gunner aim: \"right\" (default), \"left\", \"head\", or \"stick\".\n"
		"Controller choices use that physical hand independently of weapon grip.\n"
		"The selected controller losing tracking keeps native facing; head motion does not take over." },
#endif

	{ "vr.vehicle_all_up", _config_real, "0.0", "HALO_VR_VEHICLE_ALL_UP", _environment_value, _platform_vr,
		"First-person seat up offset in metres (-0.50 to 0.50), all. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_all_forward", _config_real, "0.0", "HALO_VR_VEHICLE_ALL_FORWARD", _environment_value, _platform_vr,
		"First-person seat forward offset in metres (-0.50 to 0.50), all. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_all_right", _config_real, "0.0", "HALO_VR_VEHICLE_ALL_RIGHT", _environment_value, _platform_vr,
		"First-person seat right offset in metres (-0.50 to 0.50), all. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_warthog_hide_glass", _config_boolean, "true", "HALO_VR_VEHICLE_WARTHOG_HIDE_GLASS", _environment_value, _platform_vr,
		"Hide the occupied Warthog's glass in first-person view. Other vehicles, third-person views and collision are unchanged. On preserves test25 behavior." },
	{ "vr.vehicle_warthog_up", _config_real, "0.0", "HALO_VR_VEHICLE_WARTHOG_UP", _environment_value, _platform_vr,
		"First-person seat up offset in metres (-0.50 to 0.50), warthog. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_warthog_forward", _config_real, "0.0", "HALO_VR_VEHICLE_WARTHOG_FORWARD", _environment_value, _platform_vr,
		"First-person seat forward offset in metres (-0.50 to 0.50), warthog. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_warthog_right", _config_real, "0.0", "HALO_VR_VEHICLE_WARTHOG_RIGHT", _environment_value, _platform_vr,
		"First-person seat right offset in metres (-0.50 to 0.50), warthog. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_ghost_up", _config_real, "0.0", "HALO_VR_VEHICLE_GHOST_UP", _environment_value, _platform_vr,
		"First-person seat up offset in metres (-0.50 to 0.50), ghost. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_ghost_forward", _config_real, "0.0", "HALO_VR_VEHICLE_GHOST_FORWARD", _environment_value, _platform_vr,
		"First-person seat forward offset in metres (-0.50 to 0.50), ghost. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_ghost_right", _config_real, "0.0", "HALO_VR_VEHICLE_GHOST_RIGHT", _environment_value, _platform_vr,
		"First-person seat right offset in metres (-0.50 to 0.50), ghost. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_banshee_up", _config_real, "0.0", "HALO_VR_VEHICLE_BANSHEE_UP", _environment_value, _platform_vr,
		"First-person seat up offset in metres (-0.50 to 0.50), banshee. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_banshee_forward", _config_real, "0.0", "HALO_VR_VEHICLE_BANSHEE_FORWARD", _environment_value, _platform_vr,
		"First-person seat forward offset in metres (-0.50 to 0.50), banshee. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_banshee_right", _config_real, "0.0", "HALO_VR_VEHICLE_BANSHEE_RIGHT", _environment_value, _platform_vr,
		"First-person seat right offset in metres (-0.50 to 0.50), banshee. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_scorpion_up", _config_real, "0.0", "HALO_VR_VEHICLE_SCORPION_UP", _environment_value, _platform_vr,
		"First-person seat up offset in metres (-0.50 to 0.50), scorpion. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_scorpion_forward", _config_real, "0.0", "HALO_VR_VEHICLE_SCORPION_FORWARD", _environment_value, _platform_vr,
		"First-person seat forward offset in metres (-0.50 to 0.50), scorpion. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_scorpion_right", _config_real, "0.0", "HALO_VR_VEHICLE_SCORPION_RIGHT", _environment_value, _platform_vr,
		"First-person seat right offset in metres (-0.50 to 0.50), scorpion. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_turret_up", _config_real, "0.25", "HALO_VR_VEHICLE_TURRET_UP", _environment_value, _platform_vr,
		"Test72: a turret's view (the Shade's and any of its kind), metres up from its seat: 0.25 by\n"
		"default, to see over its shield (a tester: too low). -0.5 to 0.5, with vr.vehicle_all_up." },
	{ "vr.vehicle_turret_forward", _config_real, "0.0", "HALO_VR_VEHICLE_TURRET_FORWARD", _environment_value, _platform_vr,
		"Test72: a turret's view, metres forward from its seat (-0.5 to 0.5)." },
	{ "vr.vehicle_turret_right", _config_real, "0.0", "HALO_VR_VEHICLE_TURRET_RIGHT", _environment_value, _platform_vr,
		"Test72: a turret's view, metres right of its seat (-0.5 to 0.5)." },
	{ "vr.vehicle_pelican_up", _config_real, "0.0", "HALO_VR_VEHICLE_PELICAN_UP", _environment_value, _platform_vr,
		"First-person seat up offset in metres (-0.50 to 0.50), pelican. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_pelican_forward", _config_real, "0.0", "HALO_VR_VEHICLE_PELICAN_FORWARD", _environment_value, _platform_vr,
		"First-person seat forward offset in metres (-0.50 to 0.50), pelican. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },
	{ "vr.vehicle_pelican_right", _config_real, "0.0", "HALO_VR_VEHICLE_PELICAN_RIGHT", _environment_value, _platform_vr,
		"First-person seat right offset in metres (-0.50 to 0.50), pelican. Global and matching vehicle offsets add; final axis is bounded to 0.50 m." },

	{ "vr.arms", _config_string, "\"ik\"", "HALO_VR_ARMS", _environment_value, _platform_vr,
		"With vr.aim \"hand\": the first-person arms. \"ik\" reaches them from the\n"
		"shoulders to the hands (the left to the left controller, or to the gun\n"
		"when held near it); \"hidden\" shows the gun alone; \"animated\" moves\n"
		"them with the gun as the game animates them." },
	{ "vr.two_handed", _config_string, "\"auto\"", "HALO_VR_TWO_HANDED", _environment_value, _platform_vr,
		"With vr.aim \"hand\": holding the gun in both hands steadies it, pointing\n"
		"from the weapon hand to the other. \"auto\" (default): the other hand resting at the\n"
		"gun's support grip locks there, until pulled away; \"grip\": it locks only while its\n"
		"grip is squeezed there; \"off\": no attachment." },
	{ "vr.two_hand_auto_applied", _config_boolean, "false", "HALO_VR_TWO_HAND_AUTO_APPLIED", _environment_value, _platform_vr,
		"Internal one-time migration (test21): a config on the old \"grip\" default moves to\n"
		"\"auto\" (two-hand grip locks automatically). A later choice is kept." },
	{ "vr.aim_reset_applied", _config_boolean, "false", "HALO_VR_AIM_RESET_APPLIED", _environment_value, _platform_vr,
		"Internal one-time reset (test21): per-gun aim values (vr.aim_*) set while testing the first\n"
		"test21 build, whose reticle was off, return to 0 once." },
	{ "vr.left_handed", _config_boolean, "false", "HALO_VR_LEFT_HANDED", _environment_value, _platform_vr,
		"Left-handed play: the gun starts in the left hand (fire, grenade and zoom swap triggers\n"
		"and bumpers) and, with vr.mirror_controls \"auto\", the sticks and face buttons mirror.\n"
		"Changing it in the menu also mirrors vehicle steering and Move With when they used a\n"
		"hand. Palms together and the other hand's grip still pass the gun across." },
	{ "vr.mirror_controls", _config_string, "\"auto\"", "HALO_VR_MIRROR_CONTROLS", _environment_value, _platform_vr,
		"\"auto\": with vr.left_handed, move on the right stick and turn on the left, and the\n"
		"face buttons swap hands (jump and reload on the left controller). \"off\": the standard\n"
		"layout (move on the left stick) whichever hand holds the gun." },
	{ "vr.mirror_buttons", _config_string, "\"auto\"", "HALO_VR_MIRROR_BUTTONS", _environment_value, _platform_vr,
		"Test70: \"auto\": with vr.left_handed, the face buttons are on the gun hand (A/B the\n"
		"left controller's lower/upper: jump, use/reload and the magazine release) even when\n"
		"vr.mirror_controls is \"off\" and the sticks stay standard. \"off\": the face buttons\n"
		"stay where vr.mirror_controls puts them. Bindings (vr.button_*) never change with it." },
	{ "vr.handedness_applied", _config_boolean, "false", "HALO_VR_HANDEDNESS_APPLIED", _environment_value, _platform_vr,
		"Internal one-time migration (test20d): a config already left-handed keeps standard\n"
		"sticks and buttons (vr.mirror_controls \"off\")." },
	{ "vr.melee", _config_string, "\"impact\"", "HALO_VR_MELEE", _environment_value, _platform_vr,
		"\"impact\": a hand (the weapon hand with the gun) moving at vr.melee_speed\n"
		"strikes what it sweeps through - enemies, vehicles, glass - harder the\n"
		"faster; \"swing\": a hand swung up or down that fast makes the game's own\n"
		"melee ahead. A network game's client always swings." },
	{ "vr.melee_speed", _config_real, "2.0", "HALO_VR_MELEE_SPEED", _environment_value, _platform_vr,
		"How fast a hand must move to strike (metres a second); 0 turns physical\n"
		"melee off (the right stick click still melees)." },
	{ "vr.melee_multiplayer", _config_boolean, "false", "HALO_VR_MELEE_MULTIPLAYER", _environment_value, _platform_vr,
		"Physical melee (impact or swing) in network games too. Off by default: online, a\n"
		"quick hand movement does not melee; the melee button always does." },
	{ "vr.flashlight_distance", _config_real, "0.2", "HALO_VR_FLASHLIGHT_DISTANCE", _environment_value, _platform_vr,
		"The off hand brought this close (metres) to the middle of the head\n"
		"turns the flashlight on or off; 0 turns the gesture off (0.10 to 0.30 in the menu)." },
	{ "vr.hand_swap_distance", _config_real, "0.1", "HALO_VR_HAND_SWAP_DISTANCE", _environment_value, _platform_vr,
		"HAND SWAP: the off hand's grip squeezed with the two controllers this close (metres) passes the\n"
		"gun to that hand (palms together); 0 turns the gesture off. Smaller keeps a slide rack or a\n"
		"magazine change from swapping hands (0.06 to 0.20 in the menu)." },
	{ "vr.hud_tap_distance", _config_real, "0.1", "HALO_VR_HUD_TAP_DISTANCE", _environment_value, _platform_vr,
		"The weapon hand held for a moment this close (metres) beside its own side of the head (the\n"
		"right temple for a right hand) shows or hides the HUD for the session; menus, prompts,\n"
		"messages and the reticle stay. 0 turns the gesture off. VR SETTINGS > HUD also shows or\n"
		"hides it, and every start shows it." },
	{ "vr.wrist_hud", _config_boolean, "false", "HALO_VR_WRIST_HUD", _environment_value, _platform_vr,
		"Shield and health, ammunition and grenades, and the motion tracker on a panel on the off\n"
		"hand's wrist (shown while you look at it), and left out of the HUD ahead. Off by default." },
	{ "vr.wrist_hud_along", _config_real, "0", "HALO_VR_WRIST_HUD_ALONG", _environment_value, _platform_vr,
		"Moves the wrist HUD along the forearm from its place on the wrist (metres, -0.2 to 0.2;\n"
		"positive toward the elbow)." },
	{ "vr.wrist_hud_across", _config_real, "0", "HALO_VR_WRIST_HUD_ACROSS", _environment_value, _platform_vr,
		"Moves the wrist HUD across the wrist (metres, -0.2 to 0.2; positive toward the thumb)." },
	{ "vr.wrist_hud_out", _config_real, "0", "HALO_VR_WRIST_HUD_OUT", _environment_value, _platform_vr,
		"Raises the wrist HUD off the wrist (metres, -0.2 to 0.2; negative lowers it)." },
	{ "vr.wrist_hud_size", _config_real, "1", "HALO_VR_WRIST_HUD_SIZE", _environment_value, _platform_vr,
		"The wrist HUD's size (0.5 to 2; 1 is 11 cm across)." },
	{ "vr.controls_reticle_applied", _config_boolean, "false", "HALO_VR_CONTROLS_RETICLE_APPLIED", _environment_value, _platform_vr,
		"Internal one-time move of crouch off the left stick's click (now the reticle toggle's)." },
	{ "vr.crouch_height", _config_real, "0.35", "HALO_VR_CROUCH_HEIGHT", _environment_value, _platform_vr,
		"Ducking this far (metres) below your height at the last recentre\n"
		"crouches; 0 turns it off." },
	{ "vr.haptics", _config_real, "1.0", "HALO_VR_HAPTICS", _environment_value, _platform_vr,
		"The strength of all controller haptics (shots, contact, reload, movement and interaction feedback), 0 to 1; 0 disables all haptics." },
	{ "vr.menu_3d", _config_boolean, "true", "HALO_VR_MENU_3D", _environment_value, _platform_vr,
		"The main menu in 3D: its scene around you, its menus on a panel ahead;\n"
		"false shows it all on the flat screen." },
	{ "vr.body", _config_string, "\"legs\"", "HALO_VR_BODY", _environment_value, _platform_vr,
		"\"arms\": first-person arms/hands; \"hands\": hands only; \"full\": torso\n"
		"and legs too; \"legs\": legs plus arms/hands with the torso hidden.\n"
		"Local visibility only; supported peers see the complete VR avatar." },
	{ "vr.character_arms", _config_string, "\"auto\"", "HALO_VR_CHARACTER_ARMS", _environment_value, _platform_vr,
		"Test73: ARMS FROM, with the body shown (vr.body full or legs) and the arms\n"
		"reached by IK: \"on\" (CHARACTER) the played character's own upper arms\n"
		"and forearms, as its model has them, with the first-person gloves' tracked\n"
		"fingers (the characters' own hands have no finger bones); \"off\" (FIRST\n"
		"PERSON) the map's first-person arms; \"auto\" the character's own when it\n"
		"is not the game's Spartan (a custom campaign's, such as SPV1's Chief)." },
	{ "vr.fingers", _config_boolean, "true", "HALO_VR_FINGERS", _environment_value, _platform_vr,
		"A free hand turns with your wrist and its fingers follow yours on the\n"
		"controller (Touch senses the thumb and index finger resting): point,\n"
		"thumbs up, a fist; pointing with the thumb up held a second shows the\n"
		"middle finger. It stops at what it meets - walls, vehicles, people - its\n"
		"palm laid flat to it and each finger resting on it, felt as a buzz." },
	{ "vr.diag_finger_sign", _config_real, "1.0", "HALO_VR_DIAG_FINGER_SIGN", _environment_value, _platform_vr,
		"Legacy test8 setting, retained for old configs; absolute finger posing\n"
		"in test9 determines bend direction from the hand frame instead." },
	{ "vr.arm_run", _config_boolean, "false", "HALO_VR_ARM_RUN", _environment_value, _platform_vr,
		"Run by swinging the arms as in running, ahead where the head faces, as\n"
		"fast as the swing (both arms; holding the gun in both hands, the gun's\n"
		"bob). Offline, strong forward pumping uses SPRINT SPEED (1x to 3x; default 1.5x).\n"
		"Network games retain stock speed. Deliberate stick movement takes priority." },
	{ "vr.arm_run_speed", _config_real, "0.6", "HALO_VR_ARM_RUN_SPEED", _environment_value, _platform_vr,
		"With vr.arm_run: hand-motion threshold in metres per second. This is\n"
		"arm-run sensitivity; sprint speed is configured separately." },
	{ "vr.button_sprint", _config_boolean, "false", "HALO_VR_BUTTON_SPRINT", _environment_value, _platform_vr,
		"Use left stick click for offline sprint. The VR menu temporarily frees\n"
		"that binding and restores it when this option is disabled." },
	{ "vr.button_sprint_toggle", _config_boolean, "true", "HALO_VR_BUTTON_SPRINT_TOGGLE", _environment_value, _platform_vr,
		"When Button Sprint is enabled, click left stick once to start sprinting\n"
		"and click again to stop (the default since Test73b). Off keeps the hold-to-sprint behavior." },
	{ "vr.sprint_speed", _config_real, "1.5", "HALO_VR_SPRINT_SPEED", _environment_value, _platform_vr,
		"Maximum offline sprint movement multiplier, bounded from 1.0x to 3.0x; default 1.5x." },
	{ "vr.slide", _config_boolean, "true", "HALO_VR_SLIDE", _environment_value, _platform_vr,
		"Test70: sprinting on the ground, a crouch begun slides on with the run's momentum, giving it\n"
		"up to friction and slope, steered a little by the stick; standing, jumping, a wall or a walk's\n"
		"pace ends it. A local game, or (Test73) co-op whose host's VR INTERACTIONS allows VR movement\n"
		"(SPRINT SPEED's boost itself stays offline). Turning it on in the VR menu also turns on BUTTON\n"
		"SPRINT unless ARM RUN sprints already." },
	{ "vr.slide_momentum", _config_real, "1", "HALO_VR_SLIDE_MOMENTUM", _environment_value, _platform_vr,
		"Test70: SLIDE DISTANCE, 0.5x to 2x: the lift a slide starts with and how long it may last." },
	{ "vr.slide_friction", _config_real, "1", "HALO_VR_SLIDE_FRICTION", _environment_value, _platform_vr,
		"Test70: SLIDE FRICTION, 0.25x to 3x: how quickly a slide gives up its speed." },
	{ "vr.slide_damage", _config_real, "1", "HALO_VR_SLIDE_DAMAGE", _environment_value, _platform_vr,
		"Test70: SLIDE IMPACT, 0 to 3: sliding into an NPC staggers or knocks it down (by kind, as fist\n"
		"combat's) and deals up to half a melee at 1 (never a kill by itself); 0: no impact." },
	{ "vr.slide_physical_crouch", _config_boolean, "true", "HALO_VR_SLIDE_PHYSICAL_CROUCH", _environment_value, _platform_vr,
		"Test70: CROUCH SLIDES: lowering the head (a physical crouch) while sprinting slides too." },
	{ "vr.slide_jump_cancel", _config_boolean, "true", "HALO_VR_SLIDE_JUMP_CANCEL", _environment_value, _platform_vr,
		"Test70: JUMP CANCELS: Jump ends a slide at once, its momentum carried into the jump." },
	{ "vr.melee_requires_fist", _config_boolean, "false", "HALO_VR_MELEE_REQUIRES_FIST", _environment_value, _platform_vr,
		"Physical melee requires grip and index trigger together; an active hand\n"
		"interaction takes priority over physical melee." },
	{ "vr.button_sprint_saved_mask", _config_integer, "0", NULL, _environment_value, _platform_vr,
		"Internal saved VR button mapping used while Button Sprint is active." },
	{ "vr.close_contact", _config_boolean, "true", "HALO_VR_CLOSE_CONTACT", _environment_value, _platform_vr,
		"Offline local VR player only: reduce capsule radius by up to 15 percent\n"
		"(at most 5 cm, minimum radius 18 cm) for closer hand contact. Solid\n"
		"collision and total height remain; false restores the stock radius." },
	{ "vr.weapons", _config_string, "\"physical\"", "HALO_VR_WEAPONS", _environment_value, _platform_vr,
		"With vr.aim \"hand\": \"locked\" keeps the gun in the weapon hand always;\n"
		"\"physical\" keeps it there while that hand's grip holds it (the trigger\n"
		"fires whatever gun is in the hand): let go, it falls - at a holster it\n"
		"is put away, the hand empty, and with the other hand on it, it passes to\n"
		"that hand. A gun the game puts in the hand (a level's start, a pickup)\n"
		"stays supported until the first grip. An empty hand gripping at a\n"
		"holster draws the weapon there, and by a weapon lying about takes it.\n"
		"Grenades then go on the left's lower button (a tap throws, a hold\n"
		"switches). In network games only with vr.physical_multiplayer." },
	{ "vr.physical_multiplayer", _config_boolean, "false", "HALO_VR_PHYSICAL_MULTIPLAYER", _environment_value,
		_platform_vr,
		"Physical weapons (vr.weapons) in network games too, where the other\n"
		"machines may not see the guns dropped and taken; false keeps network\n"
		"games to the locked behaviour." },
	{ "vr.holsters", _config_boolean, "true", "HALO_VR_HOLSTERS", _environment_value, _platform_vr,
		"Holsters over each shoulder and at each hip (the weapon hand buzzes\n"
		"coming into one): its grip there switches weapons, or with physical\n"
		"weapons (vr.weapons) a gun let go there is put away and an empty hand\n"
		"gripping there draws one." },
	{ "vr.holster_haptics", _config_boolean, "true", "HALO_VR_HOLSTER_HAPTICS", _environment_value, _platform_vr,
		"Vibrate when the weapon hand enters, draws from, or puts away a weapon\n"
		"at a holster. This does not disable magazine reload or other haptics;\n"
		"the global HAPTICS strength still scales every vibration." },
	{ "vr.holster_size", _config_real, "0.2", "HALO_VR_HOLSTER_SIZE", _environment_value, _platform_vr,
		"How far from its place each holster reaches, in metres (0.05 to 0.6)." },
	{ "vr.roomscale", _config_boolean, "true", "HALO_VR_ROOMSCALE", _environment_value, _platform_vr,
		"Walking about your room walks your character (on foot, not in\n"
		"vehicles or cutscenes). Walls stop it where they stop the character, so\n"
		"your room and the game drift apart; recentre (hold View) to line them up." },
	{ "vr.scope", _config_boolean, "true", "HALO_VR_SCOPE", _environment_value, _platform_vr,
		"With vr.aim \"hand\": a zoomed weapon shows its zoom in a scope held at\n"
		"the gun (an extra view rendered while zoomed); your eyes stay unzoomed." },
    { "vr.grab_contact", _config_string, "\"auto\"", "HALO_VR_GRAB_CONTACT", _environment_value, _platform_vr,
        "Offline physical contact radius: auto applies when world interaction is enabled; on always; off keeps existing close-contact behavior." },
    { "vr.grab_radius", _config_real, "0.25", "HALO_VR_GRAB_RADIUS", _environment_value, _platform_vr,
        "Offline player contact radius in metres, clamped 0.18..0.60 and never above the native radius. Occupied space blocks radius expansion." },
    { "vr.parkour_wallrun", _config_boolean, "true", "HALO_VR_PARKOUR_WALLRUN", _environment_value, _platform_vr,
        "Optional wall running: a local game, a co-op host, or (Test73) a co-op player whose host's VR INTERACTIONS allows VR movement. Jump near a wall while moving along it; the wallrun is brief and native collision remains authoritative." },
    { "vr.parkour_walljump", _config_boolean, "true", "HALO_VR_PARKOUR_WALLJUMP", _environment_value, _platform_vr,
        "Optional wall jumping, where wall running is allowed. Press jump again while airborne beside a wall to leap away; native collision remains authoritative." },
    { "vr.parkour_climb", _config_boolean, "true", "HALO_VR_PARKOUR_CLIMB", _environment_value, _platform_vr,
		"Optional grip climbing, where wall running is allowed. Touch a solid vertical world surface with a free tracked hand, then squeeze Grip; pull to climb and release to launch. HAND CONTACT controls the palm collision radius, not reach; native collision remains authoritative." },
    { "vr.wallrun_speed", _config_real, "1.5", "HALO_VR_WALLRUN_SPEED", _environment_value, _platform_vr,
        "Wallrun speed multiplier, 1..2." },
    { "vr.wallrun_duration", _config_real, "2.0", "HALO_VR_WALLRUN_DURATION", _environment_value, _platform_vr,
        "Maximum wallrun duration in seconds, 0.5..3." },
    { "vr.walljump_scale", _config_real, "1.5", "HALO_VR_WALLJUMP_SCALE", _environment_value, _platform_vr,
        "Walljump impulse multiplier, 0.5..1.5." },
    { "vr.climb_launch_scale", _config_real, "1.0", "HALO_VR_CLIMB_LAUNCH_SCALE", _environment_value, _platform_vr,
        "Climb release launch multiplier, 0..1.5." },
    { "vr.contact_volumes", _config_boolean, "true", "HALO_VR_CONTACT_VOLUMES", _environment_value, _platform_vr,
        "Use finger/palm volume contact; disable to use the prior ray-contact fallback." },
    { "vr.contact_debug", _config_boolean, "false", "HALO_VR_CONTACT_DEBUG", _environment_value, _platform_vr,
        "Opt-in detailed contact diagnostics emitted outside render hooks." },
    { "vr.vehicle_handles", _config_boolean, "true", "HALO_VR_VEHICLE_HANDLES", _environment_value, _platform_vr,
        "IMMERSIVE VEHICLE: first-person cockpit controls. Grip the Warthog's own wheel, or the Ghost handlebar, Scorpion sticks or Banshee yoke (push/pull to dive/climb), and turn physically; stick throttle remains. While shown they replace controller steering and return to centre when let go; your first-person hands hold them." },
    { "vr.vehicle_wheel_visible", _config_boolean, "true", "HALO_VR_VEHICLE_WHEEL_VISIBLE", _environment_value, _platform_vr,
        "Show virtual cockpit control geometry. Hiding it retains physical grip controls; the Warthog's wheel is then taken on the vehicle's own wheel (HOG REACH, LEFT, HEIGHT and SIZE line it up), drawn it sits 2.5 cm in front of it." },
    { "vr.vehicle_cockpit_lock", _config_boolean, "true", "HALO_VR_VEHICLE_COCKPIT_LOCK", _environment_value, _platform_vr,
        "COCKPIT LOCK: with IMMERSIVE VEHICLE controls up, the eyes, the hands and the controls are held to the vehicle as one: the cockpit stays still around you and the world moves (pitch, roll and turns), so the wheel stays on the dash under your hands. Off, the horizon follows HORIZON." },
    { "vr.vehicle_hand_lock", _config_boolean, "true", "HALO_VR_VEHICLE_HAND_LOCK", _environment_value, _platform_vr,
        "Test73, HAND LOCK (a tester's request): with IMMERSIVE VEHICLE controls up, a hand holding grip on the wheel, the handlebar, the drive stick or the flight bar is drawn locked on it where it took hold, turning with it, until the grip is let go. Off, a holding hand is drawn on the control only within 10 cm of the controller, turned as the controller is." },
    { "vr.vehicle_grip_guide", _config_boolean, "false", "HALO_VR_VEHICLE_GRIP_GUIDE", _environment_value, _platform_vr,
        "GRIP GUIDE: marks where the hands take the Warthog's own steering wheel (a rubber pad at each grip, turning with it). Off, only the vehicle's own parts are seen." },
    { "vr.vehicle_control_glow", _config_boolean, "true", "HALO_VR_VEHICLE_CONTROL_GLOW", _environment_value, _platform_vr,
        "CONTROL GLOW: the immersive vehicle controls light up while a free hand is close enough to take them." },
    { "vr.vehicle_steer_haptics", _config_boolean, "true", "HALO_VR_VEHICLE_STEER_HAPTICS", _environment_value, _platform_vr,
        "STEER HAPTICS: light ticks as the immersive vehicle controls turn, a tick crossing the centre and a firmer buzz at full lock (the grab and let-go pulses stay; HAPTICS scales all)." },
    { "vr.vehicle_comfort_reach", _config_boolean, "true", "HALO_VR_VEHICLE_COMFORT_REACH", _environment_value, _platform_vr,
        "COMFORT REACH: an immersive vehicle control on the vehicle's own place that is more than 55 cm from the eyes (the Warthog's wheel, the Ghost's handlebar) comes in toward them along the same line, so the arms do not stretch; off, every control stays on the vehicle's own place (a hidden Warthog wheel always does). Over a metre away, a control is put in front of the chest either way." },
    { "vr.vehicle_tank_drive", _config_boolean, "true", "HALO_VR_VEHICLE_TANK_DRIVE", _environment_value, _platform_vr,
        "TANK DRIVE: a tank's immersive drive stick (Scorpion, Wraith) drives as well as steers: pushed forward it drives on, pulled back it reverses (10 cm is full), leaned left or right it turns; the left thumbstick still drives, and the one pushed further wins. Off, the stick only steers." },
    { "vr.vehicle_tank_hatch", _config_boolean, "false", "HALO_VR_VEHICLE_TANK_HATCH", _environment_value, _platform_vr,
        "TANK HATCH (off by default): in a tank's first-person driver seat (Scorpion, Wraith) the eyes rise over the hull above the driver's head, as from an open hatch (measured on the vehicle itself; 12 cm over it); off, the view stays at the driver's head inside the hull. TANK UP/FWD/RIGHT still adjust it." },
    { "vr.vehicle_banshee_slide", _config_boolean, "true", "HALO_VR_VEHICLE_BANSHEE_SLIDE", _environment_value, _platform_vr,
        "BANSH SLIDE: the Banshee's immersive flight bar also turns when slid sideways (12 cm is full lock), as well as when rolled; off, rolling alone turns it." },
    { "vr.handle_all_forward", _config_real, "0.0", "HALO_VR_HANDLE_ALL_FORWARD", _environment_value, _platform_vr,
        "Moves all immersive vehicle controls forward, in metres. Added to each vehicle's own adjustment, combined limit -0.50 to 0.50 m. COMFORT REACH still limits final reach." },
    { "vr.handle_all_left", _config_real, "0.0", "HALO_VR_HANDLE_ALL_LEFT", _environment_value, _platform_vr,
        "Moves all immersive vehicle controls left, in metres. Added to each vehicle's own adjustment, combined limit -0.50 to 0.50 m. COMFORT REACH still limits final reach." },
    { "vr.handle_all_up", _config_real, "0.0", "HALO_VR_HANDLE_ALL_UP", _environment_value, _platform_vr,
        "Moves all immersive vehicle controls up, in metres. Added to each vehicle's own adjustment, combined limit -0.50 to 0.50 m. COMFORT REACH still limits final reach." },
    { "vr.handle_warthog_forward", _config_real, "0.0", "HALO_VR_HANDLE_WARTHOG_FORWARD", _environment_value, _platform_vr,
        "Virtual warthog control forward; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_warthog_left", _config_real, "0.0", "HALO_VR_HANDLE_WARTHOG_LEFT", _environment_value, _platform_vr,
        "Virtual warthog control left; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_warthog_up", _config_real, "0.0", "HALO_VR_HANDLE_WARTHOG_UP", _environment_value, _platform_vr,
        "Virtual warthog control up; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_warthog_size", _config_real, "1.0", "HALO_VR_HANDLE_WARTHOG_SIZE", _environment_value, _platform_vr,
        "Virtual warthog control size; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_ghost_forward", _config_real, "0.0", "HALO_VR_HANDLE_GHOST_FORWARD", _environment_value, _platform_vr,
        "Virtual ghost control forward; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_ghost_left", _config_real, "0.0", "HALO_VR_HANDLE_GHOST_LEFT", _environment_value, _platform_vr,
        "Virtual ghost control left; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_ghost_up", _config_real, "0.0", "HALO_VR_HANDLE_GHOST_UP", _environment_value, _platform_vr,
        "Virtual ghost control up; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_ghost_size", _config_real, "1.0", "HALO_VR_HANDLE_GHOST_SIZE", _environment_value, _platform_vr,
        "Virtual ghost control size; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_scorpion_forward", _config_real, "0.0", "HALO_VR_HANDLE_SCORPION_FORWARD", _environment_value, _platform_vr,
        "Virtual scorpion control forward; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_scorpion_left", _config_real, "0.0", "HALO_VR_HANDLE_SCORPION_LEFT", _environment_value, _platform_vr,
        "Virtual scorpion control left; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_scorpion_up", _config_real, "0.0", "HALO_VR_HANDLE_SCORPION_UP", _environment_value, _platform_vr,
        "Virtual scorpion control up; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_scorpion_size", _config_real, "1.0", "HALO_VR_HANDLE_SCORPION_SIZE", _environment_value, _platform_vr,
        "Virtual scorpion control size; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_banshee_forward", _config_real, "0.0", "HALO_VR_HANDLE_BANSHEE_FORWARD", _environment_value, _platform_vr,
        "Virtual banshee control forward; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_banshee_left", _config_real, "0.0", "HALO_VR_HANDLE_BANSHEE_LEFT", _environment_value, _platform_vr,
        "Virtual banshee control left; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_banshee_up", _config_real, "0.0", "HALO_VR_HANDLE_BANSHEE_UP", _environment_value, _platform_vr,
        "Virtual banshee control up; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.handle_banshee_size", _config_real, "1.0", "HALO_VR_HANDLE_BANSHEE_SIZE", _environment_value, _platform_vr,
        "Virtual banshee control size; position in metres, size as multiplier. Readjust only while ungripped." },
    { "vr.reload_shoulder", _config_boolean, "true", "HALO_VR_RELOAD_SHOULDER", _environment_value, _platform_vr,
        "Manual reload pouch behind the free-hand shoulder. Automatically mirrors when the gun changes hands." },
    { "vr.reload_hip", _config_boolean, "true", "HALO_VR_RELOAD_HIP", _environment_value, _platform_vr,
        "Manual reload pouch at the free-hand hip. Automatically mirrors when the gun changes hands." },
    { "vr.reload_pouch_side", _config_string, "\"auto\"", "HALO_VR_RELOAD_POUCH_SIDE", _environment_value, _platform_vr,
        "Body side of the manual reload magazine pouches: auto (the free hand's side: left for a right-handed player), left, right or both." },
    { "vr.pouch_hip_out", _config_real, "0.0", "HALO_VR_POUCH_HIP_OUT", _environment_value, _platform_vr,
        "Moves the hip magazine pouch out from the body's middle (each side mirrored) from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.pouch_hip_up", _config_real, "0.0", "HALO_VR_POUCH_HIP_UP", _environment_value, _platform_vr,
        "Moves the hip magazine pouch up from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.pouch_hip_back", _config_real, "0.0", "HALO_VR_POUCH_HIP_BACK", _environment_value, _platform_vr,
        "Moves the hip magazine pouch back from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.pouch_shoulder_out", _config_real, "0.0", "HALO_VR_POUCH_SHOULDER_OUT", _environment_value, _platform_vr,
        "Moves the shoulder magazine pouch out from the body's middle (each side mirrored) from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.pouch_shoulder_up", _config_real, "0.0", "HALO_VR_POUCH_SHOULDER_UP", _environment_value, _platform_vr,
        "Moves the shoulder magazine pouch up from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.pouch_shoulder_back", _config_real, "0.0", "HALO_VR_POUCH_SHOULDER_BACK", _environment_value, _platform_vr,
        "Moves the shoulder magazine pouch back from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.holster_hip_out", _config_real, "0.0", "HALO_VR_HOLSTER_HIP_OUT", _environment_value, _platform_vr,
        "Moves the hip holsters out from the body's middle (each side mirrored) from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.holster_hip_up", _config_real, "0.0", "HALO_VR_HOLSTER_HIP_UP", _environment_value, _platform_vr,
        "Moves the hip holsters up from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.holster_hip_back", _config_real, "0.0", "HALO_VR_HOLSTER_HIP_BACK", _environment_value, _platform_vr,
        "Moves the hip holsters back from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.holster_shoulder_out", _config_real, "0.0", "HALO_VR_HOLSTER_SHOULDER_OUT", _environment_value, _platform_vr,
        "Moves the shoulder holsters out from the body's middle (each side mirrored) from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.holster_shoulder_up", _config_real, "0.0", "HALO_VR_HOLSTER_SHOULDER_UP", _environment_value, _platform_vr,
        "Moves the shoulder holsters up from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.holster_shoulder_back", _config_real, "0.0", "HALO_VR_HOLSTER_SHOULDER_BACK", _environment_value, _platform_vr,
        "Moves the shoulder holsters back from its default, in metres (-0.20 to 0.20). A blue marker shows the zone while the row is on screen." },
    { "vr.charge_grab_radius", _config_real, "0.04", "HALO_VR_CHARGE_GRAB_RADIUS", _environment_value, _platform_vr,
        "CHARGE GRAB: how far in metres (0.02 to 0.10) past HAND CONTACT a hand still takes a weapon's charging handle or slide during a manual reload; 0.04 keeps Test55's reach." },
    { "vr.reload_pouch_size", _config_real, "0.16", "HALO_VR_RELOAD_POUCH_SIZE", _environment_value, _platform_vr,
        "Reach of each magazine pouch in metres (0.08 to 0.30)." },
	{ "vr.manual_reload", _config_boolean, "true", "HALO_VR_MANUAL_RELOAD", _environment_value, _platform_vr,
		"Optional VR physical reload for supported weapons, in a local game or (Test73) co-op whose host's VR INTERACTIONS allows it (a co-op player's reload is then committed by its host). The USE/RELOAD action drops the magazine (it never reloads automatically while this is on); take a fresh one from a hip or shoulder pouch, push it into the well, and charge the gun only if the chamber was empty. Unsupported weapons keep the stock reload." },
	{ "vr.npc_grab_assist", _config_real, "0.30", "HALO_VR_NPC_GRAB_ASSIST", _environment_value, _platform_vr,
		"GRAB ASSIST: how far from an NPC's body (its visible surface) a squeeze still takes it, in metres (0 to 0.5; 30 cm by default since Test72, and NPC GRAB and FIST COMBAT bring at least 30 cm with them). 0 is touch only. A hand that touches no object (a prop, a turret, a vehicle) takes the nearest within this reach and 3 cm more." },
	{ "vr.object_throw", _config_real, "2.0", "HALO_VR_OBJECT_THROW", _environment_value, _platform_vr,
		"OBJECT THROW: how hard a prop or a vehicle let go of while moving flies: the hand's speed times this (0 to 3; 2 by default; a vehicle at most 12 m/s, a prop 24). 0 lets it drop. WORLD GRAB and LIFT VEHICLES bring it." },
	{ "vr.test73_features_applied", _config_boolean, "false", "HALO_VR_TEST73_FEATURES_APPLIED", _environment_value, _platform_vr,
		"Internal one-time change (Test73, the owner's): every VR feature (manual reload, parkour, fist combat,\n"
		"immersive vehicles, NPC and world grabs, one-hand heavies, physical grenades, slide, bullet time, death\n"
		"ragdolls...) turned on once, as VR SETTINGS turns it on, with what it needs; turned off after, it stays off." },
	{ "vr.test73b_sprint_toggle_applied", _config_boolean, "false", "HALO_VR_TEST73B_SPRINT_TOGGLE_APPLIED", _environment_value, _platform_vr,
		"Internal one-time change (Test73b, the owner's): SPRINT TOGGLE at HOLD, its old default, goes to\n"
		"TOGGLE once (BUTTON SPRINT comes with SLIDE since Test73); HOLD chosen after it is kept." },
	{ "vr.test72_combat_defaults_applied", _config_boolean, "false", "HALO_VR_TEST72_COMBAT_DEFAULTS_APPLIED", _environment_value, _platform_vr,
		"Internal one-time move (Test72): FIST COMBAT's LUNGE, HIT ASSIST and LUNGE HOP and GRAB ASSIST still\n"
		"at their old defaults (8 m, 30 cm, 15 cm, 15 cm) go to the new ones (12 m, 50 cm, 30 cm, 30 cm),\n"
		"once; any other value is the player's and stays." },
	{ "vr.test70_defaults_applied", _config_boolean, "false", "HALO_VR_TEST70_DEFAULTS_APPLIED", _environment_value, _platform_vr,
		"Internal one-time move (Test70): GRAB ASSIST at the old automatic 10 cm, with NPC GRAB or FIST COMBAT\n"
		"on, goes to their new 15 cm, once; any other value is the player's and stays." },
	{ "vr.grenade_waist_applied", _config_boolean, "false", "HALO_VR_GRENADE_WAIST_APPLIED", _environment_value, _platform_vr,
		"Internal one-time move (Test66): the physical grenades to the waist (GRENADE DOWN 60 cm, FWD 12 cm, GAP 18 cm), once; later choices stay." },
	{ "vr.npc_one_hand_heavy", _config_boolean, "true", "HALO_VR_NPC_ONE_HAND_HEAVY", _environment_value, _platform_vr,
		"ONE-HAND HEAVIES: a living Elite or Hunter lifts with one hand (off: one hand holds it in place, the second lifts). Needs NPC GRAB." },
	{ "vr.npc_hold_strength", _config_real, "1.5", "HALO_VR_NPC_HOLD_STRENGTH", _environment_value, _platform_vr,
		"LIFT: how firmly what the hands lift follows them: NPCs, props, turrets and vehicles (0.75 to 2.5; 1.5 Spartan, the default, keeps props as before). Heavier species and objects still resist or need two hands." },
	{ "vr.throw_strength", _config_real, "2.0", "HALO_VR_THROW_STRENGTH", _environment_value, _platform_vr,
		"Multiplier on the hand's release velocity for thrown props and NPCs (0.75 to 2.0; 2.0 by default)." },
	{ "vr.climb_grab_radius", _config_real, "0.14", "HALO_VR_CLIMB_GRAB_RADIUS", _environment_value, _platform_vr,
		"How far in metres (0.04 to 0.20) from a solid surface a squeezed free hand still grips it for climbing; a hand pushed into a wall grips its surface." },
	{ "vr.immersion_auto", _config_string, "\"\"", "HALO_VR_IMMERSION_AUTO", _environment_value, _platform_vr,
		"Written by VR Settings: the settings it turned on only because an immersion feature needs them (FIST REQUIRED for grabbing and climbing, MANUAL RELOAD for MAG DEBRIS...), turned off again when no feature needs them." },
	{ "vr.active_ragdoll", _config_boolean, "true", "HALO_VR_ACTIVE_RAGDOLL", _environment_value, _platform_vr,
	    "ACTIVE RAGDOLL: NPC bodies give where they are touched, held or hit (the limb swings, the torso leans; drawn only), a slow hand pushes them, a body knocked or thrown keeps some of its posture as it staggers and gets up, and a body just killed slumps instead of dropping limp." },
	{ "vr.npc_lift_to_ragdoll", _config_boolean, "true", "HALO_VR_NPC_LIFT_TO_RAGDOLL", _environment_value, _platform_vr,
	    "GRAB RAGDOLL: ON LIFT keeps a living NPC taken by a limb on its feet (held still, a light one dragged along; it cannot walk or shoot) until the hands lift it 18 cm or yank it, then it is a ragdoll in the hands; AT ONCE makes it a ragdoll as soon as it is taken (as before). Corpses are ragdolls at once either way." },
	{ "vr.death_ragdolls", _config_boolean, "true", "HALO_VR_DEATH_RAGDOLLS", _environment_value, _platform_vr,
		"Solo (and co-op host): NPCs killed nearby fall as physical ragdolls carrying their momentum, then rest as grabbable corpses. Uses the NPC RIG SLOTS pool." },
    { "vr.death_ragdoll_range", _config_real, "60.0", "HALO_VR_DEATH_RAGDOLL_RANGE", _environment_value, _platform_vr,
        "Death ragdoll distance from a local player in metres (20 to 80, default 60). The existing NPC RIG SLOTS and settling limits still apply; lower this on busy maps if needed." },
	{ "vr.death_force", _config_string, "\"strong\"", "HALO_VR_DEATH_FORCE", _environment_value, _platform_vr,
		"DEATH RAGDOLLS: how hard a killing burst or blast throws the body: classic (Test56's: shot bodies crumple where they stand), strong (the default: back along the shots, away from explosions with lift) or huge." },
	{ "vr.npc_recovery", _config_real, "1.0", "HALO_VR_NPC_RECOVERY", _environment_value, _platform_vr,
		"The longest time in seconds (0.25 to 1.0) a living NPC lies limp after being dropped or thrown before it gets up; it gets up sooner once it is at rest. Corpses never get up." },
	{ "vr.reload_input", _config_string, "\"grip\"", "HALO_VR_RELOAD_INPUT", _environment_value, _platform_vr,
		"Input for the free hand during optional manual reload only: grip (default) or trigger. Other interaction and firing inputs are unchanged." },
	{ "vr.dropped_magazines", _config_boolean, "true", "HALO_VR_DROPPED_MAGAZINES", _environment_value, _platform_vr,
		"Keep ejected supported rifle/pistol/sniper magazines as local visual debris until the map changes, wherever MANUAL RELOAD works (Test73: co-op too, when the host allows it). Requires MANUAL RELOAD; disabling MANUAL RELOAD in the VR menu also turns this off. Cancelled reloads remove their provisional visual. No ammo, save, or network state is created." },
	{ "vr.reload_grip_fit", _config_boolean, "true", "HALO_VR_RELOAD_GRIP_FIT", _environment_value, _platform_vr,
		"Test70: with manual reload, the Pistol's and Assault Rifle's magazine is held with its centre\n"
		"in the closed hand (the reload animation's own fingers), not where their animations leave it\n"
		"(behind the fingers; the Pistol's never in the hand). The Sniper's hold is unchanged. false:\n"
		"Test67's authored place." },
	{ "vr.cooldown_shake", _config_boolean, "true", "HALO_VR_COOLDOWN_SHAKE", _environment_value, _platform_vr,
		"Offline VR toggle: shake the currently held overheated weapon to speed its authored cooling. Automatically disabled online." },
	{ "vr.custom_scenery_grab", _config_boolean, "true", "HALO_VR_CUSTOM_SCENERY_GRAB", _environment_value, _platform_vr,
		"Opt in to bounded collision-box grabbing of eligible unnamed custom scenery. The VR menu also enables WORLD GRAB; scripted, animated, unsafe or malformed tags remain refused." },
	{ "vr.custom_npc_rigs", _config_boolean, "true", "HALO_VR_CUSTOM_NPC_RIGS", _environment_value, _platform_vr,
		"Solo-only fallback for otherwise-unprofiled human, Covenant, and combat-form actors with grounded native biped capsules and a valid authored joint graph. The VR menu also enables NPC GRAB. Existing species profiles are unchanged; flying, swarm, scripted, unsafe, and malformed rigs remain refused." },
	{ "vr.npc_rescue", _config_boolean, "true", "HALO_VR_NPC_RESCUE", _environment_value, _platform_vr,
		"Optional solo VR recovery for eligible live encounter NPCs falling unsupported for 10 seconds. Returns them to a clear last-grounded pose; does not respawn, revive, or override active scripts." },
	{ "vr.npc_max_ragdolls", _config_integer, "24", "HALO_VR_NPC_MAX_RAGDOLLS", _environment_value, _platform_all | _platform_vr,
		"Maximum simultaneously active physical NPC interaction rigs (1-16), including settling/recovery. New configurations default to 8 for performance. This controls mod interaction rigs, separately from BODY RETENTION." },
	{ "vr.ragdoll_retention", _config_integer, "75", "HALO_VR_RAGDOLL_RETENTION", _environment_value, _platform_vr,
		"Solo campaign only: active-garbage cleanup trigger per 16 players. 75 (default) uses a 75 trigger / 45 target; 100 uses 100 / 60; 0 keeps OpenCE's 50 / 30. Corpses share this pool with other garbage, and memory/object pressure can still collect sooner. Multiplayer always uses native thresholds." },
    { "vr.npc_interaction", _config_boolean, "true", "HALO_VR_NPC_INTERACTION", _environment_value, _platform_vr,
        "Optional physical interaction with supported native NPC rigs. Co-op requires host permission and negotiated compatible peers; PvP stays native." },
	{ "vr.interaction_damage", _config_boolean, "true", "HALO_VR_INTERACTION_DAMAGE", _environment_value, _platform_all,
        "Physical impact damage for explicitly grabbed objects only; requires optional physical interactions." },
	{ "vr.impact_threshold", _config_real, "2.0", "HALO_VR_IMPACT_THRESHOLD", _environment_value, _platform_all,
        "Minimum surface-normal impact speed in metres/second before physical damage." },
	{ "vr.impact_damage_cap", _config_real, "1.0", "HALO_VR_IMPACT_DAMAGE_CAP", _environment_value, _platform_all,
        "Maximum impact damage as a multiple of the grabbing player native melee damage." },
	{ "vr.world_interaction", _config_boolean, "true", "HALO_VR_WORLD_INTERACTION", _environment_value, _platform_vr,
        "Optional loose weapon, equipment and movable prop grabbing. Grip close with a free hand.\n"
        "Two hands improve control. Co-op requires a consenting compatible host and peers; competitive multiplayer stays native." },
	{ "vr.interaction_reach", _config_real, "0.04", "HALO_VR_INTERACTION_REACH", _environment_value, _platform_vr,
		"Tracked palm contact-sphere radius in metres for physical object/NPC grabs and climbing (0.02 to 0.08). Grip only acquires a target while the hand collision sphere overlaps its collision model or solid surface." },
	{ "vr.vehicle_lift", _config_boolean, "true", "HALO_VR_VEHICLE_LIFT", _environment_value, _platform_vr,
		"Optional offline, contact-grabbed full-body lifting of unoccupied, unnamed Warthog, Ghost, Scorpion and turret vehicles; independent of WORLD GRAB. One/two hand motion controls bounded native translation and orientation; release leaves a speed-capped vehicle in native physics. No teleporting. Destroyed, occupied, parented, named/movie-star, unsupported and network vehicles remain native." },
	{ "vr.scope_size", _config_real, "0.06", "HALO_VR_SCOPE_SIZE", _environment_value, _platform_vr,
		"How wide the scope is, in metres." },
	{ "vr.scope_pistol_forward", _config_real, "0.0", "HALO_VR_SCOPE_PISTOL_FORWARD", _environment_value, _platform_vr,
		"The pistol (round sight) scope moved forward (toward the muzzle) from its usual place, metres (-0.30..0.30)." },
	{ "vr.scope_pistol_up", _config_real, "0.0", "HALO_VR_SCOPE_PISTOL_UP", _environment_value, _platform_vr,
		"The pistol (round sight) scope moved up from its usual place, metres (-0.30..0.30)." },
	{ "vr.scope_pistol_right", _config_real, "0.0", "HALO_VR_SCOPE_PISTOL_RIGHT", _environment_value, _platform_vr,
		"The pistol (round sight) scope moved right (in either hand) from its usual place, metres (-0.30..0.30)." },
	{ "vr.scope_pistol_scale", _config_real, "1.0", "HALO_VR_SCOPE_PISTOL_SCALE", _environment_value, _platform_vr,
		"The pistol (round sight) scope's size as a share of its usual size (1 = as usual; 0.5 to 4)." },
	{ "vr.scope_sniper_forward", _config_real, "0.0", "HALO_VR_SCOPE_SNIPER_FORWARD", _environment_value, _platform_vr,
		"The sniper rifle scope moved forward (toward the muzzle) from its usual place, metres (-0.30..0.30)." },
	{ "vr.scope_sniper_up", _config_real, "0.0", "HALO_VR_SCOPE_SNIPER_UP", _environment_value, _platform_vr,
		"The sniper rifle scope moved up from its usual place, metres (-0.30..0.30)." },
	{ "vr.scope_sniper_right", _config_real, "0.0", "HALO_VR_SCOPE_SNIPER_RIGHT", _environment_value, _platform_vr,
		"The sniper rifle scope moved right (in either hand) from its usual place, metres (-0.30..0.30)." },
	{ "vr.scope_sniper_scale", _config_real, "1.0", "HALO_VR_SCOPE_SNIPER_SCALE", _environment_value, _platform_vr,
		"The sniper rifle scope's size as a share of its usual size (1 = as usual; 0.5 to 4)." },
	{ "vr.scope_rocket_launcher_forward", _config_real, "0.0", "HALO_VR_SCOPE_ROCKET_LAUNCHER_FORWARD", _environment_value, _platform_vr,
		"SPNKR native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_rocket_launcher_up", _config_real, "0.0", "HALO_VR_SCOPE_ROCKET_LAUNCHER_UP", _environment_value, _platform_vr,
		"SPNKR native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_rocket_launcher_right", _config_real, "0.0", "HALO_VR_SCOPE_ROCKET_LAUNCHER_RIGHT", _environment_value, _platform_vr,
		"SPNKR native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_rocket_launcher_scale", _config_real, "1.0", "HALO_VR_SCOPE_ROCKET_LAUNCHER_SCALE", _environment_value, _platform_vr,
		"SPNKR native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_other_forward", _config_real, "0.0", "HALO_VR_SCOPE_OTHER_FORWARD", _environment_value, _platform_vr,
		"OTHER native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_other_up", _config_real, "0.0", "HALO_VR_SCOPE_OTHER_UP", _environment_value, _platform_vr,
		"OTHER native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_other_right", _config_real, "0.0", "HALO_VR_SCOPE_OTHER_RIGHT", _environment_value, _platform_vr,
		"OTHER native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_other_scale", _config_real, "1.0", "HALO_VR_SCOPE_OTHER_SCALE", _environment_value, _platform_vr,
		"OTHER native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_plasma_pistol_forward", _config_real, "0.0", "HALO_VR_SCOPE_PLASMA_PISTOL_FORWARD", _environment_value, _platform_vr,
		"PLASMA PISTOL native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_plasma_pistol_up", _config_real, "0.0", "HALO_VR_SCOPE_PLASMA_PISTOL_UP", _environment_value, _platform_vr,
		"PLASMA PISTOL native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_plasma_pistol_right", _config_real, "0.0", "HALO_VR_SCOPE_PLASMA_PISTOL_RIGHT", _environment_value, _platform_vr,
		"PLASMA PISTOL native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_plasma_pistol_scale", _config_real, "1.0", "HALO_VR_SCOPE_PLASMA_PISTOL_SCALE", _environment_value, _platform_vr,
		"PLASMA PISTOL native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_assault_rifle_forward", _config_real, "0.0", "HALO_VR_SCOPE_ASSAULT_RIFLE_FORWARD", _environment_value, _platform_vr,
		"ASSAULT RIFLE native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_assault_rifle_up", _config_real, "0.0", "HALO_VR_SCOPE_ASSAULT_RIFLE_UP", _environment_value, _platform_vr,
		"ASSAULT RIFLE native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_assault_rifle_right", _config_real, "0.0", "HALO_VR_SCOPE_ASSAULT_RIFLE_RIGHT", _environment_value, _platform_vr,
		"ASSAULT RIFLE native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_assault_rifle_scale", _config_real, "1.0", "HALO_VR_SCOPE_ASSAULT_RIFLE_SCALE", _environment_value, _platform_vr,
		"ASSAULT RIFLE native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_plasma_rifle_forward", _config_real, "0.0", "HALO_VR_SCOPE_PLASMA_RIFLE_FORWARD", _environment_value, _platform_vr,
		"PLASMA RIFLE native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_plasma_rifle_up", _config_real, "0.0", "HALO_VR_SCOPE_PLASMA_RIFLE_UP", _environment_value, _platform_vr,
		"PLASMA RIFLE native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_plasma_rifle_right", _config_real, "0.0", "HALO_VR_SCOPE_PLASMA_RIFLE_RIGHT", _environment_value, _platform_vr,
		"PLASMA RIFLE native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_plasma_rifle_scale", _config_real, "1.0", "HALO_VR_SCOPE_PLASMA_RIFLE_SCALE", _environment_value, _platform_vr,
		"PLASMA RIFLE native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_shotgun_forward", _config_real, "0.0", "HALO_VR_SCOPE_SHOTGUN_FORWARD", _environment_value, _platform_vr,
		"SHOTGUN native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_shotgun_up", _config_real, "0.0", "HALO_VR_SCOPE_SHOTGUN_UP", _environment_value, _platform_vr,
		"SHOTGUN native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_shotgun_right", _config_real, "0.0", "HALO_VR_SCOPE_SHOTGUN_RIGHT", _environment_value, _platform_vr,
		"SHOTGUN native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_shotgun_scale", _config_real, "1.0", "HALO_VR_SCOPE_SHOTGUN_SCALE", _environment_value, _platform_vr,
		"SHOTGUN native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_needler_forward", _config_real, "0.0", "HALO_VR_SCOPE_NEEDLER_FORWARD", _environment_value, _platform_vr,
		"NEEDLER native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_needler_up", _config_real, "0.0", "HALO_VR_SCOPE_NEEDLER_UP", _environment_value, _platform_vr,
		"NEEDLER native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_needler_right", _config_real, "0.0", "HALO_VR_SCOPE_NEEDLER_RIGHT", _environment_value, _platform_vr,
		"NEEDLER native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_needler_scale", _config_real, "1.0", "HALO_VR_SCOPE_NEEDLER_SCALE", _environment_value, _platform_vr,
		"NEEDLER native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_fuel_rod_forward", _config_real, "0.0", "HALO_VR_SCOPE_FUEL_ROD_FORWARD", _environment_value, _platform_vr,
		"FUEL ROD native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_fuel_rod_up", _config_real, "0.0", "HALO_VR_SCOPE_FUEL_ROD_UP", _environment_value, _platform_vr,
		"FUEL ROD native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_fuel_rod_right", _config_real, "0.0", "HALO_VR_SCOPE_FUEL_ROD_RIGHT", _environment_value, _platform_vr,
		"FUEL ROD native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_fuel_rod_scale", _config_real, "1.0", "HALO_VR_SCOPE_FUEL_ROD_SCALE", _environment_value, _platform_vr,
		"FUEL ROD native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.scope_flamethrower_forward", _config_real, "0.0", "HALO_VR_SCOPE_FLAMETHROWER_FORWARD", _environment_value, _platform_vr,
		"FLAMETHROWER native scope forward adjustment; applies when this weapon has zoom." },
	{ "vr.scope_flamethrower_up", _config_real, "0.0", "HALO_VR_SCOPE_FLAMETHROWER_UP", _environment_value, _platform_vr,
		"FLAMETHROWER native scope up adjustment; applies when this weapon has zoom." },
	{ "vr.scope_flamethrower_right", _config_real, "0.0", "HALO_VR_SCOPE_FLAMETHROWER_RIGHT", _environment_value, _platform_vr,
		"FLAMETHROWER native scope right adjustment; applies when this weapon has zoom." },
	{ "vr.scope_flamethrower_scale", _config_real, "1.0", "HALO_VR_SCOPE_FLAMETHROWER_SCALE", _environment_value, _platform_vr,
		"FLAMETHROWER native scope size as a share of its usual size (1 = as usual; 0.5 to 4); applies when this weapon has zoom." },
	{ "vr.button_jump", _config_string, "\"a\"", "HALO_VR_BUTTON_JUMP", _environment_value, _platform_vr,
		"Quest: the button that jumps: a, b (the gun hand's lower and upper), x, y (the other hand's),\n"
		"right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_action", _config_string, "\"b\"", "HALO_VR_BUTTON_ACTION", _environment_value, _platform_vr,
		"Quest: the button for action and reload: a, b (the gun hand's lower and upper), x, y (the other hand's),\n"
		"right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_melee", _config_string, "\"right_stick\"", "HALO_VR_BUTTON_MELEE", _environment_value, _platform_vr,
		"Quest: the button that melees: a, b (the gun hand's lower and upper), x, y (the other hand's),\n"
		"right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_crouch", _config_string, "\"right_stick_down\"", "HALO_VR_BUTTON_CROUCH", _environment_value, _platform_vr,
		"Quest: the button that crouches (default since 1.0.8: the turning stick held down; ducking\n"
		"also crouches): a, b (the gun hand's lower and upper), x, y (the other hand's),\n"
		"right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_reticle", _config_string, "\"left_stick\"", "HALO_VR_BUTTON_RETICLE", _environment_value, _platform_vr,
		"Quest: the button that shows or hides the reticle (it starts shown; not in a seat, and not\n"
		"with both sticks clicked, which recentres): a, b, x, y, right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_switch_weapon", _config_string, "\"y\"", "HALO_VR_BUTTON_SWITCH_WEAPON", _environment_value, _platform_vr,
		"Quest: the button that switches weapons: a, b (the gun hand's lower and upper), x, y (the other hand's),\n"
		"right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_grenade", _config_string, "\"x\"", "HALO_VR_BUTTON_GRENADE", _environment_value, _platform_vr,
		"Quest: the button that throws a grenade (\"grip\": the gun hand's grip, locked weapons only, throws while held): a, b (the gun hand's lower and upper), x, y (the other hand's),\n"
		"right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_switch_grenade", _config_string, "\"hold\"", "HALO_VR_BUTTON_SWITCH_GRENADE", _environment_value, _platform_vr,
		"Quest: the button that switches grenades, or \"hold\" (default): holding the grenade\n"
		"button switches and tapping it throws. Also a, b, x, y, right_stick, left_stick, right_stick_down (the turning stick held down), grip or none." },
	{ "vr.button_bullet_time", _config_string, "\"right_stick_up\"", "HALO_VR_BUTTON_BULLET_TIME", _environment_value, _platform_vr,
		"Test70: the button BULLET TIME is held (or toggled) on, when vr.bullet_time is on: the turning\n"
		"stick pushed up by default (right_stick_up; the one input no action has), or a, b, x, y,\n"
		"right_stick, left_stick, right_stick_down, grip or none." },
	{ "vr.bullet_time", _config_boolean, "true", "HALO_VR_BULLET_TIME", _environment_value, _platform_vr,
		"Test70: BULLET TIME: the world slowed while its button is held (or toggled), offline only (a\n"
		"local game, never co-op or multiplayer); the head and hands keep their full frame rate.\n"
		"Not in a seat, a menu or a cutscene." },
	{ "vr.bullet_time_scale", _config_real, "0.4", "HALO_VR_BULLET_TIME_SCALE", _environment_value, _platform_vr,
		"Test70: BULLET TIME's speed of time, 0.2 to 0.8 (0.4: two fifths)." },
	{ "vr.bullet_time_toggle", _config_boolean, "false", "HALO_VR_BULLET_TIME_TOGGLE", _environment_value, _platform_vr,
		"Test70: BULLET TIME: false holds it while the button is held; true toggles it." },
	{ "vr.button_push_to_talk", _config_string, "\"none\"", "HALO_VR_BUTTON_PUSH_TO_TALK", _environment_value, _platform_vr,
		"Quest: the controller button held to transmit when audio.voice_chat is \"push_to_talk\".\n"
		"Default none; no microphone capture occurs until PTT is bound and held, or Open Mic is explicitly selected." },
	{ "vr.align_left_pitch", _config_real, "0.0", "HALO_VR_ALIGN_LEFT_PITCH", _environment_value, _platform_vr,
		"Left controller tracking correction, local pitch: degrees (-180..180). Moves hand AND gun;\n"
		"for comfort use vr.hand_* (hand) or vr.weapon_* (gun). Zero preserves runtime tracking." },
	{ "vr.align_left_yaw", _config_real, "0.0", "HALO_VR_ALIGN_LEFT_YAW", _environment_value, _platform_vr,
		"Left controller tracking correction, local yaw: degrees (-180..180). Moves hand AND gun;\n"
		"for comfort use vr.hand_* (hand) or vr.weapon_* (gun). Zero preserves runtime tracking." },
	{ "vr.align_left_roll", _config_real, "0.0", "HALO_VR_ALIGN_LEFT_ROLL", _environment_value, _platform_vr,
		"Left controller tracking correction, local roll: degrees (-180..180). Moves hand AND gun;\n"
		"for comfort use vr.hand_* (hand) or vr.weapon_* (gun). Zero preserves runtime tracking." },
	{ "vr.align_left_right", _config_real, "0.0", "HALO_VR_ALIGN_LEFT_RIGHT", _environment_value, _platform_vr,
		"Left controller local right: metres (-0.20..0.20), in the original controller frame. Zero preserves runtime tracking." },
	{ "vr.align_left_up", _config_real, "0.0", "HALO_VR_ALIGN_LEFT_UP", _environment_value, _platform_vr,
		"Left controller local up: metres (-0.20..0.20), in the original controller frame. Zero preserves runtime tracking." },
	{ "vr.align_left_back", _config_real, "0.0", "HALO_VR_ALIGN_LEFT_BACK", _environment_value, _platform_vr,
		"Left controller local back: metres (-0.20..0.20), in the original controller frame. Zero preserves runtime tracking." },
	{ "vr.align_left_grip_aim", _config_boolean, "false", "HALO_VR_ALIGN_LEFT_GRIP_AIM", _environment_value, _platform_vr,
		"Use left grip pose for aim too. Optional controller compatibility mode; false preserves native aim." },
	{ "vr.align_right_pitch", _config_real, "0.0", "HALO_VR_ALIGN_RIGHT_PITCH", _environment_value, _platform_vr,
		"Right controller tracking correction, local pitch: degrees (-180..180). Moves hand AND gun;\n"
		"for comfort use vr.hand_* (hand) or vr.weapon_* (gun). Zero preserves runtime tracking." },
	{ "vr.align_right_yaw", _config_real, "0.0", "HALO_VR_ALIGN_RIGHT_YAW", _environment_value, _platform_vr,
		"Right controller tracking correction, local yaw: degrees (-180..180). Moves hand AND gun;\n"
		"for comfort use vr.hand_* (hand) or vr.weapon_* (gun). Zero preserves runtime tracking." },
	{ "vr.align_right_roll", _config_real, "0.0", "HALO_VR_ALIGN_RIGHT_ROLL", _environment_value, _platform_vr,
		"Right controller tracking correction, local roll: degrees (-180..180). Moves hand AND gun;\n"
		"for comfort use vr.hand_* (hand) or vr.weapon_* (gun). Zero preserves runtime tracking." },
	{ "vr.align_right_right", _config_real, "0.0", "HALO_VR_ALIGN_RIGHT_RIGHT", _environment_value, _platform_vr,
		"Right controller local right: metres (-0.20..0.20), in the original controller frame. Zero preserves runtime tracking." },
	{ "vr.align_right_up", _config_real, "0.0", "HALO_VR_ALIGN_RIGHT_UP", _environment_value, _platform_vr,
		"Right controller local up: metres (-0.20..0.20), in the original controller frame. Zero preserves runtime tracking." },
	{ "vr.align_right_back", _config_real, "0.0", "HALO_VR_ALIGN_RIGHT_BACK", _environment_value, _platform_vr,
		"Right controller local back: metres (-0.20..0.20), in the original controller frame. Zero preserves runtime tracking." },
	{ "vr.align_right_grip_aim", _config_boolean, "false", "HALO_VR_ALIGN_RIGHT_GRIP_AIM", _environment_value, _platform_vr,
		"Use right grip pose for aim too. Optional controller compatibility mode; false preserves native aim." },
	{ "vr.hand_tracking", _config_string, "\"ik\"", "HALO_VR_HAND_TRACKING", _environment_value, _platform_vr,
		"How tracked hands follow the controllers (with vr.arms \"ik\"): \"ik\" body-IK arms reach\n"
		"from the shoulders; \"floating\" hands go exactly where the controllers are, with no\n"
		"arms; \"floating_arms\" hands go exactly to the controllers and arms hang from a\n"
		"shoulder that follows them." },
	{ "vr.hand_left_pitch", _config_real, "-70.0", "HALO_VR_HAND_LEFT_PITCH", _environment_value, _platform_vr,
		"Left visible hand only: degrees (-180..180) around the controller's X axis. Default -70\n"
		"lines the empty hand up with a hand holding a Touch controller. Does not move the gun." },
	{ "vr.hand_left_yaw", _config_real, "0.0", "HALO_VR_HAND_LEFT_YAW", _environment_value, _platform_vr,
		"Left visible hand only: degrees around the controller's Y axis. Does not move the gun." },
	{ "vr.hand_left_roll", _config_real, "0.0", "HALO_VR_HAND_LEFT_ROLL", _environment_value, _platform_vr,
		"Left visible hand only: degrees around the controller's Z axis. Does not move the gun." },
	{ "vr.hand_right_pitch", _config_real, "-70.0", "HALO_VR_HAND_RIGHT_PITCH", _environment_value, _platform_vr,
		"Right visible hand only: as vr.hand_left_pitch. Does not move the gun." },
	{ "vr.hand_right_yaw", _config_real, "0.0", "HALO_VR_HAND_RIGHT_YAW", _environment_value, _platform_vr,
		"Right visible hand only: as vr.hand_left_yaw. Does not move the gun." },
	{ "vr.hand_right_roll", _config_real, "0.0", "HALO_VR_HAND_RIGHT_ROLL", _environment_value, _platform_vr,
		"Right visible hand only: as vr.hand_left_roll. Does not move the gun." },
	{ "vr.weapon_pitch", _config_real, "0.0", "HALO_VR_WEAPON_PITCH", _environment_value, _platform_vr,
		"One-handed gun angle relative to the controller's aim: degrees (-180..180). Turns the\n"
		"gun, its shots and reticle together; mirrored for the left hand. Hands are unaffected." },
	{ "vr.weapon_yaw", _config_real, "0.0", "HALO_VR_WEAPON_YAW", _environment_value, _platform_vr,
		"As vr.weapon_pitch, around the controller's up axis." },
	{ "vr.weapon_roll", _config_real, "0.0", "HALO_VR_WEAPON_ROLL", _environment_value, _platform_vr,
		"As vr.weapon_pitch, around the aim axis." },
	{ "vr.aim_pistol_up", _config_real, "0.0", "HALO_VR_AIM_PISTOL_UP", _environment_value, _platform_vr,
		"Per-gun aim adjustment (Hands + Gun > Aim Up while holding the gun): degrees (-10..10)\n"
		"the pistol's shots, reticle and scope turn up from where the gun points. The gun itself\n"
		"does not move. 0 = no adjustment." },
	{ "vr.aim_pistol_right", _config_real, "0.0", "HALO_VR_AIM_PISTOL_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the pistol." },
	{ "vr.aim_plasma_pistol_up", _config_real, "0.0", "HALO_VR_AIM_PLASMA_PISTOL_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the plasma pistol." },
	{ "vr.aim_plasma_pistol_right", _config_real, "0.0", "HALO_VR_AIM_PLASMA_PISTOL_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the plasma pistol." },
	{ "vr.aim_assault_rifle_up", _config_real, "0.0", "HALO_VR_AIM_ASSAULT_RIFLE_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the assault rifle." },
	{ "vr.aim_assault_rifle_right", _config_real, "0.0", "HALO_VR_AIM_ASSAULT_RIFLE_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the assault rifle." },
	{ "vr.aim_plasma_rifle_up", _config_real, "0.0", "HALO_VR_AIM_PLASMA_RIFLE_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the plasma rifle." },
	{ "vr.aim_plasma_rifle_right", _config_real, "0.0", "HALO_VR_AIM_PLASMA_RIFLE_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the plasma rifle." },
	{ "vr.aim_shotgun_up", _config_real, "0.0", "HALO_VR_AIM_SHOTGUN_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the shotgun." },
	{ "vr.aim_shotgun_right", _config_real, "0.0", "HALO_VR_AIM_SHOTGUN_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the shotgun." },
	{ "vr.aim_sniper_rifle_up", _config_real, "0.0", "HALO_VR_AIM_SNIPER_RIFLE_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the sniper rifle." },
	{ "vr.aim_sniper_rifle_right", _config_real, "0.0", "HALO_VR_AIM_SNIPER_RIFLE_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the sniper rifle." },
	{ "vr.aim_rocket_launcher_up", _config_real, "0.0", "HALO_VR_AIM_ROCKET_LAUNCHER_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the rocket launcher." },
	{ "vr.aim_rocket_launcher_right", _config_real, "0.0", "HALO_VR_AIM_ROCKET_LAUNCHER_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the rocket launcher." },
	{ "vr.aim_needler_up", _config_real, "0.0", "HALO_VR_AIM_NEEDLER_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the needler." },
	{ "vr.aim_needler_right", _config_real, "0.0", "HALO_VR_AIM_NEEDLER_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the needler." },
	{ "vr.aim_fuel_rod_up", _config_real, "0.0", "HALO_VR_AIM_FUEL_ROD_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the fuel rod." },
	{ "vr.aim_fuel_rod_right", _config_real, "0.0", "HALO_VR_AIM_FUEL_ROD_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the fuel rod." },
	{ "vr.aim_flamethrower_up", _config_real, "0.0", "HALO_VR_AIM_FLAMETHROWER_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the flamethrower." },
	{ "vr.aim_flamethrower_right", _config_real, "0.0", "HALO_VR_AIM_FLAMETHROWER_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the flamethrower." },
	{ "vr.aim_other_up", _config_real, "0.0", "HALO_VR_AIM_OTHER_UP", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, for the any other gun (custom maps)." },
	{ "vr.aim_other_right", _config_real, "0.0", "HALO_VR_AIM_OTHER_RIGHT", _environment_value, _platform_vr,
		"As vr.aim_pistol_up, to the right (negative: left), for the any other gun (custom maps)." },
	{ "vr.calibration_split_applied", _config_boolean, "false", "HALO_VR_CALIBRATION_SPLIT", _environment_value, _platform_vr,
		"Internal one-time migration (test20c): hand-comfort rotations saved in vr.align_* move to\n"
		"vr.hand_*, so the gun returns to the controller's aim. Roll flips (+-180) stay." },
	{ "vr.gun_anchor", _config_boolean, "true", "HALO_VR_GUN_ANCHOR", _environment_value, _platform_vr,
		"Anchors the held gun to the controller: the gun hand's wrist sits where the empty hand's\n"
		"wrist would, for every weapon, and the gun turns about the hand. false restores the\n"
		"older camera placement (each weapon's animation decides where the gun sits)." },
	{ "vr.gun_forward", _config_real, "0.0", "HALO_VR_GUN_FORWARD", _environment_value, _platform_vr,
		"With vr.gun_anchor: moves the held gun along its barrel, in metres (-0.2..0.2;\n"
		"negative pulls it back toward you)." },
	{ "vr.gun_up", _config_real, "0.0", "HALO_VR_GUN_UP", _environment_value, _platform_vr,
		"As vr.gun_forward, up (negative is down)." },
	{ "vr.gun_out", _config_real, "0.0", "HALO_VR_GUN_OUT", _environment_value, _platform_vr,
		"As vr.gun_forward, away from your body's middle (right for the right hand, left for\n"
		"the left hand; negative is inward)." },
	{ "vr.weapon_offset_right", _config_real, "0.10", "HALO_VR_WEAPON_RIGHT", _environment_value, _platform_vr,
		"Advanced. How far right of the weapon camera the gun hand's grip sits, in metres\n"
		"(placing that camera). With vr.gun_anchor the gun stays in the hand whatever this is; it\n"
		"moves only the point the wall check measures from. Adjust the gun with vr.gun_* instead." },
	{ "vr.weapon_offset_up", _config_real, "-0.12", "HALO_VR_WEAPON_UP", _environment_value, _platform_vr,
		"Advanced; as vr.weapon_offset_right, above the camera (negative is below)." },
	{ "vr.weapon_offset_back", _config_real, "-0.20", "HALO_VR_WEAPON_BACK", _environment_value, _platform_vr,
		"Advanced; as vr.weapon_offset_right, behind the camera (negative is ahead)." },
	{ "vr.snap_turn", _config_real, "0.0", "HALO_VR_SNAP_TURN", _environment_value, _platform_vr,
		"Degrees the right stick turns you at a flick; 0 turns smoothly instead." },
	{ "vr.snap_turn_amount", _config_real, "45.0", "HALO_VR_SNAP_TURN_AMOUNT", _environment_value, _platform_vr,
		"The snap turn angle the VR menu's Comfort page keeps while turning smoothly, and\n"
		"takes up again for Turning: Snap (degrees). vr.snap_turn is what the game turns by." },
	{ "vr.vignette", _config_real, "0.0", "HALO_VR_VIGNETTE", _environment_value, _platform_vr,
		"Comfort vignette: how much the edges of the view darken while you move or turn\n"
		"(0 off, the default; 0.35 low, 0.65 medium, 1 high)." },
	{ "vr.vignette_when", _config_string, "\"move_turn\"", "HALO_VR_VIGNETTE_WHEN", _environment_value, _platform_vr,
		"When the comfort vignette shows: \"move_turn\" (moving or turning by stick, the default),\n"
		"\"turn\" (turning only) or \"always\"." },
	{ "vr.smooth_turn_speed", _config_real, "120.0", "HALO_VR_SMOOTH_TURN_SPEED", _environment_value, _platform_vr,
		"With vr.snap_turn 0: degrees a second the right stick turns you at full\n"
		"push." },
	{ "vr.vehicle_smooth_turn", _config_boolean, "true", "HALO_VR_VEHICLE_SMOOTH_TURN", _environment_value, _platform_vr,
		"Test73, VEHICLE TURN (a tester's request): in a vehicle's seat the right stick turns smoothly\n"
		"(vr.smooth_turn_speed) even when vr.snap_turn snaps on foot, and snaps again once you are out;\n"
		"false: a seat turns as on foot." },
	{ "vr.force_render", _config_boolean, "false", "HALO_VR_FORCE_RENDER", _environment_value, _platform_vr,
		"Draw stereo frames with the headset off (in standby), from a head\n"
		"looking ahead: for testing." },
	{ "vr.diag_yaw", _config_real, "0.0", "HALO_VR_DIAG_YAW", _environment_value, _platform_vr,
		"With vr.force_render: turn that head left by this many degrees." },
	{ "vr.diag_turn_speed", _config_real, "0.0", "HALO_VR_DIAG_TURN_SPEED", _environment_value, _platform_vr,
		"Test73, with vr.force_render: that head keeps turning left, this many degrees a second\n"
		"from its first synthesized frame (testing MENU POSITION)." },
	{ "vr.diag_pitch", _config_real, "0.0", "HALO_VR_DIAG_PITCH", _environment_value, _platform_vr,
		"With vr.force_render: that head looks down by this many degrees (testing)." },
	{ "vr.diag_hand_yaw", _config_real, "0.0", "HALO_VR_DIAG_HAND_YAW", _environment_value, _platform_vr,
		"With vr.force_render: turn the right hand left of the head by this many\n"
		"degrees." },
	{ "vr.diag_two_handed", _config_boolean, "false", "HALO_VR_DIAG_TWO_HANDED", _environment_value, _platform_vr,
		"With vr.force_render: hold the left hand on the gun, ahead and a little\n"
		"left of the right." },
	{ "vr.diag_drive_seconds", _config_real, "0.0", "HALO_VR_DIAG_DRIVE_SECONDS", _environment_value, _platform_vr,
		"This many seconds into play, seat the player as the nearest vehicle's\n"
		"driver: for testing; 0 never." },
	{ "vr.diag_walk_speed", _config_real, "0.0", "HALO_VR_DIAG_WALK_SPEED", _environment_value, _platform_vr,
		"With vr.force_render: the synthetic head walks ahead at this speed\n"
		"(metres a second), for testing vr.roomscale; 0 still." },
	{ "vr.diag_zoom_seconds", _config_real, "0.0", "HALO_VR_DIAG_ZOOM_SECONDS", _environment_value, _platform_vr,
		"This many seconds into play, zoom in (switching first to a weapon that\n"
		"zooms): for testing the scope; 0 never." },
	{ "vr.diag_fire_frame", _config_integer, "0", "HALO_VR_DIAG_FIRE_FRAME", _environment_value, _platform_vr,
		"With vr.force_render: pull the trigger in pulses from this stereo frame on, for\n"
		"muzzle flashes in vr.dump_frame's images: for testing; 0 never." },
	{ "vr.dump_frame", _config_integer, "0", "HALO_VR_DUMP_FRAME", _environment_value, _platform_vr,
		"Write the eyes and HUD of this stereo frame as vr-eye0.bmp, vr-eye1.bmp\n"
		"and vr-hud.bmp in the data folder; 0 none." },
	{ "vr.dump_frames", _config_integer, "1", "HALO_VR_DUMP_FRAMES", _environment_value, _platform_vr,
		"With vr.dump_frame: this many stereo frames from it on, each written as\n"
		"vr-eye0-<frame>.bmp and so on (more than 1): for testing." },
	{ "vr.timing", _config_boolean, "true", "HALO_VR_TIMING", _environment_value, _platform_vr,
		"Log where each frame's time goes every 300 frames ([vr-frame])." },
	{ "vr.timing_gpu", _config_boolean, "false", "HALO_VR_TIMING_GPU", _environment_value, _platform_vr,
		"With vr.timing: wait for the GPU before showing each frame, to time it\n"
		"apart (slows the game)." },
	{ "vr.dump_cinema_frame", _config_integer, "0", "HALO_VR_DUMP_CINEMA_FRAME", _environment_value, _platform_vr,
		"As vr.dump_frame, for this frame of cutscenes on the 3D screen." },
	{ "vr.probe_seconds", _config_real, "0.0", "HALO_VR_PROBE", _environment_value, _platform_vr,
		"Before the game starts, show dim test colours in each eye for this many\n"
		"seconds and log the OpenXR frame rate; 0 skips it." },

	{ "graphics.preset", _config_string, "\"auto\"", "HALO_GRAPHICS_PRESET", _environment_value, _platform_vr,
		"Which effects the headset draws (port/linux/game/vr_graphics.c): \"low\",\n"
		"\"medium\", \"high\", or \"max\" (high, and with vr.resolution_scale 0 a\n"
		"sharper picture than the headset recommends). \"auto\": low on the first\n"
		"Quest, medium on the Quest 2, high otherwise. The settings below override\n"
		"it one effect at a time." },
	{ "graphics.shadows", _config_string, "\"auto\"", "HALO_GRAPHICS_SHADOWS", _environment_value, _platform_vr,
		"Objects' shadows: \"auto\" (as graphics.preset: high), \"on\" or \"off\"." },
	{ "graphics.dynamic_lights", _config_string, "\"auto\"", "HALO_GRAPHICS_DYNAMIC_LIGHTS", _environment_value, _platform_vr,
		"Moving lights on the level (the flashlight, plasma, explosions):\n"
		"\"auto\" (every preset), \"on\" or \"off\"." },
	{ "graphics.specular", _config_string, "\"auto\"", "HALO_GRAPHICS_SPECULAR", _environment_value, _platform_vr,
		"Shine on the level's surfaces: \"auto\" (medium and high), \"on\" or\n"
		"\"off\"." },
	{ "graphics.reflections", _config_string, "\"auto\"", "HALO_GRAPHICS_REFLECTIONS", _environment_value, _platform_vr,
		"The level's reflective surfaces: \"auto\" (high), \"on\" or \"off\"." },
	{ "graphics.bump_mapping", _config_string, "\"auto\"", "HALO_GRAPHICS_BUMP_MAPPING", _environment_value, _platform_vr,
		"Surface relief: \"auto\" (medium and high), \"on\" or \"off\"." },
	{ "graphics.detail_objects", _config_string, "\"auto\"", "HALO_GRAPHICS_DETAIL_OBJECTS", _environment_value, _platform_vr,
		"Grass and small plants: \"auto\" (medium and high), \"on\" or \"off\"." },
	{ "graphics.decals", _config_string, "\"auto\"", "HALO_GRAPHICS_DECALS", _environment_value, _platform_vr,
		"Bullet holes, scorch marks and blood: \"auto\" (every preset), \"on\" or\n"
		"\"off\"." },
	{ "graphics.particles", _config_string, "\"auto\"", "HALO_GRAPHICS_PARTICLES", _environment_value, _platform_vr,
		"Sparks, smoke and other particles: \"auto\" (every preset), \"on\" or\n"
		"\"off\"." },
	{ "graphics.contrails", _config_string, "\"auto\"", "HALO_GRAPHICS_CONTRAILS", _environment_value, _platform_vr,
		"Projectiles' trails: \"auto\" (medium and high), \"on\" or \"off\"." },
	{ "graphics.weather", _config_string, "\"auto\"", "HALO_GRAPHICS_WEATHER", _environment_value, _platform_vr,
		"Rain and snow: \"auto\" (medium and high), \"on\" or \"off\"." },
	{ "graphics.lens_flares", _config_string, "\"auto\"", "HALO_GRAPHICS_LENS_FLARES", _environment_value, _platform_vr,
		"Lens flares and the sun's glow: \"auto\" (high), \"on\" or \"off\"." },
	{ "graphics.fog_screen", _config_string, "\"auto\"", "HALO_GRAPHICS_FOG_SCREEN", _environment_value, _platform_vr,
		"The layered fog drawn across the view: \"auto\" (medium and high),\n"
		"\"on\" or \"off\"." },
	{ "graphics.camouflage_multipass", _config_string, "\"auto\"", "HALO_GRAPHICS_CAMOUFLAGE_MULTIPASS",
		_environment_value, _platform_vr,
		"Active camouflage's finer, costlier look: \"auto\" (medium and high),\n"
		"\"on\" or \"off\"." },
};

#define NUMBER_OF_CONFIG_SETTINGS (sizeof(config_settings) / sizeof(config_settings[0]))

#if defined(HALO_ANDROID) && defined(HALO_VR)
#define CONFIG_PLATFORM (_platform_android | _platform_vr)
#elif defined(_WIN32) && defined(HALO_VR)
/* Test72: the Windows PCVR build (halo-vr.exe): VR settings and Windows ones */
#define CONFIG_PLATFORM (_platform_windows | _platform_vr)
#elif defined(HALO_ANDROID)
#define CONFIG_PLATFORM (_platform_android | _platform_flat)
#elif defined(HALO_WEB)
#define CONFIG_PLATFORM (_platform_desktop | _platform_web | _platform_flat)
#elif defined(_WIN32)
#define CONFIG_PLATFORM (_platform_windows | _platform_flat)
#else
#define CONFIG_PLATFORM (_platform_linux | _platform_flat)
#endif

struct config_value
{
	int boolean;
	long integer;
	double real;
	char *string;
};

static struct config_value config_values[NUMBER_OF_CONFIG_SETTINGS];
/* Test72: values held over settings' own while a mode needs them (VR third
person, vr_third_person.c): read in their place, never written to the
file, given up all at once (config_override_clear) or each as the player
writes that setting (their choice wins) */
static unsigned char config_overridden[NUMBER_OF_CONFIG_SETTINGS];
static struct config_value config_override_values[NUMBER_OF_CONFIG_SETTINGS];
static int config_loaded = 0;
static pthread_mutex_t config_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t active_network_version_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_network_version = HALO_PORT_NETWORK_VERSION;
static int active_network_version_initialized = 0;

/* The active selection is a remote client target, not the local host wire
 * version. Zero means all versions in the upstream declared PvP range. */
static int network_protocol_version_resolve(long configured)
{
	if (configured == 0 || (configured >= HALO_PORT_NETWORK_VERSION_MINIMUM &&
		configured <= HALO_PORT_NETWORK_VERSION_MAXIMUM))
		return (int)configured;
	platform_log("network.protocol_version=%ld is unsupported; using default %d (supported: 0 or %d..%d)",
		configured, HALO_PORT_NETWORK_VERSION, HALO_PORT_NETWORK_VERSION_MINIMUM,
		HALO_PORT_NETWORK_VERSION_MAXIMUM);
	return HALO_PORT_NETWORK_VERSION;
}

static void active_network_version_initialize(void)
{
	long configured = config_integer("network.protocol_version");
	active_network_version = network_protocol_version_resolve(configured);
	if (active_network_version == 0)
		platform_log("network client target: all compatible (%d..%d); local host/listing protocol remains %d; fixed until restart",
			HALO_PORT_NETWORK_VERSION_MINIMUM, HALO_PORT_NETWORK_VERSION_MAXIMUM,
			HALO_PORT_NETWORK_VERSION);
	else
		platform_log("network client target: exact %d; local host/listing protocol remains %d; fixed until restart",
			active_network_version, HALO_PORT_NETWORK_VERSION);
}

int halo_port_active_network_version(void)
{
	int result;
	pthread_mutex_lock(&active_network_version_lock);
	if (!active_network_version_initialized)
	{
		active_network_version_initialize();
		active_network_version_initialized = 1;
	}
	result = active_network_version;
	pthread_mutex_unlock(&active_network_version_lock);
	return result;
}

/* ---------- the file */

static void config_path(char *path, size_t size)
{
#if defined(HALO_ANDROID) || defined(HALO_WEB)
	/* the data folder, which the app names (port/android/host/host_main.c;
	on the web, port/web/src/web_main.c) */
	const char *root = getenv("HALO_DATA_ROOT");

	snprintf(path, size, "%s/config.toml", root && *root ? root : ".");
#else
	/* the executable's folder, with its separator */
	const char *base = SDL_GetBasePath();

#ifdef HALO_VR
	/* Test72: the PCVR game shares the flat game's folder (its maps/: one
	extraction for both) but keeps its own settings */
	snprintf(path, size, "%sconfig-vr.toml", base ? base : "");
#else
	snprintf(path, size, "%sconfig.toml", base ? base : "");
#endif
#endif
}

/* the whole file, NUL terminated, or NULL; free() it */
static char *config_read_file(const char *path, size_t *size)
{
#ifdef HALO_ARM64_GUEST
	FILE *file = fopen(path, "rb");
	char *text = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 && fseek(file, 0, SEEK_SET) == 0)
	{
		text = malloc((size_t)length + 1);
		if (text && fread(text, 1, (size_t)length, file) == (size_t)length)
		{
			text[length] = 0;
			*size = (size_t)length;
		}
		else
		{
			free(text);
			text = NULL;
		}
	}
	fclose(file);
	return text;
#else
	/* SDL's, for UTF-8 paths on Windows */
	void *data = SDL_LoadFile(path, size);
	char *text;

	if (!data)
		return NULL;
	text = malloc(*size + 1);
	if (text)
	{
		memcpy(text, data, *size);
		text[*size] = 0;
	}
	SDL_free(data);
	return text;
#endif
}

static int config_write_file(const char *path, const char *text)
{
#ifdef HALO_ARM64_GUEST
	FILE *file = fopen(path, "wb");
	int written;

	if (!file)
		return 0;
	written = fwrite(text, 1, strlen(text), file) == strlen(text);
	return fclose(file) == 0 && written;
#else
	return SDL_SaveFile(path, text, strlen(text));
#endif
}

struct config_text
{
	char *buffer;
	size_t length, capacity;
};

static void config_append(struct config_text *text, const char *string)
{
	size_t length = strlen(string);

	if (text->length + length + 1 > text->capacity)
	{
		size_t capacity = (text->capacity ? text->capacity : 4096) * 2 + length;
		char *buffer = realloc(text->buffer, capacity);

		if (!buffer)
			return;
		text->buffer = buffer;
		text->capacity = capacity;
	}
	memcpy(text->buffer + text->length, string, length + 1);
	text->length += length;
}

/* the first length characters of text, as a string of their own */
static char *config_copy(const char *text, size_t length)
{
	char *copy = malloc(length + 1);

	if (copy)
	{
		memcpy(copy, text, length);
		copy[length] = 0;
	}
	return copy;
}

static const char *config_default_value(const struct config_setting *setting)
{
    return setting->default_value;
}

/* one setting as the file holds it: its comment, and its key at the
default */
static void config_append_setting(struct config_text *text, const struct config_setting *setting)
{
	const char *dot = strchr(setting->name, '.');
	const char *line;
	char buffer[256];

	config_append(text, "\n");
	for (line = setting->comment; *line;)
	{
		size_t length = strcspn(line, "\n");

		snprintf(buffer, sizeof(buffer), "# %.*s\n", (int)length, line);
		config_append(text, buffer);
		line += length;
		if (*line)
			line++;
	}
#ifndef HALO_ANDROID
	/* (Android apps have no environment to set) */
	switch (setting->environment_style)
	{
	case _environment_value:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=<value>)\n", setting->environment);
		break;
	case _environment_set_is_true:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it true)\n", setting->environment);
		break;
	case _environment_set_is_false:
		snprintf(buffer, sizeof(buffer), "# (for one run: %s=1 makes it false)\n", setting->environment);
		break;
	}
	config_append(text, buffer);
#endif
	snprintf(buffer, sizeof(buffer), "%s = %s\n", dot + 1, config_default_value(setting));
	config_append(text, buffer);
}

/* the file with every setting of this build at its default */
static char *config_default_text(void)
{
	struct config_text text = { NULL, 0, 0 };
	char section[32] = "";
	size_t index;

#ifdef HALO_ANDROID
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them.\n");
#else
	config_append(&text,
		"# Halo settings\n"
		"#\n"
		"# The game writes this file with the defaults when it is missing: delete\n"
		"# it to go back to them. Each setting can also be set for one run with\n"
		"# the environment variable named with it, which wins over this file.\n");
#endif
	/* each section once, where its first setting is, with all of its
	settings: TOML refuses a section twice, and the whole file would go
	unread (config_join_sections) */
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		size_t prior, item, length;
		char buffer[64];
		if (!(setting->platforms & CONFIG_PLATFORM) || !dot) continue;
		length = (size_t)(dot - setting->name);
		for (prior = 0; prior < index; prior++)
		{
			const struct config_setting *other = &config_settings[prior];
			if ((other->platforms & CONFIG_PLATFORM) &&
				!strncmp(other->name, setting->name, length) && other->name[length] == '.') break;
		}
		if (prior < index) continue;
		snprintf(section, sizeof(section), "%.*s", (int)length, setting->name);
		snprintf(buffer, sizeof(buffer), "\n[%s]\n", section);
		config_append(&text, buffer);
		for (item = index; item < NUMBER_OF_CONFIG_SETTINGS; item++)
		{
			const struct config_setting *other = &config_settings[item];
			if ((other->platforms & CONFIG_PLATFORM) &&
				!strncmp(other->name, section, length) && other->name[length] == '.')
				config_append_setting(&text, other);
		}
	}
	return text.buffer;
}

/* the settings of this build that text (the file, parsed as table) lacks,
added to it in their sections, keeping the rest as it is: a newer version's
settings appear in an older file. Returns the new text, or NULL if nothing
was missing */
static char *config_add_missing(const char *text, toml_datum_t table)
{
	char *result = NULL;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		const char *dot = strchr(setting->name, '.');
		const char *current = result ? result : text;
		struct config_text block = { NULL, 0, 0 };
		struct config_text updated = { NULL, 0, 0 };
		char header[40];
		const char *line;
		const char *insert = NULL;

		if (!(setting->platforms & CONFIG_PLATFORM) || !dot || toml_seek(table, setting->name).type != TOML_UNKNOWN)
			continue;
		snprintf(header, sizeof(header), "[%.*s]", (int)(dot - setting->name), setting->name);
		/* the end of the section's last line that is not blank */
		for (line = current; *line; )
		{
			const char *start = line;
			size_t length = strcspn(line, "\n");

			while (*start == ' ' || *start == '\t')
				start++;
			if (insert && *start == '[')
				break;
			if (!insert && !strncmp(start, header, strlen(header)))
				insert = line + length;
			else if (insert && start < line + length && *start != '\r')
				insert = line + length;
			line += length;
			if (*line)
				line++;
		}
		if (insert)
		{
			if (*insert)
				insert++;
			config_append_setting(&block, setting);
		}
		else
		{
			/* no such section: a new one at the end */
			insert = current + strlen(current);
			config_append(&block, current[0] && insert[-1] != '\n' ? "\n\n" : "\n");
			config_append(&block, header);
			config_append(&block, "\n");
			config_append_setting(&block, setting);
		}
		if (!block.buffer)
			continue;
		{
			char *before = config_copy(current, (size_t)(insert - current));

			if (before)
				config_append(&updated, before);
			free(before);
		}
		if (insert > current && insert[-1] != '\n')
			config_append(&updated, "\n");
		config_append(&updated, block.buffer);
		config_append(&updated, insert);
		free(block.buffer);
		if (updated.buffer)
		{
			free(result);
			result = updated.buffer;
			platform_log("settings: added %s (new in this version) at its default", setting->name);
		}
	}
	return result;
}

static int config_line_section(const char *line, const char *end, char *section, size_t size);

/* text from start to end, appended */
static void config_append_range(struct config_text *text, const char *start, const char *end)
{
	char *copy = config_copy(start, (size_t)(end - start));

	if (copy)
		config_append(text, copy);
	free(copy);
}

/* a file with a section twice, which TOML refuses (the defaults of the
versions from 2026-10-10 had a second [display] after [game]): each
section's later parts moved to the end of its first, the rest kept as it
is. Returns the new text, or NULL if no section is there twice */
static char *config_join_sections(const char *text)
{
	enum { PARTS_MAXIMUM = 64 };
	struct
	{
		char name[64];
		const char *start, *end;
	} parts[PARTS_MAXIMUM];
	struct config_text joined = { NULL, 0, 0 };
	const char *line;
	const char *first_part = NULL;
	int count = 0, repeated = 0, index, other;

	/* the parts, each from its section's line to the next one's */
	for (line = text; *line; )
	{
		size_t length = strcspn(line, "\n");
		char name[64];

		if (config_line_section(line, line + length, name, sizeof(name)))
		{
			if (count == PARTS_MAXIMUM)
				return NULL;
			if (count)
				parts[count - 1].end = line;
			else
				first_part = line;
			for (other = 0; other < count; other++)
				repeated |= !strcmp(parts[other].name, name);
			strcpy(parts[count].name, name);
			parts[count].start = line;
			count++;
		}
		line += length;
		if (*line)
			line++;
	}
	if (!repeated)
		return NULL;
	parts[count - 1].end = line;

	config_append_range(&joined, text, first_part);
	for (index = 0; index < count; index++)
	{
		for (other = 0; other < index && strcmp(parts[other].name, parts[index].name); other++)
			;
		if (other < index)
			continue;
		config_append_range(&joined, parts[index].start, parts[index].end);
		for (other = index + 1; other < count; other++)
		{
			const char *body;

			if (strcmp(parts[other].name, parts[index].name))
				continue;
			/* (its settings, without its section's line) */
			body = memchr(parts[other].start, '\n', (size_t)(parts[other].end - parts[other].start));
			if (!body)
				continue;
			if (joined.length && joined.buffer[joined.length - 1] != '\n')
				config_append(&joined, "\n");
			config_append_range(&joined, body + 1, parts[other].end);
		}
	}
	return joined.buffer;
}

/* ---------- values */

static int config_text_is_false(const char *text)
{
	char lower[8];
	size_t index;

	for (index = 0; index + 1 < sizeof(lower) && text[index]; index++)
		lower[index] = (char)tolower((unsigned char)text[index]);
	lower[index] = 0;
	return !strcmp(lower, "0") || !strcmp(lower, "false") || !strcmp(lower, "no") || !strcmp(lower, "off");
}

/* whether a variable that only has to be set (_environment_set_is_true or
_environment_set_is_false) is: empty, "0", "false", "no" or "off" is not */
static int config_environment_set(const char *text)
{
	return text[0] && !config_text_is_false(text);
}

static void config_set_from_text(struct config_value *value, enum config_type type, const char *text)
{
	switch (type)
	{
	case _config_boolean:
		value->boolean = !config_text_is_false(text);
		break;
	case _config_integer:
		value->integer = strtol(text, NULL, 10);
		break;
	case _config_real:
		value->real = strtod(text, NULL);
		break;
	case _config_string:
		free(value->string);
		value->string = strdup(text);
		break;
	}
}

/* the value in the file, if it is there and of the setting's type */
static void config_set_from_file(struct config_value *value, const struct config_setting *setting,
	toml_datum_t table)
{
	toml_datum_t datum = toml_seek(table, setting->name);
	int wrong_type = 0;

	if (datum.type == TOML_UNKNOWN)
		return;
	switch (setting->type)
	{
	case _config_boolean:
		if (datum.type == TOML_BOOLEAN)
			value->boolean = datum.u.boolean;
		else
			wrong_type = 1;
		break;
	case _config_integer:
		if (datum.type == TOML_INT64)
			value->integer = (long)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_real:
		if (datum.type == TOML_FP64)
			value->real = datum.u.fp64;
		else if (datum.type == TOML_INT64)
			value->real = (double)datum.u.int64;
		else
			wrong_type = 1;
		break;
	case _config_string:
		if (datum.type == TOML_STRING)
		{
			free(value->string);
			value->string = strdup(datum.u.s);
		}
		else
		{
			wrong_type = 1;
		}
		break;
	}
	if (wrong_type)
	{
		static const char *const expected[] = { "true or false", "a whole number", "a number", "a quoted string" };

		platform_log("config.toml line %d: %s should be %s; using %s", datum.lineno, setting->name,
			expected[setting->type], config_default_value(setting));
	}
}

static long config_setting_index(const char *name)
{
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		if (!strcmp(config_settings[index].name, name))
			return (long)index;
	}
	return -1;
}

/* a key in the file that is no setting, likely misspelt or in the wrong
section: said in the log, with the section of a setting of that name */
static void config_report_unknown_key(int line, const char *name)
{
	const char *key = strrchr(name, '.');
	const char *found = NULL;
	size_t index, count = 0;

	key = key ? key + 1 : name;
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *dot = strchr(config_settings[index].name, '.');

		if (dot && !strcmp(dot + 1, key))
		{
			found = config_settings[index].name;
			count++;
		}
	}
	/* (only a name one setting has: the section is then sure) */
	if (count == 1)
	{
		platform_log("config.toml line %d: unknown setting %s, ignored (%s goes under [%.*s])", line, name, key,
			(int)(strchr(found, '.') - found), found);
	}
	else
	{
		platform_log("config.toml line %d: unknown setting %s, ignored", line, name);
	}
}

/* keys in the file that are no setting, likely misspelt */
static void config_report_unknown_keys(toml_datum_t table)
{
	int section_index;

	for (section_index = 0; section_index < table.u.tab.size; section_index++)
	{
		toml_datum_t section = table.u.tab.value[section_index];
		int key_index;

		if (section.type != TOML_TABLE)
		{
			config_report_unknown_key(section.lineno, table.u.tab.key[section_index]);
			continue;
		}
		for (key_index = 0; key_index < section.u.tab.size; key_index++)
		{
			char name[128];

			snprintf(name, sizeof(name), "%s.%s", table.u.tab.key[section_index], section.u.tab.key[key_index]);
			/* Test40 exposed this opt-in. Test41 folds its output into vr.haptics;
			 * keep old user config readable without reporting it as a typo. */
			if (config_setting_index(name) < 0 && strcmp(name, "vr.weight_haptics"))
				config_report_unknown_key(section.u.tab.value[key_index].lineno, name);
		}
	}
}

/* the most lines of a file that cannot be read that are skipped to read
the rest */
#define CONFIG_MAXIMUM_SKIPPED_LINES 16

/* the line a parse error is on ("(line 12) ..."), or 0 */
static int config_error_line(const char *message)
{
	const char *at = strstr(message, "(line ");
	int line = 0;

	if (!at || sscanf(at, "(line %d)", &line) != 1)
		return 0;
	return line;
}

/* whether length bytes of text are word, in any case (ASCII) */
static int config_word_is(const char *text, size_t length, const char *word)
{
	size_t index;

	if (strlen(word) != length)
		return 0;
	for (index = 0; index < length; index++)
	{
		if (tolower((unsigned char)text[index]) != word[index])
			return 0;
	}
	return 1;
}

/* the line of text that cannot be read, said in the log with what likely
makes it so, and emptied (its number kept for the lines after it): returns
whether it was there */
static int config_skip_line(char *text, int line_number, const char *message)
{
	char *line = text, *end, *value, *value_end;
	const char *reason = strchr(message, ')');
	char key[64] = "";
	char hint[160] = "";
	int number;

	for (number = 1; number < line_number && *line; number++)
		line += strcspn(line, "\n") + (line[strcspn(line, "\n")] ? 1 : 0);
	if (line_number < 1 || number != line_number || !*line)
		return 0;
	end = line + strcspn(line, "\n");
	reason = reason ? reason + 1 : message;
	while (*reason == ' ')
		reason++;
	/* "key = value": the value as written, to the end of the line or a
	comment */
	value = memchr(line, '=', (size_t)(end - line));
	if (value)
	{
		char *key_start = line, *key_end = value;

		while (key_start < key_end && (*key_start == ' ' || *key_start == '\t'))
			key_start++;
		while (key_end > key_start && (key_end[-1] == ' ' || key_end[-1] == '\t'))
			key_end--;
		snprintf(key, sizeof(key), "%.*s", (int)(key_end - key_start), key_start);
		value++;
		while (value < end && (*value == ' ' || *value == '\t'))
			value++;
		value_end = value + strcspn(value, "#\n");
		if (value_end > end)
			value_end = end;
		while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t' || value_end[-1] == '\r'))
			value_end--;
		if (config_word_is(value, (size_t)(value_end - value), "true") ||
			config_word_is(value, (size_t)(value_end - value), "false"))
		{
			snprintf(hint, sizeof(hint), ": true and false are lowercase, %s = %.*s", key, (int)(value_end - value),
				value_end - value == 4 ? "true" : "false");
		}
		else if (value < value_end && isalpha((unsigned char)*value))
		{
			snprintf(hint, sizeof(hint), ": text needs quotes, %s = \"%.*s\"", key, (int)(value_end - value), value);
		}
	}
	platform_log("config.toml line %d cannot be read (%s), so it is skipped%s", line_number, reason, hint);
	memset(line, ' ', (size_t)(end - line));
	return 1;
}

#if !defined(HALO_ANDROID) && !defined(HALO_WEB)
/* a config.toml the game never reads, likely edited in its place: said in
the log, with the one it reads (path) */
static void config_report_unread(const char *path)
{
	char other[1100];
#ifndef _WIN32
	/* in the folder the game was started from, when that is not the
	executable's (the game's is beside the executable) */
	char folder[1024];

	if (getcwd(folder, sizeof(folder)))
	{
		snprintf(other, sizeof(other), "%s/config.toml", folder);
		if (strcmp(other, path) && SDL_GetPathInfo(other, NULL))
		{
			SDL_PathInfo mine, theirs;

			/* (the same file by another name is the one read) */
			if (!SDL_GetPathInfo(path, &mine) || !SDL_GetPathInfo(other, &theirs) ||
				mine.size != theirs.size || mine.modify_time != theirs.modify_time)
			{
				platform_log("settings: %s is not read; the game's settings are %s", other, path);
			}
		}
	}
#endif
	/* the file saved with .txt after it, by an editor that adds it (and
	Windows Explorer hides it) */
	snprintf(other, sizeof(other), "%s.txt", path);
	if (SDL_GetPathInfo(other, NULL))
	{
#ifdef _WIN32
		platform_log("settings: %s is not read: Windows saved it as config.toml.txt; rename it to config.toml", other);
#else
		platform_log("settings: %s is not read; rename it to config.toml", other);
#endif
	}
}
#endif

/* ---------- words */

/* the settings that take one of a few words: one in any case is taken (as
written here: "Windowed" is "windowed"), and another is said once in the
log */
static const struct
{
	const char *name;
	/* NULL ended; "" where empty is one */
	const char *words[24];
	/* what the game does with another, for the log (NULL: not said) */
	const char *otherwise;
} config_words[] =
{
	{ "display.mode", { "", "fullscreen", "borderless", "windowed", NULL }, "display.fullscreen's" },
	{ "display.resolution_scaling", { "native", "original", NULL }, NULL },
	{ "display.anti_aliasing", { "off", "fxaa", "smaa", "ssaa2x", "msaa2x", "msaa4x", "msaa8x", NULL }, NULL },
	{ "display.hud_button_style", { "classic", "gems", "menu_gems", NULL }, NULL },
	{ "display.menus", { "pc", "xbox", NULL }, NULL },
	{ "display.player_names", { "all", "allies", "enemies", "none", NULL }, NULL },
	{ "display.scoreboard_team_layout", { "teams", "score", NULL }, NULL },
	{ "display.chat_style", { "colors", "white", NULL }, NULL },
	{ "display.shield_glow_intensity", { "default", "light_show", NULL }, NULL },
	{ "display.shield_color", { "default", "white", "black", "red", "blue", "gray", "yellow", "green", "pink",
		"purple", "cyan", "cobalt", "orange", "teal", "sage", "brown", "tan", "maroon", "salmon", NULL }, NULL },
	{ "audio.resampler", { "sinc", "linear", NULL }, NULL },
	{ "vr.character_arms", { "auto", "on", "off", NULL }, NULL },
	{ "audio.voice_chat", { "push_to_talk", "open_mic", "off", NULL }, NULL },
	{ "game.console_log", { "important", "all", "none", NULL }, NULL },
	{ "game.language", { "", "ja", "de", "fr", "es", "it", NULL }, NULL },
	{ "network.text_chat", { "on", "links", "off", NULL }, NULL },
	{ "network.voice_mode", { "off", "team_proximity", "team_enemy_proximity", "team_global",
		"team_global_enemy_proximity", NULL }, NULL },
	{ "network.coop_friendly_fire", { "off", "on", "shields_only", "explosives_only", NULL }, NULL },
	{ "network.coop_enemies_mode", { "none", "per_player", "multiplier", NULL }, NULL },
	{ "streamer.slurs", { "off", "severe", "strong", "mild", NULL }, NULL },
	{ "streamer.swearing", { "off", "severe", "strong", "mild", NULL }, NULL },
	{ "streamer.insults", { "off", "severe", "strong", "mild", NULL }, NULL },
	{ "crash_reports.upload", { "ask", "yes", "no", NULL }, NULL },
};

#define NUMBER_OF_CONFIG_WORDS (sizeof(config_words) / sizeof(config_words[0]))

/* the word settings' values as their readers compare them (lowercase), and
the log told of one that is none of its words */
static void config_check_words(void)
{
	size_t word_index;

	for (word_index = 0; word_index < NUMBER_OF_CONFIG_WORDS; word_index++)
	{
		long index = config_setting_index(config_words[word_index].name);
		const char *const *words = config_words[word_index].words;
		struct config_value *value;
		char list[400] = "";
		size_t word;

		if (index < 0 || !(config_settings[index].platforms & CONFIG_PLATFORM))
			continue;
		value = &config_values[index];
		for (word = 0; words[word] && !config_word_is(value->string, strlen(value->string), words[word]); word++)
		{
		}
		if (words[word])
		{
			if (strcmp(value->string, words[word]))
			{
				free(value->string);
				value->string = strdup(words[word]);
			}
			continue;
		}
		/* ("a", "b" or "c", and "or empty" for "") */
		for (word = 0; words[word]; word++)
		{
			size_t length = strlen(list);

			if (!words[word][0])
				continue;
			snprintf(list + length, sizeof(list) - length, "%s\"%s\"",
				!length ? "" : !words[word + 1] && words[0][0] ? " or " : ", ", words[word]);
		}
		if (!words[0][0])
			snprintf(list + strlen(list), sizeof(list) - strlen(list), " or empty");
		platform_log("settings: %s = \"%s\" is not %s%s%s", config_words[word_index].name, value->string, list,
			config_words[word_index].otherwise ? "; using " : "",
			config_words[word_index].otherwise ? config_words[word_index].otherwise : "");
	}
}

/* (Test72: these line editors serve every build but the web's: the vehicle
defaults' migration and the safe-geometry one, which desktop builds have too) */
#if !defined(HALO_WEB)
#define CONFIG_GEOMETRY_MIGRATION 1
/* Work only on parsed, ordinary section/key lines; preserve all other text.
 * An unusual inline/dotted form is never rewritten by guessing a TOML span. */
#if defined(HALO_ANDROID) && defined(HALO_VR)
#define GEOMETRY_REVISION_KEY "vr_geometry_revision"
#define GEOMETRY_EDITION "VR"
#elif defined(HALO_ANDROID)
#define GEOMETRY_REVISION_KEY "android_geometry_revision"
#define GEOMETRY_EDITION "Android"
#else
/* Windows and Linux, flat and PC VR: their configs held Normal (false) as the
default until Test72 */
#define GEOMETRY_REVISION_KEY "desktop_geometry_revision"
#define GEOMETRY_EDITION "desktop"
#endif
static int config_line_key(const char *, const char *, const char *);
static int config_line_section(const char *, const char *, char *, size_t);
/* Preserve the previous config and replace atomically within its directory.
 * If storage fails, this run is still safe and the migration retries next launch. */
static int config_write_geometry_migration(const char *path, const char *completed, const char *original)
{
    char backup[1100], temporary[1100];
    FILE *existing;
    toml_result_t verified = toml_parse(completed, (int)strlen(completed));
    int valid = verified.ok;
    toml_free(verified);
    if (!valid || !original) return 0;
    snprintf(backup, sizeof(backup), "%s.pre-safe-geometry", path);
    snprintf(temporary, sizeof(temporary), "%s.safe-geometry.tmp", path);
    existing = fopen(backup, "rb");
    if (existing) fclose(existing);
    else {
        /* Exclusive creation proves ownership: an unreadable existing backup
         * must never be truncated or removed after a failed open. */
        FILE *created = fopen(backup, "wbx");
        int written, closed;
        if (!created) return 0;
        written = fwrite(original, 1, strlen(original), created) == strlen(original);
        closed = fclose(created) == 0;
        if (!written || !closed) {
            /* Do not leave a partial new backup that retries would trust. */
            remove(backup);
            return 0;
        }
    }
    if (!config_write_file(temporary, completed)) { remove(temporary); return 0; }
    if (rename(temporary, path) != 0) { remove(temporary); return 0; }
    return 1;
}

static char *config_replace_section_line(const char *text, const char *wanted, const char *key, const char *value, int *found)
{
    struct config_text out = {0};
    char section[64] = "", replacement[128];
    const char *line = text;
    *found = 0;
    snprintf(replacement, sizeof(replacement), "%s = %s\n", key, value);
    while (*line) {
        const char *end = line + strcspn(line, "\n"), *next = *end ? end + 1 : end;
        config_line_section(line, end, section, sizeof(section));
        if (!strcmp(section, wanted) && config_line_key(line, end, key)) {
            config_append(&out, replacement); *found = 1;
        } else {
            char *copy = config_copy(line, next - line);
            if (copy) { config_append(&out, copy); free(copy); }
        }
        line = next;
    }
    return out.buffer;
}
static char *config_replace_geometry_line(const char *text, const char *key, const char *value, int *found)
{
    return config_replace_section_line(text, "renderer", key, value, found);
}

#endif

static void config_load(void)
{
	char path[1024];
	size_t size = 0;
	char *text;
	size_t index;

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const char *default_value = config_default_value(&config_settings[index]);

		if (config_settings[index].type == _config_string)
		{
			/* written as a TOML basic string without escapes */
			size_t length = strlen(default_value);

			config_values[index].string = length >= 2 ? config_copy(default_value + 1, length - 2) : strdup("");
		}
		else
		{
			config_set_from_text(&config_values[index], config_settings[index].type, default_value);
		}
	}

	config_path(path, sizeof(path));
#if !defined(HALO_ANDROID) && !defined(HALO_WEB)
	config_report_unread(path);
#endif
	text = config_read_file(path, &size);
	if (text)
	{
		char *joined = config_join_sections(text);
		toml_result_t result;
		int skipped = 0;

		if (joined)
		{
			free(text);
			text = joined;
			size = strlen(text);
		}
		result = toml_parse(text, (int)size);
		/* (a line that cannot be read is skipped, in memory, so that the
		rest is) */
		while (!result.ok && skipped < CONFIG_MAXIMUM_SKIPPED_LINES &&
			config_skip_line(text, config_error_line(result.errmsg), result.errmsg))
		{
			toml_free(result);
			result = toml_parse(text, (int)strlen(text));
			skipped++;
		}
		if (result.ok)
		{
			char *completed;
#if defined(CONFIG_GEOMETRY_MIGRATION)
            toml_datum_t revision = toml_seek(result.toptab, "renderer." GEOMETRY_REVISION_KEY);
            int migrate = revision.type != TOML_INT64 || revision.u.int64 < 1;
            int can_write = 1;
            char *original = migrate ? strdup(text) : NULL;
            if (migrate) {
                toml_datum_t safe = toml_seek(result.toptab, "renderer.safe_geometry");
                int found;
                char *changed = config_replace_geometry_line(text, "safe_geometry", "true", &found);
                can_write = changed && (found || safe.type == TOML_UNKNOWN);
                if (can_write) {
                    char *with_revision = config_replace_geometry_line(changed, GEOMETRY_REVISION_KEY, "1", &found);
                    free(changed);
                    if (with_revision) {
                        toml_result_t verified = toml_parse(with_revision, (int)strlen(with_revision));
                        if (verified.ok) {
                            free(text); text = with_revision;
                            toml_free(result); result = verified;
                        } else {
                            toml_free(verified); free(with_revision); can_write = 0;
                        }
                    } else can_write = 0;
                } else free(changed);
                platform_log("settings: " GEOMETRY_EDITION " Safe geometry default migration%s; later Safe/Normal choices preserved",
                    can_write ? " applied" : " in memory only (custom config syntax preserved)");
            }
#endif

#if defined(CONFIG_GEOMETRY_MIGRATION)
            /* Test73: game.custom_edition on by default (OpenCE's): a config
            saved before with it off, the old default, is turned on once (no
            menu writes it); its revision, written as a missing setting, keeps
            later choices */
            {
                toml_datum_t ce_revision = toml_seek(result.toptab, "game.custom_edition_revision");

                if (ce_revision.type != TOML_INT64 || ce_revision.u.int64 < 1)
                {
                    int found = 0, applied = 0;
                    char *changed = config_replace_section_line(text, "game", "custom_edition", "true", &found);

                    if (changed && found)
                    {
                        toml_result_t verified = toml_parse(changed, (int)strlen(changed));

                        if (verified.ok)
                        {
                            free(text); text = changed; changed = NULL;
                            toml_free(result); result = verified;
                            applied = 1;
                        }
                        else
                        {
                            toml_free(verified);
                        }
                    }
                    free(changed);
                    platform_log("settings: Custom Edition maps on by default (OpenCE's)%s",
                        applied ? "; the saved setting turned on once, later choices kept" : "");
                }
            }

            /* Test73b: the map upload limits at their most by default (the
            owner's: "raise the download limit to max ... by default"); a
            config saved with one at its default before (OpenCE's 1024 and
            256) has it raised once, a value chosen otherwise kept; the
            revision, written as a missing setting, keeps later choices */
            {
                toml_datum_t share_revision = toml_seek(result.toptab, "network.map_share_revision");

                if (share_revision.type != TOML_INT64 || share_revision.u.int64 < 1)
                {
                    static const struct { const char *key; long before; const char *now; } limits[] =
                    {
                        { "map_share_upload", 1024, "20480" },
                        { "map_seed_upload", 256, "10240" },
                    };
                    int limit, applied = 0;

                    for (limit = 0; limit < (int)(sizeof(limits) / sizeof(limits[0])); limit++)
                    {
                        char name[64];
                        toml_datum_t saved;
                        int found = 0;
                        char *changed;

                        snprintf(name, sizeof(name), "network.%s", limits[limit].key);
                        saved = toml_seek(result.toptab, name);
                        if (saved.type != TOML_INT64 || saved.u.int64 != limits[limit].before)
                            continue;
                        changed = config_replace_section_line(text, "network", limits[limit].key, limits[limit].now,
                            &found);
                        if (changed && found)
                        {
                            toml_result_t verified = toml_parse(changed, (int)strlen(changed));

                            if (verified.ok)
                            {
                                free(text); text = changed; changed = NULL;
                                toml_free(result); result = verified;
                                applied++;
                            }
                            else
                            {
                                toml_free(verified);
                            }
                        }
                        free(changed);
                    }
                    platform_log("settings: map upload limits at their most by default (hosting 20 MB/s, helping "
                        "10 MB/s)%s", applied ? "; the saved defaults raised once, later choices kept" : "");
                }
            }
#endif
			if (joined && !skipped)
			{
				if (config_write_file(path, text))
					platform_log("settings: joined the sections %s had twice", path);
				else
					platform_log("settings: cannot write %s", path);
			}
			for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
				config_set_from_file(&config_values[index], &config_settings[index], result.toptab);
			config_report_unknown_keys(result.toptab);
			platform_log("settings: %s", path);
			/* (a file with lines skipped is left as the player wrote it, to
			be fixed) */
			if (skipped)
			{
				platform_log("settings: %d line%s of config.toml skipped; every other setting in it is used", skipped,
					skipped == 1 ? "" : "s");
#if defined(CONFIG_GEOMETRY_MIGRATION)
				/* (the migration in memory only, as when the file cannot be
				written: it is tried again once the file reads whole) */
				if (migrate) config_values[config_setting_index("renderer.safe_geometry")].boolean = 1;
				free(original);
#endif
			}
			else
			{
#if defined(CONFIG_GEOMETRY_MIGRATION)
            if (migrate) config_values[config_setting_index("renderer.safe_geometry")].boolean = 1;
            completed = can_write ? config_add_missing(text, result.toptab) : NULL;
            if (migrate && can_write && !completed) completed = strdup(text);
#else
            completed = config_add_missing(text, result.toptab);
#endif
#if defined(CONFIG_GEOMETRY_MIGRATION)
            if (completed && !(migrate ? config_write_geometry_migration(path, completed, original) : config_write_file(path, completed)))
                platform_log("settings: cannot write %s; Safe geometry migration will retry", path);
            free(original);
#else
            if (completed && !config_write_file(path, completed))
                platform_log("settings: cannot write %s", path);
#endif
            free(completed);
			}
		}
		else
		{
			platform_log("config.toml cannot be read (%s): every setting is at its default until that is fixed "
				"(a word value needs quotes: menus = \"pc\")", result.errmsg);
		}
		toml_free(result);
		free(text);
	}
	else
	{
		char *defaults = config_default_text();

		if (defaults && config_write_file(path, defaults))
			platform_log("settings: wrote the defaults to %s", path);
		else
			platform_log("settings: cannot write %s; using the defaults", path);
		free(defaults);
	}

	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		const struct config_setting *setting = &config_settings[index];
		/* A NULL environment name means this setting is config-file only. */
		if (!setting->environment)
			continue;
		const char *environment = getenv(setting->environment);

		if (!environment || (setting->environment_style != _environment_value && !config_environment_set(environment)))
			continue;
		switch (setting->environment_style)
		{
		case _environment_value:
			config_set_from_text(&config_values[index], setting->type, environment);
			break;
		case _environment_set_is_true:
			config_values[index].boolean = 1;
			break;
		case _environment_set_is_false:
			config_values[index].boolean = 0;
			break;
		}
	}
	config_check_words();
}

/* Names already looked up, by the address of the name: the game asks for the
same literals every tick (parkour, NPC handling and the other VR hooks ask
for several per biped), and a lookup otherwise compares the name against
every setting. An entry is used only when its name still matches. */
#define CONFIG_LOOKUP_CACHE_SIZE 512
static struct
{
	const char *name;
	long index;
} config_lookup_cache[CONFIG_LOOKUP_CACHE_SIZE];

static const struct config_value *config_value(const char *name, enum config_type type)
{
	static const struct config_value none = { 0, 0, 0.0, "" };
	unsigned long slot = ((unsigned long)(size_t)name >> 2) % CONFIG_LOOKUP_CACHE_SIZE;
	long index;

	pthread_mutex_lock(&config_lock);
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	/* (a name built in a buffer may later hold another name: the entry's
	setting is compared with it again, and unknown names are never kept) */
	if (name && config_lookup_cache[slot].name == name &&
		!strcmp(config_settings[config_lookup_cache[slot].index].name, name))
	{
		index = config_lookup_cache[slot].index;
	}
	else
	{
		index = name ? config_setting_index(name) : -1;
		if (index >= 0)
		{
			config_lookup_cache[slot].name = name;
			config_lookup_cache[slot].index = index;
		}
	}
	pthread_mutex_unlock(&config_lock);
	if (index < 0 || config_settings[index].type != type)
	{
		platform_log("settings: no %s setting %s", type == _config_string ? "string" : "such", name ? name : "(null)");
		return &none;
	}
	if (config_overridden[index])
		return &config_override_values[index];
	return &config_values[index];
}

/* ---------- writing a setting */

/* the line's key, if it is "key = ..." (after spaces), in key */
static int config_line_key(const char *line, const char *end, const char *key)
{
	size_t length = strlen(key);

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if ((size_t)(end - line) <= length || strncmp(line, key, length) != 0)
		return 0;
	line += length;
	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	return line < end && *line == '=';
}

/* the section the line opens, if it is "[section]" (after spaces) */
static int config_line_section(const char *line, const char *end, char *section, size_t size)
{
	const char *close;

	while (line < end && (*line == ' ' || *line == '\t'))
		line++;
	if (line >= end || *line != '[')
		return 0;
	close = memchr(line, ']', (size_t)(end - line));
	if (!close || (size_t)(close - line - 1) >= size)
		return 0;
	memcpy(section, line + 1, (size_t)(close - line - 1));
	section[close - line - 1] = 0;
	return 1;
}

/* sets a setting of the type given, for now and in config.toml: its line
there is changed (or added), the rest of the file kept as it is. `value`
is the setting as text, `written` as the file holds it (a string quoted) */
/* test27: settings written since the start (config_changes; upstream's) */
static volatile unsigned long config_change_count;

static int config_write_typed(const char *name, enum config_type type, const char *value, const char *written_value)
{
	const char *dot = strchr(name, '.');
	long index = config_setting_index(name);
	char section[64], key[64], wanted[80], current[64] = "", line_text[256], path[1024];
	struct config_text out = { 0 };
	size_t size = 0;
	char *text;
	const char *line;
	int written = 0, in_section = 0, succeeded;

	if (index < 0 || config_settings[index].type != type || !dot || (size_t)(dot - name) >= sizeof(section))
		return 0;
	/* (the file read first, as the other settings are) */
	config_value(name, type);
	pthread_mutex_lock(&config_lock);
	snprintf(section, sizeof(section), "%.*s", (int)(dot - name), name);
	snprintf(key, sizeof(key), "%s", dot + 1);
	snprintf(line_text, sizeof(line_text), "%s = %s\n", key, written_value);
	snprintf(wanted, sizeof(wanted), "%s", section);
	config_path(path, sizeof(path));
	text = config_read_file(path, &size);
	for (line = text ? text : ""; *line;)
	{
		const char *end = line + strcspn(line, "\n");
		const char *next = *end ? end + 1 : end;

		if (config_line_section(line, end, current, sizeof(current)))
		{
			/* (leaving the section without the key: it goes at its end) */
			if (in_section && !written)
			{
				config_append(&out, line_text);
				written = 1;
			}
			in_section = !strcmp(current, wanted);
		}
		else if (in_section && !written && config_line_key(line, end, key))
		{
			config_append(&out, line_text);
			written = 1;
			line = next;
			continue;
		}
		{
			char *copy = config_copy(line, (size_t)(next - line));

			if (copy)
			{
				config_append(&out, copy);
				free(copy);
			}
		}
		line = next;
	}
	if (!written)
	{
		if (out.length && out.buffer[out.length - 1] != '\n')
			config_append(&out, "\n");
		if (!in_section)
		{
			char header[80];

			snprintf(header, sizeof(header), "\n[%s]\n", section);
			config_append(&out, header);
		}
		config_append(&out, line_text);
	}
	succeeded = out.buffer && config_write_file(path, out.buffer);
	if (succeeded)
	{
		/* Publish the value and its generation together only after persistence
		succeeds. Failed menu saves must leave all live consumers on the old value. */
		config_set_from_text(&config_values[index], type, value);
		/* Test72: the player's own choice ends a mode's override of it */
		config_overridden[index] = 0;
		config_change_count++;
	}
	pthread_mutex_unlock(&config_lock);
	free(out.buffer);
	free(text);
	return succeeded;
}

int config_write_boolean(const char *name, int value)
{
	return config_write_typed(name, _config_boolean, value ? "true" : "false", value ? "true" : "false");
}

/* Test73 (10d, the owner: VR INTERACTIONS "on by default"): a config saved
before Test73 holds its old default, off: turned on once, as 10h turned the
VR features on (vr_menu_features_on); turned off again afterwards, it stays
off. Every build (a flat host serves VR players too). */
void config_vr_interactions_default(void)
{
	if (config_boolean("net.vr_interactions_applied"))
		return;
	if (config_write_boolean("net.physical_interactions", 1) && config_write_boolean("net.vr_interactions_applied", 1))
		platform_log("network: VR INTERACTIONS (co-op options) turned on, once (Test73's default)");
	else
		platform_log("network: VR INTERACTIONS could not be saved on; this run as the config says (retried next launch)");
}

int config_write_real(const char *name, double value)
{
	char text[64];

	snprintf(text, sizeof(text), "%.6g", value);
	/* (TOML wants a real's point) */
	if (!strpbrk(text, ".eEn"))
		strcat(text, ".0");
	return config_write_typed(name, _config_real, text, text);
}

int config_write_string(const char *name, const char *value)
{
	char quoted[256];

	if (strpbrk(value, "\"\\\n") || strlen(value) + 3 > sizeof(quoted))
		return 0;
	snprintf(quoted, sizeof(quoted), "\"%s\"", value);
	return config_write_typed(name, _config_string, value, quoted);
}

/* ---------- public code */

unsigned long config_changes(void)
{
	return config_change_count;
}

/* Test72: a value held over a setting's own (as the setting's type, from
text) until config_override_clear; 1 on success */
int config_override(const char *name, const char *text)
{
	long index;

	index = name ? config_setting_index(name) : -1;
	if (index < 0 || !text)
		return 0;
	pthread_mutex_lock(&config_lock);
	/* (the file read first, as a setting's first reading does) */
	if (!config_loaded)
	{
		config_load();
		config_loaded = 1;
	}
	config_set_from_text(&config_override_values[index], config_settings[index].type, text);
	config_overridden[index] = 1;
	config_change_count++;
	pthread_mutex_unlock(&config_lock);
	return 1;
}

/* whether a setting's value is a mode's, not its own */
int config_is_overridden(const char *name)
{
	long index = config_setting_index(name);

	return index >= 0 && config_overridden[index];
}

/* every override given up: each setting its own value again */
void config_override_clear(void)
{
	long index;
	int any = 0;

	pthread_mutex_lock(&config_lock);
	for (index = 0; index < NUMBER_OF_CONFIG_SETTINGS; index++)
	{
		if (config_overridden[index])
		{
			config_overridden[index] = 0;
			any = 1;
		}
	}
	if (any)
		config_change_count++;
	pthread_mutex_unlock(&config_lock);
}

int config_boolean(const char *name)
{
	return config_value(name, _config_boolean)->boolean;
}

long config_integer(const char *name)
{
	return config_value(name, _config_integer)->integer;
}

double config_real(const char *name)
{
	return config_value(name, _config_real)->real;
}

/* the table's default for a real setting (reset buttons); 0 when unknown */
double config_default_real(const char *name)
{
	long index = config_setting_index(name);

	return index >= 0 && config_settings[index].type == _config_real ? atof(config_settings[index].default_value) : 0.0;
}

/* test20d: the menu's combined rows. Whether a setting holds a value given
as text, compared as the setting's own type (0 for an unknown name) */
int config_matches(const char *name, const char *text)
{
	long index = config_setting_index(name);
	double difference;

	if (index < 0)
		return 0;
	switch (config_settings[index].type)
	{
	case _config_boolean:
		return config_boolean(name) == !strcmp(text, "true");
	case _config_integer:
		return config_integer(name) == atol(text);
	case _config_real:
		difference = config_real(name) - atof(text);
		return difference > -0.001 && difference < 0.001;
	default:
		return !strcmp(config_string(name), text);
	}
}

/* writes a setting from its value as text, as the setting's own type; 1 on
success (integers are not written) */
int config_write_text(const char *name, const char *text)
{
	long index = config_setting_index(name);

	if (index < 0)
		return 0;
	switch (config_settings[index].type)
	{
	case _config_boolean:
		return config_write_boolean(name, !strcmp(text, "true"));
	case _config_integer:
	{
		char *end, normalized[64];
		long value;
		errno = 0;
		value = strtol(text, &end, 10);
		if (errno == ERANGE || end == text || *end) return 0;
		snprintf(normalized, sizeof(normalized), "%ld", value);
		return config_write_typed(name, _config_integer, normalized, normalized);
	}
	case _config_real:
		return config_write_real(name, atof(text));
	case _config_string:
		return config_write_string(name, text);
	default:
		return 0;
	}
}

const char *config_string(const char *name)
{
	const char *string = config_value(name, _config_string)->string;

	return string ? string : "";
}

#ifdef HALO_VR
void config_vr_vehicle_defaults(void)
{
    char path[1024], backup[1100], temporary[1100];
    char *original, *changed;
    size_t size;
    int ok = 1, found, i;
    const char *keys[] = {"vehicle_view", "vehicle_steering", "vehicle_defaults_applied"};
    const char *values[] = {"\"chase\"", "\"right\"", "true"};
    if (config_boolean("vr.vehicle_defaults_applied")) return;
    pthread_mutex_lock(&config_lock);
    config_path(path, sizeof(path));
    original = config_read_file(path, &size);
    changed = original ? strdup(original) : NULL;
    for (i = 0; changed && i < 3; i++) {
        char *next = config_replace_section_line(changed, "vr", keys[i], values[i], &found);
        free(changed); changed = next;
        if (!found) ok = 0; /* Preserve nonstandard TOML syntax, no guessed edits. */
    }
    if (changed) {
        toml_result_t parsed = toml_parse(changed, (int)strlen(changed));
        ok = ok && parsed.ok; toml_free(parsed);
    } else ok = 0;
    snprintf(backup, sizeof(backup), "%s.pre-vehicle-defaults", path);
    snprintf(temporary, sizeof(temporary), "%s.vehicle-defaults.tmp", path);
    if (ok) {
        FILE *prior = fopen(backup, "rb");
        if (prior) fclose(prior); else ok = config_write_file(backup, original);
    }
    if (ok) {
        ok = config_write_file(temporary, changed) && rename(temporary, path) == 0;
        if (!ok) remove(temporary);
    }
    config_set_from_text(&config_values[config_setting_index("vr.vehicle_view")], _config_string, "chase");
    config_set_from_text(&config_values[config_setting_index("vr.vehicle_steering")], _config_string, "right");
    config_values[config_setting_index("vr.vehicle_defaults_applied")].boolean = ok;
    pthread_mutex_unlock(&config_lock);
    free(changed); free(original);
    platform_log("vr: third-person/right-controller defaults%s; later saved vehicle choices preserved", ok ? " saved" : " in memory only (migration will retry)");
}
#endif


int config_write(const char *name, const char *text)
{
	long index = config_setting_index(name);
	if (index < 0 || !text) return 0;
	if (config_settings[index].type == _config_integer)
	{
		char *end;
		long value;
		errno = 0;
		value = strtol(text, &end, 10);
		char canonical[32];
		if (end == text || *end || errno == ERANGE) return 0;
		snprintf(canonical, sizeof(canonical), "%ld", value);
		return config_write_typed(name, _config_integer, canonical, canonical);
	}
	if (config_settings[index].type == _config_real)
	{
		char *end;
		double value;
		errno = 0;
		value = strtod(text, &end);
		if (end == text || *end || errno == ERANGE || !isfinite(value)) return 0;
		return config_write_real(name, value);
	}
	if (config_settings[index].type == _config_boolean && strcmp(text, "true") && strcmp(text, "false"))
		return 0;
	return config_write_text(name, text);
}

int config_default(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const char *value;
	size_t length;

	if (index < 0)
		return 0;
	value = config_default_value(&config_settings[index]);
	length = strlen(value);
	/* (a string's without its quotes: the defaults have no escapes) */
	if (config_settings[index].type == _config_string && length >= 2 && value[0] == '"')
	{
		value++;
		length -= 2;
	}
	snprintf(text, size, "%.*s", (int)length, value);
	return 1;
}

int config_text(const char *name, char *text, size_t size)
{
	long index = config_setting_index(name);
	const struct config_value *value;

	if (index < 0)
		return 0;
	value = config_value(name, config_settings[index].type);
	switch (config_settings[index].type)
	{
	case _config_boolean:
		snprintf(text, size, "%s", value->boolean ? "true" : "false");
		break;
	case _config_integer:
		snprintf(text, size, "%ld", value->integer);
		break;
	case _config_real:
		snprintf(text, size, "%.15g", value->real);
		break;
	case _config_string:
		snprintf(text, size, "%s", value->string ? value->string : "");
		break;
	}
	return 1;
}

int halo_fixed_step(void)
{
	static int fixed_step = -1;

	if (fixed_step < 0)
		fixed_step = config_boolean("debug.fixed_step") != 0;
	return fixed_step;
}

void config_folder(char *path, size_t size)
{
	char file[1024];
	char *separator;

	config_path(file, sizeof(file));
	separator = strrchr(file, '/');
#ifndef HALO_ANDROID
	if (!separator || (strrchr(file, '\\') && strrchr(file, '\\') > separator))
		separator = strrchr(file, '\\');
#endif
	if (separator)
		separator[1] = 0;
	else
		file[0] = 0;
	snprintf(path, size, "%s", file);
}

char *config_file_read(const char *path, size_t *size)
{
	return config_read_file(path, size);
}
