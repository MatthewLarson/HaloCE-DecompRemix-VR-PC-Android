/*
VR_MENU.C

Quest VR settings are native DeLa widgets built from verified definitions,
using the current map's UI font and the existing setting callbacks. OpenCE's
main menu and Settings hub expose them; campaign pause keeps a direct entry,
and stock multiplayer menus get a fallback entry when OpenCE is unavailable.
The settings, four-row columns, paging, laser hit tests and immediate config
persistence remain shared across all entry paths. No solo-pause templates or
fixed child counts are required to create a functioning screen.
*/

#ifdef HALO_VR

#include "cseries.h"
#include "math/integer_math.h"
#include "math/real_math.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"

#include "halo_vr.h"
#include "../src/vr.h"
#include "../src/vr_scope_profiles.h"
#include "../src/port_config.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* port/linux/src/platform.h (a variadic call needs its prototype in scope
on the Android guest's ABI) */
void platform_log(const char *format, ...);
/* source/cache/cache_files.c */
long cache_file_add_tag(unsigned long group_tag, unsigned long parent_group_tag, char *name, void *base_address);
char const *ui_widget_event_handler_function_name(long function_index);

/* ---------- the widget definition tag ('DeLa'), as ui_widget.c views it */

#define VR_MENU_WIDGET_TAG 0x44654C61 /* 'DeLa' */

struct vr_menu_event_handler
{
	long flags;
	short event_type;
	short function;
	struct tag_reference widget_tag;
	struct tag_reference sound_effect;
	char script[32];
};

struct vr_menu_child
{
	struct tag_reference widget_tag;
	char name[32];
	long flags;
	short custom_controller_index;
	short vertical_offset;
	short horizontal_offset;
	byte unknown03A[0x50 - 0x3A];
};

struct vr_menu_game_data_input
{
	short function;
	byte unknown002[0x24 - 0x02];
};

struct vr_menu_widget
{
	short type;
	short controller_index;
	char name[32];
	rectangle2d bounds;
	long flags;
	long milliseconds_to_auto_close;
	long auto_close_fade_time;
	struct tag_reference background_bitmap;
	struct tag_block game_data_inputs;
	struct tag_block event_handlers;
	struct tag_block search_and_replace_functions;
	byte unknown06C[0xEC - 0x6C];
	struct tag_reference text_label_string_list;
	struct tag_reference text_font;
	real_argb_color text_color;
	short justification;
	word text_box_flags;
	byte unknown120[0x12E - 0x120];
	short string_list_index;
	short horizontal_offset, vertical_offset;
	byte unknown134[0x150 - 0x134];
	long list_flags;
	struct tag_reference list_header_bitmap, list_footer_bitmap;
	rectangle2d list_header_bounds, list_footer_bounds;
	byte unknown184[0x1A4 - 0x184];
	struct tag_reference extended_description_widget;
	byte unknown1B4[0x2D4 - 0x1B4];
	struct tag_block conditional_widgets;
	byte unknown2E0[0x3E0 - 0x2E0];
	struct tag_block child_widgets;
};

typedef char vr_menu_event_handler_size[sizeof(struct vr_menu_event_handler) == 0x48 ? 1 : -1];
typedef char vr_menu_child_size[sizeof(struct vr_menu_child) == 0x50 ? 1 : -1];
typedef char vr_menu_game_data_input_size[sizeof(struct vr_menu_game_data_input) == 0x24 ? 1 : -1];
typedef char vr_menu_widget_size[sizeof(struct vr_menu_widget) == 0x3EC ? 1 : -1];
typedef char vr_menu_widget_bounds[offsetof(struct vr_menu_widget, bounds) == 0x24 ? 1 : -1];
typedef char vr_menu_widget_game_data_inputs[offsetof(struct vr_menu_widget, game_data_inputs) == 0x48 ? 1 : -1];
typedef char vr_menu_widget_event_handlers[offsetof(struct vr_menu_widget, event_handlers) == 0x54 ? 1 : -1];
typedef char vr_menu_widget_text[offsetof(struct vr_menu_widget, text_label_string_list) == 0xEC ? 1 : -1];
typedef char vr_menu_widget_font[offsetof(struct vr_menu_widget, text_font) == 0xFC ? 1 : -1];
typedef char vr_menu_widget_list_flags[offsetof(struct vr_menu_widget, list_flags) == 0x150 ? 1 : -1];

/* event handler flags and events (ui_widget.c) */
#define VR_MENU_CLOSE_CURRENT 0x1
#define VR_MENU_CLOSE_ALL 0x4
#define VR_MENU_OPEN_WIDGET 0x8
#define VR_MENU_RUN_FUNCTION 0x80
#define VR_MENU_EVENT_A 0
#define VR_MENU_EVENT_B 1
#define VR_MENU_EVENT_DPAD_LEFT 10
#define VR_MENU_EVENT_DPAD_RIGHT 11
#define VR_MENU_EVENT_START 12
#define VR_MENU_EVENT_BACK 13

/* ---------- the settings */

enum
{
	_vr_setting_boolean,
	_vr_setting_real,
	_vr_setting_real_choice,
	_vr_setting_string,
	_vr_setting_degrees,
	_vr_setting_centimetres,
	_vr_setting_scope_centimetres,
	_vr_setting_vehicle_centimetres,
	_vr_setting_reset_alignment,
	_vr_setting_flip_alignment,
	/* test20c: restore a hand's orientation (key "left"/"right") or the gun's
	angle and place (key "weapon") to their built-in defaults */
	_vr_setting_reset_hand,
	_vr_setting_reset_weapon,
	/* test20d: a row that sets several settings at once, each value
	"key=value;key=value" (vr_menu_multi); both hands' angle on one axis
	(key "pitch", "yaw" or "roll"; the left hand mirrored); handedness,
	which also mirrors the hand-specific defaults */
	_vr_setting_multi,
	_vr_setting_hand_degrees,
	_vr_setting_handedness,
	/* test21: the held gun's own aim adjustment (vr.aim_<kind>_up/_right):
	key "gun" names the gun, "up" and "right" step half degrees, "reset"
	clears both for that gun */
	_vr_setting_gun_aim,
	/* test22: the pistol's and sniper rifle's scope places and sizes back
	to their defaults */
	_vr_setting_reset_scopes,
	/* test23: a Quest button for an action (key vr.button_*, values
	VR_BUTTON_SOURCE_*); a button another action has is swapped to it, and
	all of them back to their defaults */
	_vr_setting_button,
	_vr_setting_reset_buttons,
	/* test24b: comfort turning, finer: smooth or snap (vr.snap_turn 0 or
	the snap angle kept in vr.snap_turn_amount), and the snap angle itself
	(vr.snap_turn_amount, and vr.snap_turn while snapping) */
	_vr_setting_turn_mode,
	_vr_setting_snap_angle,
	/* test25: every vehicle seat offset (vr.vehicle_*_up/_forward/_right)
	back to its default; view, horizon and steering kept */
	_vr_setting_reset_vehicle_offsets,
	/* test26: what the left stick's click does: shows or hides the reticle
	(the 1.0.8 default; crouch on the turning stick held down) or crouches,
	as before 1.0.8 (values "reticle", "crouch"; vr_menu_button_set) */
	_vr_setting_crouch_click,
	/* test29: the HUD shown or hidden now (vr_set_hud_hidden: the session's,
	as the head tap's; values "true" shown), and the wrist HUD's place and
	size back to their defaults */
	_vr_setting_hud_shown,
	_vr_setting_reset_wrist,
};

#define VR_MENU_MAXIMUM_VALUES 12
/* test25: a settings row's width: its column's (the right column starts
256 across), less a gap */
#define VR_MENU_BUTTON_WIDTH 250

/* the right stick's turning, on CONTROLS and COMFORT (test24b: snap 22.5
and 60 degrees added) */
#define VR_MENU_TURNING_ROW { "TURNING", "vr.snap_turn", _vr_setting_multi, 12, { { "SMOOTH 60", "vr.snap_turn=0;vr.smooth_turn_speed=60" }, { "SMOOTH 90", "vr.snap_turn=0;vr.smooth_turn_speed=90" }, { "SMOOTH 120", "vr.snap_turn=0;vr.smooth_turn_speed=120" }, { "SMOOTH 150", "vr.snap_turn=0;vr.smooth_turn_speed=150" }, { "SMOOTH 180", "vr.snap_turn=0;vr.smooth_turn_speed=180" }, { "SMOOTH 240", "vr.snap_turn=0;vr.smooth_turn_speed=240" }, { "SMOOTH 300", "vr.snap_turn=0;vr.smooth_turn_speed=300" }, { "SNAP 22.5", "vr.snap_turn=22.5;vr.snap_turn_amount=22.5" }, { "SNAP 30", "vr.snap_turn=30;vr.snap_turn_amount=30" }, { "SNAP 45", "vr.snap_turn=45;vr.snap_turn_amount=45" }, { "SNAP 60", "vr.snap_turn=60;vr.snap_turn_amount=60" }, { "SNAP 90", "vr.snap_turn=90;vr.snap_turn_amount=90" } } }

struct vr_menu_setting
{
	char const *label, *key;
	short type, value_count;
	struct
	{
		char const *label, *value;
	} values[VR_MENU_MAXIMUM_VALUES];
};

/* a page's screens as named topics (each at most VR_MENU_SETTINGS_PER_SCREEN
rows); pages without sections are cut into screens of eight */
struct vr_menu_section
{
	char const *label;
	long count;
};

/* an effect's setting (vr_graphics.c): as the preset, or on or off */
#define VR_MENU_EFFECT(label, key) \
	{ label, key, _vr_setting_string, 3, { { "AUTO", "auto" }, { "ON", "on" }, { "OFF", "off" } } }

/* Test53: the immersion features' rows, each defined once and shown both
on its own page and on IMMERSION, which gathers them so they can be set in
one place (the same setting either way: changing one shows on the other) */
/* Test73 (10i, a tester's request; the owner: a toggle, on by default):
VEHICLE TURN: a seat turns smoothly whatever TURNING is, on foot snaps
again (vr_frame.c turn); on COMFORT beside TURNING and in VEHICLES >
VEHICLE OPTIONS */
#define VR_COMFORT_VEHICLE_TURN { "VEHICLE TURN", "vr.vehicle_smooth_turn", _vr_setting_boolean, 2, { { "AS SET", "false" }, { "SMOOTH", "true" } } }
#define VR_IMMERSION_COMBAT { "FIST COMBAT", "vr.assisted_combat", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_COMBAT_MOVES { "COMBAT MOVES", "vr.combat_moves", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_PROJECTILE_PARRY { "DEFLECT SHOTS", "vr.projectile_parry", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test72: the deflection's contact and the returned shot's speed */
#define VR_IMMERSION_DEFLECT_ASSIST { "DEFLECT AID", "vr.deflect_assist", _vr_setting_real, 3, { { "OFF", "0" }, { "NORMAL", "0.03" }, { "WIDE", "0.06" } } }
#define VR_IMMERSION_DEFLECT_STRENGTH { "DEFLECT FORCE", "vr.deflect_strength", _vr_setting_real, 3, { { "75%", "0.75" }, { "100%", "1.0" }, { "125%", "1.25" } } }
#define VR_IMMERSION_COMBAT_LIGHTS { "HIT HIGHLIGHTS", "vr.combat_highlights", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test72: the combo as a Halo-styled counter */
#define VR_IMMERSION_COMBAT_COMBO { "COMBO DISPLAY", "vr.combat_combo_display", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_COMBAT_REACH { "FIST REACH", "vr.combat_reach", _vr_setting_real, 5, { { "1 M", "1" }, { "1.5 M", "1.5" }, { "2 M", "2" }, { "2.5 M", "2.5" }, { "3 M", "3" } } }
/* Test65: Arkham Shadow's fist combat: how far a punch carries the player to
an enemy, how many more punches enemies take, and counters */
/* Test72: up to 15 m, 12 by default (the owner's 8 m, the old maximum, was too short) */
#define VR_IMMERSION_COMBAT_LUNGE { "LUNGE", "vr.combat_lunge_reach", _vr_setting_real, 7, { { "OFF", "0" }, { "4 M", "4" }, { "6 M", "6" }, { "8 M", "8" }, { "10 M", "10" }, { "12 M", "12" }, { "15 M", "15" } } }
/* Test70: HIT ASSIST and LUNGE HOP (FIST COMBAT) */
/* Test72: HIT ASSIST up to 70 cm, 50 by default; LUNGE HOP's NORMAL 30 cm */
#define VR_IMMERSION_COMBAT_HIT_ASSIST { "HIT ASSIST", "vr.combat_hit_assist", _vr_setting_real, 6, { { "OFF", "0" }, { "20 CM", "0.20" }, { "35 CM", "0.35" }, { "50 CM", "0.50" }, { "60 CM", "0.60" }, { "70 CM", "0.70" } } }
#define VR_IMMERSION_COMBAT_HOP { "LUNGE HOP", "vr.combat_lunge_hop", _vr_setting_real, 4, { { "OFF", "0" }, { "LOW", "0.15" }, { "NORMAL", "0.30" }, { "HIGH", "0.45" } } }
#define VR_IMMERSION_COMBAT_HEALTH { "ENEMY HEALTH", "vr.combat_enemy_health", _vr_setting_real, 5, { { "1X", "1" }, { "2X", "2" }, { "3X", "3" }, { "4X", "4" }, { "6X", "6" } } }
/* Test65: WEAPON PULL: a weapon within reach glows and a grip pulls it into
the hand */
/* Test66: WEAPON PULL as Half-Life 2 VR's and Alyx's gravity gloves: on or
 * off, how far, and flicked or held (the owner's Test65 session: it never
 * pulled, and had no switch) */
#define VR_IMMERSION_WEAPON_PULL { "WEAPON PULL", "vr.weapon_pull", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_PULL_REACH { "PULL REACH", "vr.weapon_pull_reach", _vr_setting_real, 5, { { "1 M", "1" }, { "2 M", "2" }, { "3 M", "3" }, { "4 M", "4" }, { "5 M", "5" } } }
#define VR_IMMERSION_PULL_STYLE { "PULL STYLE", "vr.weapon_pull_style", _vr_setting_string, 2, { { "FLICK", "flick" }, { "GRIP", "grip" } } }
#define VR_IMMERSION_PULL_GLOW { "PULL GLOW", "vr.weapon_pull_glow", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_COMBAT_COUNTERS { "COUNTERS", "vr.combat_counters", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_AIM_STEADY { "AIM STEADY", "vr.aim_steadying", _vr_setting_real, 5, { { "OFF", "0" }, { "LIGHT", "0.2" }, { "MEDIUM", "0.4" }, { "STRONG", "0.6" }, { "MAX", "0.8" } } }
#define VR_IMMERSION_GRENADES { "PHYS GRENADES", "vr.physical_grenades", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test66: the grenades at the waist (the owner: "match them to the waist
 * area"), measured from the neck so a glance down no longer moves them */
#define VR_IMMERSION_GRENADE_FWD { "GRENADE FWD", "vr.grenade_chest_forward", _vr_setting_real, 7, { { "8 CM", "0.08" }, { "12 CM", "0.12" }, { "16 CM", "0.16" }, { "20 CM", "0.20" }, { "25 CM", "0.25" }, { "30 CM", "0.30" }, { "40 CM", "0.40" } } }
#define VR_IMMERSION_GRENADE_DOWN { "GRENADE DOWN", "vr.grenade_chest_down", _vr_setting_real, 8, { { "30 CM", "0.30" }, { "40 CM", "0.40" }, { "50 CM", "0.50" }, { "55 CM", "0.55" }, { "60 CM", "0.60" }, { "65 CM", "0.65" }, { "70 CM", "0.70" }, { "80 CM", "0.80" } } }
#define VR_IMMERSION_GRENADE_SPREAD { "GRENADE GAP", "vr.grenade_chest_spread", _vr_setting_real, 6, { { "6 CM", "0.06" }, { "10 CM", "0.10" }, { "14 CM", "0.14" }, { "18 CM", "0.18" }, { "24 CM", "0.24" }, { "30 CM", "0.30" } } }
#define VR_IMMERSION_GRENADE_POWER { "THROW POWER", "vr.grenade_throw_scale", _vr_setting_real, 5, { { "0.5X", "0.5" }, { "0.75X", "0.75" }, { "1X", "1" }, { "1.5X", "1.5" }, { "2X", "2" } } }
/* Test73 (the owner: the grenades at their true size): the waist's and the hand's, as thrown */
#define VR_IMMERSION_GRENADE_SIZE { "GRENADE SIZE", "vr.grenade_scale", _vr_setting_real, 6, { { "50%", "0.5" }, { "75%", "0.75" }, { "90%", "0.9" }, { "100%", "1" }, { "125%", "1.25" }, { "150%", "1.5" } } }
#define VR_IMMERSION_WEAPONS { "WEAPONS", "vr.weapons", _vr_setting_multi, 3, { { "LOCKED", "vr.weapons=locked" }, { "PHYSICAL", "vr.weapons=physical;vr.physical_multiplayer=false" }, { "PHYSICAL + MP", "vr.weapons=physical;vr.physical_multiplayer=true" } } }
#define VR_IMMERSION_HOLSTERS { "HOLSTERS", "vr.holsters", _vr_setting_multi, 7, { { "OFF", "vr.holsters=false" }, { "10 CM", "vr.holsters=true;vr.holster_size=0.1" }, { "15 CM", "vr.holsters=true;vr.holster_size=0.15" }, { "20 CM", "vr.holsters=true;vr.holster_size=0.2" }, { "25 CM", "vr.holsters=true;vr.holster_size=0.25" }, { "30 CM", "vr.holsters=true;vr.holster_size=0.3" }, { "40 CM", "vr.holsters=true;vr.holster_size=0.4" } } }
#define VR_IMMERSION_HOLSTER_HAPTICS { "HOLSTER HAPTICS", "vr.holster_haptics", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_MELEE { "MELEE", "vr.melee", _vr_setting_multi, 4, { { "IMPACT", "vr.melee=impact;vr.melee_multiplayer=false" }, { "SWING", "vr.melee=swing;vr.melee_multiplayer=false" }, { "IMPACT + ONLINE", "vr.melee=impact;vr.melee_multiplayer=true" }, { "SWING + ONLINE", "vr.melee=swing;vr.melee_multiplayer=true" } } }
#define VR_IMMERSION_MELEE_SPEED { "MELEE SPEED", "vr.melee_speed", _vr_setting_real, 12, { { "OFF", "0" }, { "1.0", "1.0" }, { "1.2", "1.2" }, { "1.4", "1.4" }, { "1.6", "1.6" }, { "1.8", "1.8" }, { "2.0", "2.0" }, { "2.3", "2.3" }, { "2.6", "2.6" }, { "2.9", "2.9" }, { "3.2", "3.2" }, { "3.6", "3.6" } } }
#define VR_IMMERSION_FIST_REQUIRED { "FIST REQUIRED", "vr.melee_requires_fist", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_ARM_RUN { "ARM RUN", "vr.arm_run", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test72: LOCOMOTION's rows, shared with IMMERSION (the owner: "missing toggles
that should also be in the immersion section"; "Bullet Time is not a haptics
feature"). BULLET TIME's own button stays on BUTTONS, with the other buttons'
remapping (conflicts, NONE, the saved binding). */
/* Test72: TWO HANDS (CONTROLS), SCOPE (GAMEPLAY), the vehicles' VIEW (VEHICLES), also on IMMERSION */
#define VR_IMMERSION_TWO_HANDS { "TWO HANDS", "vr.two_handed", _vr_setting_string, 3, { { "AUTO LOCK", "auto" }, { "SQUEEZE", "grip" }, { "OFF", "off" } } }
#define VR_IMMERSION_SCOPE { "SCOPE", "vr.scope", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_VEHICLE_VIEW { "VIEW", "vr.vehicle_view", _vr_setting_string, 2, { { "THIRD PERSON", "chase" }, { "FIRST PERSON", "first_person" } } }
#define VR_IMMERSION_BUTTON_SPRINT { "BUTTON SPRINT", "vr.button_sprint", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_SPRINT_TOGGLE { "SPRINT TOGGLE", "vr.button_sprint_toggle", _vr_setting_boolean, 2, { { "HOLD", "false" }, { "TOGGLE", "true" } } }
#define VR_IMMERSION_SLIDE { "SLIDE", "vr.slide", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_SLIDE_DISTANCE { "SLIDE DISTANCE", "vr.slide_momentum", _vr_setting_real, 5, { { "0.5X", "0.5" }, { "0.75X", "0.75" }, { "1X", "1" }, { "1.5X", "1.5" }, { "2X", "2" } } }
#define VR_IMMERSION_SLIDE_FRICTION { "SLIDE FRICTION", "vr.slide_friction", _vr_setting_real, 3, { { "LOW", "0.6" }, { "MID", "1" }, { "HIGH", "1.5" } } }
#define VR_IMMERSION_SLIDE_IMPACT { "SLIDE IMPACT", "vr.slide_damage", _vr_setting_real, 4, { { "OFF", "0" }, { "LIGHT", "0.5" }, { "MID", "1" }, { "HEAVY", "2" } } }
#define VR_IMMERSION_CROUCH_SLIDES { "CROUCH SLIDES", "vr.slide_physical_crouch", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_JUMP_CANCELS { "JUMP CANCELS", "vr.slide_jump_cancel", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_BULLET_TIME { "BULLET TIME", "vr.bullet_time", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_BULLET_SCALE { "TIME SCALE", "vr.bullet_time_scale", _vr_setting_real, 4, { { "25%", "0.25" }, { "40%", "0.4" }, { "50%", "0.5" }, { "65%", "0.65" } } }
/* (BULLET TIME KEY before: hold or toggle, not its button) */
#define VR_IMMERSION_BULLET_MODE { "BULLET TIME MODE", "vr.bullet_time_toggle", _vr_setting_boolean, 2, { { "HOLD", "false" }, { "TOGGLE", "true" } } }
#define VR_IMMERSION_MANUAL_RELOAD { "MANUAL RELOAD", "vr.manual_reload", _vr_setting_multi, 2, { { "OFF", "vr.manual_reload=false;vr.dropped_magazines=false" }, { "ON", "vr.manual_reload=true" } } }
#define VR_IMMERSION_RELOAD_INPUT { "RELOAD INPUT", "vr.reload_input", _vr_setting_string, 2, { { "GRIP", "grip" }, { "TRIGGER", "trigger" } } }
#define VR_IMMERSION_SHOULDER_POUCH { "SHOULDER POUCH", "vr.reload_shoulder", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_HIP_POUCH { "HIP POUCH", "vr.reload_hip", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_MAG_DEBRIS { "MAG DEBRIS", "vr.dropped_magazines", _vr_setting_multi, 2, { { "OFF", "vr.dropped_magazines=false" }, { "ON", "vr.dropped_magazines=true;vr.manual_reload=true" } } }
#define VR_IMMERSION_WALLRUN { "WALLRUN", "vr.parkour_wallrun", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_WALLJUMP { "WALLJUMP", "vr.parkour_walljump", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_GRIP_CLIMB { "GRIP CLIMB", "vr.parkour_climb", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_WALL_SPEED { "WALL SPEED", "vr.wallrun_speed", _vr_setting_real, 5, { { "1X", "1" }, { "1.25X", "1.25" }, { "1.5X", "1.5" }, { "1.75X", "1.75" }, { "2X", "2" } } }
#define VR_IMMERSION_WALL_TIME { "WALL TIME", "vr.wallrun_duration", _vr_setting_real, 6, { { "0.5 S", "0.5" }, { "1 S", "1" }, { "1.5 S", "1.5" }, { "2 S", "2" }, { "2.5 S", "2.5" }, { "3 S", "3" } } }
#define VR_IMMERSION_WALL_LEAP { "WALL LEAP", "vr.walljump_scale", _vr_setting_real, 5, { { "0.5X", "0.5" }, { "0.75X", "0.75" }, { "1X", "1" }, { "1.25X", "1.25" }, { "1.5X", "1.5" } } }
#define VR_IMMERSION_CLIMB_LAUNCH { "CLIMB LAUNCH", "vr.climb_launch_scale", _vr_setting_real, 5, { { "OFF", "0" }, { "0.5X", "0.5" }, { "0.75X", "0.75" }, { "1X", "1" }, { "1.5X", "1.5" } } }
#define VR_IMMERSION_WORLD_GRAB { "WORLD GRAB", "vr.world_interaction", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_HAND_CONTACT { "HAND CONTACT", "vr.interaction_reach", _vr_setting_real, 4, { { "2 CM", "0.02" }, { "4 CM", "0.04" }, { "6 CM", "0.06" }, { "8 CM", "0.08" } } }
#define VR_IMMERSION_LIFT_VEHICLES { "LIFT VEHICLES", "vr.vehicle_lift", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_CUSTOM_SCENERY { "CUSTOM SCENERY", "vr.custom_scenery_grab", _vr_setting_multi, 2, { { "OFF", "vr.custom_scenery_grab=false" }, { "ON", "vr.custom_scenery_grab=true;vr.world_interaction=true" } } }
#define VR_IMMERSION_NPC_GRAB { "NPC GRAB", "vr.npc_interaction", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_NPC_RIG_SLOTS { "NPC RIG SLOTS", "vr.npc_max_ragdolls", _vr_setting_multi, 6, { { "4", "vr.npc_max_ragdolls=4" }, { "8", "vr.npc_max_ragdolls=8" }, { "12", "vr.npc_max_ragdolls=12" }, { "16", "vr.npc_max_ragdolls=16" }, { "20", "vr.npc_max_ragdolls=20" }, { "24", "vr.npc_max_ragdolls=24" } } }
#define VR_IMMERSION_CUSTOM_NPC_RIGS { "CUSTOM NPC RIGS", "vr.custom_npc_rigs", _vr_setting_multi, 2, { { "OFF", "vr.custom_npc_rigs=false" }, { "ON", "vr.custom_npc_rigs=true;vr.npc_interaction=true" } } }
#define VR_IMMERSION_NPC_RESCUE { "NPC RESCUE", "vr.npc_rescue", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test56: the longest a living NPC lies limp after a hand lets it go (never over 1 s) */
#define VR_IMMERSION_NPC_RECOVERY { "NPC RECOVERY", "vr.npc_recovery", _vr_setting_real, 4, { { "0.25 S", "0.25" }, { "0.5 S", "0.5" }, { "0.75 S", "0.75" }, { "1 S", "1.0" } } }
#define VR_IMMERSION_BODY_RETENTION { "BODY RETENTION", "vr.ragdoll_retention", _vr_setting_multi, 3, { { "STOCK", "vr.ragdoll_retention=0" }, { "75", "vr.ragdoll_retention=75" }, { "100", "vr.ragdoll_retention=100" } } }
#define VR_IMMERSION_IMPACT_DAMAGE { "IMPACT DAMAGE", "vr.interaction_damage", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_IMPACT_SPEED { "IMPACT SPEED", "vr.impact_threshold", _vr_setting_real, 4, { { "1 M/S", "1" }, { "2 M/S", "2" }, { "3 M/S", "3" }, { "4 M/S", "4" } } }
#define VR_IMMERSION_IMPACT_CAP { "IMPACT CAP", "vr.impact_damage_cap", _vr_setting_real, 4, { { "25%", "0.25" }, { "50%", "0.5" }, { "100%", "1" }, { "150%", "1.5" } } }
#define VR_IMMERSION_SHAKE_COOLING { "SHAKE COOLING", "vr.cooldown_shake", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_CLOSE_CONTACT { "CLOSE CONTACT", "vr.close_contact", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_PHYSICAL_HANDLES { "IMMERSIVE VEHICLE", "vr.vehicle_handles", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test66: the immersive vehicle rows, on VEHICLES and on IMMERSION's vehicle topics */
#define VR_IMMERSION_VIRTUAL_WHEEL { "VIRTUAL WHEEL", "vr.vehicle_wheel_visible", _vr_setting_boolean, 2, { { "HIDDEN", "false" }, { "VISIBLE", "true" } } }
#define VR_IMMERSION_COCKPIT_LOCK { "COCKPIT LOCK", "vr.vehicle_cockpit_lock", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_GRIP_GUIDE { "GRIP GUIDE", "vr.vehicle_grip_guide", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test73 (a tester's request: "the hands locked onto the steering wheels when holding grip") */
#define VR_IMMERSION_HAND_LOCK { "HAND LOCK", "vr.vehicle_hand_lock", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_CONTROL_GLOW { "CONTROL GLOW", "vr.vehicle_control_glow", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_STEER_HAPTICS { "STEER HAPTICS", "vr.vehicle_steer_haptics", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_COMFORT_REACH { "COMFORT REACH", "vr.vehicle_comfort_reach", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_TANK_DRIVE { "TANK DRIVE", "vr.vehicle_tank_drive", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_BANSH_SLIDE { "BANSH SLIDE", "vr.vehicle_banshee_slide", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_TANK_HATCH { "TANK HATCH", "vr.vehicle_tank_hatch", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_CTRL_REACH { "CTRL REACH", "vr.handle_all_forward", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_CTRL_LEFT { "CTRL LEFT", "vr.handle_all_left", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_CTRL_HEIGHT { "CTRL HEIGHT", "vr.handle_all_up", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_RESET_CONTROLS { "RESET CONTROLS", "control offsets", _vr_setting_reset_vehicle_offsets, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOG_REACH { "HOG REACH", "vr.handle_warthog_forward", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_HOG_LEFT { "HOG LEFT", "vr.handle_warthog_left", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_HOG_HEIGHT { "HOG HEIGHT", "vr.handle_warthog_up", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_HOG_SIZE { "HOG SIZE", "vr.handle_warthog_size", _vr_setting_real, 5, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "150%", "1.5" }, { "200%", "2" } } }
#define VR_IMMERSION_GHOST_REACH { "GHOST REACH", "vr.handle_ghost_forward", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_GHOST_LEFT { "GHOST LEFT", "vr.handle_ghost_left", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_GHOST_HEIGHT { "GHOST HEIGHT", "vr.handle_ghost_up", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_GHOST_SIZE { "GHOST SIZE", "vr.handle_ghost_size", _vr_setting_real, 5, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "150%", "1.5" }, { "200%", "2" } } }
#define VR_IMMERSION_TANK_REACH { "TANK REACH", "vr.handle_scorpion_forward", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_TANK_LEFT { "TANK LEFT", "vr.handle_scorpion_left", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_TANK_HEIGHT { "TANK HEIGHT", "vr.handle_scorpion_up", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_TANK_SIZE { "TANK SIZE", "vr.handle_scorpion_size", _vr_setting_real, 5, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "150%", "1.5" }, { "200%", "2" } } }
#define VR_IMMERSION_BANSH_REACH { "BANSH REACH", "vr.handle_banshee_forward", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_BANSH_LEFT { "BANSH LEFT", "vr.handle_banshee_left", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_BANSH_UP { "BANSH UP", "vr.handle_banshee_up", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }
#define VR_IMMERSION_BANSHEE_SIZE { "BANSHEE SIZE", "vr.handle_banshee_size", _vr_setting_real, 5, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "150%", "1.5" }, { "200%", "2" } } }
#define VR_IMMERSION_FINGERS { "FINGERS", "vr.fingers", _vr_setting_boolean, 2, { { "OFF", "false" }, { "TRACKED", "true" } } }
#define VR_IMMERSION_ROOM_SCALE { "ROOM-SCALE", "vr.roomscale", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test54: tuning that changes how the features feel, one row per decision */
#define VR_IMMERSION_POUCH_SIDE { "MAG POUCH SIDE", "vr.reload_pouch_side", _vr_setting_string, 4, { { "AUTO", "auto" }, { "LEFT", "left" }, { "RIGHT", "right" }, { "BOTH", "both" } } }
#define VR_IMMERSION_CLIMB_GRAB { "CLIMB GRAB", "vr.climb_grab_radius", _vr_setting_real, 5, { { "6 CM", "0.06" }, { "10 CM", "0.10" }, { "14 CM", "0.14" }, { "18 CM", "0.18" }, { "20 CM", "0.20" } } }
/* Test66: OBJECT THROW: a prop or vehicle let go of while moving flies off
 * the hand (the owner's Test65 request); WORLD GRAB and LIFT VEHICLES bring it */
#define VR_IMMERSION_OBJECT_THROW { "OBJECT THROW", "vr.object_throw", _vr_setting_real, 5, { { "OFF", "0" }, { "1X", "1" }, { "1.5X", "1.5" }, { "2X", "2" }, { "3X", "3" } } }
/* Test72: up to 50 cm, 30 by default (the owner's 25 cm, the old maximum, still needed a near touch) */
#define VR_IMMERSION_NPC_GRAB_ASSIST { "GRAB ASSIST", "vr.npc_grab_assist", _vr_setting_real, 7, { { "TOUCH", "0" }, { "10 CM", "0.10" }, { "15 CM", "0.15" }, { "20 CM", "0.20" }, { "30 CM", "0.30" }, { "40 CM", "0.40" }, { "50 CM", "0.50" } } }
/* Test56: NPC HOLD became LIFT (strength): the same setting (and saved key),
now for everything a hand lifts (NPCs, props, turrets, vehicles) */
#define VR_IMMERSION_NPC_HOLD { "LIFT", "vr.npc_hold_strength", _vr_setting_real, 4, { { "LIGHT", "0.75" }, { "NORMAL", "1.0" }, { "SPARTAN", "1.5" }, { "IRON", "2.5" } } }
/* Test63: ONE-HAND HEAVIES, off by default */
#define VR_IMMERSION_ONE_HAND_HEAVY { "ONE-HAND HEAVIES", "vr.npc_one_hand_heavy", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_THROW_STRENGTH { "THROW", "vr.throw_strength", _vr_setting_real, 5, { { "0.75X", "0.75" }, { "1X", "1.0" }, { "1.25X", "1.25" }, { "1.5X", "1.5" }, { "2X", "2.0" } } }
#define VR_IMMERSION_DEATH_RAGDOLLS { "DEATH RAGDOLLS", "vr.death_ragdolls", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
/* Test65: bodies that give when touched, held or hit, keep some posture as
they stagger and slump as they die (Euphoria-like); a living body taken by a
limb stays itself until the hands lift it */
/* Test66: OpenCE's dynamic ragdolls (Jolt Physics: thrown by the killing blow,
tumbling down stairs and over ledges, pushed by later hits) or the game's own
deaths; the flat build's Graphics > PHYSICS row is the same setting */
#define VR_IMMERSION_DEATH_PHYSICS { "DEATH PHYSICS", "game.death_physics", _vr_setting_string, 2, { { "DYNAMIC", "ragdoll" }, { "CLASSIC", "classic" } } }
#define VR_IMMERSION_ACTIVE_RAGDOLL { "ACTIVE RAGDOLL", "vr.active_ragdoll", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } }
#define VR_IMMERSION_GRAB_RAGDOLL { "GRAB RAGDOLL", "vr.npc_lift_to_ragdoll", _vr_setting_boolean, 2, { { "AT ONCE", "false" }, { "ON LIFT", "true" } } }
/* Test59: how hard a killing burst or blast throws a death ragdoll
(vr_npc_impulse.h): CLASSIC is Test56's */
#define VR_IMMERSION_RAGDOLL_RANGE { "RAGDOLL RANGE", "vr.death_ragdoll_range", _vr_setting_real, 4, { { "20 M", "20" }, { "40 M", "40" }, { "60 M", "60" }, { "80 M", "80" } } }
#define VR_IMMERSION_DEATH_FORCE { "DEATH FORCE", "vr.death_force", _vr_setting_string, 3, { { "CLASSIC", "classic" }, { "STRONG", "strong" }, { "HUGE", "huge" } } }
#define VR_IMMERSION_HAPTICS { "HAPTICS", "vr.haptics", _vr_setting_real, 11, { { "0%", "0.0" }, { "10%", "0.1" }, { "20%", "0.2" }, { "30%", "0.3" }, { "40%", "0.4" }, { "50%", "0.5" }, { "60%", "0.6" }, { "70%", "0.7" }, { "80%", "0.8" }, { "90%", "0.9" }, { "100%", "1.0" } } }
#define VR_IMMERSION_SPRINT_SPEED { "SPRINT SPEED", "vr.sprint_speed", _vr_setting_real, 12, { { "1.0X", "1.0" }, { "1.1X", "1.1" }, { "1.2X", "1.2" }, { "1.3X", "1.3" }, { "1.4X", "1.4" }, { "1.5X", "1.5" }, { "1.75X", "1.75" }, { "2.0X", "2.0" }, { "2.25X", "2.25" }, { "2.5X", "2.5" }, { "2.75X", "2.75" }, { "3.0X", "3.0" } } }
/* Test55: calibration: where the pouches and holsters sit (1 cm steps,
+-20 cm; a blue marker shows each zone while these rows are on screen) */
/* Test56: how far past HAND CONTACT a hand still takes a charging handle or
slide (every weapon; 4 cm was Test55's fixed lock-on) */
#define VR_IMMERSION_CHARGE_GRAB { "CHARGE GRAB", "vr.charge_grab_radius", _vr_setting_real, 5, { { "2 CM", "0.02" }, { "4 CM", "0.04" }, { "6 CM", "0.06" }, { "8 CM", "0.08" }, { "10 CM", "0.10" } } }
#define VR_IMMERSION_POUCH_SIZE { "POUCH SIZE", "vr.reload_pouch_size", _vr_setting_real, 5, { { "10 CM", "0.10" }, { "13 CM", "0.13" }, { "16 CM", "0.16" }, { "20 CM", "0.20" }, { "25 CM", "0.25" } } }
#define VR_IMMERSION_POUCH_HIP_OUT { "MAG HIP OUT", "vr.pouch_hip_out", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_POUCH_HIP_UP { "MAG HIP UP", "vr.pouch_hip_up", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_POUCH_HIP_BACK { "MAG HIP BACK", "vr.pouch_hip_back", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_POUCH_SHOULDER_OUT { "MAG SHD OUT", "vr.pouch_shoulder_out", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_POUCH_SHOULDER_UP { "MAG SHD UP", "vr.pouch_shoulder_up", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_POUCH_SHOULDER_BACK { "MAG SHD BACK", "vr.pouch_shoulder_back", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOLSTER_HIP_OUT { "GUN HIP OUT", "vr.holster_hip_out", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOLSTER_HIP_UP { "GUN HIP UP", "vr.holster_hip_up", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOLSTER_HIP_BACK { "GUN HIP BACK", "vr.holster_hip_back", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOLSTER_SHOULDER_OUT { "GUN SHD OUT", "vr.holster_shoulder_out", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOLSTER_SHOULDER_UP { "GUN SHD UP", "vr.holster_shoulder_up", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_HOLSTER_SHOULDER_BACK { "GUN SHD BACK", "vr.holster_shoulder_back", _vr_setting_centimetres, 0, { { NULL, NULL } } }
#define VR_IMMERSION_ARM_EFFORT { "ARM EFFORT", "vr.arm_run_speed", _vr_setting_real, 6, { { "0.3 M/S", "0.3" }, { "0.45", "0.45" }, { "0.6", "0.6" }, { "0.8", "0.8" }, { "1.0", "1.0" }, { "1.2", "1.2" } } }

/* test20d: one row per decision. Rows that only mattered with another
(turn speed with smooth turning, holster size with holsters, multiplayer
with physical weapons, run effort with arm run, the arms with the hand
mode) are folded into it; a combination set in config.toml by hand shows
as CUSTOM. */
static struct vr_menu_setting const vr_menu_controls[] =
{
	/* Test66: LEFT-HAND MODE (the owner's name for it): the gun in the left
	 * hand at every level start and respawn; the hands still swap by hand */
	{ "LEFT-HAND MODE", "vr.left_handed", _vr_setting_handedness, 2, { { "OFF", "false" }, { "ON", "true" } } },
	{ "MIRROR CONTROLS", "vr.mirror_controls", _vr_setting_string, 2, { { "AUTO", "auto" }, { "OFF", "off" } } },
	/* Test70: the face buttons follow the gun hand on their own (left-hand
	reload and magazine release on the gun's controller); bindings stay */
	{ "MIRROR BUTTONS", "vr.mirror_buttons", _vr_setting_string, 2, { { "AUTO", "auto" }, { "OFF", "off" } } },
	VR_MENU_TURNING_ROW,
	/* the Omni One treadmill: on foot only, never a vehicle */
	{ "OMNI ONE", "vr.omni_treadmill", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	{ "OMNI YAW", "vr.omni_yaw", _vr_setting_string, 2, { { "STANDARD", "standard" }, { "REVERSED", "reversed" } } },
	{ "OMNI SPEED", "vr.omni_speed", _vr_setting_real, 7, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1.0" }, { "125%", "1.25" }, { "150%", "1.5" }, { "175%", "1.75" }, { "200%", "2.0" } } },
	{ "OMNI DEADZONE", "vr.omni_deadzone", _vr_setting_real, 6, { { "0%", "0.0" }, { "5%", "0.05" }, { "10%", "0.1" }, { "15%", "0.15" }, { "20%", "0.2" }, { "30%", "0.3" } } },
	{ "MOVE WITH", "vr.move_relative", _vr_setting_string, 3, { { "HEAD", "head" }, { "LEFT HAND", "left" }, { "RIGHT HAND", "right" } } },
	/* test21: AUTO LOCK (default) locks the off hand at the support grip
	without squeezing; SQUEEZE needs the grip held there */
	VR_IMMERSION_TWO_HANDS,
	/* Test65: how close the palms come for the off hand's squeeze to take the
	gun (a slide rack or a magazine change no longer passes it) */
	{ "HAND SWAP", "vr.hand_swap_distance", _vr_setting_real, 7, { { "OFF", "0" }, { "6 CM", "0.06" }, { "8 CM", "0.08" }, { "10 CM", "0.1" }, { "12 CM", "0.12" }, { "15 CM", "0.15" }, { "20 CM", "0.2" } } },
	VR_IMMERSION_WEAPONS,
	VR_IMMERSION_HOLSTERS,
	VR_IMMERSION_HOLSTER_HAPTICS,
	VR_IMMERSION_WEAPON_PULL,
	VR_IMMERSION_PULL_REACH,
	VR_IMMERSION_PULL_STYLE,
	VR_IMMERSION_PULL_GLOW,
    VR_IMMERSION_NPC_RIG_SLOTS,
    VR_IMMERSION_BODY_RETENTION,
    VR_IMMERSION_NPC_GRAB,
    VR_IMMERSION_NPC_GRAB_ASSIST,
    VR_IMMERSION_NPC_HOLD,
    VR_IMMERSION_THROW_STRENGTH,
    VR_IMMERSION_OBJECT_THROW,
    VR_IMMERSION_ONE_HAND_HEAVY,
    VR_IMMERSION_DEATH_PHYSICS,
    VR_IMMERSION_DEATH_RAGDOLLS,
    VR_IMMERSION_ACTIVE_RAGDOLL,
    VR_IMMERSION_GRAB_RAGDOLL,
    VR_IMMERSION_DEATH_FORCE,
    VR_IMMERSION_RAGDOLL_RANGE,
    VR_IMMERSION_NPC_RESCUE,
    VR_IMMERSION_NPC_RECOVERY,
    VR_IMMERSION_IMPACT_SPEED,
    VR_IMMERSION_IMPACT_CAP,
    VR_IMMERSION_IMPACT_DAMAGE,
	VR_IMMERSION_WORLD_GRAB,
	VR_IMMERSION_HAND_CONTACT,
	VR_IMMERSION_LIFT_VEHICLES,
	VR_IMMERSION_SHAKE_COOLING,
	VR_IMMERSION_CUSTOM_SCENERY,
	VR_IMMERSION_CUSTOM_NPC_RIGS,
	VR_IMMERSION_MAG_DEBRIS,
	VR_IMMERSION_POUCH_SIDE,
	VR_IMMERSION_CHARGE_GRAB,
	VR_IMMERSION_POUCH_SIZE,
	VR_IMMERSION_POUCH_HIP_OUT,
	VR_IMMERSION_POUCH_HIP_UP,
	VR_IMMERSION_POUCH_HIP_BACK,
	VR_IMMERSION_POUCH_SHOULDER_OUT,
	VR_IMMERSION_POUCH_SHOULDER_UP,
	VR_IMMERSION_POUCH_SHOULDER_BACK,
	VR_IMMERSION_HOLSTER_HIP_OUT,
	VR_IMMERSION_HOLSTER_HIP_UP,
	VR_IMMERSION_HOLSTER_HIP_BACK,
	VR_IMMERSION_HOLSTER_SHOULDER_OUT,
	VR_IMMERSION_HOLSTER_SHOULDER_UP,
	VR_IMMERSION_HOLSTER_SHOULDER_BACK,
	{ "AIM", "vr.aim", _vr_setting_string, 2, { { "HAND", "hand" }, { "HEAD", "head" } } },
	{ "CONTROLS", "vr.controls", _vr_setting_string, 2, { { "VR", "vr" }, { "XBOX", "pad" } } },
	/* test26: the left stick's click: the reticle's toggle or crouch, as before */
	{ "L STICK CLICK", "buttons", _vr_setting_crouch_click, 2, { { "RETICLE", "reticle" }, { "CROUCH", "crouch" } } },
    VR_IMMERSION_PROJECTILE_PARRY,
    VR_IMMERSION_DEFLECT_ASSIST,
    VR_IMMERSION_DEFLECT_STRENGTH,
    VR_IMMERSION_COMBAT,
    VR_IMMERSION_COMBAT_MOVES,
    VR_IMMERSION_COMBAT_LIGHTS,
    VR_IMMERSION_COMBAT_COMBO,
    VR_IMMERSION_COMBAT_REACH,
    VR_IMMERSION_COMBAT_HIT_ASSIST,
    VR_IMMERSION_COMBAT_LUNGE,
    VR_IMMERSION_COMBAT_HOP,
    VR_IMMERSION_COMBAT_HEALTH,
    VR_IMMERSION_COMBAT_COUNTERS,
    VR_IMMERSION_GRENADES,
    VR_IMMERSION_GRENADE_FWD,
    VR_IMMERSION_GRENADE_DOWN,
    VR_IMMERSION_GRENADE_SPREAD,
    VR_IMMERSION_GRENADE_POWER,
    VR_IMMERSION_GRENADE_SIZE,
};

static struct vr_menu_setting const vr_menu_body[] =
{
	{ "BODY", "vr.body", _vr_setting_string, 4, { { "ARMS + HANDS", "arms" }, { "FULL", "full" }, { "LEGS + ARMS", "legs" }, { "HANDS ONLY", "hands" } } },
	/* how the hands follow: body IK, floating (arms as BODY shows them),
	Halo's own arm animation, or the gun alone */
	/* test21: FLOATING is hands with no arms again (test20c); FLOAT + ARMS
	hangs arms from a floating shoulder */
	{ "HANDS", "vr.hand_tracking", _vr_setting_multi, 5, { { "BODY IK", "vr.arms=ik;vr.hand_tracking=ik" }, { "FLOATING", "vr.arms=ik;vr.hand_tracking=floating" }, { "FLOAT + ARMS", "vr.arms=ik;vr.hand_tracking=floating_arms" }, { "ANIMATED", "vr.arms=animated;vr.hand_tracking=ik" }, { "GUN ONLY", "vr.arms=hidden;vr.hand_tracking=ik" } } },
	/* Test73: the arms of the character played (vr.character_arms) */
	{ "ARMS FROM", "vr.character_arms", _vr_setting_string, 3, { { "AUTO", "auto" }, { "CHARACTER", "on" }, { "FIRST PERSON", "off" } } },
    { "CONTACT VOLUMES", "vr.contact_volumes", _vr_setting_boolean, 2, { { "RAYS", "false" }, { "VOLUMES", "true" } } },
    { "CONTACT LOG", "vr.contact_debug", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	VR_IMMERSION_FINGERS,
	VR_IMMERSION_ROOM_SCALE,
	{ "CROUCH DEPTH", "vr.crouch_height", _vr_setting_real, 9, { { "OFF", "0" }, { "5 CM", "0.05" }, { "10 CM", "0.1" }, { "15 CM", "0.15" }, { "20 CM", "0.2" }, { "25 CM", "0.25" }, { "30 CM", "0.3" }, { "35 CM", "0.35" }, { "40 CM", "0.4" } } },
	/* test21: physical melee in network games only with "+ ONLINE"
	(vr.melee_multiplayer, off by default); the melee button always works */
	VR_IMMERSION_MELEE,
	VR_IMMERSION_MELEE_SPEED,
	VR_IMMERSION_FIST_REQUIRED,
};

/* Test72: LOCOMOTION / PARKOUR: sprint, slide, BULLET TIME (from GAMEPLAY,
whose first row is HAPTICS) and the wall runs and climbing (from GAMEPLAY) */
static struct vr_menu_setting const vr_menu_locomotion[] =
{
	VR_IMMERSION_BUTTON_SPRINT,
	VR_IMMERSION_SPRINT_TOGGLE,
	VR_IMMERSION_ARM_RUN,
	VR_IMMERSION_ARM_EFFORT,
	VR_IMMERSION_SPRINT_SPEED,
	/* Test70: SLIDE (sprint and crouch) */
	VR_IMMERSION_SLIDE,
	VR_IMMERSION_SLIDE_DISTANCE,
	VR_IMMERSION_SLIDE_FRICTION,
	VR_IMMERSION_SLIDE_IMPACT,
	VR_IMMERSION_CROUCH_SLIDES,
	VR_IMMERSION_JUMP_CANCELS,
	/* Test70: BULLET TIME (offline; its button on BUTTONS) */
	VR_IMMERSION_BULLET_TIME,
	VR_IMMERSION_BULLET_SCALE,
	VR_IMMERSION_BULLET_MODE,
	/* PARKOUR */
	VR_IMMERSION_WALLRUN,
	VR_IMMERSION_WALLJUMP,
	VR_IMMERSION_GRIP_CLIMB,
	VR_IMMERSION_WALL_SPEED,
	VR_IMMERSION_WALL_TIME,
	VR_IMMERSION_WALL_LEAP,
	VR_IMMERSION_CLIMB_LAUNCH,
	VR_IMMERSION_CLIMB_GRAB,
};

/* the feel of play, with the crosshair (formerly its own page) */
static struct vr_menu_setting const vr_menu_vr[] =
{
	/* Test72: VR THIRD PERSON (vr_third_person.c) */
	{ "THIRD PERSON", "vr.third_person", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	VR_IMMERSION_HAPTICS,
	/* (Test72: BULLET TIME went to LOCOMOTION) */
	VR_IMMERSION_SCOPE,
	{ "CUTSCENES", "vr.cutscenes", _vr_setting_string, 3, { { "IMMERSIVE", "immersive" }, { "3D SCREEN", "screen" }, { "FLAT", "flat" } } },
    { "GRAB CONTACT", "vr.grab_contact", _vr_setting_string, 3, { { "AUTO", "auto" }, { "OFF", "off" }, { "ON", "on" } } },
    { "BODY RADIUS", "vr.grab_radius", _vr_setting_real, 9, { { "18 CM", "0.18" }, { "20 CM", "0.20" }, { "22 CM", "0.22" }, { "25 CM", "0.25" }, { "30 CM", "0.30" }, { "35 CM", "0.35" }, { "40 CM", "0.40" }, { "50 CM", "0.50" }, { "60 CM", "0.60" } } },
	/* (Test72: the wall runs and climbing went to LOCOMOTION) */
	VR_IMMERSION_CLOSE_CONTACT,
	/* Test73: online co-op's own FRIENDLY FIRE (network.coop_own_friendly_fire, read as it changes:
	network_coop.c), which the flat build's campaign pause menu has (menu_tags.c pause_campaign_patch) and the
	headset's, whose campaign pause menu is its own, had not */
	{ "FRIENDLY FIRE", "network.coop_own_friendly_fire", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
};

/* test26: what the HUD shows and where: the crosshair (formerly on
GAMEPLAY; its button is on BUTTONS) and the wrist HUD. test29: the HUD
itself shown or hidden (as the head tap does) and the head tap's reach or
off (as on HEAD GESTURES), first; the wrist HUD's place and size */
static struct vr_menu_setting const vr_menu_hud[] =
{
	/* Test72: MENU POSITION (head-locked, fixed in the world, or following
	lazily), and a fixed menu's distance, size and height */
	{ "MENU POSITION", "vr.menu_anchor", _vr_setting_string, 3, { { "HEAD", "head" }, { "WORLD", "world" }, { "FOLLOW", "follow" } } },
	{ "MENU DISTANCE", "vr.menu_distance", _vr_setting_real, 6, { { "0.8 M", "0.8" }, { "1.2 M", "1.2" }, { "1.6 M", "1.6" }, { "2 M", "2" }, { "2.5 M", "2.5" }, { "3 M", "3" } } },
	{ "MENU SIZE", "vr.menu_scale", _vr_setting_real, 5, { { "75%", "0.75" }, { "90%", "0.9" }, { "100%", "1" }, { "120%", "1.2" }, { "150%", "1.5" } } },
	{ "MENU HEIGHT", "vr.menu_height", _vr_setting_real, 5, { { "-30 CM", "-0.3" }, { "-15 CM", "-0.15" }, { "EYES", "0" }, { "+15 CM", "0.15" }, { "+30 CM", "0.3" } } },
    VR_IMMERSION_AIM_STEADY,
	{ "HUD", "hud", _vr_setting_hud_shown, 2, { { "SHOWN", "true" }, { "HIDDEN", "false" } } },
	{ "HEAD TAP", "vr.hud_tap_distance", _vr_setting_real, 6, { { "OFF", "0" }, { "6 CM", "0.06" }, { "8 CM", "0.08" }, { "10 CM", "0.1" }, { "12 CM", "0.12" }, { "15 CM", "0.15" } } },
	{ "CROSSHAIR", "vr.crosshair", _vr_setting_string, 2, { { "NATIVE", "native" }, { "OFF", "off" } } },
	{ "CROSSHAIR SIZE", "vr.crosshair_size", _vr_setting_real, 8, { { "25%", "0.25" }, { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "125%", "1.25" }, { "150%", "1.5" }, { "200%", "2" }, { "300%", "3" } } },
	{ "OPACITY", "vr.crosshair_opacity", _vr_setting_real, 11, { { "0%", "0" }, { "10%", "0.1" }, { "20%", "0.2" }, { "30%", "0.3" }, { "40%", "0.4" }, { "50%", "0.5" }, { "60%", "0.6" }, { "70%", "0.7" }, { "80%", "0.8" }, { "90%", "0.9" }, { "100%", "1" } } },
	{ "WRIST HUD", "vr.wrist_hud", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	{ "WRIST ALONG", "vr.wrist_hud_along", _vr_setting_centimetres, 0, { { NULL, NULL } } },
	{ "WRIST ACROSS", "vr.wrist_hud_across", _vr_setting_centimetres, 0, { { NULL, NULL } } },
	{ "WRIST HEIGHT", "vr.wrist_hud_out", _vr_setting_centimetres, 0, { { NULL, NULL } } },
	{ "WRIST SIZE", "vr.wrist_hud_size", _vr_setting_real, 7, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "125%", "1.25" }, { "150%", "1.5" }, { "175%", "1.75" }, { "200%", "2" } } },
	{ "RESET WRIST", "wrist", _vr_setting_reset_wrist, 0, { { NULL, NULL } } },
};

/* test26: the head taps, each with its reach or off (the flashlight's
button stays): the off hand to the head toggles the flashlight, the weapon
hand to its own temple the HUD */
static struct vr_menu_setting const vr_menu_gestures[] =
{
	{ "FLASHLIGHT", "vr.flashlight_distance", _vr_setting_real, 6, { { "OFF", "0" }, { "10 CM", "0.1" }, { "15 CM", "0.15" }, { "20 CM", "0.2" }, { "25 CM", "0.25" }, { "30 CM", "0.3" } } },
	{ "HUD TAP", "vr.hud_tap_distance", _vr_setting_real, 6, { { "OFF", "0" }, { "6 CM", "0.06" }, { "8 CM", "0.08" }, { "10 CM", "0.1" }, { "12 CM", "0.12" }, { "15 CM", "0.15" } } },
};

/* Test72: in topics (vr_menu_vehicles_sections), each screen titled
"VEHICLES: <TOPIC>", the index listing them: a vehicle's own view offsets and
control handles together, ALL VEHICLES' moving every one (a tester could not
find the Shade's: "not sure I want to lower every vehicle"). The rows' saved
keys are as they were. */
static struct vr_menu_setting const vr_menu_vehicles[] =
{
	/* DRIVING */
	VR_IMMERSION_VEHICLE_VIEW,
	/* test25: first person, a driver's view: the horizon level (the
	cockpit tilts against the view), or tilting with the vehicle, half or
	whole (the cockpit stays put) */
	{ "HORIZON", "vr.vehicle_tilt", _vr_setting_real, 3, { { "LEVEL", "0" }, { "HALF", "0.5" }, { "VEHICLE", "1" } } },
	{ "STEERING", "vr.vehicle_steering", _vr_setting_string, 4, { { "RIGHT HAND", "right" }, { "LEFT HAND", "left" }, { "HEAD", "head" }, { "STICK", "stick" } } },
    VR_IMMERSION_PHYSICAL_HANDLES,
    VR_IMMERSION_VIRTUAL_WHEEL,
    /* Test65: the eyes, hands and controls held to the vehicle as one
     * (the owner's Test64 Warthog: "lock the head and wheel in place"), and
     * an optional mark where the hands take the Warthog's rim */
    VR_IMMERSION_COCKPIT_LOCK,
    /* Test73: a holding hand locked on the control, turning with it */
    VR_IMMERSION_HAND_LOCK,
    VR_IMMERSION_GRIP_GUIDE,
	/* VEHICLE OPTIONS */
    /* Test57: the immersive controls' highlight within reach, their steering
     * haptics, and the Banshee's bar turning when slid across */
    VR_IMMERSION_CONTROL_GLOW,
    VR_IMMERSION_STEER_HAPTICS,
    VR_IMMERSION_BANSH_SLIDE,
    /* Test59: controls past 55 cm from the eyes come in toward them */
    VR_IMMERSION_COMFORT_REACH,
    /* Test59: the tank's drive stick pushed or pulled drives */
    VR_IMMERSION_TANK_DRIVE,
    /* Test57b: a tank's first-person eyes over its hull, as from an open hatch */
    VR_IMMERSION_TANK_HATCH,
    /* Test73 (10i): a seat turns smoothly for a snap turner */
    VR_COMFORT_VEHICLE_TURN,
	/* ALL VEHICLES: every vehicle's view and controls moved, before each one's own */
	{ "VIEW UP", "vr.vehicle_all_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_all_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_all_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	VR_IMMERSION_CTRL_REACH,
    VR_IMMERSION_CTRL_LEFT,
    VR_IMMERSION_CTRL_HEIGHT,
	{ "RESET OFFSETS", "vehicle offsets", _vr_setting_reset_vehicle_offsets, 0, { { NULL, NULL } } },
    VR_IMMERSION_RESET_CONTROLS,
	/* WARTHOG */
	{ "HOG GLASS", "vr.vehicle_warthog_hide_glass", _vr_setting_boolean, 2, { { "VISIBLE", "false" }, { "HIDDEN", "true" } } },
	{ "VIEW UP", "vr.vehicle_warthog_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_warthog_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_warthog_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
    VR_IMMERSION_HOG_REACH,
    VR_IMMERSION_HOG_LEFT,
    VR_IMMERSION_HOG_HEIGHT,
    VR_IMMERSION_HOG_SIZE,
	/* GHOST */
	{ "VIEW UP", "vr.vehicle_ghost_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_ghost_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_ghost_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
    VR_IMMERSION_GHOST_REACH,
    VR_IMMERSION_GHOST_LEFT,
    VR_IMMERSION_GHOST_HEIGHT,
    VR_IMMERSION_GHOST_SIZE,
	/* BANSHEE */
	{ "VIEW UP", "vr.vehicle_banshee_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_banshee_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_banshee_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
    VR_IMMERSION_BANSH_REACH,
    VR_IMMERSION_BANSH_LEFT,
    VR_IMMERSION_BANSH_UP,
    VR_IMMERSION_BANSHEE_SIZE,
	/* SCORPION */
	{ "VIEW UP", "vr.vehicle_scorpion_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_scorpion_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_scorpion_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
    VR_IMMERSION_TANK_REACH,
    VR_IMMERSION_TANK_LEFT,
    VR_IMMERSION_TANK_HEIGHT,
    VR_IMMERSION_TANK_SIZE,
	/* PELICAN */
	{ "VIEW UP", "vr.vehicle_pelican_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_pelican_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_pelican_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	/* TURRETS + SHADE (Test72): a turret's gunner, the Shade's, and any gun
	seat's aim; the view 25 cm up by default */
	{ "TURRET AIM", "vr.turret_aim", _vr_setting_string, 4, { { "RIGHT HAND", "right" }, { "LEFT HAND", "left" }, { "HEAD", "head" }, { "STICK", "stick" } } },
	{ "VIEW UP", "vr.vehicle_turret_up", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW FWD", "vr.vehicle_turret_forward", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
	{ "VIEW RIGHT", "vr.vehicle_turret_right", _vr_setting_vehicle_centimetres, 0, { { NULL, NULL } } },
};
static struct vr_menu_section const vr_menu_vehicles_sections[] =
{
	{ "DRIVING", 8 }, { "VEHICLE OPTIONS", 7 }, { "ALL VEHICLES", 8 }, { "WARTHOG", 8 }, { "GHOST", 7 },
	{ "BANSHEE", 7 }, { "SCORPION", 7 }, { "PELICAN", 3 }, { "TURRETS + SHADE", 4 },
};
/* (a page whose topics do not cover its rows is not built: vr_menu_paged) */
typedef char vr_menu_vehicles_sections_cover_the_rows[
	8 + 7 + 8 + 8 + 7 + 7 + 7 + 3 + 4 == NUMBEROF(vr_menu_vehicles) ? 1 : -1];

static struct vr_menu_setting const vr_menu_graphics[] =
{
	{ "PRESET", "graphics.preset", _vr_setting_string, 5, { { "AUTO", "auto" }, { "LOW", "low" }, { "MEDIUM", "medium" }, { "HIGH", "high" }, { "MAX", "max" } } },
	/* a slider of the eye images' size against the headset's
	recommendation (Quest 3: 1680x1760), the Quest 3's own panels (2064x2208)
	marked ("125% Q3": test29, the row's width), supersampling above; FOV's glasses window draws about 40%
	of the pixels, which pays for the higher notches */
	{ "RESOLUTION", "vr.resolution_scale", _vr_setting_real, 12, { { "AUTO", "0" }, { "60%", "0.6" }, { "70%", "0.7" }, { "80%", "0.8" }, { "90%", "0.9" }, { "100%", "1" }, { "110%", "1.1" }, { "125% Q3", "1.25" }, { "140%", "1.4" }, { "160%", "1.6" }, { "180%", "1.8" }, { "200%", "2" } } },
	{ "FOV", "vr.fov_mode", _vr_setting_string, 2, { { "FULL", "full" }, { "GLASSES 70X66", "glasses" } } },
	VR_MENU_EFFECT("SHADOWS", "graphics.shadows"),
	VR_MENU_EFFECT("LIGHTS", "graphics.dynamic_lights"),
	VR_MENU_EFFECT("SPECULAR", "graphics.specular"),
	VR_MENU_EFFECT("REFLECTIONS", "graphics.reflections"),
	VR_MENU_EFFECT("BUMP MAPS", "graphics.bump_mapping"),
	VR_MENU_EFFECT("GRASS", "graphics.detail_objects"),
	VR_MENU_EFFECT("FOG LAYERS", "graphics.fog_screen"),
};

static struct vr_menu_setting const vr_menu_effects[] =
{
	VR_MENU_EFFECT("DECALS", "graphics.decals"),
	VR_MENU_EFFECT("PARTICLES", "graphics.particles"),
	VR_MENU_EFFECT("CONTRAILS", "graphics.contrails"),
	VR_MENU_EFFECT("WEATHER", "graphics.weather"),
	VR_MENU_EFFECT("LENS FLARES", "graphics.lens_flares"),
	VR_MENU_EFFECT("CAMO", "graphics.camouflage_multipass"),
	{ "REFRESH", "vr.refresh_rate", _vr_setting_real, 5, { { "AUTO", "0" }, { "72 HZ", "72" }, { "80 HZ", "80" }, { "90 HZ", "90" }, { "120 HZ", "120" } } },
	/* Test66: OpenCE Build 201's shield options, in VR too: your shield in an
	armor color, shields lighting what is round them as they flare, and the
	flare on the first-person arms (the VR hands) */
	{ "SHIELD COLOR", "display.shield_color", _vr_setting_string, 12, { { "DEFAULT", "default" }, { "WHITE", "white" }, { "RED", "red" }, { "BLUE", "blue" }, { "GRAY", "gray" }, { "YELLOW", "yellow" }, { "GREEN", "green" }, { "PINK", "pink" }, { "PURPLE", "purple" }, { "CYAN", "cyan" }, { "ORANGE", "orange" }, { "TEAL", "teal" } } },
	{ "SHIELD GLOW", "display.shield_glow", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	{ "GLOW REACH", "display.shield_glow_intensity", _vr_setting_string, 2, { { "DEFAULT", "default" }, { "LIGHT SHOW", "light_show" } } },
	{ "ARM SHIELD", "display.viewmodel_shield", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	/* Test72: the eye drawn first (vr.eye_order): a test for a picture
	spoilt in one eye only */
	{ "EYE ORDER", "vr.eye_order", _vr_setting_string, 2, { { "LEFT FIRST", "left" }, { "RIGHT FIRST", "right" } } },
	/* Test72: HaloFreak95's fast decals (off on the Quest until measured) */
	{ "FAST DECALS", "display.decals_on_gpu", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
	/* Test72: HaloFreak95's blood layers */
	{ "BLOOD LAYERS", "game.blood_layers", _vr_setting_multi, 3, { { "ALL", "game.blood_layers=0" }, { "6", "game.blood_layers=6" }, { "3", "game.blood_layers=3" } } },
	/* Test72: HaloFreak95's holstered weapons (others' carried guns worn) */
	{ "HOLSTERED GUNS", "display.holstered_weapons", _vr_setting_boolean, 2, { { "OFF", "false" }, { "ON", "true" } } },
};

/* test20c/d: the visible hands (never the gun: one row turns both, the
left mirrored; per-hand values stay in config.toml as vr.hand_left_* and
vr.hand_right_*), then the held gun: its angle (shots and reticle follow)
and its place in the hand (vr.gun_*, with vr.gun_anchor) */
static struct vr_menu_setting const vr_menu_hands[] =
{
    { "HAND PITCH", "pitch", _vr_setting_hand_degrees, 0, { { NULL,NULL } } },
    { "HAND YAW", "yaw", _vr_setting_hand_degrees, 0, { { NULL,NULL } } },
    { "HAND ROLL", "roll", _vr_setting_hand_degrees, 0, { { NULL,NULL } } },
    { "RESET HANDS", "both", _vr_setting_reset_hand, 0, { { NULL,NULL } } },
    { "GUN PITCH", "vr.weapon_pitch", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "GUN YAW", "vr.weapon_yaw", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "GUN ROLL", "vr.weapon_roll", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "RESET GUN", "weapon", _vr_setting_reset_weapon, 0, { { NULL,NULL } } },
    { "GUN FORWARD", "vr.gun_forward", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "GUN UP", "vr.gun_up", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "GUN OUT", "vr.gun_out", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "GUN GRIP", "vr.gun_anchor", _vr_setting_boolean, 2, { { "ANCHORED", "true" }, { "CLASSIC", "false" } } },
    { "AIM FOR", "gun", _vr_setting_gun_aim, 0, { { NULL,NULL } } },
    { "AIM UP", "up", _vr_setting_gun_aim, 0, { { NULL,NULL } } },
    { "AIM RIGHT", "right", _vr_setting_gun_aim, 0, { { NULL,NULL } } },
    { "RESET AIM", "reset", _vr_setting_gun_aim, 0, { { NULL,NULL } } },
};

/* test24b: comfort: turning (smooth or snap, each finely set: smooth
speed, snap angle) and the vignette, which darkens the view's edges while
you move or turn by stick */
static struct vr_menu_setting const vr_menu_comfort[] =
{
	{ "TURNING", "turn", _vr_setting_turn_mode, 2, { { "SMOOTH", "smooth" }, { "SNAP", "snap" } } },
	{ "SMOOTH SPEED", "vr.smooth_turn_speed", _vr_setting_real, 12, { { "30", "30" }, { "45", "45" }, { "60", "60" }, { "75", "75" }, { "90", "90" }, { "105", "105" }, { "120", "120" }, { "150", "150" }, { "180", "180" }, { "210", "210" }, { "240", "240" }, { "300", "300" } } },
	{ "SNAP ANGLE", "vr.snap_turn_amount", _vr_setting_snap_angle, 9, { { "10 DEG", "10" }, { "15 DEG", "15" }, { "20 DEG", "20" }, { "22.5 DEG", "22.5" }, { "30 DEG", "30" }, { "40 DEG", "40" }, { "45 DEG", "45" }, { "60 DEG", "60" }, { "90 DEG", "90" } } },
	{ "VIGNETTE", "vr.vignette", _vr_setting_real, 4, { { "OFF", "0" }, { "LOW", "0.35" }, { "MEDIUM", "0.65" }, { "HIGH", "1" } } },
	{ "VIGNETTE ON", "vr.vignette_when", _vr_setting_string, 3, { { "MOVING", "move_turn" }, { "TURNING", "turn" }, { "ALWAYS", "always" } } },
	/* Test73 (10i): smooth in a seat whatever TURNING is */
	VR_COMFORT_VEHICLE_TURN,
	/* Test73b (a request, relayed: "a world scale slider/option in the menu"):
	how big the world is around you, 100% its true size (vr.world_size;
	vr_reload_settings) */
	{ "WORLD SCALE", "vr.world_size", _vr_setting_real, 12, { { "70%", "0.7" }, { "80%", "0.8" }, { "85%", "0.85" }, { "90%", "0.9" }, { "95%", "0.95" }, { "100%", "1.0" }, { "105%", "1.05" }, { "110%", "1.1" }, { "115%", "1.15" }, { "120%", "1.2" }, { "130%", "1.3" }, { "150%", "1.5" } } },
};

/* test22: the scopes' places and sizes, the pistol's and the sniper
rifle's apart (vr.scope_pistol_*, vr.scope_sniper_*: 0 and 100%, the usual) */
static struct vr_menu_setting const vr_menu_scopes[] =
{
/* Test72: a section a weapon (vr_menu_scopes_sections), its name the
screen's title, so the rows say only what they move */
#define SCOPE_MENU(key, label, gun) \
    { "FORWARD", "vr.scope_" #key "_forward", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }, \
    { "UP", "vr.scope_" #key "_up", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }, \
    { "RIGHT", "vr.scope_" #key "_right", _vr_setting_scope_centimetres, 0, { { NULL,NULL } } }, \
    /* (Test73, the owner's: up to 400%, not 200) */ \
    { "SIZE", "vr.scope_" #key "_scale", _vr_setting_real, 11, { { "50%", "0.5" }, { "75%", "0.75" }, { "100%", "1" }, { "125%", "1.25" }, { "150%", "1.5" }, { "175%", "1.75" }, { "200%", "2" }, { "250%", "2.5" }, { "300%", "3" }, { "350%", "3.5" }, { "400%", "4" } } },
    VR_SCOPE_PROFILES(SCOPE_MENU)
#undef SCOPE_MENU
    { "RESET SCOPES", "scopes", _vr_setting_reset_scopes, 0, { { NULL,NULL } } },
};
/* Test72: each weapon's scope its own topic, named in full, in
VR_SCOPE_PROFILES' order (pistol, sniper, rocket launcher, other, plasma
pistol, assault rifle, plasma rifle, shotgun, needler, fuel rod, flamethrower) */
static struct vr_menu_section const vr_menu_scopes_sections[] =
{
	{ "PISTOL", 4 }, { "SNIPER RIFLE", 4 }, { "ROCKET LAUNCHER", 4 }, { "OTHER WEAPONS", 4 },
	{ "PLASMA PISTOL", 4 }, { "ASSAULT RIFLE", 4 }, { "PLASMA RIFLE", 4 }, { "SHOTGUN", 4 },
	{ "NEEDLER", 4 }, { "FUEL ROD GUN", 4 }, { "FLAMETHROWER", 4 }, { "RESET", 1 },
};
typedef char vr_menu_scopes_sections_cover_the_profiles[
	NUMBEROF(vr_menu_scopes_sections) == VR_SCOPE_PROFILE_COUNT + 1 ? 1 : -1];

/* test23: the Quest's buttons (vr.button_*), remappable. "A"/"B" are the
gun hand's lower/upper buttons and "X"/"Y" the other hand's (mirrored with
Mirror Controls); the grip is the gun hand's, with locked weapons only */
static struct vr_menu_setting const vr_menu_buttons[] =
{
    /* Test67: a player asked for a "reset to default controls" here: it was
    the page's last row, below every binding; now also its first, plainly
    named, and either says DONE once pressed */
    { "RESET CONTROLS", "buttons", _vr_setting_reset_buttons, 0, { { NULL,NULL } } },
    { "JUMP", "vr.button_jump", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    VR_IMMERSION_SHOULDER_POUCH,
    VR_IMMERSION_HIP_POUCH,
    VR_IMMERSION_MANUAL_RELOAD,
    VR_IMMERSION_RELOAD_INPUT,
    { "USE / RELOAD", "vr.button_action", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "MELEE", "vr.button_melee", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "CROUCH", "vr.button_crouch", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "NEXT WEAPON", "vr.button_switch_weapon", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "GRENADE", "vr.button_grenade", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "NEXT GRENADE", "vr.button_switch_grenade", _vr_setting_button, 11, { { "HOLD", "hold" }, { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    /* test26: shows or hides the reticle (starts shown) */
    { "RETICLE", "vr.button_reticle", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "PUSH TO TALK", "vr.button_push_to_talk", _vr_setting_button, 7, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "NONE", "none" } } },
    /* Test70: BULLET TIME's button (the turning stick pushed up by default) */
    { "BULLET TIME", "vr.button_bullet_time", _vr_setting_button, 10, { { "A", "a" }, { "B", "b" }, { "X", "x" }, { "Y", "y" }, { "R STICK", "right_stick" }, { "L STICK", "left_stick" }, { "R DOWN", "right_stick_down" }, { "GRIP", "grip" }, { "R UP", "right_stick_up" }, { "NONE", "none" } } },
    { "RESET CONTROLS", "buttons", _vr_setting_reset_buttons, 0, { { NULL,NULL } } },
};

/* Build167 voice is off by default. Volume zero also mutes received voices;
 * host routing remains in Server Setup and is not overridden by this client. */
static struct vr_menu_setting const vr_menu_voice[] =
{
	{ "MICROPHONE", "audio.voice_chat", _vr_setting_string, 3,
		{ { "OFF", "off" }, { "PUSH TO TALK", "push_to_talk" }, { "OPEN MIC", "open_mic" } } },
	{ "VOICE VOLUME", "audio.voice_volume", _vr_setting_real, 9,
		{ { "MUTED", "0" }, { "25%", "0.25" }, { "50%", "0.5" }, { "75%", "0.75" },
		  { "100%", "1" }, { "125%", "1.25" }, { "150%", "1.5" }, { "175%", "1.75" }, { "200%", "2" } } },
};

static struct vr_menu_setting const vr_menu_align_left[] =
{
    { "PITCH", "vr.align_left_pitch", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "YAW", "vr.align_left_yaw", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "ROLL", "vr.align_left_roll", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "RIGHT", "vr.align_left_right", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "UP", "vr.align_left_up", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "BACK", "vr.align_left_back", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "AIM SOURCE", "vr.align_left_grip_aim", _vr_setting_boolean, 2, { { "NATIVE", "false" }, { "GRIP", "true" } } },
    { "FLIP ROLL 180", "vr.align_left_roll", _vr_setting_flip_alignment, 0, { { NULL,NULL } } },
    { "RESET LEFT", "left", _vr_setting_reset_alignment, 0, { { NULL,NULL } } },
};

static struct vr_menu_setting const vr_menu_align_right[] =
{
    { "PITCH", "vr.align_right_pitch", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "YAW", "vr.align_right_yaw", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "ROLL", "vr.align_right_roll", _vr_setting_degrees, 0, { { NULL,NULL } } },
    { "RIGHT", "vr.align_right_right", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "UP", "vr.align_right_up", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "BACK", "vr.align_right_back", _vr_setting_centimetres, 0, { { NULL,NULL } } },
    { "AIM SOURCE", "vr.align_right_grip_aim", _vr_setting_boolean, 2, { { "NATIVE", "false" }, { "GRIP", "true" } } },
    { "FLIP ROLL 180", "vr.align_right_roll", _vr_setting_flip_alignment, 0, { { NULL,NULL } } },
    { "RESET RIGHT", "right", _vr_setting_reset_alignment, 0, { { NULL,NULL } } },
};

/* Test53: every VR immersion feature on one page (physical weapons and
melee, manual reload, parkour, world/NPC/vehicle grabbing, bodies, impact
damage, contact and tracking). The rows stay on their usual pages too. */
/* Test55: one IMMERSION category, its screens topics (vr_menu_immersion_sections):
every immersion row once, each also still on its usual page. Turning a
feature on turns on what it needs (vr_menu_dependencies); optional extras are
never turned on for the player. */
static struct vr_menu_setting const vr_menu_immersion[] =
{
	/* WEAPONS */
	VR_IMMERSION_WEAPONS,
	VR_IMMERSION_TWO_HANDS,
	VR_IMMERSION_SCOPE,
	VR_IMMERSION_HOLSTERS,
	VR_IMMERSION_HOLSTER_HAPTICS,
	VR_IMMERSION_MELEE,
	VR_IMMERSION_MELEE_SPEED,
	VR_IMMERSION_SHAKE_COOLING,
	/* PICKUPS */
	VR_IMMERSION_WEAPON_PULL,
	VR_IMMERSION_PULL_REACH,
	VR_IMMERSION_PULL_STYLE,
	VR_IMMERSION_PULL_GLOW,
	/* FIST COMBAT */
	VR_IMMERSION_COMBAT,
	VR_IMMERSION_COMBAT_MOVES,
	VR_IMMERSION_COMBAT_REACH,
	VR_IMMERSION_COMBAT_HIT_ASSIST,
	VR_IMMERSION_COMBAT_LUNGE,
	VR_IMMERSION_COMBAT_HOP,
	VR_IMMERSION_COMBAT_HEALTH,
	VR_IMMERSION_COMBAT_COUNTERS,
	/* (Test70: gathered from WEAPONS and FIST COMBAT) */
	/* FIST OPTIONS */
	VR_IMMERSION_COMBAT_LIGHTS,
	VR_IMMERSION_COMBAT_COMBO,
	VR_IMMERSION_PROJECTILE_PARRY,
	VR_IMMERSION_DEFLECT_ASSIST,
	VR_IMMERSION_DEFLECT_STRENGTH,
	VR_IMMERSION_FIST_REQUIRED,
	VR_IMMERSION_AIM_STEADY,
	/* GRENADES */
	VR_IMMERSION_GRENADES,
	VR_IMMERSION_GRENADE_FWD,
	VR_IMMERSION_GRENADE_DOWN,
	VR_IMMERSION_GRENADE_SPREAD,
	VR_IMMERSION_GRENADE_POWER,
	VR_IMMERSION_GRENADE_SIZE,
	/* RELOAD */
	VR_IMMERSION_MANUAL_RELOAD,
	VR_IMMERSION_RELOAD_INPUT,
	VR_IMMERSION_POUCH_SIDE,
	VR_IMMERSION_MAG_DEBRIS,
	VR_IMMERSION_SHOULDER_POUCH,
	VR_IMMERSION_HIP_POUCH,
	VR_IMMERSION_CHARGE_GRAB,
	VR_IMMERSION_HAND_CONTACT,
	/* POUCH PLACES */
	VR_IMMERSION_POUCH_SIZE,
	VR_IMMERSION_POUCH_HIP_OUT,
	VR_IMMERSION_POUCH_HIP_UP,
	VR_IMMERSION_POUCH_HIP_BACK,
	VR_IMMERSION_POUCH_SHOULDER_OUT,
	VR_IMMERSION_POUCH_SHOULDER_UP,
	VR_IMMERSION_POUCH_SHOULDER_BACK,
	/* HOLSTER PLACES */
	VR_IMMERSION_HOLSTER_HIP_OUT,
	VR_IMMERSION_HOLSTER_HIP_UP,
	VR_IMMERSION_HOLSTER_HIP_BACK,
	VR_IMMERSION_HOLSTER_SHOULDER_OUT,
	VR_IMMERSION_HOLSTER_SHOULDER_UP,
	VR_IMMERSION_HOLSTER_SHOULDER_BACK,
	/* Test72: LOCOMOTION (sprint, slide, BULLET TIME) before PARKOUR */
	/* LOCOMOTION */
	VR_IMMERSION_BUTTON_SPRINT,
	VR_IMMERSION_SPRINT_TOGGLE,
	VR_IMMERSION_ARM_RUN,
	VR_IMMERSION_ARM_EFFORT,
	VR_IMMERSION_SPRINT_SPEED,
	VR_IMMERSION_SLIDE,
	VR_IMMERSION_BULLET_TIME,
	VR_IMMERSION_BULLET_MODE,
	/* LOCOMOTION OPTIONS */
	VR_IMMERSION_BULLET_SCALE,
	VR_IMMERSION_SLIDE_DISTANCE,
	VR_IMMERSION_SLIDE_FRICTION,
	VR_IMMERSION_SLIDE_IMPACT,
	VR_IMMERSION_CROUCH_SLIDES,
	VR_IMMERSION_JUMP_CANCELS,
	/* PARKOUR */
	VR_IMMERSION_WALLRUN,
	VR_IMMERSION_WALLJUMP,
	VR_IMMERSION_GRIP_CLIMB,
	VR_IMMERSION_CLIMB_GRAB,
	VR_IMMERSION_WALL_SPEED,
	VR_IMMERSION_WALL_TIME,
	VR_IMMERSION_WALL_LEAP,
	VR_IMMERSION_CLIMB_LAUNCH,
	/* BODY */
	VR_IMMERSION_HAPTICS,
	VR_IMMERSION_FINGERS,
	VR_IMMERSION_ROOM_SCALE,
	VR_IMMERSION_CLOSE_CONTACT,
	/* GRAB + NPCS */
	VR_IMMERSION_WORLD_GRAB,
	VR_IMMERSION_LIFT_VEHICLES,
	VR_IMMERSION_NPC_GRAB,
	VR_IMMERSION_NPC_GRAB_ASSIST,
	VR_IMMERSION_NPC_HOLD,
	VR_IMMERSION_THROW_STRENGTH,
	VR_IMMERSION_ONE_HAND_HEAVY,
	VR_IMMERSION_OBJECT_THROW,
	/* Test66: every immersive vehicle setting together, out of WEAPONS: */
	/* VEHICLES */
	VR_IMMERSION_PHYSICAL_HANDLES,
    VR_IMMERSION_VIRTUAL_WHEEL,
    VR_IMMERSION_COCKPIT_LOCK,
    VR_IMMERSION_GRIP_GUIDE,
    VR_IMMERSION_CONTROL_GLOW,
    VR_IMMERSION_STEER_HAPTICS,
    VR_IMMERSION_COMFORT_REACH,
    VR_IMMERSION_TANK_DRIVE,
	/* VEHICLE OPTIONS */
    VR_IMMERSION_VEHICLE_VIEW,
    VR_IMMERSION_HAND_LOCK,
    VR_IMMERSION_BANSH_SLIDE,
    VR_IMMERSION_TANK_HATCH,
    VR_IMMERSION_CTRL_REACH,
    VR_IMMERSION_CTRL_LEFT,
    VR_IMMERSION_CTRL_HEIGHT,
    VR_IMMERSION_RESET_CONTROLS,
	/* WARTHOG + GHOST */
    VR_IMMERSION_HOG_REACH,
    VR_IMMERSION_HOG_LEFT,
    VR_IMMERSION_HOG_HEIGHT,
    VR_IMMERSION_HOG_SIZE,
    VR_IMMERSION_GHOST_REACH,
    VR_IMMERSION_GHOST_LEFT,
    VR_IMMERSION_GHOST_HEIGHT,
    VR_IMMERSION_GHOST_SIZE,
	/* TANK + BANSHEE */
    VR_IMMERSION_TANK_REACH,
    VR_IMMERSION_TANK_LEFT,
    VR_IMMERSION_TANK_HEIGHT,
    VR_IMMERSION_TANK_SIZE,
    VR_IMMERSION_BANSH_REACH,
    VR_IMMERSION_BANSH_LEFT,
    VR_IMMERSION_BANSH_UP,
    VR_IMMERSION_BANSHEE_SIZE,
	/* RAGDOLLS */
	VR_IMMERSION_DEATH_PHYSICS,
	VR_IMMERSION_DEATH_RAGDOLLS,
	VR_IMMERSION_ACTIVE_RAGDOLL,
	VR_IMMERSION_GRAB_RAGDOLL,
	VR_IMMERSION_DEATH_FORCE,
	VR_IMMERSION_RAGDOLL_RANGE,
	VR_IMMERSION_BODY_RETENTION,
	/* ADVANCED */
	VR_IMMERSION_IMPACT_DAMAGE,
	VR_IMMERSION_IMPACT_SPEED,
	VR_IMMERSION_IMPACT_CAP,
	VR_IMMERSION_NPC_RESCUE,
	VR_IMMERSION_NPC_RECOVERY,
	VR_IMMERSION_CUSTOM_SCENERY,
	VR_IMMERSION_CUSTOM_NPC_RIGS,
	VR_IMMERSION_NPC_RIG_SLOTS,
};

static struct vr_menu_section const vr_menu_immersion_sections[] =
{
	{ "WEAPONS", 8 }, { "PICKUPS", 4 }, { "FIST COMBAT", 8 }, { "FIST OPTIONS", 7 }, { "GRENADES", 6 }, { "RELOAD", 8 }, { "POUCH PLACES", 7 }, { "HOLSTER PLACES", 6 },
	{ "LOCOMOTION", 8 }, { "LOCOMOTION OPTIONS", 6 }, { "PARKOUR", 8 }, { "BODY", 4 }, { "GRAB + NPCS", 8 },
	{ "VEHICLES", 8 }, { "VEHICLE OPTIONS", 8 }, { "WARTHOG + GHOST", 8 }, { "TANK + BANSHEE", 8 },
	{ "RAGDOLLS", 7 }, { "ADVANCED", 8 },
};


/* Test54: immersion features turn on what they need. A hard requirement
(MAG DEBRIS needs MANUAL RELOAD, CUSTOM SCENERY needs WORLD GRAB, CUSTOM NPC
RIGS needs NPC GRAB) is turned on with its feature, and turning the
requirement off turns its features off. A soft one (FIST REQUIRED, so an open
hand reaching to grab or climb never strikes) is turned on with the first
grab or climb feature but may be turned off by hand. What was turned on only
for a feature is counted in vr.immersion_auto and turned off again when the
last feature needing it is; a setting chosen by hand stays as chosen. */
static struct vr_menu_dependency
{
	char const *feature, *requires;
	boolean soft;
} const vr_menu_dependency_table[] =
{
	{ "vr.assisted_combat", "vr.melee_requires_fist", FALSE },
	{ "vr.assisted_combat", "vr.melee", FALSE },
	{ "vr.assisted_combat", "vr.melee_speed", FALSE },
	{ "vr.assisted_combat", "vr.npc_interaction", TRUE },
	{ "vr.assisted_combat", "vr.projectile_parry", TRUE },
	{ "vr.assisted_combat", "vr.combat_moves", TRUE },
	/* Test63: ONE-HAND HEAVIES is a way of grabbing: it brings NPC GRAB */
	{ "vr.npc_one_hand_heavy", "vr.npc_interaction", FALSE },
	{ "vr.assisted_combat", "vr.close_contact", TRUE },
	/* Test66: grabbing works the moment it is on (the owner's Test65:
	GRAB ASSIST had to be found and raised): NPC GRAB and FIST COMBAT bring
	an assist of 10 cm from the body's surface (more chosen by hand stays) */
	{ "vr.npc_interaction", "vr.npc_grab_assist", TRUE },
	{ "vr.assisted_combat", "vr.npc_grab_assist", TRUE },
	{ "vr.dropped_magazines", "vr.manual_reload", FALSE },
	{ "vr.custom_scenery_grab", "vr.world_interaction", FALSE },
	{ "vr.custom_npc_rigs", "vr.npc_interaction", FALSE },
	{ "vr.npc_interaction", "vr.melee_requires_fist", TRUE },
	{ "vr.world_interaction", "vr.melee_requires_fist", TRUE },
	{ "vr.parkour_climb", "vr.melee_requires_fist", TRUE },
	{ "vr.vehicle_lift", "vr.melee_requires_fist", TRUE },
	/* Test60: what the bodies and grabs play best with comes on with them
	(the owner's Test59 session: DEATH RAGDOLLS on left NPC RESCUE off).
	Soft: turned on with the feature, and the player may still turn it off. */
	{ "vr.death_ragdolls", "vr.npc_interaction", TRUE },
	{ "vr.death_ragdolls", "vr.npc_rescue", TRUE },
	{ "vr.death_ragdolls", "vr.interaction_damage", TRUE },
	{ "vr.npc_interaction", "vr.npc_rescue", TRUE },
	{ "vr.npc_interaction", "vr.interaction_damage", TRUE },
	{ "vr.world_interaction", "vr.interaction_damage", TRUE },
	{ "vr.vehicle_lift", "vr.interaction_damage", TRUE },
	/* Test66: what is grabbed is thrown */
	{ "vr.vehicle_lift", "vr.object_throw", TRUE },
	{ "vr.world_interaction", "vr.object_throw", TRUE },
	{ "vr.holster_haptics", "vr.holsters", TRUE },
	/* the immersive vehicle controls' own refinements turn the controls on */
	{ "vr.vehicle_wheel_visible", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_control_glow", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_cockpit_lock", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_hand_lock", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_grip_guide", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_steer_haptics", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_comfort_reach", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_tank_drive", "vr.vehicle_handles", TRUE },
	{ "vr.vehicle_banshee_slide", "vr.vehicle_handles", TRUE },
	/* Test61: the immersive controls are only there in a first-person seat
	(the owner's Test60 session: IMMERSIVE VEHICLE on left VIEW in third
	person): VIEW goes to FIRST PERSON with them, and back to THIRD PERSON
	choosing it turns them off. The seat-only refinements (TANK HATCH, HOG
	GLASS) bring FIRST PERSON with them; the pouches MANUAL RELOAD; SPRINT
	TOGGLE BUTTON SPRINT. */
	{ "vr.vehicle_handles", "vr.vehicle_view", FALSE },
	{ "vr.vehicle_tank_hatch", "vr.vehicle_view", TRUE },
	{ "vr.vehicle_warthog_hide_glass", "vr.vehicle_view", TRUE },
	{ "vr.reload_shoulder", "vr.manual_reload", TRUE },
	{ "vr.reload_hip", "vr.manual_reload", TRUE },
	{ "vr.button_sprint_toggle", "vr.button_sprint", TRUE },
	/* Test70: a slide needs a sprint: BUTTON SPRINT, unless ARM RUN sprints
	already (vr_menu_requirement_met) */
	{ "vr.slide", "vr.button_sprint", TRUE },
	/* Test61: the grabs take every NPC and every piece of scenery (the owner
	turned CUSTOM NPC RIGS and CUSTOM SCENERY on by hand in Test60) */
	{ "vr.npc_interaction", "vr.custom_npc_rigs", TRUE },
	{ "vr.world_interaction", "vr.custom_scenery_grab", TRUE },
};

static boolean vr_menu_button_sprint_set(boolean enabled);
#include "vr_button_remap.h"

/* Test61: a requirement that is a choice, not a switch: the value it needs
(on) and the one it goes back to (off); NULL for a switch */
static char const *vr_menu_requirement_value(char const *requires, boolean on)
{
	if (!strcmp(requires, "vr.melee")) return on ? "impact" : "swing";
	if (!strcmp(requires, "vr.melee_speed")) return on ? "2.0" : "0";
	/* Test70: 15 cm (the owner raised it by hand: Halo's body bubble keeps
	the hand off the skin), 10 cm when no feature needs it */
	/* Test72: 30 cm (the owner had GRAB ASSIST at the old 25 cm maximum) */
	if (!strcmp(requires, "vr.npc_grab_assist")) return on ? "0.30" : "0.10";
	if (!strcmp(requires, "vr.object_throw")) return on ? "2" : "0";
	if (!strcmp(requires, "vr.vehicle_view"))
		return on ? "first_person" : "chase";
	return NULL;
}

/* Test61: whether a requirement holds, and setting it on or back off */
static boolean vr_menu_requirement_met(struct vr_menu_dependency const *dependency)
{
	char const *value = vr_menu_requirement_value(dependency->requires, TRUE);
	if (!strcmp(dependency->requires, "vr.melee_speed")) {
		real speed = config_real("vr.melee_speed");
		return isfinite(speed) && speed > 0.0f;
	}
	if (!strcmp(dependency->requires, "vr.object_throw")) {
		real strength = config_real("vr.object_throw");
		return isfinite(strength) && strength > 0.0f;
	}
	if (!strcmp(dependency->requires, "vr.npc_grab_assist")) {
		/* (Test72: 15 cm or more is a choice of the player's, kept; less is
		raised to the 30 cm above. Counting only 30 cm as enough raised a
		player's 20 cm, then dropped it to 10 cm when the feature went off) */
		real assist = config_real("vr.npc_grab_assist");
		return isfinite(assist) && assist >= 0.145f;
	}
	if (!strcmp(dependency->feature, "vr.slide") && !strcmp(dependency->requires, "vr.button_sprint"))
		return config_boolean("vr.button_sprint") || config_boolean("vr.arm_run");
	return value ? (boolean)config_matches(dependency->requires, value) : config_boolean(dependency->requires);
}

static void vr_menu_requirement_write(struct vr_menu_dependency const *dependency, boolean on)
{
	char const *value = vr_menu_requirement_value(dependency->requires, on);
	if (value)
		config_write_text(dependency->requires, value);
	else if (!strcmp(dependency->requires, "vr.button_sprint"))
		vr_menu_button_sprint_set(on); /* (it frees the left stick's click) */
	else
		config_write_boolean(dependency->requires, on);
}

static boolean vr_menu_auto_has(char const *list, char const *key)
{
	size_t length = strlen(key);
	char const *at = list;
	while (at && *at)
	{
		size_t item = strcspn(at, ",");
		if (item == length && !strncmp(at, key, length))
			return TRUE;
		at += item;
		if (*at == ',')
			at++;
	}
	return FALSE;
}

static void vr_menu_auto_set(char *list, size_t size, char const *key, boolean present)
{
	char result[256] = "";
	char const *at = list;
	while (*at)
	{
		size_t item = strcspn(at, ",");
		if (item && item < 96 && !(item == strlen(key) && !strncmp(at, key, item)) &&
			strlen(result) + item + 2 < sizeof(result))
		{
			if (*result)
				strcat(result, ",");
			strncat(result, at, item);
		}
		at += item;
		if (*at == ',')
			at++;
	}
	if (present && strlen(result) + strlen(key) + 2 < sizeof(result))
	{
		if (*result)
			strcat(result, ",");
		strcat(result, key);
	}
	snprintf(list, size, "%s", result);
}

/* Test61: `feature` is on only because `requirement` turned it on (CUSTOM NPC
RIGS by NPC GRAB): it does not keep `requirement` on in turn */
static boolean vr_menu_turned_on_by(char const *automatic, char const *feature, char const *requirement)
{
	long index;
	if (!vr_menu_auto_has(automatic, feature))
		return FALSE;
	for (index = 0; index < (long)NUMBEROF(vr_menu_dependency_table); index++)
		if (vr_menu_dependency_table[index].soft && !strcmp(vr_menu_dependency_table[index].feature, requirement) &&
			!strcmp(vr_menu_dependency_table[index].requires, feature))
			return TRUE;
	return FALSE;
}

/* whether this menu choice wrote `key` (the row's own key, or one a combined
row's value sets) */
static boolean vr_menu_choice_writes(struct vr_menu_setting const *setting, char const *value, char const *key)
{
	char pattern[96];
	if (!strcmp(setting->key, key))
		return TRUE;
	if (setting->type != _vr_setting_multi)
		return FALSE;
	snprintf(pattern, sizeof(pattern), "%s=", key);
	return strstr(value, pattern) != NULL;
}

static boolean vr_menu_dependencies(struct vr_menu_setting const *setting, char const *value)
{
	char automatic[256], before[256];
	int pass;
	long index;
	boolean changed = FALSE;

	snprintf(automatic, sizeof(automatic), "%s", config_string("vr.immersion_auto"));
	snprintf(before, sizeof(before), "%s", automatic);
	/* a requirement chosen by hand is the player's from now on */
	for (index = 0; index < (long)NUMBEROF(vr_menu_dependency_table); index++)
		if (vr_menu_choice_writes(setting, value, vr_menu_dependency_table[index].requires))
			vr_menu_auto_set(automatic, sizeof(automatic), vr_menu_dependency_table[index].requires, FALSE);
	for (pass = 0; pass < 3; pass++)
	{
		for (index = 0; index < (long)NUMBEROF(vr_menu_dependency_table); index++)
		{
			struct vr_menu_dependency const *dependency = &vr_menu_dependency_table[index];
			if (!config_boolean(dependency->feature) || vr_menu_requirement_met(dependency))
				continue;
			if (vr_menu_choice_writes(setting, value, dependency->requires))
			{
				/* the requirement was just turned off by hand */
				if (!dependency->soft)
				{
					config_write_boolean(dependency->feature, FALSE);
					changed = TRUE;
					platform_log("vr: %s off (it needs %s)", dependency->feature, dependency->requires);
				}
				continue;
			}
			/* a soft requirement turned off earlier stays off, unless its
			feature was just turned on: by this choice, or (Test60) by another
			feature this choice turned on (DEATH RAGDOLLS turns on NPC GRAB,
			and so what NPC GRAB plays best with) */
			if (dependency->soft && !vr_menu_choice_writes(setting, value, dependency->feature) &&
				!(vr_menu_auto_has(automatic, dependency->feature) && !vr_menu_auto_has(before, dependency->feature)))
				continue;
			vr_menu_requirement_write(dependency, TRUE);
			changed = TRUE;
			vr_menu_auto_set(automatic, sizeof(automatic), dependency->requires, TRUE);
			platform_log("vr: %s on (needed by %s)", dependency->requires, dependency->feature);
		}
		/* release what was turned on only for features now off */
		for (index = 0; index < (long)NUMBEROF(vr_menu_dependency_table); index++)
		{
			char const *requirement = vr_menu_dependency_table[index].requires;
			long other;
			boolean needed = FALSE;
			if (!vr_menu_auto_has(automatic, requirement))
				continue;
			for (other = 0; other < (long)NUMBEROF(vr_menu_dependency_table); other++)
				if (!strcmp(vr_menu_dependency_table[other].requires, requirement) &&
					config_boolean(vr_menu_dependency_table[other].feature) &&
					!vr_menu_turned_on_by(automatic, vr_menu_dependency_table[other].feature, requirement))
					needed = TRUE;
			if (!needed)
			{
				vr_menu_requirement_write(&vr_menu_dependency_table[index], FALSE);
				changed = TRUE;
				vr_menu_auto_set(automatic, sizeof(automatic), requirement, FALSE);
				platform_log("vr: %s off (no feature needs it now)", requirement);
			}
		}
	}
	if (strcmp(before, automatic))
		config_write_string("vr.immersion_auto", automatic);
	return changed;
}

/* a page of settings, opened from the categories: its first `left_count`
settings in the left column, the rest in the right */
static struct vr_menu_page
{
	char const *label;
	struct vr_menu_setting const *settings;
	long count;
	struct vr_menu_section const *sections;
	long section_count;
} const vr_menu_pages[] =
{
	{ "IMMERSION", vr_menu_immersion, NUMBEROF(vr_menu_immersion),
		vr_menu_immersion_sections, NUMBEROF(vr_menu_immersion_sections) },
	{ "CONTROLS", vr_menu_controls, NUMBEROF(vr_menu_controls) },
	{ "BUTTONS", vr_menu_buttons, NUMBEROF(vr_menu_buttons) },
	{ "VOICE CHAT", vr_menu_voice, NUMBEROF(vr_menu_voice) },
	{ "LOCOMOTION", vr_menu_locomotion, NUMBEROF(vr_menu_locomotion) },
	{ "COMFORT", vr_menu_comfort, NUMBEROF(vr_menu_comfort) },
	{ "BODY", vr_menu_body, NUMBEROF(vr_menu_body) },
	{ "HANDS + GUN", vr_menu_hands, NUMBEROF(vr_menu_hands) },
	{ "SCOPES", vr_menu_scopes, NUMBEROF(vr_menu_scopes),
		vr_menu_scopes_sections, NUMBEROF(vr_menu_scopes_sections) },
	{ "GAMEPLAY", vr_menu_vr, NUMBEROF(vr_menu_vr) },
	{ "HUD + RETICLE", vr_menu_hud, NUMBEROF(vr_menu_hud) },
	{ "HEAD GESTURES", vr_menu_gestures, NUMBEROF(vr_menu_gestures) },
	{ "VEHICLES", vr_menu_vehicles, NUMBEROF(vr_menu_vehicles),
		vr_menu_vehicles_sections, NUMBEROF(vr_menu_vehicles_sections) },
	{ "GRAPHICS", vr_menu_graphics, NUMBEROF(vr_menu_graphics) },
	{ "DISPLAY", vr_menu_effects, NUMBEROF(vr_menu_effects) },
	/* advanced: tracking corrections that move hand and gun together */
	{ "CONTROLLER LEFT", vr_menu_align_left, NUMBEROF(vr_menu_align_left) },
	{ "CONTROLLER RIGHT", vr_menu_align_right, NUMBEROF(vr_menu_align_right) },
};

#define VR_MENU_PAGE_COUNT ((long)NUMBEROF(vr_menu_pages))
/* Four rows per column. The fifth left row is reserved for navigation;
 * the fifth right row belongs to the stock button hints. Capacity derives
 * from the definitions, so new settings/categories create further screens. */
#define VR_MENU_SETTINGS_PER_SCREEN 8
#define VR_MENU_PAGES(n) (((n) + VR_MENU_SETTINGS_PER_SCREEN - 1) / VR_MENU_SETTINGS_PER_SCREEN)
/* Test63: a category of this many screens or more opens an index of them */
#define VR_MENU_INDEX_FROM 3
#define VR_MENU_MAXIMUM_SCREENS 64


/* a combined row's value, "key=value;key=value": whether every setting
holds its value, or (write) each written; FALSE for a malformed value */
static boolean vr_menu_multi(char const *values, boolean write, boolean *written)
{
	char pair[96];
	char const *at = values;
	boolean all = TRUE;

	while (*at)
	{
		size_t length = strcspn(at, ";");
		char *equals;

		if (length >= sizeof(pair))
			return FALSE;
		memcpy(pair, at, length);
		pair[length] = 0;
		at += length + (at[length] == ';');
		equals = strchr(pair, '=');
		if (!equals)
			return FALSE;
		*equals = 0;
		if (write)
			*written = config_write_text(pair, equals + 1) && *written;
		else if (!config_matches(pair, equals + 1))
			all = FALSE;
	}
	return all;
}

/* both hands' angle on one axis, as the right hand's (the left's is its
mirror image: the same pitch, yaw and roll the other way). Left-handed, the
left hand's is shown, mirrored */
static double vr_menu_hand_angle(char const *axis)
{
	char key[64];
	double value, sign = strcmp(axis, "pitch") ? -1.0 : 1.0;
	boolean left = config_boolean("vr.left_handed");

	snprintf(key, sizeof(key), "vr.hand_%s_%s", left ? "left" : "right", axis);
	value = config_real(key);
	if (!isfinite(value))
		value = 0.0;
	return (left ? value * sign : value) + 0.0; /* never "-0" */
}

/* test23: a remappable button's action (VR_BUTTON_ACTION_*, NONE: not
one), and the button it has, as the game reads it */
/* test24b: the snap angle: the one snapping now, or the one kept for it
while turning smoothly (vr.snap_turn_amount) */
static double vr_menu_snap_angle(void)
{
	double angle = config_real("vr.snap_turn");

	if (!(angle > 0.0))
		angle = config_real("vr.snap_turn_amount");
	return angle > 0.0 && angle <= 180.0 ? angle : 45.0;
}

static int vr_menu_button_action(char const *key)
{
	int action;

	for (action = 0; action < VR_BUTTON_ACTIONS; action++)
	{
		if (!strcmp(key, vr_button_action_key(action)))
			return action;
	}
	return NONE;
}

static int vr_menu_button_source(int action)
{
	return vr_button_source_of(config_string(vr_button_action_key(action)), vr_button_default(action));
}

/* Button Sprint owns left stick click while enabled. Preserve its old action,
 * then restore only bindings the user has not changed in the meantime. */
static boolean vr_menu_button_sprint_set(boolean enabled)
{
	boolean current = config_boolean("vr.button_sprint");
	long saved = config_integer("vr.button_sprint_saved_mask");
	int action;
	boolean written = TRUE;
	if (current == enabled) return TRUE;
	if (enabled)
	{
		long mask = 0;
		char text[16];
		for (action = 0; action < VR_BUTTON_ACTIONS; ++action)
			if (vr_menu_button_source(action) == VR_BUTTON_SOURCE_LEFT_STICK)
				mask |= 1L << action;
		snprintf(text, sizeof(text), "%ld", mask);
		written = config_write("vr.button_sprint_saved_mask", text);
		for (action = 0; action < VR_BUTTON_ACTIONS; ++action)
			if (written && (mask & (1L << action)))
				written = config_write_string(vr_button_action_key(action), "none");
		if (written) written = config_write_boolean("vr.button_sprint", TRUE);
		if (!written)
		{
			for (action = 0; action < VR_BUTTON_ACTIONS; ++action)
				if (mask & (1L << action))
					config_write_string(vr_button_action_key(action), "left_stick");
			config_write("vr.button_sprint_saved_mask", "0");
		}
	}
	else
	{
		/* (Test70: every action's bit; 255 dropped push-to-talk's, bit 8) */
		if (saved < 0 || saved >= (1L << VR_BUTTON_ACTIONS)) saved = 0;
		for (action = 0; action < VR_BUTTON_ACTIONS; ++action)
		{
			if (!(saved & (1L << action))) continue;
			if (vr_menu_button_source(action) == VR_BUTTON_SOURCE_NONE)
				written = config_write_string(vr_button_action_key(action), "left_stick") && written;
			else
				platform_log("vr: did not restore %s on left stick click; it was changed while Button Sprint was enabled",
					vr_button_action_key(action));
		}
		if (written) written = config_write_boolean("vr.button_sprint", FALSE);
		if (written) written = config_write("vr.button_sprint_saved_mask", "0");
	}
	vr_reload_settings();
	platform_log("vr: button sprint %s%s", enabled ? "enabled (left stick click)" : "disabled",
		written ? "" : " (config write failed)");
	return written;
}

/* test23: `action` takes `source`; another action on that button takes
this one's old button (the switch grenade's "hold" is no button: the other
is left with none), so no button ever does two actions. The grenade put on
another action's button leaves "switch grenade" to holding it */
static boolean vr_menu_button_set(int action, int source)
{
	int other, old = vr_menu_button_source(action), displaced = FALSE;
	boolean written = config_write_string(vr_button_action_key(action), vr_button_source_value(source));

	if (source == VR_BUTTON_SOURCE_NONE || source == VR_BUTTON_SOURCE_HOLD)
		return written;
	for (other = 0; other < VR_BUTTON_ACTIONS; other++)
	{
		int moved;

		if (other == action || vr_menu_button_source(other) != source)
			continue;
		moved = old == VR_BUTTON_SOURCE_HOLD ? VR_BUTTON_SOURCE_NONE : old;
		if (other == VR_BUTTON_ACTION_SWITCH_GRENADE && action == VR_BUTTON_ACTION_GRENADE)
			moved = VR_BUTTON_SOURCE_HOLD;
		displaced |= moved == old;
		written = config_write_string(vr_button_action_key(other), vr_button_source_value(moved)) && written;
		platform_log("vr: %s was on %s, moved to %s", vr_button_action_key(other),
			vr_button_source_value(source), vr_button_source_value(moved));
	}
	/* the grip throws while held and cannot be held to switch grenades:
	switching takes the grenade's old button (the layout locked weapons had
	before 1.0.5), unless the grip's old action just took that button */
	if (action == VR_BUTTON_ACTION_GRENADE && source == VR_BUTTON_SOURCE_GRIP && !displaced &&
		vr_menu_button_source(VR_BUTTON_ACTION_SWITCH_GRENADE) == VR_BUTTON_SOURCE_HOLD &&
		old != VR_BUTTON_SOURCE_GRIP)
	{
		written = config_write_string(vr_button_action_key(VR_BUTTON_ACTION_SWITCH_GRENADE), vr_button_source_value(old)) && written;
		platform_log("vr: vr.button_switch_grenade moved to %s (the grip cannot be held to switch)", vr_button_source_value(old));
	}
	return written;
}

/* Test70: a button row edited in place (vr_button_remap.h): A, or left
and right, starts choosing on that row with nothing written; A sets the
choice. One that another action has asks what to do (CANCEL, SWAP, CLEAR
the other, USE BOTH), chosen with left and right and taken with A. The
choosing lapses after a while untouched, or when another row changes. */
static long vr_menu_value_index(struct vr_menu_setting const *setting);
unsigned long system_milliseconds(void);
enum { VR_REMAP_IDLE, VR_REMAP_CHOOSING, VR_REMAP_CONFLICT };
#define VR_REMAP_TIMEOUT_MS 20000
static struct
{
	int state, action, value, other, resolution;
	unsigned long at;
} vr_remap;

static void vr_menu_remap_idle(void)
{
	memset(&vr_remap, 0, sizeof(vr_remap));
	vr_remap.state = VR_REMAP_IDLE;
}

static boolean vr_menu_remap_active(int action)
{
	if (vr_remap.state != VR_REMAP_IDLE &&
		(unsigned long)(system_milliseconds() - vr_remap.at) > VR_REMAP_TIMEOUT_MS)
		vr_menu_remap_idle();
	return vr_remap.state != VR_REMAP_IDLE && vr_remap.action == action;
}

/* an action's row label on the BUTTONS page ("USE / RELOAD") */
static char const *vr_menu_button_label(int action)
{
	long index;
	char const *key = vr_button_action_key(action);

	for (index = 0; index < (long)NUMBEROF(vr_menu_buttons); index++)
		if (vr_menu_buttons[index].type == _vr_setting_button && !strcmp(vr_menu_buttons[index].key, key))
			return vr_menu_buttons[index].label;
	return key;
}

/* every action's button now */
static void vr_menu_button_sources(int *sources)
{
	int action;

	for (action = 0; action < VR_BUTTON_ACTIONS; action++)
		sources[action] = vr_menu_button_source(action);
}

/* the row's value index of a button (NONE: not among its values) */
static long vr_menu_button_value(struct vr_menu_setting const *setting, int source)
{
	long index;

	for (index = 0; index < setting->value_count; index++)
		if (!strcmp(setting->values[index].value, vr_button_source_value(source)))
			return index;
	return NONE;
}

/* the row's value Button Sprint keeps (the left stick), or -1 */
static int vr_menu_button_sprint_skip(struct vr_menu_setting const *setting)
{
	return config_boolean("vr.button_sprint") ?
		(int)vr_menu_button_value(setting, VR_BUTTON_SOURCE_LEFT_STICK) : -1;
}

/* Writes the buttons that differ from `before` */
static boolean vr_menu_button_sources_write(int const *before, int const *after)
{
	int action;
	boolean written = TRUE;

	for (action = 0; action < VR_BUTTON_ACTIONS; action++)
	{
		if (before[action] == after[action])
			continue;
		written = config_write_string(vr_button_action_key(action), vr_button_source_value(after[action])) && written;
		platform_log("vr: %s set to %s (was %s)", vr_button_action_key(action),
			vr_button_source_value(after[action]), vr_button_source_value(before[action]));
	}
	return written;
}

/* A, left or right on a button row (step 0 is A) */
static void vr_menu_button_row_change(struct vr_menu_setting const *setting, int action, long step)
{
	int sources[VR_BUTTON_ACTIONS], before[VR_BUTTON_ACTIONS];
	int skip = vr_menu_button_sprint_skip(setting);
	long value_index;

	vr_menu_button_sources(sources);
	memcpy(before, sources, sizeof(before));
	if (!vr_menu_remap_active(action))
	{
		vr_menu_remap_idle();
		value_index = vr_menu_value_index(setting);
		vr_remap.state = VR_REMAP_CHOOSING;
		vr_remap.action = action;
		vr_remap.value = value_index == NONE ? 0 : (int)value_index;
		vr_remap.at = system_milliseconds();
		/* left or right also steps at once: the row shows the next choice */
		if (step)
			vr_remap.value = vr_remap_step(vr_remap.value, (int)step, (int)setting->value_count, skip);
		platform_log("vr: choosing a button for %s", vr_button_action_key(action));
		return;
	}
	vr_remap.at = system_milliseconds();
	if (vr_remap.state == VR_REMAP_CHOOSING)
	{
		int source = vr_button_source_of(setting->values[vr_remap.value].value, VR_BUTTON_SOURCE_NONE);

		if (step)
		{
			vr_remap.value = vr_remap_step(vr_remap.value, (int)step, (int)setting->value_count, skip);
			return;
		}
		vr_remap.other = vr_remap_conflict(sources, VR_BUTTON_ACTIONS, action, source,
			VR_BUTTON_SOURCE_NONE, VR_BUTTON_SOURCE_HOLD);
		if (source != sources[action] && vr_remap.other >= 0)
		{
			/* the player decides; CANCEL is offered first */
			vr_remap.state = VR_REMAP_CONFLICT;
			vr_remap.resolution = VR_REMAP_CANCEL;
			platform_log("vr: %s is %s's: asking (cancel, swap, clear, use both)",
				vr_button_source_value(source), vr_button_action_key(vr_remap.other));
			return;
		}
		if (source != sources[action])
		{
			vr_remap_apply(sources, VR_BUTTON_ACTIONS, action, source, -1, VR_REMAP_SWAP,
				VR_BUTTON_SOURCE_NONE, VR_BUTTON_SOURCE_HOLD);
			if (!vr_menu_button_sources_write(before, sources))
				platform_log("vr: %s not saved to config.toml", vr_button_action_key(action));
			vr_reload_settings();
		}
		vr_menu_remap_idle();
		return;
	}
	/* VR_REMAP_CONFLICT */
	if (step)
	{
		vr_remap.resolution = (vr_remap.resolution + (step < 0 ? -1 : 1) + VR_REMAP_RESOLUTIONS) % VR_REMAP_RESOLUTIONS;
		return;
	}
	{
		int source = vr_button_source_of(setting->values[vr_remap.value].value, VR_BUTTON_SOURCE_NONE);

		if (vr_remap_apply(sources, VR_BUTTON_ACTIONS, action, source, vr_remap.other, vr_remap.resolution,
			VR_BUTTON_SOURCE_NONE, VR_BUTTON_SOURCE_HOLD))
		{
			if (!vr_menu_button_sources_write(before, sources))
				platform_log("vr: %s not saved to config.toml", vr_button_action_key(action));
			vr_reload_settings();
		}
		platform_log("vr: %s on %s: %s", vr_button_action_key(action), vr_button_source_value(source),
			vr_remap.resolution == VR_REMAP_CANCEL ? "cancelled" : vr_remap.resolution == VR_REMAP_SWAP ? "swapped" :
			vr_remap.resolution == VR_REMAP_CLEAR ? "the other cleared" : "both on one button");
		vr_menu_remap_idle();
	}
}

/* Test70: a button's mark on its row: '*' it does nothing for now (Button
Sprint has the left stick's click; HOLD with the grenade on the grip; the grip
with physical weapons), '+' another action has it too; 0 none. The hints line
says what they mean (vr_menu_button_hints) */
static char vr_menu_button_mark(int action, int const *sources)
{
	int source = sources[action];

	if (source == VR_BUTTON_SOURCE_NONE && config_boolean("vr.button_sprint") &&
		(config_integer("vr.button_sprint_saved_mask") & (1L << action)))
		return '*';
	if (action == VR_BUTTON_ACTION_SWITCH_GRENADE && source == VR_BUTTON_SOURCE_HOLD &&
		sources[VR_BUTTON_ACTION_GRENADE] == VR_BUTTON_SOURCE_GRIP)
		return '*';
	if (source == VR_BUTTON_SOURCE_GRIP && !strcmp(config_string("vr.weapons"), "physical"))
		return '*';
	if (vr_remap_conflict(sources, VR_BUTTON_ACTIONS, action, source, VR_BUTTON_SOURCE_NONE, VR_BUTTON_SOURCE_HOLD) >= 0)
		return '+';
	return 0;
}

/* Test72: a button choice as the player's controllers name it: with the
face buttons mirrored (LEFT-HAND MODE), the gun hand's A and B are the left
controller's X and Y, and X and Y the right controller's A and B. The
binding saved stays the logical one. */
static char const *vr_menu_value_label(struct vr_menu_setting const *setting, int index)
{
	char const *value, *label;

	if (!setting || index < 0 || index >= setting->value_count)
		return "";
	value = setting->values[index].value;
	label = setting->values[index].label;
	if (setting->type != _vr_setting_button || !value)
		return label;
	{
		/* the controller (0 left, 1 right) and the button (0 lower, 1
		upper) the logical choice is: A and B the gun hand's, X and Y the
		other's, the gun hand the left one with the face buttons mirrored */
		int lower = !strcmp(value, "a") || !strcmp(value, "x");
		int gun_side = !strcmp(value, "a") || !strcmp(value, "b");
		int hand;
		if (!lower && strcmp(value, "b") && strcmp(value, "y"))
			return label;
		hand = gun_side ? (vr_buttons_mirrored() ? 0 : 1) : (vr_buttons_mirrored() ? 1 : 0);
		/* Test72: the controllers' own names: an Index's left controller has
		A and B too; the Touch, Pico, Frame and HP controllers X and Y */
		if (vr_controller_profile() == 2 /* HALO_XR_PROFILE_INDEX */)
			return hand ? (lower ? "A" : "B") : (lower ? "L-A" : "L-B");
		return hand ? (lower ? "A" : "B") : (lower ? "X" : "Y");
	}
}

/* A button row's text, kept to the row's width: its button and mark; while
choosing, the choice with a '?' (not yet set); while asking, the answer
chosen with a '?' */
static void vr_menu_button_row_text(struct vr_menu_setting const *setting, int action, char *line, size_t size)
{
	int sources[VR_BUTTON_ACTIONS];
	int source = vr_menu_button_source(action);
	char mark;

	vr_menu_button_sources(sources);
	if (vr_menu_remap_active(action))
	{
		static char const *const resolutions[VR_REMAP_RESOLUTIONS] = { "CANCEL", "SWAP", "CLEAR", "BOTH" };
		snprintf(line, size, "%s: %s?", setting->label, vr_remap.state == VR_REMAP_CONFLICT ?
			resolutions[vr_remap.resolution] : vr_menu_value_label(setting, vr_remap.value));
		return;
	}
	mark = vr_menu_button_mark(action, sources);
	/* Button Sprint set the action's button aside: the left stick's click is
	what it gets back */
	if (mark == '*' && source == VR_BUTTON_SOURCE_NONE)
	{
		snprintf(line, size, "%s: L STICK*", setting->label);
		return;
	}
	snprintf(line, size, mark ? "%s: %s%c" : "%s: %s", setting->label,
		vr_menu_button_value(setting, source) != NONE ? vr_menu_value_label(setting, vr_menu_button_value(setting, source)) : "CUSTOM",
		mark);
}

/* Test70: the hints line while a button row is being chosen (what A does,
whose a taken button is), or what a row's mark means; FALSE: the usual hints */
static boolean vr_menu_button_hints(char *line, size_t size)
{
	int sources[VR_BUTTON_ACTIONS], action, stars = 0, pluses = 0;

	vr_menu_button_sources(sources);
	if (vr_remap.state != VR_REMAP_IDLE && vr_menu_remap_active(vr_remap.action))
	{
		long index;
		struct vr_menu_setting const *setting = NULL;
		char const *candidate, *other;

		for (index = 0; index < (long)NUMBEROF(vr_menu_buttons); index++)
			if (vr_menu_buttons[index].type == _vr_setting_button &&
				vr_menu_button_action(vr_menu_buttons[index].key) == vr_remap.action)
				setting = &vr_menu_buttons[index];
		if (!setting)
			return FALSE;
		candidate = vr_menu_value_label(setting, vr_remap.value);
		if (vr_remap.state == VR_REMAP_CONFLICT)
		{
			other = vr_menu_button_label(vr_remap.other);
			switch (vr_remap.resolution)
			{
			case VR_REMAP_CANCEL: snprintf(line, size, "%s IS %s'S. A: KEEP AS IT WAS", candidate, other); break;
			case VR_REMAP_SWAP: snprintf(line, size, "A: SWAP WITH %s", other); break;
			case VR_REMAP_CLEAR: snprintf(line, size, "A: TAKE %s, %s GETS NONE", candidate, other); break;
			default: snprintf(line, size, "A: %s DOES BOTH", candidate); break;
			}
			return TRUE;
		}
		{
			int source = vr_button_source_of(setting->values[vr_remap.value].value, VR_BUTTON_SOURCE_NONE);
			int taken = source == sources[vr_remap.action] ? -1 :
				vr_remap_conflict(sources, VR_BUTTON_ACTIONS, vr_remap.action, source, VR_BUTTON_SOURCE_NONE, VR_BUTTON_SOURCE_HOLD);
			if (taken >= 0)
				snprintf(line, size, "%s IS %s'S. A: ASK  < >: CHANGE", candidate, vr_menu_button_label(taken));
			else
				snprintf(line, size, "A: SET %s%s  < >: CHANGE", candidate,
					source == vr_button_default(vr_remap.action) ? " (DEFAULT)" : "");
		}
		return TRUE;
	}
	for (action = 0; action < VR_BUTTON_ACTIONS; action++)
	{
		char mark = vr_menu_button_mark(action, sources);
		stars += mark == '*';
		pluses += mark == '+';
	}
	if (!stars && !pluses)
		return FALSE;
	snprintf(line, size, "%s%s%sA: CHOOSE  B: BACK", stars ? "*: OFF FOR NOW  " : "", pluses ? "+: SHARED  " : "", "");
	return TRUE;
}

/* the setting's value now, as an index into its values (NONE: none of them) */
static long vr_menu_value_index(
	struct vr_menu_setting const *setting)
{
	long index;

	for (index = 0; index < setting->value_count; index++)
	{
		char const *value = setting->values[index].value;

		switch (setting->type)
		{
		case _vr_setting_boolean:
		case _vr_setting_handedness:
			if (config_boolean(setting->key) == !strcmp(value, "true"))
				return index;
			break;
		case _vr_setting_multi:
			if (vr_menu_multi(value, FALSE, NULL))
				return index;
			break;
		case _vr_setting_real:
		case _vr_setting_real_choice:
		{
			double current = config_real(setting->key);
			/* Old configs stored 12-40 cm as ray reach. The same key now
			 * stores a 2-8 cm palm-contact radius; display its bounded effective
			 * value instead of leaving HAND CONTACT marked CUSTOM. */
			if (!strcmp(setting->key, "vr.interaction_reach"))
				current = current < 0.02 ? 0.02 : current > 0.08 ? 0.08 : current;
			if (fabs(current - atof(value)) < 0.001)
				return index;
			break;
		}
		case _vr_setting_string:
			if (!strcmp(config_string(setting->key), value))
				return index;
			break;
		case _vr_setting_turn_mode:
			if ((config_real("vr.snap_turn") > 0.0) == !strcmp(value, "snap"))
				return index;
			break;
		case _vr_setting_snap_angle:
			if (fabs(vr_menu_snap_angle() - atof(value)) < 0.001)
				return index;
			break;
		case _vr_setting_button:
			if (!strcmp(vr_button_source_value(vr_menu_button_source(vr_menu_button_action(setting->key))), value))
				return index;
			break;
		case _vr_setting_crouch_click:
			if (vr_menu_button_source(!strcmp(value, "crouch") ? VR_BUTTON_ACTION_CROUCH : VR_BUTTON_ACTION_RETICLE) ==
				VR_BUTTON_SOURCE_LEFT_STICK)
				return index;
			break;
		case _vr_setting_hud_shown:
			if (!vr_hud_hidden() == !strcmp(value, "true"))
				return index;
			break;
		}
	}
	return NONE;
}

/* ---------- the tags */

/* Test63: jump: 0 a NEXT row; 1 an index row opening a topic; 2 an index
row opening a page (its first setting named); 3 the index's MORE row */
struct vr_menu_navigation { long tag, next, total; char const *label; int jump; };
/* cseries_windows.c's: when RESET BUTTONS was last pressed, for its DONE */
unsigned long system_milliseconds(void);
static unsigned long vr_menu_buttons_reset_at = 0;
#define VR_MENU_RESET_DONE_MS 2000

static struct
{
	/* the tags this made for the map loaded, and what they point at: the
	pause menu's item, the categories' screen and its buttons, and each
	page's screen and buttons */
	long button_tag_index, categories_tag_index;
	long title_tag_index, hints_tag_index;
	boolean loaded;
	long category_tag_indices[NUMBEROF(vr_menu_pages)];
	long page_tag_indices[NUMBEROF(vr_menu_pages)];
	long *setting_tag_indices[NUMBEROF(vr_menu_pages)];
 struct vr_menu_navigation *navigation;
 long navigation_count, navigation_capacity;
 /* Test72: the title the next screen built shows (a sectioned page's
 topic), else the shared "VR SETTINGS" one */
 long screen_title_tag_index;
 void **allocations;
 long allocation_count, allocation_capacity;
} vr_menu;

/* Track lifetime without a fixed allocation/name ceiling. All allocations
 * are made while map tags load, never in the render loop. */
static void *vr_menu_allocate(long size)
{
    void *memory;
    if (size <= 0) return NULL;
    if (vr_menu.allocation_count == vr_menu.allocation_capacity) {
        long capacity = vr_menu.allocation_capacity ? vr_menu.allocation_capacity * 2 : 128;
        void **grown;
        if (capacity <= vr_menu.allocation_capacity || capacity > 32768) return NULL;
        grown = malloc(capacity * sizeof(*grown));
        if (!grown) return NULL;
        if (vr_menu.allocations) {
            memcpy(grown, vr_menu.allocations, vr_menu.allocation_count * sizeof(*grown));
            free(vr_menu.allocations);
        }
        vr_menu.allocations = grown; vr_menu.allocation_capacity = capacity;
    }
    memory = malloc((size_t)size);
    if (memory) {
        memset(memory, 0, (size_t)size);
        vr_menu.allocations[vr_menu.allocation_count++] = memory;
    }
    return memory;
}

/* Each tag retains its own name until map teardown. */
static char *vr_menu_name(char const *format, long a, long b)
{
    char *name = vr_menu_allocate(64);
    if (name) snprintf(name, 64, format, a, b);
    return name;
}

static struct vr_menu_widget *vr_menu_widget_get(
	long tag_index)
{
	return tag_index != NONE ? tag_get(VR_MENU_WIDGET_TAG, tag_index) : NULL;
}

static void vr_menu_reference(
	struct tag_reference *reference,
	long tag_index)
{
	reference->group_tag = VR_MENU_WIDGET_TAG;
	reference->name = tag_index != NONE ? tag_get_name(tag_index) : "";
	reference->name_length = (long)strlen(reference->name);
	reference->index = tag_index;
}

/* a widget cloned from `template_index`, named, added as a tag of its own */
static long vr_menu_clone(
	long template_index,
	char *tag_name,
	char const *widget_name,
	struct vr_menu_widget **out_widget)
{
	struct vr_menu_widget *source = vr_menu_widget_get(template_index);
	struct vr_menu_widget *widget = tag_name && source ? vr_menu_allocate(sizeof(*widget)) : NULL;
	long tag_index;

	*out_widget = NULL;
	if (!widget)
		return NONE;
	memcpy(widget, source, sizeof(*widget));
	memset(widget->name, 0, sizeof(widget->name));
	strncpy(widget->name, widget_name, sizeof(widget->name) - 1);
	tag_index = cache_file_add_tag(VR_MENU_WIDGET_TAG, (unsigned long)NONE, tag_name, widget);
	if (tag_index != NONE)
		*out_widget = widget;
	return tag_index;
}

static boolean vr_menu_handlers(
	struct vr_menu_widget *widget,
	struct vr_menu_event_handler const *handlers,
	long count)
{
	struct vr_menu_event_handler *copy;
    if (!count) {
        widget->event_handlers.count = 0;
        widget->event_handlers.address = NULL;
        return TRUE;
    }
    if (count < 0 || !handlers) return FALSE;
    copy = vr_menu_allocate(count * (long)sizeof(*copy));

	if (!copy)
		return FALSE;
	memcpy(copy, handlers, (size_t)count * sizeof(*copy));
	widget->event_handlers.count = count;
	widget->event_handlers.address = copy;
	return TRUE;
}

/* a button cloned from the pause menu's "RESUME GAME", its text from code */
static long vr_menu_button(
	long template_index,
	char *tag_name,
	char const *widget_name,
	struct vr_menu_event_handler const *handlers,
	long handler_count)
{
	struct vr_menu_widget *widget;
	struct vr_menu_game_data_input *input;
	long tag_index = vr_menu_clone(template_index, tag_name, widget_name, &widget);

	if (tag_index == NONE || !(input = vr_menu_allocate(sizeof(*input))) ||
		!vr_menu_handlers(widget, handlers, handler_count))
	{
		return NONE;
	}
	/* test25: as wide as a column (the text is laid out and clipped in the
	button's own bounds: the pause menu's are narrower, and longer rows lost
	their ends, "ALL FORWARD: < 0 C") */
	if (widget->bounds.x1 - widget->bounds.x0 < VR_MENU_BUTTON_WIDTH)
	{
		static boolean logged;

		if (!logged)
			platform_log("vr: VR settings rows widened from %d to %d", widget->bounds.x1 - widget->bounds.x0,
				VR_MENU_BUTTON_WIDTH);
		logged = TRUE;
		widget->bounds.x1 = (short)(widget->bounds.x0 + VR_MENU_BUTTON_WIDTH);
	}
	input->function = VR_MENU_GAME_DATA_FUNCTION;
	widget->game_data_inputs.count = 1;
	widget->game_data_inputs.address = input;
	widget->text_label_string_list.index = NONE;
	widget->text_label_string_list.name = "";
	widget->text_label_string_list.name_length = 0;
	return tag_index;
}

/* one handler, opening `widget_index` on A (NONE: the handler set later) */
static void vr_menu_open_handler(
	struct vr_menu_event_handler *handler,
	long widget_index)
{
	memset(handler, 0, sizeof(*handler));
	handler->flags = VR_MENU_OPEN_WIDGET;
	handler->event_type = VR_MENU_EVENT_A;
	vr_menu_reference(&handler->widget_tag, widget_index);
	vr_menu_reference(&handler->sound_effect, NONE);
}

/* Native DeLa definitions, independent of a map's optional solo pause tags.
 * All references are initialized to NONE, including fields a container does
 * not currently use. A zero tag index would name an unrelated map asset. */
static long vr_menu_native(char *name, char const *label, short type,
    short width, short height, struct vr_menu_widget **out)
{
    struct vr_menu_widget *widget = name ? vr_menu_allocate(sizeof(*widget)) : NULL;
    long index;
    *out = NULL;
    if (!widget) return NONE;
    widget->type = type;
    widget->controller_index = 4; /* inherit the invoking controller */
    strncpy(widget->name, label, sizeof(widget->name) - 1);
    widget->bounds.x1 = width;
    widget->bounds.y1 = height;
    widget->flags = type == 3 ? (1 | (1 << 5)) : type == 0 ? (1 | (1 << 1)) : 0;
    vr_menu_reference(&widget->background_bitmap, NONE);
    widget->background_bitmap.group_tag = 'bitm';
    vr_menu_reference(&widget->text_label_string_list, NONE);
    widget->text_label_string_list.group_tag = 'ustr';
    vr_menu_reference(&widget->text_font, NONE);
    widget->text_font.group_tag = 'font';
    vr_menu_reference(&widget->list_header_bitmap, NONE);
    widget->list_header_bitmap.group_tag = 'bitm';
    vr_menu_reference(&widget->list_footer_bitmap, NONE);
    widget->list_footer_bitmap.group_tag = 'bitm';
    vr_menu_reference(&widget->extended_description_widget, NONE);
    index = cache_file_add_tag(VR_MENU_WIDGET_TAG, (unsigned long)NONE, name, widget);
    if (index != NONE) *out = widget;
    return index;
}

static void vr_menu_child_set(struct vr_menu_child *child, long tag, short x, short y)
{
    memset(child, 0, sizeof(*child));
    vr_menu_reference(&child->widget_tag, tag);
    child->horizontal_offset = x;
    child->vertical_offset = y;
    child->custom_controller_index = 4;
    strcpy(child->name, "vr_settings");
}

/* Four settings per column, plus a separate NEXT row. Fixed 640x480 menu
 * coordinates are also the native pointer/Quest laser hit-test space. */
static long vr_menu_screen(
    long pause_index, long list_index, char *screen_name, char *list_name,
    char const *widget_name, long const *buttons, long button_count,
    long left_count, short column_x, short hints_x)
{
    struct vr_menu_widget *screen, *list;
    struct vr_menu_child *children;
    struct vr_menu_event_handler handlers[3];
    long list_tag_index, screen_tag_index, index;
    (void)hints_x;
    if (button_count <= 0 || button_count > VR_MENU_SETTINGS_PER_SCREEN + 1 ||
        left_count != 4 || column_x < VR_MENU_BUTTON_WIDTH || column_x + VR_MENU_BUTTON_WIDTH > 512)
        return NONE;
    list_tag_index = vr_menu_clone(list_index, list_name, "vr settings list", &list);
    if (list_tag_index == NONE || !(children = vr_menu_allocate(button_count * sizeof(*children))))
        return NONE;
    for (index = 0; index < button_count; index++) {
        boolean navigation = index == VR_MENU_SETTINGS_PER_SCREEN;
        long row = navigation ? 4 : (index < left_count ? index : index - left_count);
        if (buttons[index] == NONE) return NONE;
        vr_menu_child_set(&children[index], buttons[index],
            navigation || index < left_count ? 0 : column_x, (short)(row * 28));
    }
    list->bounds.x1 = (short)(column_x + VR_MENU_BUTTON_WIDTH);
    list->bounds.y1 = 5 * 28;
    list->child_widgets.count = button_count;
    list->child_widgets.address = children;
    screen_tag_index = vr_menu_clone(pause_index, screen_name, widget_name, &screen);
    if (screen_tag_index == NONE || !(children = vr_menu_allocate(3 * sizeof(*children))))
        return NONE;
    vr_menu_child_set(&children[0], vr_menu.screen_title_tag_index != NONE ?
        vr_menu.screen_title_tag_index : vr_menu.title_tag_index, 64, 90);
    vr_menu.screen_title_tag_index = NONE;
    vr_menu_child_set(&children[1], list_tag_index, 64, 142);
    vr_menu_child_set(&children[2], vr_menu.hints_tag_index, 64, 340);
    screen->child_widgets.count = 3;
    screen->child_widgets.address = children;
    memset(handlers, 0, sizeof(handlers));
    handlers[0].flags = VR_MENU_CLOSE_CURRENT;
    handlers[0].event_type = VR_MENU_EVENT_B;
    handlers[1] = handlers[0];
    handlers[1].event_type = VR_MENU_EVENT_BACK;
    handlers[2].flags = VR_MENU_CLOSE_ALL;
    handlers[2].event_type = VR_MENU_EVENT_START;
    for (index = 0; index < 3; index++) {
        vr_menu_reference(&handlers[index].widget_tag, NONE);
        vr_menu_reference(&handlers[index].sound_effect, NONE);
    }
    return vr_menu_handlers(screen, handlers, 3) ? screen_tag_index : NONE;
}

/* Build back-to-front so each NEXT button references an existing screen.
 * The native widget stack makes B return to the preceding page, then the
 * categories. No looping links or unbounded history growth. */
/* Test55: a page's screens: its sections (topics) when it has them, each
screen's first row and length; otherwise screens of eight */
static long vr_menu_screen_count(struct vr_menu_section const *sections, long section_count, long count)
{
    return sections && section_count > 0 ? section_count : VR_MENU_PAGES(count);
}

static long vr_menu_paged(long pause, long list, long resume, long group,
    long const *buttons, long count, short column_x, short hints_x,
    struct vr_menu_section const *sections, long section_count)
{
    long pages = vr_menu_screen_count(sections, section_count, count), next = NONE, sub;
    if (sections && section_count > 0) {
        long total = 0;
        for (sub = 0; sub < section_count; sub++) {
            if (sections[sub].count <= 0 || sections[sub].count > VR_MENU_SETTINGS_PER_SCREEN) return NONE;
            total += sections[sub].count;
        }
        if (total != count) return NONE;
    }
    long screens[VR_MENU_MAXIMUM_SCREENS];
    if (pages > VR_MENU_MAXIMUM_SCREENS) return NONE;
    for (sub = pages - 1; sub >= 0; --sub) {
        long visible[VR_MENU_SETTINGS_PER_SCREEN + 1];
        long first = sub * VR_MENU_SETTINGS_PER_SCREEN, length;
        struct vr_menu_event_handler handler;
        if (sections && section_count > 0) {
            long before;
            for (first = 0, before = 0; before < sub; before++) first += sections[before].count;
            length = sections[sub].count;
        } else length = MIN(VR_MENU_SETTINGS_PER_SCREEN, count - first);
        memcpy(visible, buttons + first, length * sizeof(long));
        if (next != NONE) {
            long slot = vr_menu.navigation_count;
            if (slot >= vr_menu.navigation_capacity) return NONE;
            vr_menu_open_handler(&handler, next);
            visible[length] = vr_menu_button(resume,
                vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_next_%ld_%ld", group, sub),
                "vr_next_page", &handler, 1);
            if (visible[length] == NONE) return NONE;
            vr_menu.navigation[slot].tag = visible[length++];
            vr_menu.navigation[slot].next = sub + 2;
            vr_menu.navigation[slot].total = pages;
            vr_menu.navigation[slot].label = sections && section_count > 0 ? sections[sub + 1].label : NULL;
            vr_menu.navigation[slot].jump = 0;
            vr_menu.navigation_count++;
        }
        /* Test72: a topic's screen titled "PAGE: TOPIC" (the owner's
         * SCOPES: which weapon a screen moves; IMMERSION's topics too) */
        vr_menu.screen_title_tag_index = NONE;
        if (sections && section_count > 0 && group >= 0 && group < VR_MENU_PAGE_COUNT) {
            long slot = vr_menu.navigation_count, title;
            if (slot >= vr_menu.navigation_capacity) return NONE;
            title = vr_menu_button(resume,
                vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_title_%ld_%ld", group, sub),
                "vr_title", NULL, 0);
            if (title == NONE) return NONE;
            vr_menu_widget_get(title)->bounds.x1 = 512;
            vr_menu.navigation[slot].tag = title;
            vr_menu.navigation[slot].next = group;
            vr_menu.navigation[slot].total = pages;
            vr_menu.navigation[slot].label = sections[sub].label;
            vr_menu.navigation[slot].jump = 4;
            vr_menu.navigation_count++;
            vr_menu.screen_title_tag_index = title;
        }
        next = vr_menu_screen(pause, list,
            vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_page_%ld_%ld", group, sub),
            vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_list_%ld_%ld", group, sub),
            "vr_settings_page", visible, length, 4, column_x, hints_x);
        if (next == NONE) return NONE;
        screens[sub] = next;
    }
    /* Test63: a category of three screens or more opens an index of them
     * first (the owner: "jump right to a section without clicking back eight
     * times"): its topics by name, or each page by its first setting; a row
     * opens that screen, B comes back to the index in one press, NEXT still
     * pages on. Eight rows an index screen, MORE to the rest. Test64: a
     * category's own pages only. The list of categories (group
     * VR_MENU_PAGE_COUNT, 17 categories, three screens) is itself the list
     * of topics; Test63 built an index for it too, naming each row from
     * vr_menu_pages[VR_MENU_PAGE_COUNT], one past the table: the owner's
     * Test63 log, signal 11 in vr_menu_setting_text opening VR SETTINGS. */
    if (pages >= VR_MENU_INDEX_FROM && group >= 0 && group < VR_MENU_PAGE_COUNT) {
        long index_screens = VR_MENU_PAGES(pages), following = NONE, chunk;
        for (chunk = index_screens - 1; chunk >= 0; --chunk) {
            long visible[VR_MENU_SETTINGS_PER_SCREEN + 1], length = 0, target;
            struct vr_menu_event_handler handler;
            for (target = chunk * VR_MENU_SETTINGS_PER_SCREEN;
                target < MIN(pages, (chunk + 1) * VR_MENU_SETTINGS_PER_SCREEN); target++) {
                long slot = vr_menu.navigation_count, first = 0, before;
                if (slot >= vr_menu.navigation_capacity) return NONE;
                vr_menu_open_handler(&handler, screens[target]);
                visible[length] = vr_menu_button(resume,
                    vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_jump_%ld_%ld", group, target),
                    "vr_next_page", &handler, 1);
                if (visible[length] == NONE) return NONE;
                if (sections && section_count > 0)
                    for (before = 0; before < target; before++) first += sections[before].count;
                else first = target * VR_MENU_SETTINGS_PER_SCREEN;
                vr_menu.navigation[slot].tag = visible[length++];
                vr_menu.navigation[slot].next = target + 1;
                vr_menu.navigation[slot].total = pages;
                vr_menu.navigation[slot].label = sections && section_count > 0 ? sections[target].label :
                    vr_menu_pages[group].settings[first].label;
                vr_menu.navigation[slot].jump = sections && section_count > 0 ? 1 : 2;
                vr_menu.navigation_count++;
            }
            if (following != NONE) {
                long slot = vr_menu.navigation_count;
                if (slot >= vr_menu.navigation_capacity) return NONE;
                vr_menu_open_handler(&handler, following);
                visible[length] = vr_menu_button(resume,
                    vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_more_%ld_%ld", group, chunk),
                    "vr_next_page", &handler, 1);
                if (visible[length] == NONE) return NONE;
                vr_menu.navigation[slot].tag = visible[length++];
                vr_menu.navigation[slot].next = chunk + 2;
                vr_menu.navigation[slot].total = index_screens;
                vr_menu.navigation[slot].label = NULL;
                vr_menu.navigation[slot].jump = 3;
                vr_menu.navigation_count++;
            }
            /* (an index screen of exactly eight rows and MORE fills the nine) */
            following = vr_menu_screen(pause, list,
                vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_index_%ld_%ld", group, chunk),
                vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_index_list_%ld_%ld", group, chunk),
                "vr_settings_page", visible, length, 4, column_x, hints_x);
            if (following == NONE) return NONE;
        }
        next = following;
    }
    return next;
}

/* Add a row without relying on a stock list's exact child count. The
 * optional before index preserves native profile option/description indices. */
static boolean vr_menu_insert(long list_index, long tag, short x, short y, long before)
{
    struct vr_menu_widget *list = vr_menu_widget_get(list_index);
    struct vr_menu_child *old, *grown;
    long count, index;
    if (!list || list->type != 3 || tag == NONE) return FALSE;
    count = list->child_widgets.count;
    old = list->child_widgets.address;
    if (count < 0 || count > 128 || (count && !old)) return FALSE;
    for (index = 0; index < count; index++)
        if (old[index].widget_tag.index == tag) return TRUE;
    if (before < 0 || before > count) before = count;
    grown = vr_menu_allocate((count + 1) * sizeof(*grown));
    if (!grown) return FALSE;
    if (before) memcpy(grown, old, before * sizeof(*grown));
    vr_menu_child_set(&grown[before], tag, x, y);
    if (before < count) memcpy(grown + before + 1, old + before, (count - before) * sizeof(*grown));
    list->child_widgets.address = grown;
    list->child_widgets.count = count + 1;
    list->bounds.y1 = MAX(list->bounds.y1, y + 28);
    return TRUE;
}

/* Fit the extra main-menu entry above the profile footer (y=446). The
 * imported 33px artwork keeps its size; only its 3px gaps become 1px. */
static boolean vr_menu_main(long list_index)
{
    struct vr_menu_widget *list = vr_menu_widget_get(list_index);
    struct vr_menu_child *children;
    long index, y = 247, count;
    if (!list || list->type != 3 || list->child_widgets.count < 0 ||
        list->child_widgets.count > 16 || !list->child_widgets.address) return FALSE;
    count = list->child_widgets.count;
    children = list->child_widgets.address;
    for (index = 0; index < count; index++) {
        struct vr_menu_widget *item = vr_menu_widget_get(children[index].widget_tag.index);
        if (children[index].widget_tag.index == vr_menu.button_tag_index) return TRUE;
        if (!item || item->bounds.y1 < item->bounds.y0) return FALSE;
        y += item->bounds.y1 - item->bounds.y0 + 1;
    }
    if (y + 28 > 446 || !vr_menu_insert(list_index, vr_menu.button_tag_index, 192, (short)y, NONE))
        return FALSE;
    children = list->child_widgets.address;
    y = 247;
    for (index = 0; index < count; index++) {
        struct vr_menu_widget *item = vr_menu_widget_get(children[index].widget_tag.index);
        children[index].vertical_offset = (short)(y - item->bounds.y0);
        y += item->bounds.y1 - item->bounds.y0 + 1;
    }
    return TRUE;
}

/* A stock/CE pause list is identified by its real native quit callback,
 * not its number/order of children. Shared lists are only extended once. */
static boolean vr_menu_pause_list(long screen_index, long list_index)
{
    struct vr_menu_widget *screen = vr_menu_widget_get(screen_index);
    struct vr_menu_widget *list = vr_menu_widget_get(list_index);
    struct vr_menu_child *children;
    long index, bottom = 0;
    short list_x = 0, list_y = 0;
    if (!screen || !list || list->type != 3 || list->child_widgets.count <= 0 ||
        list->child_widgets.count > 128 || !list->child_widgets.address) return FALSE;
    children = list->child_widgets.address;
    for (index = 0; index < list->child_widgets.count; index++) {
        struct vr_menu_widget *button;
        if (children[index].widget_tag.index == vr_menu.button_tag_index) return TRUE;
        button = vr_menu_widget_get(children[index].widget_tag.index);
        if (button) bottom = MAX(bottom, children[index].vertical_offset + button->bounds.y1);
    }
    children = screen->child_widgets.address;
    for (index = 0; index < screen->child_widgets.count; index++)
        if (children[index].widget_tag.index == list_index) {
            list_x = children[index].horizontal_offset + list->bounds.x0;
            list_y = children[index].vertical_offset + list->bounds.y0;
        }
    /* Do not write beyond a custom CE screen. The existing Settings route
     * remains usable if that map's direct pause list has no spare room. */
    if (list_x + VR_MENU_BUTTON_WIDTH > 640 || list_y + bottom + 28 > 440) {
        platform_log("vr: pause layout has no room for a VR row: %s", tag_get_name(list_index));
        return FALSE;
    }
    if (!vr_menu_insert(list_index, vr_menu.button_tag_index, 0, (short)bottom, NONE)) return FALSE;
    /* The original solo hints shared this row. Move only an overlapping
     * non-list sibling (typically hints); no hard-coded child[4] access. */
    for (index = 0; index < screen->child_widgets.count; index++) {
        struct vr_menu_widget *other;
        short x, y;
        if (children[index].widget_tag.index == list_index) continue;
        other = vr_menu_widget_get(children[index].widget_tag.index);
        if (!other) continue;
        x = children[index].horizontal_offset + other->bounds.x0;
        y = children[index].vertical_offset + other->bounds.y0;
        if (other->bounds.y1 - other->bounds.y0 <= 40 &&
            x < list_x + VR_MENU_BUTTON_WIDTH && x + other->bounds.x1 - other->bounds.x0 > list_x &&
            y < list_y + bottom + 28 && y + other->bounds.y1 - other->bounds.y0 > list_y + bottom) {
            if (328 + other->bounds.x1 - other->bounds.x0 <= 640)
                children[index].horizontal_offset = (short)(328 - other->bounds.x0);
            else if (y + 28 + other->bounds.y1 - other->bounds.y0 <= 480)
                children[index].vertical_offset += 28;
        }
    }
    return TRUE;
}

static void vr_menu_multiplayer_fallback(void)
{
    long collection = tag_loaded('Soul', "ui\\shell\\multiplayer");
    struct tag_block *screens;
    long screen, child, button, handler;
    if (collection == NONE) return;
    screens = tag_get('Soul', collection);
    if (!screens || screens->count < 0 || screens->count > 128 || !screens->address) return;
    for (screen = 0; screen < screens->count; screen++) {
        long screen_index = ((struct tag_reference *)screens->address)[screen].index;
        struct vr_menu_widget *root = vr_menu_widget_get(screen_index);
        struct vr_menu_child *lists;
        if (!root || root->child_widgets.count > 128 || !root->child_widgets.address) continue;
        lists = root->child_widgets.address;
        for (child = 0; child < root->child_widgets.count; child++) {
            struct vr_menu_widget *list = vr_menu_widget_get(lists[child].widget_tag.index);
            struct vr_menu_child *buttons;
            boolean quit = FALSE;
            if (!list || list->type != 3 || list->child_widgets.count > 128 || !list->child_widgets.address) continue;
            buttons = list->child_widgets.address;
            for (button = 0; button < list->child_widgets.count && !quit; button++) {
                struct vr_menu_widget *item = vr_menu_widget_get(buttons[button].widget_tag.index);
                struct vr_menu_event_handler *events;
                if (!item || item->event_handlers.count > 128 || !item->event_handlers.address) continue;
                events = item->event_handlers.address;
                for (handler = 0; handler < item->event_handlers.count; handler++) {
                    char const *name;
                    if (!(events[handler].flags & VR_MENU_RUN_FUNCTION)) continue;
                    name = ui_widget_event_handler_function_name(events[handler].function);
                    if (name && !strcmp(name, "mp game player quit")) { quit = TRUE; break; }
                }
            }
            if (quit) vr_menu_pause_list(screen_index, lists[child].widget_tag.index);
        }
    }
}

/* Called after widgets/textures close, before native menu tags and the map
 * table are released. Idempotent; nothing is freed from a live UI callback. */
void vr_menu_tags_unloaded(void)
{
    long index;
    for (index = 0; index < vr_menu.allocation_count; index++) free(vr_menu.allocations[index]);
    /* cseries maps free to debug_free, which rejects NULL. First load and
     * repeated/failed-load teardown can legitimately have no registry. */
    if (vr_menu.allocations) free(vr_menu.allocations);
    memset(&vr_menu, 0, sizeof(vr_menu));
    vr_menu.button_tag_index = vr_menu.categories_tag_index = NONE;
    vr_menu.title_tag_index = vr_menu.hints_tag_index = NONE;
    vr_menu.screen_title_tag_index = NONE;
    for (index = 0; index < VR_MENU_PAGE_COUNT; index++)
        vr_menu.category_tag_indices[index] = vr_menu.page_tag_indices[index] = NONE;
}

void vr_menu_tags_loaded(void)
{
    static short const hints_x = 328, right_column_x = 328 - 72;
    long pause_index, list_index, resume_index, font, page, index;
    struct vr_menu_widget *widget;
    struct vr_menu_event_handler handlers[3];
    if (vr_menu.loaded) return; /* menu hooks may be requested twice */
    vr_menu_tags_unloaded();
    vr_menu.loaded = TRUE;
    font = tag_loaded('font', "ui\\small_ui");
    if (font == NONE) font = tag_loaded('font', "ui\\large_ui");
    if (font == NONE) font = tag_loaded('font', "ui\\interstate");
    if (font == NONE) {
        platform_log("vr: map has no UI font; preserving its menus, VR runtime remains active");
        return;
    }
    pause_index = vr_menu_native(vr_menu_name("vr\\menu\\screen_template", 0, 0),
        "vr_screen", 0, 640, 480, &widget);
    if (pause_index == NONE) return;
    /* A new root replaces and deletes its caller; every VR screen must own
     * its solo pause. ui_widget.c suppresses this flag for network sessions
     * and the main menu when the screen is instantiated, not at map load. */
    index = tag_loaded('bitm', "pc\\bitmaps/gradient");
    if (index != NONE) {
        vr_menu_reference(&widget->background_bitmap, index);
        widget->background_bitmap.group_tag = 'bitm';
    }
    list_index = vr_menu_native(vr_menu_name("vr\\menu\\list_template", 0, 0),
        "vr_list", 3, 506, 140, &widget);
    if (list_index == NONE) return;
    resume_index = vr_menu_native(vr_menu_name("vr\\menu\\text_template", 0, 0),
        "vr_text", 1, VR_MENU_BUTTON_WIDTH, 28, &widget);
    if (resume_index == NONE) return;
    vr_menu_reference(&widget->text_font, font);
    widget->text_font.group_tag = 'font';
    widget->text_color.alpha = 1.0f;
    widget->text_color.red = 0.25f;
    widget->text_color.green = 0.75f;
    widget->text_color.blue = 1.0f;
    widget->horizontal_offset = 2;
    widget->vertical_offset = 4;
    vr_menu.screen_title_tag_index = NONE; /* Test72: no topic title yet (a first load has had no unload) */
    vr_menu.title_tag_index = vr_menu_button(resume_index,
        vr_menu_name("vr\\menu\\title", 0, 0), "vr_title", NULL, 0);
    vr_menu.hints_tag_index = vr_menu_button(resume_index,
        vr_menu_name("vr\\menu\\hints", 0, 0), "vr_hints", NULL, 0);
    if (vr_menu.title_tag_index == NONE || vr_menu.hints_tag_index == NONE) return;
    vr_menu_widget_get(vr_menu.hints_tag_index)->bounds.x1 = 512;
    vr_menu.navigation_capacity = VR_MENU_PAGES(VR_MENU_PAGE_COUNT);
    for (page = 0; page < VR_MENU_PAGE_COUNT; page++) {
        long screens = vr_menu_screen_count(vr_menu_pages[page].sections,
            vr_menu_pages[page].section_count, vr_menu_pages[page].count);
        vr_menu.navigation_capacity += screens;
        /* Test72: a sectioned page's screen titles */
        if (vr_menu_pages[page].sections && vr_menu_pages[page].section_count > 0)
            vr_menu.navigation_capacity += screens;
        /* Test63: the index's rows and MORE rows */
        if (screens >= VR_MENU_INDEX_FROM)
            vr_menu.navigation_capacity += screens + VR_MENU_PAGES(screens);
    }
    vr_menu.navigation = vr_menu_allocate(vr_menu.navigation_capacity * sizeof(*vr_menu.navigation));
    if (!vr_menu.navigation) return;
    for (page = 0; page < VR_MENU_PAGE_COUNT; page++) {
        vr_menu.setting_tag_indices[page] = vr_menu_allocate(vr_menu_pages[page].count * sizeof(long));
        if (!vr_menu.setting_tag_indices[page]) return;
        for (index = 0; index < vr_menu_pages[page].count; index++) vr_menu.setting_tag_indices[page][index] = NONE;
    }

	/* each setting: A or right steps on, left back */
	memset(handlers, 0, sizeof(handlers));
	handlers[0].flags = VR_MENU_RUN_FUNCTION;
	handlers[0].event_type = VR_MENU_EVENT_A;
	/* Test70: A confirms (a button row's choice; elsewhere it steps on) */
	handlers[0].function = VR_MENU_CONFIRM_FUNCTION;
	handlers[1] = handlers[0];
	handlers[1].function = VR_MENU_NEXT_FUNCTION;
	handlers[1].event_type = VR_MENU_EVENT_DPAD_RIGHT;
	handlers[2] = handlers[0];
	handlers[2].event_type = VR_MENU_EVENT_DPAD_LEFT;
	handlers[2].function = VR_MENU_PREVIOUS_FUNCTION;
	for (index = 0; index < 3; index++)
	{
		vr_menu_reference(&handlers[index].widget_tag, NONE);
		vr_menu_reference(&handlers[index].sound_effect, NONE);
	}

	/* each page: its settings' buttons and its screen */
	for (page = 0; page < VR_MENU_PAGE_COUNT; page++)
	{
		struct vr_menu_page const *definition = &vr_menu_pages[page];

		for (index = 0; index < definition->count; index++)
		{
			char widget_name[32];

			snprintf(widget_name, sizeof(widget_name), "vr_setting_%ld_%ld", page, index);
			vr_menu.setting_tag_indices[page][index] = vr_menu_button(resume_index,
				vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_setting_%ld_%ld", page, index),
				widget_name, handlers, 3);
			if (vr_menu.setting_tag_indices[page][index] == NONE)
				return;
		}
		vr_menu.page_tag_indices[page] = vr_menu_paged(pause_index, list_index, resume_index,
            page, vr_menu.setting_tag_indices[page], definition->count, right_column_x, hints_x,
            definition->sections, definition->section_count);
		if (vr_menu.page_tag_indices[page] == NONE)
			return;
	}

	/* the categories, each opening its page, in one column */
	for (page = 0; page < VR_MENU_PAGE_COUNT; page++)
	{
		vr_menu_open_handler(&handlers[0], vr_menu.page_tag_indices[page]);
		vr_menu.category_tag_indices[page] = vr_menu_button(resume_index,
			vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_category_%ld", page, 0),
			"vr_category_button", handlers, 1);
		if (vr_menu.category_tag_indices[page] == NONE)
			return;
	}
	vr_menu.categories_tag_index = vr_menu_paged(pause_index, list_index, resume_index,
        VR_MENU_PAGE_COUNT, vr_menu.category_tag_indices, VR_MENU_PAGE_COUNT, right_column_x, hints_x, NULL, 0);
	if (vr_menu.categories_tag_index == NONE)
		return;

	/* the pause menu's new item, opening the categories */
	vr_menu_open_handler(&handlers[0], vr_menu.categories_tag_index);
	vr_menu.button_tag_index = vr_menu_button(resume_index,
		vr_menu_name("ui\\shell\\solo_game\\pause_game\\vr_settings_button", 0, 0),
		"vr_settings_button", handlers, 1);
    if (vr_menu.button_tag_index == NONE) return;
    {
        long main = tag_loaded(VR_MENU_WIDGET_TAG, "pc\\main_menu/main_menu_select_list");
        long settings = tag_loaded(VR_MENU_WIDGET_TAG,
            "pc\\main_menu/settings_select/player_setup/player_profile_edit/profile_edit_select_list");
        boolean settings_added = FALSE, main_added = FALSE;
        if (main != NONE) main_added = vr_menu_main(main);
        if (settings != NONE) {
            struct vr_menu_child *child;
            long row = vr_menu_native(vr_menu_name("vr\\menu\\settings_row", 0, 0),
                "vr_settings_row", 3, VR_MENU_BUTTON_WIDTH, 28, &widget);
            if (row != NONE && (child = vr_menu_allocate(sizeof(*child)))) {
                vr_menu_child_set(child, vr_menu.button_tag_index, 0, 0);
                /* The outer Settings list owns up/down. A one-item inner
                 * list must not consume those events and trap gamepad focus. */
                widget->flags = 1;
                widget->child_widgets.count = 1;
                widget->child_widgets.address = child;
                /* The native profile description callback recognizes nested
                 * button bars; no nonexistent tenth picture/string is read. */
                settings_added = vr_menu_insert(settings, row, 51, 377,
                    vr_menu_widget_get(settings)->child_widgets.count - 1);
            }
        }
        pause_index = tag_loaded(VR_MENU_WIDGET_TAG, "ui\\shell\\solo_game\\pause_game\\pause_game");
        list_index = tag_loaded(VR_MENU_WIDGET_TAG, "ui\\shell\\solo_game\\pause_game\\pause_list");
        if (pause_index != NONE && list_index != NONE) vr_menu_pause_list(pause_index, list_index);
        /* OpenCE already puts SETTINGS on every multiplayer pause screen.
         * The nested VR row makes that route work without growing its art.
         * Stock-menu fallback gets a direct VR entry when that route is absent. */
        if (!settings_added) vr_menu_multiplayer_fallback();
        platform_log("vr: native VR settings ready: %ld categories, four rows per column; main=%d settings=%d",
            VR_MENU_PAGE_COUNT, main_added, settings_added);
    }
}

/* ---------- the widgets' callbacks */

enum
{
	_vr_menu_none,
	_vr_menu_pause_item,
	_vr_menu_category,
    _vr_menu_navigation,
    _vr_menu_title,
    _vr_menu_hints,
	_vr_menu_setting,
};

/* what the widget's definition is: the pause menu's item, a category
(its page in *page) or a setting (its page and index) */
static int vr_menu_widget_kind(
	long definition_tag_index,
	long *page,
	long *setting)
{
	long index;

	if (definition_tag_index == NONE || !vr_menu.loaded)
		return _vr_menu_none;
    if (definition_tag_index == vr_menu.title_tag_index) return _vr_menu_title;
    if (definition_tag_index == vr_menu.hints_tag_index) return _vr_menu_hints;
	if (definition_tag_index == vr_menu.button_tag_index)
        return _vr_menu_pause_item;
    for (index = 0; index < vr_menu.navigation_count; index++)
        if (vr_menu.navigation[index].tag == definition_tag_index) {
            *setting = index;
            return _vr_menu_navigation;
        }
	for (*page = 0; *page < VR_MENU_PAGE_COUNT; (*page)++)
	{
		if (vr_menu.category_tag_indices[*page] == definition_tag_index)
			return _vr_menu_category;
		for (index = 0; vr_menu.setting_tag_indices[*page] && index < vr_menu_pages[*page].count; index++)
		{
			if (vr_menu.setting_tag_indices[*page][index] == definition_tag_index)
			{
				*setting = index;
				return _vr_menu_setting;
			}
		}
	}
	return _vr_menu_none;
}

/* Only generated root screens own the solo pause, including NEXT targets.
 * Do not infer ownership from a tag name supplied by a map. */
boolean vr_menu_is_screen(long definition_tag_index)
{
    long index;
    if (definition_tag_index == NONE || !vr_menu.loaded) return FALSE;
    if (definition_tag_index == vr_menu.categories_tag_index) return TRUE;
    for (index = 0; index < VR_MENU_PAGE_COUNT; ++index)
        if (definition_tag_index == vr_menu.page_tag_indices[index]) return TRUE;
    for (index = 0; index < vr_menu.navigation_count; ++index) {
        struct vr_menu_widget *button = vr_menu_widget_get(vr_menu.navigation[index].tag);
        struct vr_menu_event_handler *handler = button ? button->event_handlers.address : NULL;
        if (handler && button->event_handlers.count == 1 &&
            definition_tag_index == handler->widget_tag.index) return TRUE;
    }
    return FALSE;
}

boolean vr_menu_is_setting(long definition_tag_index)
{
	long page = NONE, setting = NONE;
	return vr_menu_widget_kind(definition_tag_index, &page, &setting) == _vr_menu_setting;
}

boolean vr_menu_setting_text(
	long definition_tag_index,
	wchar_t *text,
	long size)
{
	long page = NONE, setting_index = NONE;
	int kind = vr_menu_widget_kind(definition_tag_index, &page, &setting_index);
	char line[64];
	long index;

	if (kind == _vr_menu_none || size <= 0)
		return FALSE;
	if (kind == _vr_menu_pause_item || kind == _vr_menu_title)
	{
		snprintf(line, sizeof(line), "VR SETTINGS");
	}
	else if (kind == _vr_menu_hints)
    {
        /* Test70: a button row being chosen, or a row's mark, explained here */
        if (!vr_menu_button_hints(line, sizeof(line)))
            snprintf(line, sizeof(line), "A: NEXT / SET   < >: CHANGE   B: BACK");
    }
	else if (kind == _vr_menu_navigation && vr_menu.navigation[setting_index].jump == 1)
        snprintf(line, sizeof(line), "%s", vr_menu.navigation[setting_index].label);
	else if (kind == _vr_menu_navigation && vr_menu.navigation[setting_index].jump == 2)
        snprintf(line, sizeof(line), "PAGE %ld: %s...", vr_menu.navigation[setting_index].next,
            vr_menu.navigation[setting_index].label);
	else if (kind == _vr_menu_navigation && vr_menu.navigation[setting_index].jump == 4)
        snprintf(line, sizeof(line), "%s: %s", vr_menu.navigation[setting_index].next >= 0 &&
            vr_menu.navigation[setting_index].next < VR_MENU_PAGE_COUNT ?
            vr_menu_pages[vr_menu.navigation[setting_index].next].label : "VR SETTINGS",
            vr_menu.navigation[setting_index].label);
	else if (kind == _vr_menu_navigation && vr_menu.navigation[setting_index].jump == 3)
        snprintf(line, sizeof(line), "MORE (%ld/%ld)", vr_menu.navigation[setting_index].next,
            vr_menu.navigation[setting_index].total);
	else if (kind == _vr_menu_navigation)
    {
        if (vr_menu.navigation[setting_index].label)
            snprintf(line, sizeof(line), "NEXT: %s (%ld/%ld)", vr_menu.navigation[setting_index].label,
                vr_menu.navigation[setting_index].next, vr_menu.navigation[setting_index].total);
        else
            snprintf(line, sizeof(line), "NEXT PAGE (%ld/%ld)",
                vr_menu.navigation[setting_index].next, vr_menu.navigation[setting_index].total);
    }
    else if (kind == _vr_menu_category)
	{
		snprintf(line, sizeof(line), "%s", vr_menu_pages[page].label);
	}
	else
	{
		struct vr_menu_setting const *setting = &vr_menu_pages[page].settings[setting_index];
		long value_index = vr_menu_value_index(setting);

		/* Test55: a pouch or holster place row on screen shows the zones */
		if (!strncmp(setting->key, "vr.pouch_", 9) || !strcmp(setting->key, "vr.reload_pouch_size"))
			vr_calibration_note_shown(1);
		else if (!strncmp(setting->key, "vr.holster_hip_", 15) || !strncmp(setting->key, "vr.holster_shoulder_", 20))
			vr_calibration_note_shown(2);
		/* Test63: a vehicle-control offset or size row: the seated driver
		sees the controls' blue preview, moving as the value changes */
		else if (!strncmp(setting->key, "vr.handle_", 10))
			vr_calibration_note_shown(4);

        if(setting->type == _vr_setting_gun_aim) {
            int kind=vr_gun_class(); char key[64];
            if(!strcmp(setting->key,"gun")) snprintf(line,sizeof(line),"%s: %s",setting->label,vr_gun_class_label(kind));
            else if(!strcmp(setting->key,"reset")) snprintf(line,sizeof(line),"%s: APPLY",setting->label);
            else if(kind<0) snprintf(line,sizeof(line),"%s: HOLD A GUN",setting->label);
            else { double value; snprintf(key,sizeof(key),"vr.aim_%s_%s",vr_gun_class_key(kind),setting->key);
                value=config_real(key); if(!isfinite(value)) value=0;
                snprintf(line,sizeof(line),"%s: < %.1f DEG >",setting->label,value+0.0); }
        }
        else if(setting->type == _vr_setting_hand_degrees)
            snprintf(line,sizeof(line),"%s: < %.0f DEG >",setting->label,vr_menu_hand_angle(setting->key));
		else if(setting->type == _vr_setting_degrees || setting->type == _vr_setting_centimetres ||
			setting->type == _vr_setting_scope_centimetres || setting->type == _vr_setting_vehicle_centimetres) {
            double value=config_real(setting->key); if(!isfinite(value)) value=0;
            snprintf(line,sizeof(line),"%s: < %.0f %s >",setting->label,
                setting->type!=_vr_setting_degrees?value*100:value,
                setting->type!=_vr_setting_degrees?"CM":"DEG");
        } else if(setting->type == _vr_setting_reset_alignment || setting->type == _vr_setting_flip_alignment ||
            setting->type == _vr_setting_reset_hand || setting->type == _vr_setting_reset_weapon ||
            setting->type == _vr_setting_reset_scopes || setting->type == _vr_setting_reset_buttons ||
            setting->type == _vr_setting_reset_vehicle_offsets || setting->type == _vr_setting_reset_wrist)
            snprintf(line,sizeof(line),"%s: %s",setting->label,
                setting->type == _vr_setting_reset_buttons && vr_menu_buttons_reset_at &&
                system_milliseconds() - vr_menu_buttons_reset_at < VR_MENU_RESET_DONE_MS ? "DONE" : "APPLY");
        else if(setting->type == _vr_setting_button && vr_menu_button_action(setting->key) != NONE)
            vr_menu_button_row_text(setting, vr_menu_button_action(setting->key), line, sizeof(line));
        else
		snprintf(line, sizeof(line), setting->type == _vr_setting_real || setting->type == _vr_setting_snap_angle ?
			"%s: < %s >" : "%s: %s", setting->label,
			value_index != NONE ? setting->values[value_index].label : "CUSTOM");
	}
	for (index = 0; line[index] && index < size - 1; index++)
		text[index] = (wchar_t)(unsigned char)line[index];
	text[index] = 0;
	return TRUE;
}

static boolean vr_menu_apply(struct vr_menu_setting const *setting, char const *value);

boolean vr_menu_setting_change(
	long definition_tag_index,
	long step)
{
	long page = NONE, setting_index = NONE;
	struct vr_menu_setting const *setting;
	long value_index;
	char const *value;
	boolean written = FALSE;

	if (vr_menu_widget_kind(definition_tag_index, &page, &setting_index) != _vr_menu_setting)
		return FALSE;
	setting = &vr_menu_pages[page].settings[setting_index];
	/* Test70: A (step 0) sets a button row's choice; elsewhere it steps on,
	as it always did. Changing another row ends a row's choosing */
	if (setting->type != _vr_setting_button)
	{
		vr_menu_remap_idle();
		if (!step)
			step = 1;
	}
    if(setting->type == _vr_setting_degrees || setting->type == _vr_setting_centimetres ||
		setting->type == _vr_setting_scope_centimetres || setting->type == _vr_setting_vehicle_centimetres) {
        double value=config_real(setting->key), unit=setting->type==_vr_setting_degrees?5.0:0.01;
        double limit=setting->type==_vr_setting_degrees?180.0:
			setting->type==_vr_setting_vehicle_centimetres?0.50:
			setting->type==_vr_setting_scope_centimetres?VR_SCOPE_ADJUST_LIMIT_METRES:0.20;
        if(!isfinite(value)) value=0.0;
        value=step>0?(floor(value/unit+0.00001)+1)*unit:(ceil(value/unit-0.00001)-1)*unit;
        value=fmax(-limit,fmin(limit,value)); written=config_write_real(setting->key,value);
        vr_reload_settings(); platform_log("vr: %s %.3f%s",setting->key,value,written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_reset_scopes) {
        #define SCOPE_KEY(key, label, gun) #key,
        static const char *const kinds[]={VR_SCOPE_PROFILES(SCOPE_KEY)};
#undef SCOPE_KEY
        static const char *const parts[]={"forward","up","right","scale"};
        char key[64]; int kind, part;
        written=TRUE;
        for(kind=0;kind<VR_SCOPE_PROFILE_COUNT;kind++) for(part=0;part<4;part++) {
            snprintf(key,sizeof(key),"vr.scope_%s_%s",kinds[kind],parts[part]);
            written=config_write_real(key,config_default_real(key))&&written; }
        vr_reload_settings(); platform_log("vr: reset scope places and sizes%s",written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_hud_shown) {
        /* (the session's, as the head tap's: nothing written) */
        vr_set_hud_hidden(!vr_hud_hidden());
        return TRUE;
    }
    if(setting->type == _vr_setting_reset_wrist) {
        static const char *const keys[]={"vr.wrist_hud_along","vr.wrist_hud_across","vr.wrist_hud_out","vr.wrist_hud_size"};
        int key;
        written=TRUE;
        for(key=0;key<(int)NUMBEROF(keys);key++) written=config_write_real(keys[key],config_default_real(keys[key]))&&written;
        vr_reload_settings(); platform_log("vr: reset the wrist HUD's place and size%s",written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_reset_vehicle_offsets && !strcmp(setting->key,"control offsets")) {
        /* Test66: RESET CONTROLS: every immersive control's reach, left,
         * height and size back to its default */
        static const char *const controls[]={"all","warthog","ghost","scorpion","banshee"};
        static const char *const axes[]={"forward","left","up","size"};
        char key[64]; int control, axis;
        written=TRUE;
        for(control=0;control<(int)NUMBEROF(controls);control++) for(axis=0;axis<4;axis++) {
            if(!control&&axis==3)continue;
            snprintf(key,sizeof(key),"vr.handle_%s_%s",controls[control],axes[axis]);
            written=config_write_real(key,config_default_real(key))&&written; }
        vr_reload_settings(); platform_log("vr: reset the immersive vehicle controls' places and sizes%s",written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_reset_vehicle_offsets) {
        static const char *const vehicles[]={"all","warthog","ghost","banshee","scorpion","pelican","turret"};
        static const char *const axes[]={"up","forward","right"};
        char key[64]; int vehicle, axis;
        written=TRUE;
        for(vehicle=0;vehicle<(int)NUMBEROF(vehicles);vehicle++) for(axis=0;axis<3;axis++) {
            snprintf(key,sizeof(key),"vr.vehicle_%s_%s",vehicles[vehicle],axes[axis]);
            written=config_write_real(key,config_default_real(key))&&written; }
        vr_reload_settings(); platform_log("vr: reset vehicle seat offsets%s",written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_turn_mode) {
        /* smooth keeps the snap angle for later; snap takes it up again */
        boolean snapping=config_real("vr.snap_turn")>0.0;
        double angle=vr_menu_snap_angle();
        written=config_write_real("vr.snap_turn_amount",angle);
        written=config_write_real("vr.snap_turn",snapping?0.0:angle)&&written;
        vr_reload_settings(); platform_log("vr: turning %s (snap angle %.1f)%s",snapping?"smooth":"snap",angle,written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_snap_angle) {
        /* a step from the angle now, as the numeric rows step */
        double current=vr_menu_snap_angle(), angle;
        long i;
        value_index=step>0?setting->value_count-1:0;
        if(step>0) { for(i=0;i<setting->value_count;i++) if(atof(setting->values[i].value)>current+0.00001) { value_index=i; break; } }
        else { for(i=setting->value_count-1;i>=0;i--) if(atof(setting->values[i].value)<current-0.00001) { value_index=i; break; } }
        angle=atof(setting->values[value_index].value);
        written=config_write_real("vr.snap_turn_amount",angle);
        if(config_real("vr.snap_turn")>0.0) written=config_write_real("vr.snap_turn",angle)&&written;
        vr_reload_settings(); platform_log("vr: snap angle %.1f%s",angle,written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_reset_buttons) {
        int action;
        vr_menu_remap_idle();
        written=vr_menu_button_sprint_set(FALSE);
        for(action=0;action<VR_BUTTON_ACTIONS;action++)
            written=config_write_string(vr_button_action_key(action),vr_button_source_value(vr_button_default(action)))&&written;
        vr_reload_settings(); platform_log("vr: reset buttons to their defaults%s",written?"":" (save failed)");
        vr_menu_buttons_reset_at = system_milliseconds();
        if (!vr_menu_buttons_reset_at) vr_menu_buttons_reset_at = 1;
        return TRUE;
    }
    if(setting->type == _vr_setting_button) {
        /* Test70: chosen in place, a conflict resolved by the player
        (vr_menu_button_row_change); Button Sprint's left stick still
        skipped while it is on (Test63) */
        int action=vr_menu_button_action(setting->key);
        if(action==NONE) return TRUE;
        vr_menu_button_row_change(setting, action, step);
        return TRUE;
    }
    if(setting->type == _vr_setting_gun_aim) {
        /* the held gun's kind only; nothing without a gun */
        int kind=vr_gun_class(); char key[64];
        if(kind<0||!strcmp(setting->key,"gun")) return TRUE;
        if(!strcmp(setting->key,"reset")) {
            snprintf(key,sizeof(key),"vr.aim_%s_up",vr_gun_class_key(kind)); written=config_write_real(key,0.0);
            snprintf(key,sizeof(key),"vr.aim_%s_right",vr_gun_class_key(kind)); written=config_write_real(key,0.0)&&written;
            vr_reload_settings(); platform_log("vr: reset %s aim%s",vr_gun_class_label(kind),written?"":" (save failed)");
            return TRUE;
        }
        {
            double value;
            snprintf(key,sizeof(key),"vr.aim_%s_%s",vr_gun_class_key(kind),setting->key);
            value=config_real(key); if(!isfinite(value)) value=0.0;
            value=step>0?(floor(value/0.5+0.00001)+1)*0.5:(ceil(value/0.5-0.00001)-1)*0.5;
            value=fmax(-VR_GUN_AIM_LIMIT,fmin(VR_GUN_AIM_LIMIT,value));
            written=config_write_real(key,value+0.0); vr_reload_settings();
            platform_log("vr: %s %.1f%s",key,value,written?"":" (save failed)");
        }
        return TRUE;
    }
    if(setting->type == _vr_setting_hand_degrees) {
        /* both hands at once, the left mirrored */
        double value=vr_menu_hand_angle(setting->key), sign=strcmp(setting->key,"pitch")?-1.0:1.0;
        char key[64];
        value=step>0?(floor(value/5.0+0.00001)+1)*5.0:(ceil(value/5.0-0.00001)-1)*5.0;
        value=fmax(-180.0,fmin(180.0,value));
        snprintf(key,sizeof(key),"vr.hand_right_%s",setting->key); written=config_write_real(key,value);
        snprintf(key,sizeof(key),"vr.hand_left_%s",setting->key); written=config_write_real(key,value*sign+0.0)&&written;
        vr_reload_settings(); platform_log("vr: both hands %s %.1f (left %.1f)%s",setting->key,value,value*sign,written?"":" (save failed)");
        return TRUE;
    }
    if(setting->type == _vr_setting_flip_alignment) {
        double roll=config_real(setting->key);
        if(!isfinite(roll)) roll=0.0;
        roll=roll>0.0?roll-180.0:roll+180.0;
        roll=fmax(-180.0,fmin(180.0,roll));
        written=config_write_real(setting->key,roll); vr_reload_settings();
        platform_log("vr: %s flipped to %.1f%s",setting->key,roll,written?"":" (save failed)"); return TRUE;
    }
    if(setting->type == _vr_setting_reset_hand || setting->type == _vr_setting_reset_weapon) {
        static const char *const hand_axes[]={"pitch","yaw","roll"};
        static const char *const hand_sides[]={"left","right"};
        static const char *const weapon_keys[]={"vr.weapon_pitch","vr.weapon_yaw","vr.weapon_roll",
            "vr.gun_forward","vr.gun_up","vr.gun_out",
            "vr.weapon_offset_right","vr.weapon_offset_up","vr.weapon_offset_back"};
        char key[64]; int a, h;
        written=TRUE;
        if(setting->type == _vr_setting_reset_hand) {
            /* key "left", "right" or "both" */
            for(h=0;h<2;h++) if(!strcmp(setting->key,"both")||!strcmp(setting->key,hand_sides[h]))
                for(a=0;a<3;a++) { snprintf(key,sizeof(key),"vr.hand_%s_%s",hand_sides[h],hand_axes[a]);
                    written=config_write_real(key,config_default_real(key))&&written; }
        } else for(a=0;a<(int)NUMBEROF(weapon_keys);a++) written=config_write_real(weapon_keys[a],config_default_real(weapon_keys[a]))&&written;
        vr_reload_settings();
        platform_log("vr: reset %s %s%s",setting->key,setting->type == _vr_setting_reset_hand ? "hand orientation" : "gun angle and place",
            written?"":" (save failed)"); return TRUE;
    }
    if(setting->type == _vr_setting_reset_alignment) {
        const char *axes[]={"pitch","yaw","roll","right","up","back"}; char key[64]; int a;
        written=TRUE;
        for(a=0;a<6;a++) { snprintf(key,sizeof(key),"vr.align_%s_%s",setting->key,axes[a]);
            written=config_write_real(key,0.0)&&written; }
        snprintf(key,sizeof(key),"vr.align_%s_grip_aim",setting->key);
        written=config_write_boolean(key,FALSE)&&written; vr_reload_settings();
        platform_log("vr: reset %s controller calibration%s",setting->key,written?"":" (save failed)"); return TRUE;
    }
	value_index = vr_menu_value_index(setting);
	if (setting->type == _vr_setting_real)
	{
		/* Move in the requested direction even from a custom numeric value;
         * nearest-then-step skipped valid thresholds (e.g. 0.34 -> 0.40). */
        double current = config_real(setting->key);
        long i;
        if (step > 0) {
            value_index = setting->value_count - 1;
            for (i = 0; i < setting->value_count; i++)
                if (atof(setting->values[i].value) > current + 0.00001) { value_index = i; break; }
        } else {
            value_index = 0;
            for (i = setting->value_count - 1; i >= 0; i--)
                if (atof(setting->values[i].value) < current - 0.00001) { value_index = i; break; }
        }
	}
	else
		value_index = value_index == NONE ? 0 : (value_index + step + setting->value_count) % setting->value_count;
	value = setting->values[value_index].value;
	(void)written;
	return vr_menu_apply(setting, value);
}

/* a row's choice written, as a player's is, and what it needs with it
(vr_menu_dependencies); Test73: the update's VR features too
(vr_menu_features_on) */
static boolean vr_menu_apply(
	struct vr_menu_setting const *setting,
	char const *value)
{
	boolean written = FALSE;

	if (setting->type == _vr_setting_boolean && !strcmp(setting->key, "vr.button_sprint"))
	{
		vr_menu_button_sprint_set(!strcmp(value, "true"));
		return TRUE;
	}
	switch (setting->type)
	{
	case _vr_setting_boolean:
		written = config_write_boolean(setting->key, !strcmp(value, "true"));
		break;
	case _vr_setting_multi:
		written = TRUE;
		if (!vr_menu_multi(value, TRUE, &written))
			written = FALSE;
		break;
	case _vr_setting_handedness:
	{
		/* the gun's hand, and what was chosen for the old hand follows it:
		vehicle steering and Move With (the sticks and face buttons mirror
		through vr.mirror_controls) */
		boolean left = !strcmp(value, "true");
		char const *from = left ? "right" : "left", *to = left ? "left" : "right";

		written = config_write_boolean(setting->key, left);
		if (!strcmp(config_string("vr.vehicle_steering"), from))
			written = config_write_string("vr.vehicle_steering", to) && written;
		if (!strcmp(config_string("vr.move_relative"), from))
			written = config_write_string("vr.move_relative", to) && written;
		break;
	}
	case _vr_setting_real:
	case _vr_setting_real_choice:
		written = config_write_real(setting->key, atof(value));
		break;
	case _vr_setting_string:
		written = config_write_string(setting->key, value);
		/* Older configs may have disabled the old 3D screen toggle. Selecting
		the new 3D SCREEN mode explicitly enables that screen. */
		if (!strcmp(setting->key, "vr.cutscenes") && !strcmp(value, "screen"))
			written = config_write_boolean("vr.cinema_3d", TRUE) && written;
		break;
	case _vr_setting_crouch_click:
		/* crouch: on the left stick's click with the reticle's toggle off it
		(no button; BUTTONS can give it one) and the turning stick held down
		free, the layout before 1.0.8; the reticle: crouch back on the
		turning stick held down. Each through the BUTTONS page's swap, so
		no button does two things */
		if (!strcmp(value, "crouch"))
		{
			written = vr_menu_button_set(VR_BUTTON_ACTION_RETICLE, VR_BUTTON_SOURCE_NONE);
			written = vr_menu_button_set(VR_BUTTON_ACTION_CROUCH, VR_BUTTON_SOURCE_LEFT_STICK) && written;
		}
		else
		{
			written = vr_menu_button_set(VR_BUTTON_ACTION_CROUCH, VR_BUTTON_SOURCE_RIGHT_STICK_DOWN);
			written = vr_menu_button_set(VR_BUTTON_ACTION_RETICLE, VR_BUTTON_SOURCE_LEFT_STICK) && written;
		}
		break;
	}
	vr_reload_settings();
	if (written && vr_menu_dependencies(setting, value))
		vr_reload_settings();
	platform_log("vr: %s set to %s%s", setting->key, value, written ? "" : " (not saved to config.toml)");
	return TRUE;
}

/* Test73 (the owner, for the next release: "make it so all the vr features
(manual reload, parkour, fist combat, immersive vehicles, npc/wall grab, one
hand heavies, etc.) are all enabled by default upon update. this would be
for everyone ... overwriting previous settings in those areas"): each VR
feature turned on, once (vr_initialize, vr.test73_features_applied), as the
player would turn it on in VR SETTINGS: its row's ON, with what it needs and
plays best with (IMMERSIVE VEHICLE the first-person seat, FIST COMBAT impact
melee, SLIDE a sprint...). Turned off afterwards, a feature stays off. Not
here, being ways to play rather than features: ARM RUN, BUTTON SPRINT and
SPRINT TOGGLE (SLIDE brings a sprint itself), BULLET TIME MODE, GRIP GUIDE,
TANK HATCH (the owner keeps the tank's own view), the online switches */
static char const *const vr_menu_update_features[] =
{
	/* reloads */
	"vr.manual_reload", "vr.dropped_magazines", "vr.reload_shoulder", "vr.reload_hip", "vr.cooldown_shake",
	/* weapons, fists and grenades */
	"vr.holster_haptics", "vr.weapon_pull", "vr.weapon_pull_glow", "vr.scope",
	"vr.assisted_combat", "vr.combat_moves", "vr.projectile_parry", "vr.combat_highlights", "vr.combat_combo_display",
	"vr.combat_counters", "vr.close_contact", "vr.physical_grenades",
	/* movement and parkour */
	"vr.slide", "vr.slide_physical_crouch", "vr.slide_jump_cancel", "vr.bullet_time",
	"vr.parkour_wallrun", "vr.parkour_walljump", "vr.parkour_climb", "vr.roomscale", "vr.fingers",
	/* grabs and bodies */
	"vr.world_interaction", "vr.custom_scenery_grab", "vr.vehicle_lift", "vr.npc_interaction", "vr.custom_npc_rigs",
	"vr.npc_rescue", "vr.npc_one_hand_heavy", "vr.interaction_damage",
	"vr.death_ragdolls", "vr.active_ragdoll", "vr.npc_lift_to_ragdoll",
	/* immersive vehicles */
	"vr.vehicle_handles", "vr.vehicle_wheel_visible", "vr.vehicle_cockpit_lock", "vr.vehicle_hand_lock",
	"vr.vehicle_control_glow", "vr.vehicle_steer_haptics", "vr.vehicle_comfort_reach", "vr.vehicle_tank_drive",
	"vr.vehicle_banshee_slide",
};

/* a feature's row (any page's) and its ON: "true", or a combined row's
value that sets the key true ("vr.manual_reload=true", ...); FALSE: none */
static boolean vr_menu_feature_row(
	char const *key,
	struct vr_menu_setting const **row,
	char const **on)
{
	char pattern[96];
	long page, index, value;

	snprintf(pattern, sizeof(pattern), "%s=true", key);
	for (page = 0; page < VR_MENU_PAGE_COUNT; page++)
	{
		for (index = 0; index < vr_menu_pages[page].count; index++)
		{
			struct vr_menu_setting const *setting = &vr_menu_pages[page].settings[index];

			if (strcmp(setting->key, key))
				continue;
			if (setting->type == _vr_setting_boolean)
			{
				*row = setting;
				*on = "true";
				return TRUE;
			}
			for (value = 0; setting->type == _vr_setting_multi && value < setting->value_count; value++)
			{
				if (!strncmp(setting->values[value].value, pattern, strlen(pattern)))
				{
					*row = setting;
					*on = setting->values[value].value;
					return TRUE;
				}
			}
		}
	}
	return FALSE;
}

void vr_menu_features_on(void)
{
	long index, turned = 0, missing = 0;

	for (index = 0; index < (long)NUMBEROF(vr_menu_update_features); index++)
	{
		struct vr_menu_setting const *row;
		char const *on;

		if (!vr_menu_feature_row(vr_menu_update_features[index], &row, &on))
		{
			platform_log("vr: the update's VR features: no row for %s", vr_menu_update_features[index]);
			missing++;
			continue;
		}
		if (!config_boolean(vr_menu_update_features[index]))
			turned++;
		vr_menu_apply(row, on);
	}
	platform_log("vr: the update's VR features on, once: %ld of %ld turned on now (the rest were on)%s; any can be "
		"turned off again in VR SETTINGS", turned, (long)NUMBEROF(vr_menu_update_features) - missing,
		missing ? ", some not found" : "");
}

#endif
