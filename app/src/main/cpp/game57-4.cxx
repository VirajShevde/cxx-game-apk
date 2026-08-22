#include "SDL.h"

#include <SDL_image.h>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <array>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <cstdio>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

// Screen & Grid Setup
constexpr int SCREEN_WIDTH  = 2000;
constexpr int SCREEN_HEIGHT = 1200;
constexpr int TILE_SIZE     = 50;
constexpr int COLS          = 100;  // Large world: 100 columns
constexpr int ROWS          = 60;   // Large world: 60 rows

// Limits & Capacity
constexpr int MAX_BULLETS      = 100;
constexpr int MAX_EXPLOSIONS   = 30;
constexpr int MAX_HEALTH_PACKS = 8;
constexpr int MAX_ENEMIES      = 20;

// Map editor bottom UI panel: palette row + action button row.
constexpr int EDITOR_UI_HEIGHT = 170;

// Core Enums
enum GameState  { MODE_SELECTION, TACTICAL_CONFIG, PLAYING, GAME_OVER, GAME_WON, MAP_EDITOR, WEAPON_MENU };
enum class GameMode   { TACTICAL, ENDLESS };
enum class Team       { GREEN_DEFUSER, RED_BOMBER };
// Display names for the two factions - Team enum values stay as-is (used all over
// existing save/load and comparison logic), this is purely what gets shown to the player.
inline const char* faction_name(Team t) { return (t == Team::RED_BOMBER) ? "RAIDERS" : "SENTINELS"; }
enum class WeaponType { PISTOL, RIFLE, ROCKET, LASER, VECTOR_REFLECT, COUNT };
enum class Direction  { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT };
enum class EnemyType  { RED, PINK }; // RED = shooter, PINK = cardinal+diagonal shooter
enum class EnemyDifficulty { NORMAL, HARD, EXPERT };

// ---------------------------------------------------------------------------
// Ally Bot foundation (Phase 2 of bot rollout). Bots are Sentinel-team allies
// that fight alongside the player against Raiders. This struct/name pool is
// the data model; AI movement+targeting, HUD wiring, and the tactical-config
// bot-count slider are separate follow-up passes that consume this.
// ---------------------------------------------------------------------------
constexpr int MAX_BOTS = 20;

// Moved above Bot/Enemy (was previously declared later in the file, after
// both structs already referenced it - that ordering doesn't compile).
struct Point { int x{0}, y{0}; };

const std::array<std::string, 24> BOT_NAME_POOL = {
    "VIPER", "ECHO", "TALON", "RAVEN", "FOXTROT", "SABLE", "GHOST", "NOMAD",
    "COBRA", "SHADE", "REAPER", "HAWKEYE", "BLITZ", "STORM", "WOLF", "ONYX",
    "CIPHER", "RUNNER", "SPARROW", "FROST", "BLAZE", "DAGGER", "SCOUT", "ANVIL"
};

// Separate callsign pool for Raiders (the enemy team) so their leaderboard
// names never collide with the Sentinel bot roster.
const std::array<std::string, 24> RAIDER_NAME_POOL = {
    "JACKAL", "VULTURE", "RAZOR", "MAULER", "PHANTOM", "SCORPION", "RENEGADE", "GUTTER",
    "HAVOC", "WARHEAD", "SIEGE", "MARAUDER", "GRIM", "ASHEN", "TYRANT", "BRUTUS",
    "CRUSHER", "FANG", "OUTLAW", "RUIN", "SLASH", "VENOM", "WRECKER", "SKULL"
};

struct Bot {
    float x{0.0f}, y{0.0f};
    SDL_Rect rect{0, 0, 60, 60};
    int hp{60}, maxHp{60};
    Uint32 lastShootTime{0};
    Uint32 lastPathCalc{0};
    Uint32 lastCalloutTime{0}; // throttles comm chirps so a bot doesn't spam callouts
    Uint32 lastGlassBreakTime{0};
    Point nextTile{0, 0};
    WeaponType weaponType{WeaponType::PISTOL};
    EnemyDifficulty tier{EnemyDifficulty::NORMAL}; // bot skill tier, mirrors tactical difficulty
    std::string name;
    int kills{0};
    int deaths{0};
    int bombDefuses{0};
    bool active{false};
    bool defusingBomb{false};
    Uint32 defuseStartTime{0};
    // Individuality so bots don't all funnel into one clump on the same target:
    // each bot picks its own stand-off point around whoever it's engaging, and
    // moves at a slightly different pace from its squadmates.
    float flankAngle{0.0f};
    float speedJitter{1.0f};
};

std::array<Bot, MAX_BOTS> bots{};
int activeBotCount = 0; // set from tactical config slider (0-20)

// --- Dodge Roll / Dash tuning -----------------------------------------------
constexpr float LONG_PRESS_DURATION = 0.30f;   // seconds joystick must be held deflected to auto-trigger a roll
constexpr float JOYSTICK_DEFLECT_THRESHOLD = 0.35f; // fraction of stick radius considered "deflected"
constexpr float DOUBLE_TAP_WINDOW   = 0.20f;   // seconds between arrow taps to count as a double-tap
constexpr float DASH_SPEED_MULT     = 3.6f;    // burst speed multiplier over walk speed
constexpr float WALK_SPEED          = 8.0f;
constexpr float AI_SPEED_NORMAL     = 7.0f;
constexpr float AI_SPEED_ALERT      = 8.5f;
constexpr float AI_SPEED_HARD       = 10.5f;
constexpr float AI_SPEED_EXPERT     = 12.0f;
constexpr Uint32 AI_PATH_INTERVAL_MS = 150;
constexpr float DOUBLE_TAP_DASH_DURATION = 0.16f; // double-tap dash: short, snappy burst
constexpr float JOYSTICK_ROLL_DURATION   = 0.23f; // long-press roll: slightly longer, more of a "roll"
constexpr float DASH_COOLDOWN       = 0.4f;    // recovery cooldown shared by both dash triggers
constexpr float DIAGONAL_ALIGN_TOLERANCE = 0.35f; // |dx|/|dy| ratio tolerance for pink enemy diagonal shots (fraction of larger axis)
constexpr float CARDINAL_ALIGN_TOLERANCE = 18.0f; // px - how close to dx≈0 or dy≈0 counts as "cardinal aligned"

struct WeaponProperties {
    std::string name;
    Uint32 cooldownMs;
    float speed;
    int damage;
    SDL_Color color;
};

const WeaponProperties WEAPON_PROPS[] = {
    { "PISTOL", 220, 35.0f, 20, {  80, 230, 120, 255 } },
    { "RIFLE",   80, 48.0f, 10, { 240, 220,  60, 255 } },
    { "ROCKET", 525, 25.0f, 65, { 240, 120,  40, 255 } },
    { "LASER",  350,  0.0f, 35, {  50, 180, 255, 255 } },
    { "VECTOR-REFLECT", 180, 42.0f, 22, { 180, 100, 255, 255 } }
};

struct Bomb {
    float x{0.0f}, y{0.0f};
    SDL_Rect rect{0, 0, 35, 35};
    bool planted{false};
    Uint32 plantTime{0};
    Uint32 fuseDuration{15000};
    bool defused{false};
};

struct LaserBeam {
    SDL_Point start;
    SDL_Point end;
    Uint32 spawnTime{0};
    Uint32 duration{100};
    SDL_Color color{50, 180, 255, 255};
    bool active{false};
    bool isPlayerShot{true}; // distinguishes player vs enemy beams for rendering
};

struct GlassShatter {
    float x{0.0f}, y{0.0f};
    Uint32 spawnTime{0};
    Uint32 duration{420};
    bool active{false};
};

struct Explosion {
    float x{0.0f}, y{0.0f};
    float maxRadius{160.0f};
    Uint32 spawnTime{0};
    Uint32 duration{300};
    bool active{false};
};

struct HealthPack {
    int r{0}, c{0};
    bool active{false};
    SDL_Rect rect{0, 0, 0, 0};
};

struct Enemy {
    float x{0.0f}, y{0.0f};
    SDL_Rect rect{0, 0, 60, 60}; // Updated width and height to 60 to match player dimensions
    int hp{60};
    int maxHp{60};
    Uint32 lastShootTime{0};
    Uint32 lastPathCalc{0};
    Uint32 lastGlassBreakTime{0}; // separate cooldown for shooting through glass blocking its path
    Point nextTile{0, 0};
    WeaponType weaponType{WeaponType::PISTOL};
    Team team{Team::RED_BOMBER};
    EnemyType enemyType{EnemyType::RED};
    bool active{false};
    std::string name;    // callsign shown on the leaderboard alongside the player/bots
    int kills{0};         // player/bot kills this Raider has landed
    int deaths{0};         // 1 once this Raider goes down, 0 while still active
    int bombPlants{0};    // number of bombs this Raider has successfully planted
};

struct SoundEffect {
    float frequency{0.0f};
    int durationSamples{0};
    int currentSample{0};
    float volume{0.0f};
    bool isPlayerShot{false};
    bool isExplosion{false};
    bool isBeep{false};
    bool isGlassShatter{false};
};

struct Bullet {
    float x{0.0f}, y{0.0f};
    float vx{0.0f}, vy{0.0f};
    Direction dir{Direction::DIR_UP};
    WeaponType type{WeaponType::PISTOL};
    bool active{false};
    bool isPlayerBullet{false};
    int ownerBotIndex{-1}; // -1 = player fired; >=0 = index into bots[] (friendly bullet, for kill attribution)
    bool diagonal{false}; // true for non-cardinal (8-way) shots - rendered/sized differently than cardinal bolts
    int bounceCount{0};
    SDL_Rect rect{0, 0, 0, 0};
};
// 5x7 Bitmap Font Table
const std::unordered_map<char, std::array<uint8_t, 5>> FONT_5X7 = {
    {'A', {0x7E, 0x09, 0x09, 0x09, 0x7E}}, {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3E, 0x41, 0x41, 0x41, 0x22}}, {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}}, {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3E, 0x41, 0x49, 0x49, 0x3A}}, {'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}}, {'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}}, {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}}, {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}}, {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x26, 0x49, 0x49, 0x49, 0x32}}, {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}}, {'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}}, {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x03, 0x04, 0x78, 0x04, 0x03}}, {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'0', {0x3E, 0x41, 0x41, 0x41, 0x3E}}, {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}}, 
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}}, {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}}, 
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}}, {'5', {0x27, 0x45, 0x45, 0x45, 0x39}}, 
    {'6', {0x3E, 0x49, 0x49, 0x49, 0x32}}, {'7', {0x01, 0x01, 0x01, 0x01, 0x7F}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}}, {'9', {0x26, 0x49, 0x49, 0x49, 0x3E}}, 
    {':', {0x00, 0x36, 0x36, 0x00, 0x00}}, {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
    {'[', {0x00, 0x7F, 0x41, 0x41, 0x00}}, {']', {0x00, 0x41, 0x41, 0x7F, 0x00}},
    {'|', {0x00, 0x00, 0x7F, 0x00, 0x00}}, {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'(', {0x00, 0x1C, 0x22, 0x41, 0x00}}, {')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
    {'\'', {0x00, 0x02, 0x05, 0x00, 0x00}},
     {'.', {0x00, 0x00, 0x60, 0x00, 0x00}},
     {'/', {0x01, 0x02, 0x04, 0x08, 0x10}},
     {'%', {0x63, 0x13, 0x08, 0x64, 0x63}}
};

void draw_text(SDL_Renderer* renderer, const std::string& text, int x, int y, int scale = 3, SDL_Color color = {255, 255, 255, 255}) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int curX = x;
    for (char c : text) {
        c = static_cast<char>(std::toupper(c));
        auto it = FONT_5X7.find(c);
        if (it != FONT_5X7.end()) {
            const auto& cols = it->second;
            for (int col = 0; col < 5; ++col) {
                for (int row = 0; row < 7; ++row) {
                    if (cols[col] & (1 << row)) {
                        SDL_Rect pixel = { curX + col * scale, y + row * scale, scale, scale };
                        SDL_RenderFillRect(renderer, &pixel);
                    }
                }
            }
        }
        curX += 6 * scale;
    }
}

// Each glyph advances curX by 6*scale (see loop above), so a string's rendered
// width is exactly length*6*scale - use these to center text precisely instead
// of hand-tuned pixel offsets that drift whenever the copy or scale changes.
int text_width(const std::string& text, int scale) { return static_cast<int>(text.length()) * 6 * scale; }
int centered_text_x(const std::string& text, int scale, int containerX = 0, int containerW = SCREEN_WIDTH) {
    return containerX + (containerW - text_width(text, scale)) / 2;
}

// SDL_RenderDrawRect draws its outline with SDL_RenderDrawLine, and with
// SDL_RenderSetLogicalSize scaling active the right/bottom line can round
// away to nothing (the fill rect scales fine, but a 1px line landing on a
// fractional scaled pixel can vanish) - the top and left edges start at an
// integer coordinate so they don't show the same rounding loss. Building the
// outline out of four small filled rects sidesteps that rounding entirely,
// so all four sides render consistently regardless of the logical scale.
void draw_rect_outline(SDL_Renderer* renderer, const SDL_Rect& r, int thickness = 2) {
    SDL_Rect top    = { r.x, r.y, r.w, thickness };
    SDL_Rect bottom = { r.x, r.y + r.h - thickness, r.w, thickness };
    SDL_Rect left   = { r.x, r.y, thickness, r.h };
    SDL_Rect right  = { r.x + r.w - thickness, r.y, thickness, r.h };
    SDL_RenderFillRect(renderer, &top);
    SDL_RenderFillRect(renderer, &bottom);
    SDL_RenderFillRect(renderer, &left);
    SDL_RenderFillRect(renderer, &right);
}

SDL_AudioSpec audioSpec{};
SDL_AudioDeviceID audioDevice{0};
SoundEffect currentEffect{};

void audio_callback(void*, Uint8* stream, int len) {
    auto* buffer = reinterpret_cast<int16_t*>(stream);
    int samples = len / 2;

    for (int i = 0; i < samples; ++i) {
        if (currentEffect.currentSample < currentEffect.durationSamples) {
            float progress = static_cast<float>(currentEffect.currentSample) / static_cast<float>(currentEffect.durationSamples);
            float envelope = 0.0f;
            float mix = 0.0f;

            if (currentEffect.isExplosion) {
                envelope = std::exp(-progress * 4.0f) * currentEffect.volume;
                float noise = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
                float subTone = std::sin(2.0f * M_PI * (currentEffect.frequency * (1.0f - progress * 0.4f)) * currentEffect.currentSample / 44100.0f);
                mix = (noise * 0.85f) + (subTone * 0.5f);
            } else if (currentEffect.isGlassShatter) {
                envelope = std::exp(-progress * 5.5f) * currentEffect.volume;
                float noise1 = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
                float noise2 = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
                float crack = std::sin(2.0f * M_PI * (1800.0f - 900.0f * progress) * currentEffect.currentSample / 44100.0f);
                mix = noise1 * 0.75f + noise2 * 0.35f + crack * 0.25f;
            } else if (currentEffect.isBeep) {
                // Clean "ti" tone, sharp attack and quick decay, no noise.
                envelope = std::exp(-progress * 6.0f) * currentEffect.volume;
                mix = std::sin(2.0f * M_PI * currentEffect.frequency * currentEffect.currentSample / 44100.0f);
            } else {
                envelope = std::exp(-progress * 14.0f) * currentEffect.volume;
                float noise = (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX)) * 2.0f - 1.0f;
                float tone = std::sin(2.0f * M_PI * currentEffect.frequency * currentEffect.currentSample / 44100.0f);
                mix = (noise * 0.8f) + (tone * 0.2f);
            }

            buffer[i] = static_cast<int16_t>(std::clamp(mix * envelope, -1.0f, 1.0f) * 32767.0f);
            currentEffect.currentSample++;
        } else {
            buffer[i] = 0;
        }
    }
}

void play_gunshot_sound(bool isPlayer, WeaponType wType = WeaponType::PISTOL) {
    SDL_LockAudioDevice(audioDevice);
    float baseFreq = 520.0f;
    if (wType == WeaponType::RIFLE)  baseFreq = 720.0f;
    if (wType == WeaponType::ROCKET) baseFreq = 160.0f;
    if (wType == WeaponType::LASER)  baseFreq = 920.0f;

    currentEffect.frequency = isPlayer ? baseFreq : 280.0f;
    currentEffect.durationSamples = static_cast<int>(44100 * 0.10);
    currentEffect.currentSample = 0;
    currentEffect.volume = isPlayer ? 0.9f : 0.7f;
    currentEffect.isPlayerShot = isPlayer;
    currentEffect.isExplosion = false;
    currentEffect.isBeep = false;
    currentEffect.isGlassShatter = false;
    SDL_UnlockAudioDevice(audioDevice);
}

void play_explosion_sound() {
    SDL_LockAudioDevice(audioDevice);
    currentEffect.frequency = 55.0f;
    currentEffect.durationSamples = static_cast<int>(44100 * 0.45);
    currentEffect.currentSample = 0;
    currentEffect.volume = 1.0f;
    currentEffect.isExplosion = true;
    currentEffect.isBeep = false;
    currentEffect.isGlassShatter = false;
    SDL_UnlockAudioDevice(audioDevice);
}

void play_fuse_beep(float frequency, float volume) {
    SDL_LockAudioDevice(audioDevice);
    currentEffect.frequency = frequency;
    currentEffect.durationSamples = static_cast<int>(44100 * 0.06);
    currentEffect.currentSample = 0;
    currentEffect.volume = volume;
    currentEffect.isExplosion = false;
    currentEffect.isBeep = true;
    currentEffect.isGlassShatter = false;
    SDL_UnlockAudioDevice(audioDevice);
}

void play_glass_shatter_sound() {
    SDL_LockAudioDevice(audioDevice);
    currentEffect.frequency = 1500.0f;
    currentEffect.durationSamples = static_cast<int>(44100 * 0.22);
    currentEffect.currentSample = 0;
    currentEffect.volume = 0.75f;
    currentEffect.isPlayerShot = false;
    currentEffect.isExplosion = false;
    currentEffect.isBeep = false;
    currentEffect.isGlassShatter = true;
    SDL_UnlockAudioDevice(audioDevice);
}

int gameMap[ROWS][COLS];
int currentMapCols = COLS;
int currentMapRows = ROWS;
int editorMapCols = COLS;
int editorMapRows = ROWS;

// Bombsite tags: 8 = A, 9 = B. They are walkable floor tiles.
Point bombSiteA{-1, -1};
Point bombSiteB{-1, -1};
bool bombSiteAValid = false;
bool bombSiteBValid = false;
Point selectedBombSite{-1, -1};

// Player spawn tag painted in the map editor (tile value 11). Read back out
// and cleared by init_game_arena() the same way enemy spawn markers are.
Point customPlayerSpawn{-1, -1};
bool customPlayerSpawnValid = false;

// Camera in WORLD pixels. The player can travel through the entire large map.
int cameraX = 0, cameraY = 0;

// Tablet-friendly map editor controls.
// Panel is anchored near the top, just below the "MAP EDITOR" title row,
// rather than at the bottom - so the toolbar sits right under the header.
constexpr int EDITOR_PANEL_TOP = 80;
constexpr int EDITOR_PANEL_BOTTOM = EDITOR_PANEL_TOP + EDITOR_UI_HEIGHT;
constexpr int EDITOR_PALETTE_Y = EDITOR_PANEL_TOP + 35;
constexpr int EDITOR_ACTION_Y  = EDITOR_PANEL_TOP + 105;

struct EditorTileOption { int value; const char* label; SDL_Color color; SDL_Rect rect; };
// Tile values 1-5 are plain terrain/wall blocks. 6/7 are enemy spawn markers -
// placing one paints a marker tile that spawn logic reads back out at load
// time (see init_game_arena); they never render as solid ground. 11 is the
// player spawn marker, same idea - read back out and cleared at load time.
const std::array<EditorTileOption, 7> editorPalette = {{
    { 1, "WALL",       {70, 75, 90, 255},    {20,  EDITOR_PALETTE_Y, 90, 55} },
    { 11, "PLAYER",    {60, 220, 100, 255},  {120, EDITOR_PALETTE_Y, 100, 55} },
    { 6, "RED ENEMY",  {220, 60, 60, 255},   {230, EDITOR_PALETTE_Y, 110, 55} },
    { 7, "PINK ENEMY", {255, 105, 180, 255}, {350, EDITOR_PALETTE_Y, 110, 55} },
    { 8, "BOMB A",     {255, 150, 70, 255},  {470, EDITOR_PALETTE_Y, 90, 55} },
    { 9, "BOMB B",     {255, 150, 70, 255},  {570, EDITOR_PALETTE_Y, 90, 55} },
    { 10, "GLASS",     {150, 220, 255, 180}, {670, EDITOR_PALETTE_Y, 110, 55} },
}};

// Return the color defined by the editor palette for a tile value.
// This keeps placed map tags visually consistent with their palette buttons.
SDL_Color editor_palette_color(int value) {
    for (const auto& option : editorPalette) {
        if (option.value == value) return option.color;
    }
    return SDL_Color{20, 20, 25, 255};
}

// --- Touchscreen pinch-zoom for the map editor ---------------------------
float editorZoom = 1.0f;
constexpr float EDITOR_ZOOM_MIN = 0.5f;
constexpr float EDITOR_ZOOM_MAX = 2.5f;
constexpr float EDITOR_ZOOM_STEP = 0.25f;
SDL_FingerID editorFingerId2 = -1;
int editorTouch1X = 0, editorTouch1Y = 0;
int editorTouch2X = 0, editorTouch2Y = 0;
float editorPinchStartDist = 0.0f;
float editorPinchStartZoom = 1.0f;
bool editorPinching = false;
// Placed at the end of the action row (after EXIT), well clear of the
// top-right back button so the two never overlap.
const SDL_Rect editorZoomOutButton { 985,  EDITOR_ACTION_Y, 90, 50 };
const SDL_Rect editorZoomInButton  { 1085, EDITOR_ACTION_Y, 90, 50 };

// Editor controls are laid out compactly so the whole editor remains visible
// when SDL scales the 2000x1200 logical canvas onto a phone/tablet.
const SDL_Rect editorSaveButton  {20,  EDITOR_ACTION_Y, 145, 50};   // SAVE AS
const SDL_Rect editorOverwriteButton {175, EDITOR_ACTION_Y, 145, 50}; // SAVE current map
const SDL_Rect editorLoadButton  {330, EDITOR_ACTION_Y, 140, 50};
const SDL_Rect editorSizeButton  {485, EDITOR_ACTION_Y, 150, 50};
const SDL_Rect editorEraseButton {650, EDITOR_ACTION_Y, 150, 50};
const SDL_Rect editorExitButton  {815, EDITOR_ACTION_Y, 150, 50};


int editorSelectedTile = 1;
bool editorEraseMode = false;
std::string editorStatusMsg;
Uint32 editorStatusMsgTime = 0;
std::string mapFilePath = "custom_map.txt";
bool useCustomMap = false;
bool aiGeneratedMapActive = false;

// Maps created by the editor are real files in SDL_GetPrefPath().  The browser
// below lists those files so the player can select which device-saved map to use.
std::vector<std::string> availableMapFiles;
int selectedMapIndex = -1;
bool mapDropdownOpen = false;
int hoveredMapIndex = -1; // row currently under the pointer while the dropdown is open
// Main-menu option: dodge/roll is disabled until the player ticks this box.
bool dodgeRollEnabled = false;

// --- Editor "save as" dialog (asks for a filename before writing to disk) ---
bool editorShowSaveDialog = false;
bool editorShowLoadDialog = false;
bool editorShowSizeDialog = false;
std::string editorFilenameInput;
const SDL_Rect editorDialogBox   { SCREEN_WIDTH / 2 - 320, SCREEN_HEIGHT / 2 - 110, 640, 220 };
const SDL_Rect editorDialogField { editorDialogBox.x + 30, editorDialogBox.y + 80, 580, 55 };
const SDL_Rect editorDialogSave  { editorDialogBox.x + 30, editorDialogBox.y + 150, 270, 50 };
const SDL_Rect editorDialogCancel{ editorDialogBox.x + 340, editorDialogBox.y + 150, 270, 50 };

const SDL_Rect editorSizeDialogBox { SCREEN_WIDTH / 2 - 330, SCREEN_HEIGHT / 2 - 220, 660, 440 };
const SDL_Rect editorSizeWidthMinus { editorSizeDialogBox.x + 55, editorSizeDialogBox.y + 120, 90, 60 };
const SDL_Rect editorSizeWidthPlus  { editorSizeDialogBox.x + 515, editorSizeDialogBox.y + 120, 90, 60 };
const SDL_Rect editorSizeHeightMinus{ editorSizeDialogBox.x + 55, editorSizeDialogBox.y + 220, 90, 60 };
const SDL_Rect editorSizeHeightPlus { editorSizeDialogBox.x + 515, editorSizeDialogBox.y + 220, 90, 60 };
const SDL_Rect editorSizeApply      { editorSizeDialogBox.x + 55, editorSizeDialogBox.y + 325, 255, 60 };
const SDL_Rect editorSizeCancel     { editorSizeDialogBox.x + 350, editorSizeDialogBox.y + 325, 255, 60 };

void editor_open_save_dialog() {
    editorShowSaveDialog = true;
    editorFilenameInput.clear();
    // Same forced-edge fix as the profile name dialog: guarantees the keyboard
    // actually reappears even if a prior dialog session left SDL's internal
    // text-input state "active" after the OS dismissed the keyboard directly.
    SDL_StopTextInput();
    SDL_StartTextInput();
}

void editor_close_save_dialog() {
    editorShowSaveDialog = false;
    SDL_StopTextInput();
}

std::string map_directory() {
    const size_t slash = mapFilePath.find_last_of("/\\\\");
    return (slash == std::string::npos) ? std::string() : mapFilePath.substr(0, slash + 1);
}

std::string map_basename(const std::string& path) {
    const size_t slash = path.find_last_of("/\\\\");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string sanitize_map_name(std::string name) {
    const size_t slash = name.find_last_of("/\\\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.size() >= 4 && name.substr(name.size() - 4) == ".txt") name.resize(name.size() - 4);

    std::string clean;
    for (unsigned char ch : name) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == ' ') clean += static_cast<char>(ch);
    }
    while (!clean.empty() && clean.front() == ' ') clean.erase(clean.begin());
    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
    return clean.empty() ? "custom_map" : clean;
}

void refresh_map_list() {
    availableMapFiles.clear();
    const std::string dirPath = map_directory();
    if (dirPath.empty()) return;

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;

    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
            availableMapFiles.push_back(name);
    }
    closedir(dir);

    std::sort(availableMapFiles.begin(), availableMapFiles.end());
    const std::string currentName = map_basename(mapFilePath);
    selectedMapIndex = -1;
    for (int i = 0; i < static_cast<int>(availableMapFiles.size()); ++i) {
        if (availableMapFiles[i] == currentName) {
            selectedMapIndex = i;
            break;
        }
    }
}

bool select_map_file_by_index(int index) {
    if (index < 0 || index >= static_cast<int>(availableMapFiles.size())) return false;
    mapFilePath = map_directory() + availableMapFiles[index];
    selectedMapIndex = index;
    useCustomMap = true;
    mapDropdownOpen = false;
    return true;
}

bool delete_map_file_by_index(int index) {
    if (index < 0 || index >= static_cast<int>(availableMapFiles.size())) return false;
    std::string name = availableMapFiles[index];
    if (std::remove((map_directory() + name).c_str()) != 0) return false;
    if (map_basename(mapFilePath) == name) useCustomMap = false;
    refresh_map_list();
    return true;
}

// ---------------------------------------------------------------------------
// Player profile picture picker. On Android this launches the real system
// photo picker (see the JNI bridge below + MainActivity.java) so the player
// browses actual device storage/gallery. On desktop builds, where SDL2 has
// no native "Open File" dialog, it falls back to the same in-app browser
// pattern map loading already uses (list files found on-device, tap one to
// select) - images just need to be copied into portrait_directory().
// ---------------------------------------------------------------------------
constexpr int PROFILE_PORTRAIT_W = 135;
constexpr int PROFILE_PORTRAIT_H = 150;

SDL_Texture* playerPortraitTexture = nullptr;
std::string playerPortraitPath;
std::vector<std::string> availablePortraitFiles;
bool portraitPickerOpen = false;

std::string portrait_directory() {
    // Same on-device save folder maps live in, one subfolder over.
    const std::string base = map_directory();
    return base.empty() ? std::string() : base + "portraits/";
}

void ensure_portrait_directory() {
    const std::string dir = portrait_directory();
    if (dir.empty()) return;
#if defined(_WIN32)
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
}

bool has_image_extension(const std::string& name) {
    auto endsWithCI = [&](const char* ext) {
        const size_t len = std::strlen(ext);
        if (name.size() < len) return false;
        std::string tail = name.substr(name.size() - len);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return tail == ext;
    };
    return endsWithCI(".png") || endsWithCI(".jpg") || endsWithCI(".jpeg") || endsWithCI(".bmp");
}

void refresh_portrait_list() {
    availablePortraitFiles.clear();
    ensure_portrait_directory();
    const std::string dirPath = portrait_directory();
    if (dirPath.empty()) return;

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (has_image_extension(name)) availablePortraitFiles.push_back(name);
    }
    closedir(dir);
    std::sort(availablePortraitFiles.begin(), availablePortraitFiles.end());
}

// Loads an image file and returns it as a texture that exactly fills
// targetW x targetH: the source is first center-cropped to the target's
// aspect ratio (so nothing gets squashed or stretched), then that cropped
// region is scaled to the exact target dimensions ("cover" fit).
SDL_Texture* load_cropped_portrait_texture(SDL_Renderer* renderer, const std::string& path,
                                            int targetW, int targetH) {
    SDL_Surface* src = IMG_Load(path.c_str());
    if (!src) {
        std::cerr << "Portrait load failed for " << path << ": " << IMG_GetError() << std::endl;
        return nullptr;
    }

    const float targetAspect = static_cast<float>(targetW) / targetH;
    const float srcAspect = static_cast<float>(src->w) / src->h;
    SDL_Rect cropRect{0, 0, src->w, src->h};
    if (srcAspect > targetAspect) {
        cropRect.w = std::max(1, static_cast<int>(src->h * targetAspect));
        cropRect.x = (src->w - cropRect.w) / 2;
    } else if (srcAspect < targetAspect) {
        cropRect.h = std::max(1, static_cast<int>(src->w / targetAspect));
        cropRect.y = (src->h - cropRect.h) / 2;
    }

    SDL_Surface* target = SDL_CreateRGBSurfaceWithFormat(0, targetW, targetH, 32, SDL_PIXELFORMAT_RGBA32);
    if (!target) { SDL_FreeSurface(src); return nullptr; }

    SDL_BlitScaled(src, &cropRect, target, nullptr);
    SDL_FreeSurface(src);

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, target);
    SDL_FreeSurface(target);
    return tex;
}

bool select_portrait_file_by_index(SDL_Renderer* renderer, int index) {
    if (index < 0 || index >= static_cast<int>(availablePortraitFiles.size())) return false;
    const std::string path = portrait_directory() + availablePortraitFiles[index];
    SDL_Texture* tex = load_cropped_portrait_texture(renderer, path, PROFILE_PORTRAIT_W, PROFILE_PORTRAIT_H);
    if (!tex) return false;

    if (playerPortraitTexture) SDL_DestroyTexture(playerPortraitTexture);
    playerPortraitTexture = tex;
    playerPortraitPath = path;
    portraitPickerOpen = false;
    return true;
}

#if defined(__ANDROID__)
// ---------------------------------------------------------------------------
// Android native photo picker bridge. MainActivity.java (a small subclass of
// SDL2's SDLActivity - see MainActivity.java alongside this file) launches
// Android's real system photo picker (ACTION_OPEN_DOCUMENT) when the player
// taps the portrait. Java can't hand SDL_image a content:// URI directly, so
// MainActivity copies the picked file into the app's own storage first, then
// calls nativeOnImagePicked() below with that real filesystem path.
//
// IMPORTANT: the exported symbol name below must exactly match your app's
// Java package (dots become underscores) - "com_yourcompany_tacticalshooter"
// is a placeholder. If your package is e.g. com.acme.raidgame, this becomes
// Java_com_acme_raidgame_MainActivity_nativeOnImagePicked, and the package
// line at the top of MainActivity.java must match too.
// ---------------------------------------------------------------------------
#include <SDL2/SDL_system.h>
#include <jni.h>
#include <mutex>

std::mutex androidPortraitPickMutex;
std::string androidPortraitPickedPath;
bool androidPortraitPickPending = false;

extern "C" JNIEXPORT void JNICALL
Java_com_yourcompany_tacticalshooter_MainActivity_nativeOnImagePicked(JNIEnv* env, jobject /*thiz*/, jstring path) {
    // Runs on the Java UI thread - just stash the result behind a mutex.
    // The game loop (a different thread) picks it up next frame.
    const char* cpath = env->GetStringUTFChars(path, nullptr);
    {
        std::lock_guard<std::mutex> lock(androidPortraitPickMutex);
        androidPortraitPickedPath = cpath ? cpath : "";
        androidPortraitPickPending = true;
    }
    if (cpath) env->ReleaseStringUTFChars(path, cpath);
}

// Calls MainActivity.openImagePicker() over JNI to launch the system photo
// picker. This is the Android equivalent of opening the in-app list on desktop.
void android_open_image_picker() {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return;

    jclass clazz = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(clazz, "openImagePicker", "()V");
    if (method) env->CallVoidMethod(activity, method);

    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(activity);
}

// Call once per frame from the main loop: if MainActivity handed us a picked
// image since last frame, load + crop it into the portrait texture.
void android_poll_picked_portrait(SDL_Renderer* renderer) {
    std::string pickedPath;
    bool havePick = false;
    {
        std::lock_guard<std::mutex> lock(androidPortraitPickMutex);
        if (androidPortraitPickPending) {
            pickedPath = androidPortraitPickedPath;
            havePick = true;
            androidPortraitPickPending = false;
        }
    }
    if (!havePick || pickedPath.empty()) return;

    SDL_Texture* tex = load_cropped_portrait_texture(renderer, pickedPath, PROFILE_PORTRAIT_W, PROFILE_PORTRAIT_H);
    if (!tex) return;
    if (playerPortraitTexture) SDL_DestroyTexture(playerPortraitTexture);
    playerPortraitTexture = tex;
    playerPortraitPath = pickedPath;
}
#endif

// Converts desktop mouse coordinates to the game's logical 2000x1200 canvas.
// Touch coordinates are already normalized and converted separately.
SDL_Point mouse_to_logical(SDL_Renderer* renderer, int x, int y) {
    // Convert the mouse position manually for SDL2; no the removed SDL2-incompatible conversion helper call is used.
    /*     the renderer's logical viewport, which also handles aspect-ratio
    letterboxing on devices whose screen is not exactly 2000x1200. */
    int logicalW = SCREEN_WIDTH;
    int logicalH = SCREEN_HEIGHT;
    SDL_RenderGetLogicalSize(renderer, &logicalW, &logicalH);

    int outputW = 0, outputH = 0;
    SDL_GetRendererOutputSize(renderer, &outputW, &outputH);

    SDL_Rect viewport{0, 0, outputW, outputH};
    SDL_RenderGetViewport(renderer, &viewport);

    if (viewport.w <= 0 || viewport.h <= 0)
        return SDL_Point{x, y};

    const float scaleX = static_cast<float>(logicalW) / static_cast<float>(viewport.w);
    const float scaleY = static_cast<float>(logicalH) / static_cast<float>(viewport.h);

    SDL_Point p;
    p.x = static_cast<int>((static_cast<float>(x) - viewport.x) * scaleX);
    p.y = static_cast<int>((static_cast<float>(y) - viewport.y) * scaleY);

    // Keep converted coordinates inside the logical canvas.
    p.x = (p.x < 0) ? 0 : ((p.x >= logicalW) ? logicalW - 1 : p.x);
    p.y = (p.y < 0) ? 0 : ((p.y >= logicalH) ? logicalH - 1 : p.y);
    return p;
}

bool editorTouchActive = false;
SDL_FingerID editorFingerId = -1;
int editorLastTouchX = 0, editorLastTouchY = 0;
bool editorDragged = false;

// Bomb defusing: the player must continuously hold the button near the bomb.
constexpr Uint32 DEFUSE_TIME_MS = 3000;
bool defuseHeld = false;
Uint32 defuseStartTime = 0;
Uint32 lastFuseBeepTime = 0;

GameState currentGameState = GameState::MODE_SELECTION;
GameMode selectedMode = GameMode::TACTICAL;
EnemyDifficulty tacticalEnemyDifficulty = EnemyDifficulty::NORMAL;
bool tacticalDifficultyChosen = false; // difficulty selector starts with nothing highlighted
                                        // until the player actually taps NORMAL/HARD/EXPERT
int customTacticalEnemies = 5;
int customBotCount = 3; // ally Sentinel bot count, adjustable 0-20 in TACTICAL_CONFIG
int playerKills = 0;
int playerDeaths = 0;
int playerBombDefuses = 0;
int playerWins = 0;
int playerLosses = 0;

// --- Tactical round tracking ------------------------------------------------
// A tactical mission is 10 rounds. Bots keep the same roster (names, kills,
// deaths) across all 10 rounds of one mission - only a brand new mission
// (Start Mission from TACTICAL_CONFIG) reshuffles/resets the bot roster.
constexpr int MAX_TACTICAL_ROUNDS = 10;
int currentRound = 1;
std::string playerName = "VETERAN_01";
bool profileEditActive = false;
std::string profileNameInput;
bool showLeaderboardPanel = false; // manual toggle via the HUD button during play
bool showMatchEndLeaderboard = false; // shown after the mission-accomplished screen
bool showMissionAccomplishedScreen = false; // short result screen before the final leaderboard
Uint32 missionAccomplishedAt = 0;

void profile_open_edit_dialog() {
    profileEditActive = true;
    profileNameInput = playerName;
    // Force a fresh OFF->ON edge every time the dialog opens. If the on-screen
    // keyboard was dismissed directly (tap outside the field, back gesture,
    // the keyboard's own dismiss key) without going through Save/Cancel, SDL
    // still considers text input "active" - calling Start again in that state
    // is a no-op on many platforms and the keyboard never reappears. Stopping
    // first guarantees the edge that actually summons it.
    SDL_StopTextInput();
    SDL_StartTextInput();
}

void profile_close_edit_dialog() {
    profileEditActive = false;
    SDL_StopTextInput();
}

int playerMaxHp = 100, playerHp = 100;

// Weapon state: current + previous, so quick-swap (Q) and the hotbar can both
// reference "what was I just holding" without any extra bookkeeping elsewhere.
struct PlayerWeaponState {
    WeaponType current{WeaponType::PISTOL};
    WeaponType previous{WeaponType::PISTOL};
    std::array<WeaponType, 4> equipped{{WeaponType::PISTOL, WeaponType::RIFLE, WeaponType::ROCKET, WeaponType::LASER}};
    int equippedCount{4};
};
PlayerWeaponState playerWeaponState;
WeaponType& playerWeapon = playerWeaponState.current;
Uint32 weaponSwitchFlashAt = 0;

bool is_weapon_equipped(WeaponType w) {
    for (int i = 0; i < playerWeaponState.equippedCount; ++i)
        if (playerWeaponState.equipped[i] == w) return true;
    return false;
}

void refresh_weapon_radial();

WeaponType cycle_weapon(WeaponType w, int dir) {
    if (playerWeaponState.equippedCount <= 0) return WeaponType::PISTOL;
    int currentIndex = 0;
    for (int i = 0; i < playerWeaponState.equippedCount; ++i) {
        if (playerWeaponState.equipped[i] == w) { currentIndex = i; break; }
    }
    int next = (currentIndex + dir + playerWeaponState.equippedCount) % playerWeaponState.equippedCount;
    return playerWeaponState.equipped[next];
}

void set_player_weapon(WeaponType newWeapon, Uint32 currentTime) {
    if (!is_weapon_equipped(newWeapon) || newWeapon == playerWeaponState.current) return;
    playerWeaponState.previous = playerWeaponState.current;
    playerWeaponState.current = newWeapon;
    weaponSwitchFlashAt = currentTime;
}

void toggle_weapon_loadout(WeaponType w, Uint32 currentTime) {
    for (int i = 0; i < playerWeaponState.equippedCount; ++i) {
        if (playerWeaponState.equipped[i] == w) {
            if (playerWeaponState.equippedCount <= 1) return;
            for (int j = i; j < playerWeaponState.equippedCount - 1; ++j)
                playerWeaponState.equipped[j] = playerWeaponState.equipped[j + 1];
            --playerWeaponState.equippedCount;
            if (!is_weapon_equipped(playerWeaponState.current))
                playerWeaponState.current = playerWeaponState.equipped[0];
            refresh_weapon_radial();
            weaponSwitchFlashAt = currentTime;
            return;
        }
    }
    if (playerWeaponState.equippedCount >= 4) return;
    playerWeaponState.equipped[playerWeaponState.equippedCount++] = w;
    refresh_weapon_radial();
}

// Electric Shield: pistol-only special ability. While active, blocks all incoming damage
// (direct bullets + rocket splash) and instantly destroys any enemy that touches the player.
constexpr Uint32 SHIELD_DURATION_MS = 3000; // how long the shield stays up once activated
constexpr Uint32 SHIELD_COOLDOWN_MS = 5000; // cooldown after it drops before it can be used again
bool playerShieldActive = false;
Uint32 playerShieldActivatedAt = 0;
Uint32 playerShieldReadyAt = 0; // shield can be (re)activated once currentTime >= this

// Dash/dodge-roll I-Frames - global (not just a main() local) so damage-applying
// helpers like trigger_explosion() can also respect it.
bool playerIsInvulnerable = false;

void try_activate_shield(Uint32 currentTime) {
    if (playerWeapon != WeaponType::PISTOL) return; // pistol-exclusive
    if (playerShieldActive) return;                 // already up
    if (currentTime < playerShieldReadyAt) return;   // still on cooldown
    playerShieldActive = true;
    playerShieldActivatedAt = currentTime;
}

int score = 0;
int highScore = 0;
int currentLevel = 1;
int activeEnemyCount = 1;
Bomb tacticalBomb{};
int botDefuserIndex = -1; // Sentinel assigned to the current planted bomb

std::array<Bullet, MAX_BULLETS> bullets{};
std::array<Explosion, MAX_EXPLOSIONS> explosions{};
std::array<GlassShatter, 32> glassShatters{};
std::array<HealthPack, MAX_HEALTH_PACKS> healthPacks{};
std::array<Enemy, MAX_ENEMIES> enemies{};
std::array<LaserBeam, 16> laserBeams{}; // pool - each shot gets its own slot instead of
                                         // clobbering one shared beam when player and enemy
                                         // lasers fire close together

// --- Leaderboard --------------------------------------------------------
struct LBEntry {
    std::string name;
    int kills{0};
    int deaths{0};
    int bombDefuses{0};
    int bombPlants{0};
    int scoreValue{0};
    bool isPlayer{false};
    Team team{Team::GREEN_DEFUSER};
};

std::vector<LBEntry> build_leaderboard() {
    std::vector<LBEntry> rows;

    // Sentinel section: the player is always first in this faction's roster,
    // followed by the allied Sentinel bots.
    rows.push_back({playerName, playerKills, playerDeaths, playerBombDefuses, 0, score, true, Team::GREEN_DEFUSER});
    for (int i = 0; i < activeBotCount; ++i) {
        const Bot& bot = bots[i];
        if (bot.name.empty()) continue;
        rows.push_back({bot.name, bot.kills, bot.deaths, bot.bombDefuses, 0, bot.kills * 10 + bot.bombDefuses * 100, false, Team::GREEN_DEFUSER});
    }

    // Raider section.
    for (int i = 0; i < activeEnemyCount; ++i) {
        const Enemy& enemy = enemies[i];
        if (enemy.name.empty()) continue;
        rows.push_back({enemy.name, enemy.kills, enemy.deaths, 0, enemy.bombPlants, enemy.kills * 10 + enemy.bombPlants * 100, false, Team::RED_BOMBER});
    }
    return rows;
}

// Shared leaderboard panel - Sentinel section is always shown first, followed
// by the Raider section. Each faction is sorted independently by score.
void render_leaderboard_panel(SDL_Renderer* renderer, const SDL_Rect& box, const std::string& title) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Dark translucent overlay, as in the supplied leaderboard reference.
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 175);
    SDL_Rect fullScreen{0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
    SDL_RenderFillRect(renderer, &fullScreen);

    // Main dark panel with a thin light outline.
    SDL_SetRenderDrawColor(renderer, 18, 22, 30, 252);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 235, 235, 240, 255);
    draw_rect_outline(renderer, box);

    // IMAGE-1: centered cyan LEADERBOARD title.
    draw_text(renderer, title, centered_text_x(title, 3, box.x, box.w),
              box.y + 24, 3, {100, 220, 255, 255});

    // The two teams intentionally have DIFFERENT stat columns:
    // Sentinels = KILLS / DEATHS / DEFUSED / SCORE
    // Raiders   = KILLS / DEATHS / PLANTED / SCORE
    // This removes DEFUSED from Raiders and PLANTED from Sentinels.
    const int nameX   = box.x + 28;
    const int killsX  = box.x + 270;
    const int deathsX = box.x + 405;
    const int actionX = box.x + 550;
    const int scoreX  = box.x + 690;
    const int tableRight = box.x + box.w - 14;

    const SDL_Color columnColor{205, 205, 215, 255};
    const int headerH = 38;
    const int rowH = 55;
    const int sectionH = 48;
    const int gapBetweenTeams = 28;

    std::vector<LBEntry> rows = build_leaderboard();
    std::vector<LBEntry> sentinels;
    std::vector<LBEntry> raiders;
    for (const LBEntry& row : rows) {
        if (row.team == Team::GREEN_DEFUSER) sentinels.push_back(row);
        else raiders.push_back(row);
    }

    auto sortByScore = [](const LBEntry& a, const LBEntry& b) {
        return a.scoreValue > b.scoreValue;
    };
    if (sentinels.size() > 1) std::sort(sentinels.begin() + 1, sentinels.end(), sortByScore);
    std::sort(raiders.begin(), raiders.end(), sortByScore);

    int y = box.y + 92;

    // Draw a full-width faction title bar. Raiders gets its own separate bar.
    auto drawTeamBar = [&](const char* label, SDL_Color accent) {
        SDL_SetRenderDrawColor(renderer, 34, 38, 46, 255);
        SDL_Rect bar{box.x + 14, y, box.w - 28, sectionH};
        SDL_RenderFillRect(renderer, &bar);
        draw_text(renderer, label, box.x + 28, y + 10, 2, accent);
        y += sectionH;
    };

    auto drawHeader = [&](const char* actionLabel) {
        SDL_SetRenderDrawColor(renderer, 10, 13, 18, 230);
        SDL_Rect header{box.x + 14, y, box.w - 28, headerH};
        SDL_RenderFillRect(renderer, &header);

        draw_text(renderer, "NAME",   nameX,   y + 10, 2, columnColor);
        draw_text(renderer, "KILLS",  killsX,  y + 10, 2, columnColor);
        draw_text(renderer, "DEATHS", deathsX, y + 10, 2, columnColor);
        draw_text(renderer, actionLabel, actionX, y + 10, 2, columnColor);
        draw_text(renderer, "SCORE",  scoreX,  y + 10, 2, columnColor);
        y += headerH;
    };

    auto drawEntry = [&](const LBEntry& row, bool sentinel) {
        const SDL_Color textColor = sentinel
            ? SDL_Color{60, 245, 100, 255}
            : SDL_Color{255, 85, 85, 255};

        // Player row gets the same subtle blue-gray highlight visible in Image 1.
        if (row.isPlayer) {
            SDL_SetRenderDrawColor(renderer, 45, 70, 100, 190);
        } else {
            SDL_SetRenderDrawColor(renderer, 24, 28, 36, 185);
        }
        SDL_Rect rowRect{box.x + 14, y, box.w - 28, rowH};
        SDL_RenderFillRect(renderer, &rowRect);

        std::string name = row.name;
        if (name.size() > 14) name.resize(14);

        // Fixed X positions prevent all column text from mixing together.
        draw_text(renderer, name, nameX, y + 14, 2, textColor);
        draw_text(renderer, std::to_string(row.kills),  killsX,  y + 14, 2, textColor);
        draw_text(renderer, std::to_string(row.deaths), deathsX, y + 14, 2, textColor);
        if (sentinel) {
            draw_text(renderer, std::to_string(row.bombDefuses), actionX, y + 14, 2, textColor);
        } else {
            draw_text(renderer, std::to_string(row.bombPlants), actionX, y + 14, 2, textColor);
        }
        draw_text(renderer, std::to_string(row.scoreValue), scoreX, y + 14, 2, textColor);
        y += rowH;
    };

    // SENTINELS -------------------------------------------------------------
    drawTeamBar("SENTINELS", {60, 245, 90, 255});
    drawHeader("DEFUSED");
    for (const LBEntry& row : sentinels) drawEntry(row, true);

    // Separate space + separate RAIDERS title bar, exactly as requested.
    y += gapBetweenTeams;

    // Keep the lower section inside the panel even if bot count is increased.
    drawTeamBar("RAIDERS", {255, 85, 85, 255});
    drawHeader("PLANTED");
    for (const LBEntry& row : raiders) drawEntry(row, false);

    // No extra DEFUSED column appears in the Raider table, and no PLANTED
    // column appears in the Sentinel table.
    (void)tableRight;
}



// Small live tactical map shown during gameplay. It deliberately displays ONLY
// the map layout and the player's current position/direction; no enemies, bots,
// bomb carrier, bomb, health packs, or other tactical information are revealed.
void render_live_minimap(SDL_Renderer* renderer, const SDL_Rect& box, float playerWorldX, float playerWorldY, float dirX, float dirY) {
    // Blueprint-style tactical map: technical, clean, and readable rather than
    // a solid green minimap.  The live map intentionally reveals only layout
    // plus the player's current position/direction.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_SetRenderDrawColor(renderer, 5, 12, 24, 245);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 45, 135, 190, 255);
    draw_rect_outline(renderer, box, 2);

    const int headerH = 28;
    const int pad = 10;
    const int mapX = box.x + pad;
    const int mapY = box.y + headerH + pad;
    const int mapW = box.w - pad * 2;
    const int mapH = box.h - headerH - pad * 2;

    // Technical header.
    draw_text(renderer, "LIVE MAP // BLUEPRINT", box.x + 12, box.y + 7, 1,
              {100, 205, 255, 255});
    SDL_SetRenderDrawColor(renderer, 35, 100, 145, 180);
    SDL_RenderDrawLine(renderer, box.x + 10, box.y + headerH,
                       box.x + box.w - 10, box.y + headerH);

    const float sx = static_cast<float>(mapW) / std::max(1, currentMapCols);
    const float sy = static_cast<float>(mapH) / std::max(1, currentMapRows);

    // Blueprint grid.
    SDL_SetRenderDrawColor(renderer, 20, 70, 105, 95);
    for (int c = 0; c <= currentMapCols; ++c) {
        int x = mapX + static_cast<int>(c * sx);
        SDL_RenderDrawLine(renderer, x, mapY, x, mapY + mapH);
    }
    for (int r = 0; r <= currentMapRows; ++r) {
        int y = mapY + static_cast<int>(r * sy);
        SDL_RenderDrawLine(renderer, mapX, y, mapX + mapW, y);
    }

    // Draw floor/walls as technical blueprint geometry.
    for (int r = 0; r < currentMapRows; ++r) {
        for (int c = 0; c < currentMapCols; ++c) {
            int tile = gameMap[r][c];
            bool walkable = (tile == 0 || tile == 8 || tile == 9 || tile == 10 || tile == 11);
            int x0 = mapX + static_cast<int>(c * sx);
            int y0 = mapY + static_cast<int>(r * sy);
            int x1 = mapX + static_cast<int>((c + 1) * sx);
            int y1 = mapY + static_cast<int>((r + 1) * sy);
            SDL_Rect cell{x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};

            if (walkable) {
                SDL_SetRenderDrawColor(renderer, 8, 35, 58, 150);
                SDL_RenderFillRect(renderer, &cell);
            } else {
                SDL_SetRenderDrawColor(renderer, 11, 24, 38, 245);
                SDL_RenderFillRect(renderer, &cell);
                SDL_SetRenderDrawColor(renderer, 55, 155, 205, 210);
                draw_rect_outline(renderer, cell, 1);
            }
        }
    }

    // Highlight the map boundary like a technical drafting frame.
    SDL_SetRenderDrawColor(renderer, 70, 175, 225, 210);
    SDL_RenderDrawLine(renderer, mapX, mapY, mapX + mapW, mapY);
    SDL_RenderDrawLine(renderer, mapX, mapY + mapH, mapX + mapW, mapY + mapH);
    SDL_RenderDrawLine(renderer, mapX, mapY, mapX, mapY + mapH);
    SDL_RenderDrawLine(renderer, mapX + mapW, mapY, mapX + mapW, mapY + mapH);

    float worldCx = playerWorldX + 30.0f;
    float worldCy = playerWorldY + 30.0f;
    float mx = mapX + (worldCx / std::max(1.0f, currentMapCols * static_cast<float>(TILE_SIZE))) * mapW;
    float my = mapY + (worldCy / std::max(1.0f, currentMapRows * static_cast<float>(TILE_SIZE))) * mapH;

    // Player marker: cyan drafting arrow with a small centre dot.
    float dx = dirX, dy = dirY;
    if (std::abs(dx) < 0.01f && std::abs(dy) < 0.01f) { dx = 0.0f; dy = -1.0f; }
    float len = std::sqrt(dx * dx + dy * dy);
    if (len > 0.001f) { dx /= len; dy /= len; }
    float px = -dy, py = dx;
    const float tipLen = 14.0f, baseLen = 7.0f, halfBase = 6.0f;
    SDL_Point tip{static_cast<int>(mx + dx * tipLen), static_cast<int>(my + dy * tipLen)};
    SDL_Point left{static_cast<int>(mx - dx * baseLen + px * halfBase), static_cast<int>(my - dy * baseLen + py * halfBase)};
    SDL_Point right{static_cast<int>(mx - dx * baseLen - px * halfBase), static_cast<int>(my - dy * baseLen - py * halfBase)};
    SDL_SetRenderDrawColor(renderer, 120, 235, 255, 255);
    SDL_RenderDrawLine(renderer, tip.x, tip.y, left.x, left.y);
    SDL_RenderDrawLine(renderer, left.x, left.y, right.x, right.y);
    SDL_RenderDrawLine(renderer, right.x, right.y, tip.x, tip.y);
    SDL_RenderDrawLine(renderer, static_cast<int>(mx), static_cast<int>(my), tip.x, tip.y);
    SDL_Rect centre{static_cast<int>(mx) - 2, static_cast<int>(my) - 2, 5, 5};
    SDL_RenderFillRect(renderer, &centre);

    // Small drafting labels for orientation.
    draw_text(renderer, "N", box.x + box.w - 25, box.y + 7, 1, {110, 220, 255, 255});
std::string current_play_map_name;
                                     
draw_text(renderer, current_play_map_name, box.x + 12, box.y + box.h - 15, 1,

              {80, 160, 200, 220});
}

bool save_map_to_file(const std::string& filename) {
    std::ofstream outFile(filename, std::ios::out | std::ios::trunc);
    if (!outFile.is_open()) return false;
    outFile << "FORMAT 2\n";
    outFile << "SIZE " << editorMapCols << " " << editorMapRows << "\n";
    // Space-separate tile values so multi-digit tiles such as GLASS (10)
    // cannot be loaded back as WALL (1) followed by FLOOR (0).
    for (int r = 0; r < editorMapRows; ++r) {
        for (int c = 0; c < editorMapCols; ++c) {
            if (c) outFile << ' ';
            outFile << gameMap[r][c];
        }
        outFile << "\n";
    }
    outFile.flush();
    bool ok = outFile.good();
    outFile.close();
    return ok;
}

bool load_map_from_file(const std::string& filename) {
    std::ifstream inFile(filename);
    if (!inFile.is_open()) return false;

    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            gameMap[r][c] = 0;

    std::string line;
    int r = 0;
    int loadedCols = COLS, loadedRows = ROWS;

    if (std::getline(inFile, line)) {
        std::istringstream header(line);
        std::string tag;
        int w = 0, h = 0;
        if (line == "FORMAT 2") {
            // New format: the next line contains SIZE and all rows are whitespace-separated.
            if (std::getline(inFile, line)) {
                std::istringstream sizeHeader(line);
                if (sizeHeader >> tag >> w >> h && tag == "SIZE") {
                    loadedCols = std::clamp(w, 20, COLS);
                    loadedRows = std::clamp(h, 15, ROWS);
                }
            }
        } else if ((header >> tag >> w >> h) && tag == "SIZE") {
            // Version-1 map: SIZE followed by one-character-per-tile rows.
            loadedCols = std::clamp(w, 20, COLS);
            loadedRows = std::clamp(h, 15, ROWS);
        } else {
            for (int c = 0; c < loadedCols && c < static_cast<int>(line.size()); ++c)
                if (std::isdigit(static_cast<unsigned char>(line[c]))) gameMap[0][c] = line[c] - '0';
            r = 1;
        }
    }

    while (std::getline(inFile, line) && r < loadedRows) {
        std::istringstream rowStream(line);
        std::vector<int> values;
        int value = 0;
        while (rowStream >> value) values.push_back(value);

        if (!values.empty()) {
            // New format: whitespace-separated tile IDs, including 10 = GLASS.
            for (int c = 0; c < loadedCols && c < static_cast<int>(values.size()); ++c)
                gameMap[r][c] = values[c];
        } else {
            // Backward compatibility with old maps containing one digit per tile.
            for (int c = 0; c < loadedCols && c < static_cast<int>(line.size()); ++c)
                if (std::isdigit(static_cast<unsigned char>(line[c]))) gameMap[r][c] = line[c] - '0';
        }
        ++r;
    }
    inFile.close();

    currentMapCols = editorMapCols = loadedCols;
    currentMapRows = editorMapRows = loadedRows;

    for (int c = 0; c < currentMapCols; ++c) {
        gameMap[0][c] = 1;
        gameMap[currentMapRows - 1][c] = 1;
    }
    for (int rr = 0; rr < currentMapRows; ++rr) {
        gameMap[rr][0] = 1;
        gameMap[rr][currentMapCols - 1] = 1;
    }
    return r > 0;
}

void scan_bombsites() {
    bombSiteA = {-1, -1};
    bombSiteB = {-1, -1};
    bombSiteAValid = bombSiteBValid = false;
    for (int r = 0; r < currentMapRows; ++r) {
        for (int c = 0; c < currentMapCols; ++c) {
            if (gameMap[r][c] == 8 && !bombSiteAValid) {
                bombSiteA = {c, r};
                bombSiteAValid = true;
            } else if (gameMap[r][c] == 9 && !bombSiteBValid) {
                bombSiteB = {c, r};
                bombSiteBValid = true;
            }
        }
    }
}

Point nearest_free_tile(Point p) {
    auto ok = [](int x, int y) {
        return x > 0 && x < currentMapCols - 1 && y > 0 && y < currentMapRows - 1 &&
               (gameMap[y][x] == 0 || gameMap[y][x] == 8 || gameMap[y][x] == 9 || gameMap[y][x] == 10);
    };
    if (ok(p.x, p.y)) return p;
    for (int rad = 1; rad < std::max(currentMapCols, currentMapRows); ++rad) {
        for (int dy = -rad; dy <= rad; ++dy) {
            for (int dx = -rad; dx <= rad; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != rad) continue;
                if (ok(p.x + dx, p.y + dy)) return {p.x + dx, p.y + dy};
            }
        }
    }
    return {1, 1};
}

void reset_editor_map_size(int w, int h) {
    editorMapCols = std::clamp(w, 20, COLS);
    editorMapRows = std::clamp(h, 15, ROWS);
    currentMapCols = editorMapCols;
    currentMapRows = editorMapRows;
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            gameMap[r][c] = 0;
    for (int c = 0; c < editorMapCols; ++c) {
        gameMap[0][c] = 1;
        gameMap[editorMapRows - 1][c] = 1;
    }
    for (int r = 0; r < editorMapRows; ++r) {
        gameMap[r][0] = 1;
        gameMap[r][editorMapCols - 1] = 1;
    }
    cameraX = cameraY = 0;
    editorZoom = 1.0f;
    scan_bombsites();
}

bool is_glass_tile(int c, int r) {
    return c >= 0 && c < currentMapCols && r >= 0 && r < currentMapRows && gameMap[r][c] == 10;
}

bool rect_hits_glass(const SDL_Rect& rect, int* hitCol = nullptr, int* hitRow = nullptr) {
    int startCol = std::max(0, rect.x / TILE_SIZE);
    int endCol = std::min(currentMapCols - 1, (rect.x + rect.w - 1) / TILE_SIZE);
    int startRow = std::max(0, rect.y / TILE_SIZE);
    int endRow = std::min(currentMapRows - 1, (rect.y + rect.h - 1) / TILE_SIZE);
    for (int r = startRow; r <= endRow; ++r) {
        for (int c = startCol; c <= endCol; ++c) {
            if (is_glass_tile(c, r)) {
                if (hitCol) *hitCol = c;
                if (hitRow) *hitRow = r;
                return true;
            }
        }
    }
    return false;
}

bool check_wall_collision(SDL_Rect rect) {
    int startCol = rect.x / TILE_SIZE;
    int endCol = (rect.x + rect.w - 1) / TILE_SIZE;
    int startRow = rect.y / TILE_SIZE;
    int endRow = (rect.y + rect.h - 1) / TILE_SIZE;

    for (int r = startRow; r <= endRow; ++r) {
        for (int c = startCol; c <= endCol; ++c) {
            // STRICT BOUNDARY: If outside the array, it's an instant collision
            if (r < 0 || r >= currentMapRows || c < 0 || c >= currentMapCols) {
                return true; 
            }
            // Solid walls block movement. Glass (10) is now also solid to movement -
            // it must be shot open first; once shattered the tile becomes 0 (floor)
            // and this check naturally stops blocking it. Bombsite tags (8/9) stay walkable.
            if (gameMap[r][c] == 1 || gameMap[r][c] == 10) {
                return true;
            }
        }
    }
    return false;
}


// Returns whether a full-size enemy can actually occupy a tile.  The old
// pathfinder treated every non-wall tile as walkable, but a 60x60 enemy can
// overlap a wall even when its tile itself is open.  That was the main cause
// of enemies getting pinned against corners and then appearing unresponsive.
bool ai_tile_walkable(int c, int r) {
    if (c < 1 || c >= currentMapCols - 1 || r < 1 || r >= currentMapRows - 1) return false;
    if (!(gameMap[r][c] == 0 || gameMap[r][c] == 8 || gameMap[r][c] == 9)) return false;
    SDL_Rect probe{c * TILE_SIZE + 5, r * TILE_SIZE + 5, 60, 60};
    return !check_wall_collision(probe);
}

Point get_next_ai_step(Point start, Point target) {
    start.x = std::clamp(start.x, 1, currentMapCols - 2);
    start.y = std::clamp(start.y, 1, currentMapRows - 2);
    target.x = std::clamp(target.x, 1, currentMapCols - 2);
    target.y = std::clamp(target.y, 1, currentMapRows - 2);
    if (start.x == target.x && start.y == target.y) return start;

    Point parent[ROWS][COLS];
    bool visited[ROWS][COLS] = {{false}};
    std::vector<Point> queue;
    size_t head = 0;

    queue.push_back(start);
    visited[start.y][start.x] = true;

    const int dx[] = {0, 0, -1, 1};
    const int dy[] = {-1, 1, 0, 0};
    bool found = false;

    while (head < queue.size()) {
        Point curr = queue[head++];
        if (curr.x == target.x && curr.y == target.y) { found = true; break; }

        for (int i = 0; i < 4; ++i) {
            int nx = curr.x + dx[i], ny = curr.y + dy[i];
            if (nx < 1 || nx >= currentMapCols - 1 || ny < 1 || ny >= currentMapRows - 1) continue;
            if (visited[ny][nx] || !ai_tile_walkable(nx, ny)) continue;
            visited[ny][nx] = true;
            parent[ny][nx] = curr;
            queue.push_back(Point{nx, ny});
        }
    }

    if (!found) {
        // If the exact target is temporarily unreachable, pick the best
        // walkable neighbour instead of repeatedly pushing into the same wall.
        Point best = start;
        int bestDist = 0x7fffffff;
        for (int i = 0; i < 4; ++i) {
            int nx = start.x + dx[i], ny = start.y + dy[i];
            if (!ai_tile_walkable(nx, ny)) continue;
            int d = std::abs(nx - target.x) + std::abs(ny - target.y);
            if (d < bestDist) { bestDist = d; best = Point{nx, ny}; }
        }
        return best;
    }

    // Walk the parent chain backwards until we reach the first step.
    Point curr = target;
    while (!(parent[curr.y][curr.x].x == start.x && parent[curr.y][curr.x].y == start.y)) {
        curr = parent[curr.y][curr.x];
        if (curr.x == start.x && curr.y == start.y) break;
    }
    return curr;
}
// ownerIsPlayerTeam/ownerIndex identify who lit the fuse on this blast, so a
// kill from splash damage gets credited the same way a direct hit does:
//   ownerIsPlayerTeam == true,  ownerIndex == -1   -> player
//   ownerIsPlayerTeam == true,  ownerIndex >= 0     -> that bots[] slot
//   ownerIsPlayerTeam == false, ownerIndex >= 0     -> that enemies[] slot
//   ownerIndex == -2                                 -> no attribution (e.g. the bomb fuse)
void trigger_explosion(float blastX, float blastY, float radius, int baseDamage, SDL_Rect& player, int& pHp,
                        bool ownerIsPlayerTeam = true, int ownerIndex = -2) {
    for (auto& exp : explosions) {
        if (!exp.active) {
            exp.x = blastX;
            exp.y = blastY;
            exp.maxRadius = radius;
            exp.spawnTime = SDL_GetTicks();
            exp.duration = 300;
            exp.active = true;
            break;
        }
    }

    auto apply_splash = [&](const SDL_Rect& target, int& targetHp) {
        float cx = target.x + target.w / 2.0f;
        float cy = target.y + target.h / 2.0f;
        float dist = std::sqrt((cx - blastX) * (cx - blastX) + (cy - blastY) * (cy - blastY));
        if (dist <= radius) {
            float factor = 1.0f - (dist / radius);
            int scaledBaseDamage = baseDamage + (currentLevel * 5);
            int damage = std::max(20, static_cast<int>(scaledBaseDamage * factor));
            targetHp -= damage;
        }
    };

    auto credit_kill = [&]() {
        if (ownerIndex == -2) return; // unattributed blast (bomb fuse, etc.)
        if (ownerIsPlayerTeam) {
            if (ownerIndex >= 0 && ownerIndex < activeBotCount) bots[ownerIndex].kills++;
            else playerKills++;
        } else if (ownerIndex >= 0 && ownerIndex < activeEnemyCount) {
            enemies[ownerIndex].kills++;
        }
    };

    if (pHp > 0 && !playerShieldActive && !playerIsInvulnerable) {
        int hpBefore = pHp;
        apply_splash(player, pHp);
        if (pHp < hpBefore && defuseHeld) { defuseHeld = false; defuseStartTime = 0; }
        if (hpBefore > 0 && pHp <= 0) {
            playerDeaths++;
            credit_kill();
        }
    }
    for (auto& enemy : enemies) {
        if (enemy.active && enemy.hp > 0) {
            apply_splash(enemy.rect, enemy.hp);
            if (enemy.hp <= 0) {
                enemy.active = false;
                enemy.deaths = 1;
                score += 10;
                highScore = std::max(highScore, score);
                if (ownerIsPlayerTeam) credit_kill(); // only player-team splash scores a Raider kill
            }
        }
    }
    for (int bi = 0; bi < activeBotCount; ++bi) {
        Bot& bot = bots[bi];
        if (!ownerIsPlayerTeam && bot.active && bot.hp > 0) { // only a Raider's blast can splash a friendly bot
            apply_splash(bot.rect, bot.hp);
            if (bot.hp <= 0) {
                bot.active = false;
                bot.deaths++;
                credit_kill();
            }
        }
    }
    play_explosion_sound();
}

void fire_laser(float startX, float startY, float dirX, float dirY, bool isPlayer, const SDL_Rect* playerHitbox = nullptr, EnemyType shooterType = EnemyType::RED, int ownerBotIndex = -1) {
    // Laser is hitscan and supports all 8 aiming directions.
    // Normalize the direction so diagonal lasers travel at the same effective speed.
    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len < 0.0001f) return;
    dirX /= len;
    dirY /= len;
    const float stepX = dirX * 12.0f;
    const float stepY = dirY * 12.0f;

    SDL_Rect beamCheck = { static_cast<int>(startX), static_cast<int>(startY), 4, 4 };
    std::vector<Enemy*> hitEnemies; // dedup so a wide enemy isn't hit multiple times as the beam steps through it

    while (beamCheck.x > 0 && beamCheck.x < currentMapCols * TILE_SIZE && beamCheck.y > 0 && beamCheck.y < currentMapRows * TILE_SIZE) {
        beamCheck.x += static_cast<int>(stepX);
        beamCheck.y += static_cast<int>(stepY);

        // Glass now blocks like a wall (see check_wall_collision), so the beam must
        // shatter it here first - same as bullets do - or every laser would stop dead
        // at the first pane instead of blasting through it.
        int glassCol = -1, glassRow = -1;
        if (rect_hits_glass(beamCheck, &glassCol, &glassRow)) {
            gameMap[glassRow][glassCol] = 0;
            for (auto& gs : glassShatters) {
                if (!gs.active) {
                    gs.x = glassCol * TILE_SIZE + TILE_SIZE / 2.0f;
                    gs.y = glassRow * TILE_SIZE + TILE_SIZE / 2.0f;
                    gs.spawnTime = SDL_GetTicks();
                    gs.duration = 420;
                    gs.active = true;
                    break;
                }
            }
            play_glass_shatter_sound();
        }

        if (check_wall_collision(beamCheck)) break;

        if (isPlayer) {
            // Pierces through every enemy in its path rather than stopping at the first one -
            // it's drawn as a continuous beam, so it should hit like one. Each enemy it
            // touches dies instantly - the laser one-shots everything in its line, not
            // just the first target.
            for (auto& enemy : enemies) {
                if (enemy.active && enemy.hp > 0 && SDL_HasIntersection(&beamCheck, &enemy.rect)) {
                    if (std::find(hitEnemies.begin(), hitEnemies.end(), &enemy) != hitEnemies.end()) continue;
                    hitEnemies.push_back(&enemy);

                    enemy.hp -= WEAPON_PROPS[static_cast<int>(WeaponType::LASER)].damage;
                    if (enemy.hp <= 0) {
                        enemy.active = false;
                        enemy.deaths = 1;
                        score += 10;
                        // Attribute the kill the same way bullet hits do: to the
                        // firing bot if this beam came from one, otherwise the player.
                        if (ownerBotIndex >= 0 && ownerBotIndex < activeBotCount) {
                            bots[ownerBotIndex].kills++;
                        } else {
                            playerKills++;
                        }
                    }
                    highScore = std::max(highScore, score);
                }
            }
        } else if (playerHitbox && SDL_HasIntersection(&beamCheck, playerHitbox)) {
            if (!playerShieldActive && !playerIsInvulnerable) {
                playerHp -= WEAPON_PROPS[static_cast<int>(WeaponType::LASER)].damage;
                highScore = std::max(highScore, score);
                if (playerHp <= 0) {
                    currentGameState = GameState::GAME_OVER;
                    playerDeaths++;
                    // This beam's shooter is the Raider at ownerBotIndex (repurposed
                    // as the enemy roster index for enemy-fired shots).
                    if (ownerBotIndex >= 0 && ownerBotIndex < activeEnemyCount) {
                        enemies[ownerBotIndex].kills++;
                    }
                }
                if (defuseHeld) { defuseHeld = false; defuseStartTime = 0; }
            }
            break; // there's only one player - beam stops once it lands
        }
    }

    for (auto& beam : laserBeams) {
        if (!beam.active) {
            beam.start = { static_cast<int>(startX), static_cast<int>(startY) };
            beam.end = { beamCheck.x, beamCheck.y };
            beam.spawnTime = SDL_GetTicks();
            beam.duration = 100;
            // Beam color identifies the shooter: player laser is green, RED enemy
            // laser is red, PINK enemy laser is pink.
            if (isPlayer) {
                beam.color = SDL_Color{60, 220, 120, 255};
            } else if (shooterType == EnemyType::PINK) {
                beam.color = SDL_Color{255, 105, 180, 255};
            } else {
                beam.color = SDL_Color{220, 60, 60, 255};
            }
            beam.isPlayerShot = isPlayer;
            beam.active = true;
            break;
        }
    }

    play_gunshot_sound(isPlayer, WeaponType::LASER);
}

void spawn_cardinal_bullet(float startX, float startY, Direction dir, bool isPlayer, WeaponType wType, const SDL_Rect* playerHitbox = nullptr, EnemyType shooterType = EnemyType::RED, int ownerBotIndex = -1) {
    if (wType == WeaponType::LASER) {
        float dx = 0.0f, dy = 0.0f;
        if (dir == Direction::DIR_UP)    dy = -1.0f;
        if (dir == Direction::DIR_DOWN)  dy =  1.0f;
        if (dir == Direction::DIR_LEFT)  dx = -1.0f;
        if (dir == Direction::DIR_RIGHT) dx =  1.0f;
        fire_laser(startX, startY, dx, dy, isPlayer, playerHitbox, shooterType, ownerBotIndex);
        return;
    }

    const auto& props = WEAPON_PROPS[static_cast<int>(wType)];
    float speed = props.speed;
    int sz = (wType == WeaponType::ROCKET) ? 16 : 8;

    for (auto& b : bullets) {
        if (!b.active) {
            b.x = startX; b.y = startY;
            b.isPlayerBullet = isPlayer;
            b.ownerBotIndex = ownerBotIndex;
            b.dir = dir; b.type = wType; b.bounceCount = 0;

            switch (dir) {
                case Direction::DIR_UP:    b.vx = 0.0f; b.vy = -speed; b.rect.w = sz; b.rect.h = sz * 2; break;
                case Direction::DIR_DOWN:  b.vx = 0.0f; b.vy =  speed; b.rect.w = sz; b.rect.h = sz * 2; break;
                case Direction::DIR_LEFT:  b.vx = -speed; b.vy = 0.0f; b.rect.w = sz * 2; b.rect.h = sz; break;
                case Direction::DIR_RIGHT: b.vx =  speed; b.vy = 0.0f; b.rect.w = sz * 2; b.rect.h = sz; break;
            }

            b.rect.x = static_cast<int>(b.x) - b.rect.w / 2;
            b.rect.y = static_cast<int>(b.y) - b.rect.h / 2;
            b.active = true;
            break;
        }
    }
    play_gunshot_sound(isPlayer, wType);
}

// Spawns a bullet along an arbitrary (possibly diagonal) normalized direction vector.
// Both axes are scaled by the same weapon speed scalar so diagonal shots travel at the
// exact same magnitude as cardinal shots (no ~41% diagonal speed-up from unnormalized dx,dy).
void spawn_vector_bullet(float startX, float startY, float dirX, float dirY, bool isPlayer, WeaponType wType, const SDL_Rect* playerHitbox = nullptr, EnemyType shooterType = EnemyType::RED, int ownerBotIndex = -1) {
    if (wType == WeaponType::LASER) {
        // Laser follows the complete aim vector, including all four diagonals.
        fire_laser(startX, startY, dirX, dirY, isPlayer, playerHitbox, shooterType, ownerBotIndex);
        return;
    }

    float len = std::sqrt(dirX * dirX + dirY * dirY);
    if (len < 0.0001f) return;
    float nx = dirX / len, ny = dirY / len; // (x̂, ŷ) = (dx, dy) / sqrt(dx² + dy²)

    const auto& props = WEAPON_PROPS[static_cast<int>(wType)];
    float speed = props.speed;
    int sz = (wType == WeaponType::ROCKET) ? 16 : 8;

    bool isCardinal = (std::abs(nx) < 0.001f || std::abs(ny) < 0.001f);

    for (auto& b : bullets) {
        if (!b.active) {
            b.x = startX; b.y = startY;
            b.isPlayerBullet = isPlayer;
            b.ownerBotIndex = ownerBotIndex;
            b.type = wType;
            b.bounceCount = 0;
            b.vx = nx * speed;
            b.vy = ny * speed;
            b.diagonal = !isCardinal;

            if (isCardinal) {
                b.dir = (std::abs(nx) > std::abs(ny)) ? ((nx > 0) ? Direction::DIR_RIGHT : Direction::DIR_LEFT)
                                                       : ((ny > 0) ? Direction::DIR_DOWN : Direction::DIR_UP);
                if (b.dir == Direction::DIR_UP || b.dir == Direction::DIR_DOWN) { b.rect.w = sz; b.rect.h = sz * 2; }
                else                                                            { b.rect.w = sz * 2; b.rect.h = sz; }
            } else {
                // Diagonal bolt: square footprint so it reads correctly moving at 45 degrees.
                b.rect.w = static_cast<int>(sz * 1.5f);
                b.rect.h = static_cast<int>(sz * 1.5f);
            }

            b.rect.x = static_cast<int>(b.x) - b.rect.w / 2;
            b.rect.y = static_cast<int>(b.y) - b.rect.h / 2;
            b.active = true;
            break;
        }
    }
    play_gunshot_sound(isPlayer, wType);
}

static float tactical_difficulty_multiplier() {
    if (selectedMode != GameMode::TACTICAL) return 1.0f;
    switch (tacticalEnemyDifficulty) {
        case EnemyDifficulty::HARD: return 1.0f;
        case EnemyDifficulty::EXPERT: return 1.0f;
        default: return 1.0f;
    }
}

static float tactical_enemy_speed(bool alert) {
    if (selectedMode != GameMode::TACTICAL || tacticalEnemyDifficulty == EnemyDifficulty::NORMAL)
        return alert ? AI_SPEED_ALERT : AI_SPEED_NORMAL;
    float base = (tacticalEnemyDifficulty == EnemyDifficulty::EXPERT) ? AI_SPEED_EXPERT : AI_SPEED_HARD;
    return alert ? base + 1.5f : base;
}

static Uint32 tactical_enemy_shoot_multiplier(bool alert) {
    if (selectedMode != GameMode::TACTICAL) return alert ? 1U : 2U;
    if (tacticalEnemyDifficulty == EnemyDifficulty::EXPERT) return alert ? 1U : 1U;
    if (tacticalEnemyDifficulty == EnemyDifficulty::HARD) return alert ? 1U : 1U;
    return alert ? 2U : 3U;
}

static float tactical_enemy_damage_multiplier() {
    if (selectedMode != GameMode::TACTICAL) return 1.0f;
    if (tacticalEnemyDifficulty == EnemyDifficulty::EXPERT) return 1.20f;
    if (tacticalEnemyDifficulty == EnemyDifficulty::HARD) return 1.10f;
    return 1.0f;
}

// --- Bot (ally Sentinel) stat tiers ------------------------------------
// Bots inherit whichever tier the player picked for enemy difficulty in
// TACTICAL_CONFIG - "Normal mode -> normal bots, Hard -> hard bots, Expert
// -> expert bots" - so there's a single difficulty knob, not a separate one.
static float bot_speed(EnemyDifficulty tier) {
    switch (tier) {
        case EnemyDifficulty::EXPERT: return AI_SPEED_EXPERT;
        case EnemyDifficulty::HARD:   return AI_SPEED_HARD;
        default:                      return AI_SPEED_NORMAL;
    }
}
static Uint32 bot_shoot_multiplier(EnemyDifficulty tier) {
    switch (tier) {
        case EnemyDifficulty::EXPERT: return 1U;
        case EnemyDifficulty::HARD:   return 1U;
        default:                      return 2U;
    }
}
static float bot_damage_multiplier(EnemyDifficulty tier) {
    switch (tier) {
        case EnemyDifficulty::EXPERT: return 1.20f;
        case EnemyDifficulty::HARD:   return 1.10f;
        default:                      return 1.0f;
    }
}

void spawn_single_enemy(Enemy& enemy, int index) {
    std::array<Point, 8> spawnPoints = {
        Point{92, 3}, Point{92, 54}, Point{3, 54}, Point{50, 3},
        Point{25, 28}, Point{75, 18}, Point{18, 45}, Point{78, 45}
    };
    Point sp = spawnPoints[index % spawnPoints.size()];
    
    enemy.x = sp.x * TILE_SIZE + 5;
    enemy.y = sp.y * TILE_SIZE + 5;
    enemy.rect.x = static_cast<int>(enemy.x);
    enemy.rect.y = static_cast<int>(enemy.y);
    enemy.maxHp = static_cast<int>((60 + (currentLevel * 5)) * tactical_difficulty_multiplier());
    enemy.hp = enemy.maxHp;
    enemy.team = Team::RED_BOMBER;

    int wChoice = index % 5;
    if (wChoice == 0)      enemy.weaponType = WeaponType::PISTOL;
    else if (wChoice == 1) enemy.weaponType = WeaponType::RIFLE;
    else if (wChoice == 2) enemy.weaponType = WeaponType::ROCKET;
    else if (wChoice == 3) enemy.weaponType = WeaponType::LASER;
    else                   enemy.weaponType = WeaponType::VECTOR_REFLECT;

    // Every third spawn is a Pink Enemy variant - can shoot cardinal OR diagonal.
    enemy.enemyType = (index % 3 == 2) ? EnemyType::PINK : EnemyType::RED;

    enemy.name = RAIDER_NAME_POOL[index % RAIDER_NAME_POOL.size()];
    enemy.kills = 0;
    enemy.deaths = 0;

    enemy.active = true;
}

// Places an enemy at a specific world tile (used for enemy markers painted in the map editor).
void spawn_enemy_at_tile(Enemy& enemy, int index, int tileCol, int tileRow, EnemyType type) {
    enemy.x = tileCol * TILE_SIZE + 5;
    enemy.y = tileRow * TILE_SIZE + 5;
    enemy.rect.x = static_cast<int>(enemy.x);
    enemy.rect.y = static_cast<int>(enemy.y);
    enemy.maxHp = static_cast<int>((60 + (currentLevel * 5)) * tactical_difficulty_multiplier());
    enemy.hp = enemy.maxHp;
    enemy.team = Team::RED_BOMBER;
    enemy.enemyType = type;

    int wChoice = index % 5;
    if (wChoice == 0)      enemy.weaponType = WeaponType::PISTOL;
    else if (wChoice == 1) enemy.weaponType = WeaponType::RIFLE;
    else if (wChoice == 2) enemy.weaponType = WeaponType::ROCKET;
    else if (wChoice == 3) enemy.weaponType = WeaponType::LASER;
    else                   enemy.weaponType = WeaponType::VECTOR_REFLECT;

    enemy.name = RAIDER_NAME_POOL[index % RAIDER_NAME_POOL.size()];
    enemy.kills = 0;
    enemy.deaths = 0;

    enemy.active = true;
}

// Ally bot spawn - places a Sentinel-team bot near the player's spawn area,
// gives it a random callsign from the pool (no repeats within a match), and
// a weapon/tier matching the selected tactical difficulty.
void spawn_bots(int count, EnemyDifficulty tier, Point playerSpawnTile, bool keepRoster = false) {
    // Snapshot the outgoing roster before it gets wiped below - when keepRoster
    // is set (advancing to the next round of the same 10-round mission instead
    // of starting a brand new one) each slot's name/kills/deaths carry over
    // instead of being reshuffled/reset.
    std::array<Bot, MAX_BOTS> priorBots = bots;

    std::vector<int> nameOrder(BOT_NAME_POOL.size());
    for (size_t i = 0; i < nameOrder.size(); ++i) nameOrder[i] = static_cast<int>(i);
    for (size_t i = nameOrder.size(); i > 1; --i)
        std::swap(nameOrder[i - 1], nameOrder[static_cast<size_t>(std::rand()) % i]);

    // Spread bots in a ring around the player's spawn so they don't all stack
    // on one tile and instantly collide with each other.
    for (auto& bot : bots) bot = Bot{};
    activeBotCount = std::clamp(count, 0, MAX_BOTS);
    for (int i = 0; i < activeBotCount; ++i) {
        Bot& bot = bots[i];
        float angle = (2.0f * static_cast<float>(M_PI) * i) / std::max(1, activeBotCount);
        int ring = 2 + (i / 8); // wider rings once more than 8 bots need spreading
        Point spawnTile = nearest_free_tile({
            playerSpawnTile.x + static_cast<int>(std::cos(angle) * ring),
            playerSpawnTile.y + static_cast<int>(std::sin(angle) * ring)
        });
        bot.x = spawnTile.x * TILE_SIZE + 5;
        bot.y = spawnTile.y * TILE_SIZE + 5;
        bot.rect.x = static_cast<int>(bot.x);
        bot.rect.y = static_cast<int>(bot.y);
        bot.tier = tier;
        bot.maxHp = static_cast<int>(70 * bot_damage_multiplier(tier)); // sturdier tier fights harder, so give it a bit more HP too
        bot.hp = bot.maxHp;

        bool reuse = keepRoster && !priorBots[i].name.empty();
        bot.name = reuse ? priorBots[i].name : BOT_NAME_POOL[nameOrder[i % nameOrder.size()]];

        int wChoice = i % 4; // bots skip VECTOR_REFLECT - reflect bounces are unpredictable for AI aim
        WeaponType freshWeapon = (wChoice == 0) ? WeaponType::PISTOL
                                : (wChoice == 1) ? WeaponType::RIFLE
                                : (wChoice == 2) ? WeaponType::ROCKET
                                                  : WeaponType::LASER;
        bot.weaponType = reuse ? priorBots[i].weaponType : freshWeapon;
        bot.kills = reuse ? priorBots[i].kills : 0;
        bot.deaths = reuse ? priorBots[i].deaths : 0;
        bot.bombDefuses = reuse ? priorBots[i].bombDefuses : 0;
        bot.defusingBomb = false;
        bot.defuseStartTime = 0;

        // Golden-angle spread (~137.5 deg apart) so every bot claims a different
        // stand-off point around whatever it's engaging, plus a randomized pace
        // so squadmates visibly move independently instead of in lockstep.
        bot.flankAngle = std::fmod(i * 2.399963f, 2.0f * static_cast<float>(M_PI));
        bot.speedJitter = 0.85f + 0.3f * (static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX));

        bot.active = true;
    }
}

// Signature was missing entirely in the original source (a bare `return`
// floated here with no enclosing function) - restored based on its call
// site usage: usable(c, r) below calls it with a tile column/row and
// expects a walkable/free-tile bool back.
bool health_pack_tile_is_free(int col, int row) {
    return col > 0 && col < currentMapCols - 1 && row > 0 && row < currentMapRows - 1 && (gameMap[row][col] == 0 || gameMap[row][col] == 8 || gameMap[row][col] == 9);
}

Point find_health_pack_tile(Point preferred, const std::vector<Point>& alreadyUsed) {
    auto usable = [&](int c, int r) {
        if (!health_pack_tile_is_free(c, r)) return false;
        for (const Point& used : alreadyUsed) {
            if (used.x == c && used.y == r) return false;
        }
        return true;
    };

    if (usable(preferred.x, preferred.y)) return preferred;

    const int maxRadius = std::max(currentMapCols, currentMapRows);
    for (int radius = 1; radius < maxRadius; ++radius) {
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != radius) continue;
                const int c = preferred.x + dx;
                const int r = preferred.y + dy;
                if (usable(c, r)) return Point{c, r};
            }
        }
    }
    return Point{-1, -1};
}

void generate_ai_map() {
    currentMapCols = editorMapCols = 100;
    currentMapRows = editorMapRows = 60;
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            gameMap[r][c] = 0;

    for (int c = 0; c < currentMapCols; ++c) {
        gameMap[0][c] = 1;
        gameMap[currentMapRows - 1][c] = 1;
    }
    for (int r = 0; r < currentMapRows; ++r) {
        gameMap[r][0] = 1;
        gameMap[r][currentMapCols - 1] = 1;
    }

    // Procedural AI map: varied cover with open routes between the two sites.
    for (int i = 0; i < 20; ++i) {
        int x = 6 + std::rand() % 86;
        int y = 4 + std::rand() % 50;
        bool horizontal = (std::rand() % 2) == 0;
        int len = 4 + std::rand() % 9;
        for (int j = 0; j < len; ++j) {
            int c = horizontal ? x + j : x;
            int r = horizontal ? y : y + j;
            if (c > 1 && c < currentMapCols - 2 && r > 1 && r < currentMapRows - 2)
                gameMap[r][c] = 1;
        }
    }

    // Guaranteed open player start area.
    for (int r = 2; r <= 8; ++r)
        for (int c = 2; c <= 10; ++c)
            gameMap[r][c] = 0;

    bombSiteA = {78, 12};
    bombSiteB = {78, 46};
    for (Point site : {bombSiteA, bombSiteB}) {
        for (int r = site.y - 2; r <= site.y + 2; ++r)
            for (int c = site.x - 2; c <= site.x + 2; ++c)
                if (r > 0 && r < currentMapRows - 1 && c > 0 && c < currentMapCols - 1)
                    gameMap[r][c] = 0;
    }
    gameMap[bombSiteA.y][bombSiteA.x] = 8;
    gameMap[bombSiteB.y][bombSiteB.x] = 9;
    bombSiteAValid = bombSiteBValid = true;

    // Open a central lane so either site is reachable.
    for (int c = 10; c <= 78; ++c) gameMap[29][c] = 0;
}

void init_game_arena(bool keepRoster = false)
{
    highScore = std::max(highScore, score);
    if (!keepRoster) {
        // Brand new mission: full reset, including round count and the bot roster.
        score = 0;
        playerKills = 0;
        playerDeaths = 0;
        playerBombDefuses = 0;
        currentRound = 1;
    }
    currentLevel = 1;
    playerShieldActive = false;
    playerShieldReadyAt = 0;
    activeEnemyCount = (selectedMode == GameMode::ENDLESS) ? 4 : std::min(customTacticalEnemies, MAX_ENEMIES);
    playerHp = playerMaxHp;
    showLeaderboardPanel = false;
    showMatchEndLeaderboard = false;
    showMissionAccomplishedScreen = false;
    missionAccomplishedAt = 0;

    tacticalBomb.planted = false;
    tacticalBomb.defused = false;
    botDefuserIndex = -1;
    tacticalBomb.x = 0; tacticalBomb.y = 0; tacticalBomb.rect = {0,0,35,35};
    defuseHeld = false;
    defuseStartTime = 0;
    lastFuseBeepTime = 0;

    bool customMapLoaded = false;
    if (useCustomMap) customMapLoaded = load_map_from_file(mapFilePath);

    if (!customMapLoaded && !aiGeneratedMapActive) {
        for (int r = 0; r < ROWS; ++r) {
            for (int c = 0; c < COLS; ++c) {
                if (r == 0 || r == ROWS - 1 || c == 0 || c == COLS - 1) gameMap[r][c] = 1;
                else gameMap[r][c] = 0;
            }
        }
        // A few starter structures spread across the large world.
        for (int r = 5; r <= 15; ++r) { gameMap[r][12] = 1; gameMap[r][35] = 1; gameMap[r][70] = 1; }
        for (int r = 35; r <= 48; ++r) { gameMap[r][20] = 1; gameMap[r][55] = 1; gameMap[r][82] = 1; }
        for (int c = 20; c <= 40; ++c) { gameMap[20][c] = 1; gameMap[42][c] = 1; }
        for (int c = 62; c <= 90; ++c) { gameMap[27][c] = 1; }
    }

    scan_bombsites();
    if (!bombSiteAValid) {
        bombSiteA = nearest_free_tile({std::max(8, currentMapCols - 22), std::max(6, currentMapRows / 4)});
        bombSiteAValid = true;
        gameMap[bombSiteA.y][bombSiteA.x] = 8;
    }
    if (!bombSiteBValid) {
        bombSiteB = nearest_free_tile({std::max(8, currentMapCols - 22), std::max(8, (currentMapRows * 3) / 4)});
        bombSiteBValid = true;
        gameMap[bombSiteB.y][bombSiteB.x] = 9;
    }
    // The carrier chooses one of the two sites for this round; both A and B are valid.
    if (bombSiteAValid && bombSiteBValid)
        selectedBombSite = (std::rand() % 2 == 0) ? bombSiteA : bombSiteB;
    else if (bombSiteAValid) selectedBombSite = bombSiteA;
    else selectedBombSite = bombSiteB;

    tacticalBomb.x = selectedBombSite.x * TILE_SIZE + 8;
    tacticalBomb.y = selectedBombSite.y * TILE_SIZE + 8;
    tacticalBomb.rect = {static_cast<int>(tacticalBomb.x), static_cast<int>(tacticalBomb.y), 35, 35};

    for (auto& b : bullets) b.active = false;
    for (auto& exp : explosions) exp.active = false;
    for (auto& gs : glassShatters) gs.active = false;
    for (auto& beam : laserBeams) beam.active = false;
    for (auto& enemy : enemies) enemy.active = false;

    // Enemy spawn tiles painted in the editor (6 = red spawn, 7 = pink spawn) take
    // priority over the default spawn-point list when a custom map supplies any.
    std::vector<Point> customRedSpawns, customPinkSpawns;
    customPlayerSpawnValid = false;
    customPlayerSpawn = {-1, -1};
    if (customMapLoaded) {
        for (int r = 0; r < ROWS; ++r) {
            for (int c = 0; c < COLS; ++c) {
                if (gameMap[r][c] == 6) { customRedSpawns.push_back({c, r}); gameMap[r][c] = 0; }
                else if (gameMap[r][c] == 7) { customPinkSpawns.push_back({c, r}); gameMap[r][c] = 0; }
                else if (gameMap[r][c] == 11) {
                    // Only one player spawn is meaningful - first one found wins,
                    // any extras are cleared back to floor.
                    if (!customPlayerSpawnValid) { customPlayerSpawn = {c, r}; customPlayerSpawnValid = true; }
                    gameMap[r][c] = 0;
                }
            }
        }
    }

    if (!customRedSpawns.empty() || !customPinkSpawns.empty()) {
        std::vector<Point> allSpawns = customRedSpawns;
        allSpawns.insert(allSpawns.end(), customPinkSpawns.begin(), customPinkSpawns.end());
        activeEnemyCount = std::min(static_cast<int>(allSpawns.size()), MAX_ENEMIES);
        for (int i = 0; i < activeEnemyCount; ++i) {
            EnemyType t = (i < static_cast<int>(customRedSpawns.size())) ? EnemyType::RED : EnemyType::PINK;
            spawn_enemy_at_tile(enemies[i], i, allSpawns[i].x, allSpawns[i].y, t);
        }
    } else {
        for (int i = 0; i < activeEnemyCount; ++i) {
            spawn_single_enemy(enemies[i], i);
        }
    }

    // Ally Sentinel bots spawn in a ring around wherever the player will start.
    // ENDLESS mode has no difficulty selector, so bots default to NORMAL tier there.
    {
        Point playerSpawnTile = customPlayerSpawnValid ? customPlayerSpawn : Point{5, 5};
        EnemyDifficulty botTier = (selectedMode == GameMode::TACTICAL) ? tacticalEnemyDifficulty : EnemyDifficulty::NORMAL;
        spawn_bots(customBotCount, botTier, playerSpawnTile, keepRoster);
    }

    const std::array<Point, 8> preferredPackLocations = {
        Point{8, 8}, Point{92, 8}, Point{8, 52}, Point{92, 52},
        Point{30, 10}, Point{70, 10}, Point{30, 48}, Point{70, 48}
    };

    // Never place a health pack inside a colored editor block or wall.
    std::vector<Point> usedPackTiles;
    usedPackTiles.reserve(MAX_HEALTH_PACKS);
    for (int i = 0; i < MAX_HEALTH_PACKS; ++i) {
        Point packTile = find_health_pack_tile(preferredPackLocations[i], usedPackTiles);
        if (packTile.x < 0) {
            healthPacks[i].active = false;
            continue;
        }
        usedPackTiles.push_back(packTile);
        healthPacks[i].r = packTile.y;
        healthPacks[i].c = packTile.x;
        healthPacks[i].active = true;
        healthPacks[i].rect = { packTile.x * TILE_SIZE + 10, packTile.y * TILE_SIZE + 10, 30, 30 };
    }
    
        // --- Add this to the end of init_game_arena() ---
    
    // Force unbreakable visual outer walls for the map array
    for (int r = 0; r < ROWS; ++r) {
        gameMap[r][0] = 1;               // Left wall
        gameMap[r][COLS - 1] = 1;        // Right wall
    }
    for (int c = 0; c < COLS; ++c) {
        gameMap[0][c] = 1;               // Top wall
        gameMap[ROWS - 1][c] = 1;        // Bottom wall
    }
} // End of init_game_arena()

// Counts enemies currently alive, used to scale the bomb fuse at plant time.
int count_active_enemies() {
    int n = 0;
    for (int i = 0; i < activeEnemyCount; ++i) {
        if (enemies[i].active && enemies[i].hp > 0) ++n;
    }
    return n;
}

// Bomb plant trigger: fuse length scales with how many enemies are still
// alive at the moment of planting - a bigger surviving squad buys the
// defenders (well, the bombers) more time.
//   timer = 15.0 + max(0, enemyCount - 3) * 3.5      (seconds)
void trigger_bomb_plant(Bomb& bomb, Uint32 currentTime, int planterIndex = -1) {
    int enemyCount = count_active_enemies();
    float fuseSeconds = 15.0f + static_cast<float>(std::max(0, enemyCount - 3)) * 3.5f;
    bomb.fuseDuration = static_cast<Uint32>(fuseSeconds * 1000.0f);
    bomb.planted = true;
    bomb.plantTime = currentTime;
    if (planterIndex >= 0 && planterIndex < activeEnemyCount) {
        enemies[planterIndex].bombPlants++;
    }
    botDefuserIndex = -1;
    for (int bi = 0; bi < activeBotCount; ++bi) {
        bots[bi].defusingBomb = false;
        bots[bi].defuseStartTime = 0;
    }
}


// Eight-way fire pad: every direction has its own button, including diagonals.
constexpr int AIM_PAD_SIZE = 70;
constexpr int AIM_PAD_GAP = 8;
constexpr int AIM_PAD_X = SCREEN_WIDTH - (AIM_PAD_SIZE * 3 + AIM_PAD_GAP * 2) - 20;
constexpr int AIM_PAD_Y = SCREEN_HEIGHT - (AIM_PAD_SIZE * 3 + AIM_PAD_GAP * 2) - 20;
const SDL_Rect padUpLeft    { AIM_PAD_X, AIM_PAD_Y, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padUp        { AIM_PAD_X + AIM_PAD_SIZE + AIM_PAD_GAP, AIM_PAD_Y, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padUpRight   { AIM_PAD_X + (AIM_PAD_SIZE + AIM_PAD_GAP) * 2, AIM_PAD_Y, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padLeft      { AIM_PAD_X, AIM_PAD_Y + AIM_PAD_SIZE + AIM_PAD_GAP, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padRight     { AIM_PAD_X + (AIM_PAD_SIZE + AIM_PAD_GAP) * 2, AIM_PAD_Y + AIM_PAD_SIZE + AIM_PAD_GAP, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padDownLeft  { AIM_PAD_X, AIM_PAD_Y + (AIM_PAD_SIZE + AIM_PAD_GAP) * 2, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padDown      { AIM_PAD_X + AIM_PAD_SIZE + AIM_PAD_GAP, AIM_PAD_Y + (AIM_PAD_SIZE + AIM_PAD_GAP) * 2, AIM_PAD_SIZE, AIM_PAD_SIZE };
const SDL_Rect padDownRight { AIM_PAD_X + (AIM_PAD_SIZE + AIM_PAD_GAP) * 2, AIM_PAD_Y + (AIM_PAD_SIZE + AIM_PAD_GAP) * 2, AIM_PAD_SIZE, AIM_PAD_SIZE };
// Action buttons have exactly the same total breadth as the complete 3x3 arrow pad.
 // Shield and Defuse are separate full-width rows with visible vertical spacing above the arrow pad.
constexpr int ACTION_BUTTON_W = AIM_PAD_SIZE * 3 + AIM_PAD_GAP * 2;
constexpr int ACTION_BUTTON_H = 60;
constexpr int ACTION_BUTTON_GAP = 22; // clear vertical space between Shield and Defuse
constexpr int ACTION_TO_PAD_GAP = 18;  // clear space above the arrow pad
const SDL_Rect defuseButton { AIM_PAD_X, AIM_PAD_Y - ACTION_TO_PAD_GAP - ACTION_BUTTON_H, ACTION_BUTTON_W, ACTION_BUTTON_H };
const SDL_Rect shieldButton { AIM_PAD_X, defuseButton.y - ACTION_BUTTON_GAP - ACTION_BUTTON_H, ACTION_BUTTON_W, ACTION_BUTTON_H };

// Leaderboard toggle button, sat directly beside the health bar (health bar
// spans x 20-320 at y 20-44).
const SDL_Rect btnLeaderboard { 1635, 20, 150, 42 };
// Leaderboard panel sized and positioned to match the supplied reference image:
// tall centered modal with generous vertical spacing for the two team sections.
const SDL_Rect leaderboardPanelBox { SCREEN_WIDTH / 2 - 337, 149, 674, 1029 };

const SDL_Rect btnTacticalMode { SCREEN_WIDTH / 2 - 300, 300, 600, 85 };
const SDL_Rect btnInfinityMode { SCREEN_WIDTH / 2 - 300, 405, 600, 85 };
const SDL_Rect btnMapEditor    { SCREEN_WIDTH / 2 - 300, 510, 600, 85 };
const SDL_Rect btnMapSelect    { SCREEN_WIDTH / 2 - 300, 615, 600, 75 };
const SDL_Rect btnUseCustomMap { SCREEN_WIDTH / 2 - 300, 710, 600, 65 };
// Dodge + Roll is a normal full-width menu option, below the map options.
// It is no longer placed as a side option on the right.
const SDL_Rect btnAIGenerated { SCREEN_WIDTH / 2 - 300, 795, 600, 75 };
const SDL_Rect aiGeneratedTick { btnAIGenerated.x + 18, btnAIGenerated.y + 20, 35, 35 };
const SDL_Rect btnDodgeRoll { SCREEN_WIDTH / 2 - 300, 885, 600, 75 };
const SDL_Rect dodgeRollTick { btnDodgeRoll.x + 18, btnDodgeRoll.y + 20, 35, 35 };
const SDL_Rect btnWeaponMenu { SCREEN_WIDTH / 2 - 300, 980, 600, 75 };

// Player profile name button (top-right of the main menu) + its edit dialog,
// styled the same as the map editor's save-as dialog.
const SDL_Rect btnProfileName { 230, 410, 340, 65 };
const SDL_Rect btnProfileImage { 68, 365, 135, 150 };
// Sits below the whole profile panel (which ends at y=870), clear of the
// name field/stats/badges above it and the tactical/infinity/editor buttons
// which all start at x=700 - previously this sat directly on top of
// btnProfileName and got painted over by it.
const SDL_Rect portraitPickerBox { 38, 880, 585, 160 };
const SDL_Rect btnMainMenuExit { SCREEN_WIDTH - 62, 20, 42, 42 };
const SDL_Rect profileDialogBox   { SCREEN_WIDTH / 2 - 320, SCREEN_HEIGHT / 2 - 110, 640, 220 };
const SDL_Rect profileDialogField { profileDialogBox.x + 30, profileDialogBox.y + 80, 580, 55 };
const SDL_Rect profileDialogSave  { profileDialogBox.x + 30, profileDialogBox.y + 150, 270, 50 };
const SDL_Rect profileDialogCancel{ profileDialogBox.x + 340, profileDialogBox.y + 150, 270, 50 };
const SDL_Rect weaponMenuBack { SCREEN_WIDTH / 2 - 300, 1020, 600, 75 };
const SDL_Rect btnBackToMenu { SCREEN_WIDTH - 260, 35, 220, 65 };
const SDL_Rect btnPlayingBack { 1845, 20, 42, 42 };

bool dpadUpPressed = false, dpadDownPressed = false, dpadLeftPressed = false, dpadRightPressed = false;
bool dpadUpLeftPressed = false, dpadUpRightPressed = false, dpadDownLeftPressed = false, dpadDownRightPressed = false;
SDL_FingerID padFingerId[8] = {-1,-1,-1,-1,-1,-1,-1,-1};
SDL_FingerID moveFingerId = -1;

// --- Screen partitioning ------------------------------------------------
// Lower-left (x < 0.5, y > 0.5): movement joystick.
// Middle-left ring around (RADIAL_CX, RADIAL_CY): radial weapon wheel - tap a
// wedge to equip it directly, checked first so it always wins any overlap.
bool in_joystick_zone(float normX, float normY) { return normX < 0.5f && normY > 0.5f; }

// --- Middle-left radial weapon-select wheel --------------------------------
// Blender pie-menu style: a ring split into 4 wedges (one per weapon) around a
// dead-zone hub. Sits at the vertical middle of the left edge - a short thumb
// reach for one-tap switching mid-battle, instead of hunting a list.
constexpr int RADIAL_CX       = 190;
constexpr int RADIAL_CY       = SCREEN_HEIGHT / 2;
constexpr int RADIAL_RADIUS   = 150;
constexpr int RADIAL_DEADZONE = 36;

struct RadialSlot { WeaponType type; std::string name; SDL_Color color; int dirX; int dirY; };
// Sector order matches radial_sector_at(): 0=up, 1=right, 2=down, 3=left.
std::array<RadialSlot, 4> weaponRadial = {{
    { WeaponType::PISTOL, "PISTOL", { 80,230,120,255 }, 0,-1 },
    { WeaponType::RIFLE, "RIFLE", { 240,220,60,255 }, 1,0 },
    { WeaponType::ROCKET, "ROCKET", { 240,120,40,255 }, 0,1 },
    { WeaponType::LASER, "LASER", { 50,180,255,255 }, -1,0 }
}};

void refresh_weapon_radial() {
    static const int dirs[4][2] = {{0,-1},{1,0},{0,1},{-1,0}};
    for (int i = 0; i < 4; ++i) {
        if (i < playerWeaponState.equippedCount) {
            WeaponType w = playerWeaponState.equipped[i];
            weaponRadial[i].type = w;
            weaponRadial[i].name = WEAPON_PROPS[static_cast<int>(w)].name;
            weaponRadial[i].color = WEAPON_PROPS[static_cast<int>(w)].color;
        } else {
            weaponRadial[i].type = WeaponType::COUNT;
            weaponRadial[i].name = "EMPTY";
            weaponRadial[i].color = {70,70,80,255};
        }
        weaponRadial[i].dirX = dirs[i][0];
        weaponRadial[i].dirY = dirs[i][1];
    }
}

bool in_weapon_zone(float normX, float normY) {
    float dx = normX * SCREEN_WIDTH  - RADIAL_CX;
    float dy = normY * SCREEN_HEIGHT - RADIAL_CY;
    constexpr float reach = RADIAL_RADIUS + 20.0f; // slightly generous so an edge tap still registers
    return (dx * dx + dy * dy) <= reach * reach;
}

// -1 if the point is in the dead zone or past the outer ring, else an index into weaponRadial.
int radial_sector_at(int dx, int dy) {
    long distSq = static_cast<long>(dx) * dx + static_cast<long>(dy) * dy;
    if (distSq < static_cast<long>(RADIAL_DEADZONE) * RADIAL_DEADZONE) return -1;
    if (distSq > static_cast<long>(RADIAL_RADIUS) * RADIAL_RADIUS) return -1;
    if (std::abs(dx) > std::abs(dy)) return (dx > 0) ? 1 : 3; // RIGHT : LEFT
    return (dy < 0) ? 0 : 2;                                  // UP : DOWN
}

// --- Weapon-switch swipe gesture (touch only, fallback when a tap lands in
// the wheel's dead zone rather than on a wedge) -----------------------------
bool weaponSwipeActive = false;
SDL_FingerID weaponSwipeFingerId = -1;
float weaponSwipeStartY = 0.0f;
bool weaponSwipeTriggered = false; // fires at most once per swipe gesture
int weaponSwipeFeedbackDir = 0;    // -1 = cycled prev, +1 = cycled next, 0 = none (for HUD flash)

void render_health_bar(SDL_Renderer* renderer, int x, int y, int w, int h, int currentHp, int maxHp, SDL_Color fillColor) {
    if (currentHp < 0) currentHp = 0;
    float hpRatio = static_cast<float>(currentHp) / static_cast<float>(maxHp);

    SDL_SetRenderDrawColor(renderer, 30, 30, 30, 220);
    SDL_Rect bgRect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &bgRect);

    SDL_SetRenderDrawColor(renderer, fillColor.r, fillColor.g, fillColor.b, fillColor.a);
    SDL_Rect fillRect = {x, y, static_cast<int>(w * hpRatio), h};
    SDL_RenderFillRect(renderer, &fillRect);

    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
    draw_rect_outline(renderer, bgRect);
}

void render_health_pack(SDL_Renderer* renderer, const HealthPack& hp) {
    if (!hp.active) return;
    SDL_SetRenderDrawColor(renderer, 220, 50, 90, 255);
    SDL_Rect hpScreen = hp.rect;
    hpScreen.x -= cameraX; hpScreen.y -= cameraY;
    SDL_RenderFillRect(renderer, &hpScreen);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, hpScreen);

    int cx = hp.rect.x + hp.rect.w / 2;
    int cy = hp.rect.y + hp.rect.h / 2;
    SDL_Rect vertical = {cx - 3 - cameraX, hp.rect.y + 8 - cameraY, 6, hp.rect.h - 16};
    SDL_Rect horizontal = {hp.rect.x + 8 - cameraX, cy - 3 - cameraY, hp.rect.w - 16, 6};
    SDL_RenderFillRect(renderer, &vertical);
    SDL_RenderFillRect(renderer, &horizontal);
}

void render_bullet(SDL_Renderer* renderer, const Bullet& b) {
    SDL_Rect bRect = b.rect; bRect.x -= cameraX; bRect.y -= cameraY;
    const auto& props = WEAPON_PROPS[static_cast<int>(b.type)];

    if (b.type == WeaponType::VECTOR_REFLECT) {
        SDL_SetRenderDrawColor(renderer, props.color.r, props.color.g, props.color.b, 255);
        SDL_RenderFillRect(renderer, &bRect);
        SDL_SetRenderDrawColor(renderer, 245, 245, 255, 255);
        draw_rect_outline(renderer, bRect);
        return;
    }

    if (b.type == WeaponType::ROCKET) {
        SDL_SetRenderDrawColor(renderer, 240, 100, 30, 255);
        SDL_RenderFillRect(renderer, &bRect);
        SDL_SetRenderDrawColor(renderer, 255, 220, 0, 255);
        SDL_Rect core = {bRect.x + 3, bRect.y + 3, bRect.w - 6, bRect.h - 6};
        SDL_RenderFillRect(renderer, &core);
        return;
    }

    if (b.isPlayerBullet) {
        SDL_SetRenderDrawColor(renderer, props.color.r, props.color.g, props.color.b, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 240, 70, 70, 255);
    }
    SDL_RenderFillRect(renderer, &bRect);
}

void render_glass_shatters(SDL_Renderer* renderer, Uint32 currentTime) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (auto& gs : glassShatters) {
        if (!gs.active) continue;
        Uint32 elapsed = currentTime - gs.spawnTime;
        if (elapsed >= gs.duration) {
            gs.active = false;
            continue;
        }

        const float p = static_cast<float>(elapsed) / static_cast<float>(gs.duration);
        const float alpha = 1.0f - p;
        const int cx = static_cast<int>(gs.x) - cameraX;
        const int cy = static_cast<int>(gs.y) - cameraY;

        // Bright initial flash makes the break unmistakable.
        if (p < 0.22f) {
            Uint8 flashAlpha = static_cast<Uint8>(255.0f * (1.0f - p / 0.22f));
            SDL_SetRenderDrawColor(renderer, 235, 250, 255, flashAlpha);
            int flashSize = 8 + static_cast<int>((1.0f - p / 0.22f) * 10.0f);
            SDL_Rect flash{cx - flashSize / 2, cy - flashSize / 2, flashSize, flashSize};
            SDL_RenderFillRect(renderer, &flash);
        }

        // Twelve shards fly outward and fade.
        for (int i = 0; i < 12; ++i) {
            float a = (static_cast<float>(i) / 12.0f) * 2.0f * static_cast<float>(M_PI) + 0.12f * std::sin(i * 7.0f);
            float speed = 20.0f + static_cast<float>((i * 13) % 23);
            int sx = cx + static_cast<int>(std::cos(a) * speed * p);
            int sy = cy + static_cast<int>(std::sin(a) * speed * p);
            int ex = sx + static_cast<int>(std::cos(a) * (7.0f - 3.0f * p));
            int ey = sy + static_cast<int>(std::sin(a) * (7.0f - 3.0f * p));
            Uint8 shardAlpha = static_cast<Uint8>(255.0f * alpha);
            SDL_SetRenderDrawColor(renderer, 205, 240, 255, shardAlpha);
            SDL_RenderDrawLine(renderer, sx, sy, ex, ey);
            SDL_RenderDrawLine(renderer, sx + 1, sy, ex + 1, ey);
        }

        // Fading crack lines remain around the impact point.
        SDL_SetRenderDrawColor(renderer, 235, 250, 255, static_cast<Uint8>(220.0f * alpha));
        for (int i = 0; i < 8; ++i) {
            float a = (static_cast<float>(i) / 8.0f) * 2.0f * static_cast<float>(M_PI);
            int r1 = 3 + static_cast<int>(p * 8.0f);
            int r2 = 12 + static_cast<int>(p * 20.0f);
            SDL_RenderDrawLine(renderer, cx + static_cast<int>(std::cos(a) * r1),
                               cy + static_cast<int>(std::sin(a) * r1),
                               cx + static_cast<int>(std::cos(a) * r2),
                               cy + static_cast<int>(std::sin(a) * r2));
        }
    }
}

void render_explosions(SDL_Renderer* renderer, Uint32 currentTime) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (auto& exp : explosions) {
        if (!exp.active) continue;
        Uint32 elapsed = currentTime - exp.spawnTime;
        if (elapsed >= exp.duration) { exp.active = false; continue; }

        float progress = static_cast<float>(elapsed) / static_cast<float>(exp.duration);
        float currentRadius = exp.maxRadius * std::sin(progress * M_PI);

        SDL_Rect outer = { static_cast<int>(exp.x - currentRadius) - cameraX, static_cast<int>(exp.y - currentRadius) - cameraY, static_cast<int>(currentRadius * 2), static_cast<int>(currentRadius * 2) };
        SDL_SetRenderDrawColor(renderer, 255, 120, 20, static_cast<Uint8>(200 * (1.0f - progress)));
        SDL_RenderFillRect(renderer, &outer);

        float innerR = currentRadius * 0.5f;
        SDL_Rect inner = { static_cast<int>(exp.x - innerR) - cameraX, static_cast<int>(exp.y - innerR) - cameraY, static_cast<int>(innerR * 2), static_cast<int>(innerR * 2) };
        SDL_SetRenderDrawColor(renderer, 255, 230, 60, static_cast<Uint8>(240 * (1.0f - progress)));
        SDL_RenderFillRect(renderer, &inner);
    }
}

// --- Tactical-mode C4 charge -----------------------------------------------
// Replaces the old plain colored block with a small procedural device: two
// stacked explosive bricks with tape straps, a slate-grey digital controller
// on top holding an LED countdown screen, three colored wires running from
// the controller into the bricks, and a status LED whose blink rate ramps up
// exponentially as the fuse runs down. `screenRect` is the bomb's existing
// collision rect (already camera-offset) - visuals are drawn a bit larger
// than it for legibility, but gameplay collision is untouched.
void render_bomb_entity(SDL_Renderer* renderer, const Bomb& bomb, const SDL_Rect& screenRect, Uint32 currentTime) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    int cx = screenRect.x + screenRect.w / 2;
    int cy = screenRect.y + screenRect.h / 2;

    int brickW = screenRect.w + 14;
    int brickH = 14;
    SDL_Rect brickTop    = { cx - brickW / 2, cy - brickH, brickW, brickH };
    SDL_Rect brickBottom = { cx - brickW / 2, cy,          brickW, brickH };

    // Explosive bricks (khaki C4) with dark outlines.
    SDL_SetRenderDrawColor(renderer, 150, 140, 90, 255);
    SDL_RenderFillRect(renderer, &brickTop);
    SDL_RenderFillRect(renderer, &brickBottom);
    SDL_SetRenderDrawColor(renderer, 40, 38, 30, 255);
    draw_rect_outline(renderer, brickTop);
    draw_rect_outline(renderer, brickBottom);

    // Dark electrical-tape straps wrapping each brick.
    SDL_SetRenderDrawColor(renderer, 20, 20, 22, 255);
    SDL_Rect tapeTop    = { brickTop.x + brickW / 2 - 3, brickTop.y - 2,    6, brickH + 4 };
    SDL_Rect tapeBottom = { brickBottom.x + brickW / 2 - 3, brickBottom.y - 2, 6, brickH + 4 };
    SDL_RenderFillRect(renderer, &tapeTop);
    SDL_RenderFillRect(renderer, &tapeBottom);

    // Digital controller box mounted on top of the upper brick.
    SDL_Rect controller = { cx - 12, brickTop.y - 16, 24, 16 };
    SDL_SetRenderDrawColor(renderer, 70, 74, 82, 255);
    SDL_RenderFillRect(renderer, &controller);
    SDL_SetRenderDrawColor(renderer, 15, 15, 18, 255);
    draw_rect_outline(renderer, controller);

    // Fuse progress, used by both the LED screen fill and the blink rate below.
    Uint32 elapsed = bomb.planted ? (currentTime - bomb.plantTime) : 0;
    Uint32 remainingMs = (bomb.planted && elapsed < bomb.fuseDuration) ? (bomb.fuseDuration - elapsed) : 0;
    float remainingFrac = bomb.planted
        ? static_cast<float>(remainingMs) / static_cast<float>(std::max<Uint32>(1, bomb.fuseDuration))
        : 1.0f;

    // LED countdown screen: dark glass with a green fill bar draining to empty.
    SDL_Rect screen = { controller.x + 3, controller.y + 3, controller.w - 12, controller.h - 8 };
    SDL_SetRenderDrawColor(renderer, 10, 30, 15, 255);
    SDL_RenderFillRect(renderer, &screen);
    SDL_SetRenderDrawColor(renderer, 60, 255, 90, static_cast<Uint8>(120 + 135 * remainingFrac));
    SDL_Rect screenFill = { screen.x + 1, screen.y + 1,
                             std::max(1, static_cast<int>((screen.w - 2) * remainingFrac)), screen.h - 2 };
    SDL_RenderFillRect(renderer, &screenFill);

    // Three colored wires running from the controller down into the bricks.
    SDL_SetRenderDrawColor(renderer, 220, 40, 40, 255); // red
    SDL_RenderDrawLine(renderer, controller.x + 2, controller.y + controller.h, brickTop.x + 4, brickTop.y + 2);
    SDL_SetRenderDrawColor(renderer, 60, 120, 240, 255); // blue
    SDL_RenderDrawLine(renderer, cx, controller.y + controller.h, cx, brickBottom.y + brickH - 2);
    SDL_SetRenderDrawColor(renderer, 230, 210, 40, 255); // yellow
    SDL_RenderDrawLine(renderer, controller.x + controller.w - 2, controller.y + controller.h, brickTop.x + brickW - 4, brickTop.y + 2);

    // Status LED - a steady amber dot before planting; once planted it blinks
    // red, speeding up exponentially as the remaining time approaches zero.
    if (bomb.planted) {
        float urgency = 1.0f - remainingFrac; // 0 = just planted, 1 = about to blow
        float blinkHz = 1.0f + std::pow(urgency, 3.0f) * 11.0f; // ~1Hz -> ~12Hz near zero
        float phase = std::fmod(static_cast<float>(currentTime) * blinkHz / 1000.0f, 1.0f);
        bool ledOn = phase < 0.5f;
        SDL_SetRenderDrawColor(renderer, ledOn ? 255 : 60, ledOn ? 40 : 10, ledOn ? 40 : 10, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 255, 210, 40, 255);
    }
    SDL_Rect led = { controller.x + controller.w - 6, controller.y + 2, 3, 3 };
    SDL_RenderFillRect(renderer, &led);
}

void render_arrow_pad(SDL_Renderer* renderer) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    auto draw_btn = [&](const SDL_Rect& r, bool pressed, int dx, int dy) {
        SDL_SetRenderDrawColor(renderer, pressed ? 80 : 40, pressed ? 180 : 110, pressed ? 240 : 200, 150);
        SDL_RenderFillRect(renderer, &r);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
        draw_rect_outline(renderer, r);

        const int cx = r.x + r.w / 2;
        const int cy = r.y + r.h / 2;
        const int tipX = cx + dx * 22;
        const int tipY = cy + dy * 22;
        const int backX = cx - dx * 10;
        const int backY = cy - dy * 10;
        const int sideX = -dy * 10;
        const int sideY = dx * 10;

        SDL_RenderDrawLine(renderer, backX, backY, tipX, tipY);
        SDL_RenderDrawLine(renderer, tipX, tipY, backX + sideX, backY + sideY);
        SDL_RenderDrawLine(renderer, tipX, tipY, backX - sideX, backY - sideY);
    };

    draw_btn(padUpLeft,    dpadUpLeftPressed,    -1, -1);
    draw_btn(padUp,        dpadUpPressed,         0, -1);
    draw_btn(padUpRight,   dpadUpRightPressed,   1, -1);
    draw_btn(padLeft,      dpadLeftPressed,     -1,  0);
    draw_btn(padRight,     dpadRightPressed,     1,  0);
    draw_btn(padDownLeft,  dpadDownLeftPressed, -1,  1);
    draw_btn(padDown,      dpadDownPressed,      0,  1);
    draw_btn(padDownRight, dpadDownRightPressed, 1,  1);
}
void draw_filled_circle(SDL_Renderer* renderer, int cx, int cy, int radius) {
    if (radius <= 0) return;
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - dy * dy)));
        SDL_RenderDrawLine(renderer, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

void draw_circle_outline(SDL_Renderer* renderer, int cx, int cy, int radius, int thickness) {
    if (radius <= 0) return;
    constexpr int segments = 48;
    for (int t = 0; t < thickness; ++t) {
        int r = radius - t;
        if (r <= 0) continue;
        for (int i = 0; i < segments; ++i) {
            float a0 = (static_cast<float>(i) / segments) * 2.0f * static_cast<float>(M_PI);
            float a1 = (static_cast<float>(i + 1) / segments) * 2.0f * static_cast<float>(M_PI);
            int x0 = cx + static_cast<int>(std::cos(a0) * r), y0 = cy + static_cast<int>(std::sin(a0) * r);
            int x1 = cx + static_cast<int>(std::cos(a1) * r), y1 = cy + static_cast<int>(std::sin(a1) * r);
            SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
        }
    }
}

// Compact "pick an image from device" glyph: a small picture-frame icon with
// a sun + mountain motif, plus a little "+" badge to read as an action button
// rather than a static thumbnail. Drawn with primitives, same as the other
// vector glyphs in this file (no texture/image loading pipeline here).
void draw_image_select_icon(SDL_Renderer* renderer, int cx, int cy, int size, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);

    const int halfW = size;
    const int halfH = static_cast<int>(size * 0.72f);
    SDL_Rect frame{cx - halfW, cy - halfH, halfW * 2, halfH * 2};
    draw_rect_outline(renderer, frame, 3);

    // Sun.
    draw_circle_outline(renderer, frame.x + halfW / 2, frame.y + halfH / 2 - 2,
                         std::max(3, size / 6), 2);

    // Mountain skyline, clipped to the frame's bottom half.
    SDL_Point peak1{frame.x + halfW / 2 + 2, frame.y + frame.h - halfH / 2};
    SDL_Point valley{frame.x + halfW, frame.y + frame.h - 8};
    SDL_Point peak2{frame.x + frame.w - halfW / 3, frame.y + frame.h - halfH / 2 - 6};
    SDL_RenderDrawLine(renderer, frame.x + 4, frame.y + frame.h - 8, peak1.x, peak1.y);
    SDL_RenderDrawLine(renderer, peak1.x, peak1.y, valley.x, valley.y);
    SDL_RenderDrawLine(renderer, valley.x, valley.y, peak2.x, peak2.y);
    SDL_RenderDrawLine(renderer, peak2.x, peak2.y, frame.x + frame.w - 4, frame.y + frame.h - 8);

    // "+" action badge, bottom-right corner of the frame.
    const int badgeR = std::max(9, size / 3);
    const int bx = frame.x + frame.w + 2;
    const int by = frame.y + frame.h + 2;
    draw_filled_circle(renderer, bx, by, badgeR);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_circle_outline(renderer, bx, by, badgeR, 2);
    SDL_RenderDrawLine(renderer, bx - badgeR / 2, by, bx + badgeR / 2, by);
    SDL_RenderDrawLine(renderer, bx, by - badgeR / 2, bx, by + badgeR / 2);
}

// Simple vector glyphs so each weapon reads as a distinct silhouette at a glance,
// drawn with primitives (this project has no texture/image loading pipeline yet).
void draw_weapon_icon(SDL_Renderer* renderer, WeaponType type, int cx, int cy, int s, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
    switch (type) {
        case WeaponType::PISTOL: {
            SDL_Rect barrel{ cx - s, cy - s / 3, static_cast<int>(s * 1.6f), s / 2 };
            SDL_Rect grip  { cx - s / 3, cy - s / 6, s / 3, s };
            SDL_RenderFillRect(renderer, &barrel);
            SDL_RenderFillRect(renderer, &grip);
            break;
        }
        case WeaponType::RIFLE: {
            SDL_Rect body { cx - s, cy - s / 5, static_cast<int>(s * 2.2f), s / 3 };
            SDL_Rect stock{ cx + static_cast<int>(s * 0.9f), cy - s / 8, s / 2, s / 2 };
            SDL_Rect mag  { cx - s / 4, cy + s / 6, s / 5, s / 2 };
            SDL_RenderFillRect(renderer, &body);
            SDL_RenderFillRect(renderer, &stock);
            SDL_RenderFillRect(renderer, &mag);
            break;
        }
        case WeaponType::ROCKET: {
            SDL_RenderDrawLine(renderer, cx - s, cy, cx - s / 2, cy - s / 2);
            SDL_RenderDrawLine(renderer, cx - s, cy, cx - s / 2, cy + s / 2);
            SDL_Rect body{ cx - s / 2, cy - s / 4, static_cast<int>(s * 1.3f), s / 2 };
            SDL_RenderFillRect(renderer, &body);
            SDL_RenderDrawLine(renderer, cx + s / 2, cy - s / 4, cx + s, cy - s / 2);
            SDL_RenderDrawLine(renderer, cx + s / 2, cy + s / 4, cx + s, cy + s / 2);
            break;
        }
        case WeaponType::LASER: {
            SDL_RenderDrawLine(renderer, cx - s / 3, cy - s, cx + s / 4, cy - s / 5);
            SDL_RenderDrawLine(renderer, cx + s / 4, cy - s / 5, cx - s / 5, cy);
            SDL_RenderDrawLine(renderer, cx - s / 5, cy, cx + s / 3, cy + s);
            break;
        }
        case WeaponType::VECTOR_REFLECT: {
            SDL_RenderDrawLine(renderer, cx - s, cy + s, cx, cy - s);
            SDL_RenderDrawLine(renderer, cx, cy - s, cx + s, cy - s / 4);
            SDL_RenderDrawLine(renderer, cx + s, cy - s / 4, cx + s / 3, cy + s);
            SDL_RenderDrawLine(renderer, cx + s / 3, cy + s, cx - s / 3, cy + s / 3);
            break;
        }
        default: break;
    }
}

// Fills one quarter-diamond wedge of the radial wheel: the region within
// `radius` of (cx, cy) that lies on the (dirX, dirY) side of both 45-degree
// diagonals through the center. Scanned row by row like draw_filled_circle.
void draw_radial_wedge(SDL_Renderer* renderer, int cx, int cy, int radius, int dirX, int dirY, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int dy = -radius; dy <= radius; ++dy) {
        int maxDx = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - dy * dy)));
        int absDy = std::abs(dy);
        if (dirY != 0) {
            // Top/bottom wedge: rows on the matching side of center, width
            // narrowed by the diagonals (bounded by |dx| <= |dy|).
            if (dy == 0 || (dirY > 0) != (dy > 0)) continue;
            int half = std::min(maxDx, absDy);
            if (half <= 0) continue;
            SDL_RenderDrawLine(renderer, cx - half, cy + dy, cx + half, cy + dy);
        } else {
            // Left/right wedge: the outer band of each row past the diagonals.
            if (absDy >= maxDx) continue;
            if (dirX > 0) SDL_RenderDrawLine(renderer, cx + absDy, cy + dy, cx + maxDx, cy + dy);
            else          SDL_RenderDrawLine(renderer, cx - maxDx, cy + dy, cx - absDy, cy + dy);
        }
    }
}

// Middle-left radial weapon wheel (Blender pie-menu style). Always visible during
// battles so a single tap on a wedge equips that weapon instantly - no scrolling
// through a list. Never blocks gameplay; drawn every frame during PLAYING.
void render_weapon_radial(SDL_Renderer* renderer, Uint32 currentTime) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // Soft backing disc so the wheel reads clearly over the arena.
    SDL_SetRenderDrawColor(renderer, 10, 12, 18, 130);
    draw_filled_circle(renderer, RADIAL_CX, RADIAL_CY, RADIAL_RADIUS + 14);

    // Brief brightness "pop" on the wedge that was just switched to, so a
    // tap/hotkey/scroll all read as a felt, instant change.
    float flashT = std::min(1.0f, static_cast<float>(currentTime - weaponSwitchFlashAt) / 180.0f);
    Uint8 flashBoost = static_cast<Uint8>((1.0f - flashT) * 60.0f);

    for (const auto& slot : weaponRadial) {
        if (slot.type == WeaponType::COUNT) continue;
        bool isSelected = (playerWeapon == slot.type);
        SDL_Color fill = isSelected ? slot.color : SDL_Color{45, 48, 60, 235};
        if (isSelected) {
            fill.r = static_cast<Uint8>(std::min(255, fill.r + flashBoost));
            fill.g = static_cast<Uint8>(std::min(255, fill.g + flashBoost));
            fill.b = static_cast<Uint8>(std::min(255, fill.b + flashBoost));
        }
        draw_radial_wedge(renderer, RADIAL_CX, RADIAL_CY, RADIAL_RADIUS, slot.dirX, slot.dirY, fill);
    }

    // Dividers between wedges, then outer ring outline.
    SDL_SetRenderDrawColor(renderer, 15, 18, 25, 255);
    for (int d = 0; d < 4; ++d) {
        float ang = static_cast<float>(M_PI) / 4.0f + d * (static_cast<float>(M_PI) / 2.0f);
        int ex = RADIAL_CX + static_cast<int>(std::cos(ang) * RADIAL_RADIUS);
        int ey = RADIAL_CY + static_cast<int>(std::sin(ang) * RADIAL_RADIUS);
        SDL_RenderDrawLine(renderer, RADIAL_CX, RADIAL_CY, ex, ey);
    }
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 200);
    draw_circle_outline(renderer, RADIAL_CX, RADIAL_CY, RADIAL_RADIUS, 3);

    // Punch the dead-zone hub back out and ring it, then show the equipped
    // weapon's icon in the hub - like Blender's pie-menu center readout.
    SDL_SetRenderDrawColor(renderer, 15, 18, 25, 255);
    draw_filled_circle(renderer, RADIAL_CX, RADIAL_CY, RADIAL_DEADZONE);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 160);
    draw_circle_outline(renderer, RADIAL_CX, RADIAL_CY, RADIAL_DEADZONE, 2);
    draw_weapon_icon(renderer, playerWeapon, RADIAL_CX, RADIAL_CY - 2, RADIAL_DEADZONE / 2,
                      WEAPON_PROPS[static_cast<int>(playerWeapon)].color);

    for (const auto& slot : weaponRadial) {
        if (slot.type == WeaponType::COUNT) continue;
        bool isSelected = (playerWeapon == slot.type);
        int iconCx = RADIAL_CX + slot.dirX * static_cast<int>(RADIAL_RADIUS * 0.62f);
        int iconCy = RADIAL_CY + slot.dirY * static_cast<int>(RADIAL_RADIUS * 0.62f);

        SDL_Color iconColor = isSelected ? SDL_Color{20, 20, 20, 255} : slot.color;
        draw_weapon_icon(renderer, slot.type, iconCx, iconCy - 10, 16, iconColor);

        SDL_Color labelColor = isSelected ? SDL_Color{20, 20, 20, 255} : SDL_Color{225, 225, 225, 255};
        int textW = static_cast<int>(slot.name.length()) * 6 * 2;
        draw_text(renderer, slot.name, iconCx - textW / 2, iconCy + 14, 2, labelColor);
    }

    // Live feedback while a dead-zone swipe is in progress.
    if (weaponSwipeActive && weaponSwipeFeedbackDir != 0 && currentTime - weaponSwitchFlashAt < 220) {
        int ax = RADIAL_CX;
        int ay = RADIAL_CY;
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
        if (weaponSwipeFeedbackDir > 0) { // next -> swiped up
            SDL_RenderDrawLine(renderer, ax, ay + 14, ax, ay - 14);
            SDL_RenderDrawLine(renderer, ax, ay - 14, ax - 10, ay - 2);
            SDL_RenderDrawLine(renderer, ax, ay - 14, ax + 10, ay - 2);
        } else { // previous -> swiped down
            SDL_RenderDrawLine(renderer, ax, ay - 14, ax, ay + 14);
            SDL_RenderDrawLine(renderer, ax, ay + 14, ax - 10, ay + 2);
            SDL_RenderDrawLine(renderer, ax, ay + 14, ax + 10, ay + 2);
        }
    }
}

int map_index_at_menu_point(int x, int y) {
    if (!mapDropdownOpen || availableMapFiles.empty()) return -1;
    const int rowH = 52;
    const int shownRows = std::min(7, static_cast<int>(availableMapFiles.size()));
    SDL_Rect listBox{btnMapSelect.x, btnMapSelect.y + btnMapSelect.h + 8, btnMapSelect.w, shownRows * rowH};
    SDL_Point pt{x, y};
    if (!SDL_PointInRect(&pt, &listBox)) return -1;
    const int idx = (y - listBox.y) / rowH;
    return (idx >= 0 && idx < shownRows) ? idx : -1;
}

int portrait_picker_row_at(int x, int y) {
    if (!portraitPickerOpen || availablePortraitFiles.empty()) return -1;
    const int rowH = 38;
    const int shownRows = std::min(4, static_cast<int>(availablePortraitFiles.size()));
    SDL_Rect listBox{portraitPickerBox.x, portraitPickerBox.y, portraitPickerBox.w, shownRows * rowH};
    SDL_Point pt{x, y};
    if (!SDL_PointInRect(&pt, &listBox)) return -1;
    const int idx = (y - listBox.y) / rowH;
    return (idx >= 0 && idx < shownRows) ? idx : -1;
}

void hover_select_map_at(int x, int y) {
    const int idx = map_index_at_menu_point(x, y);
    if (idx >= 0 && idx < static_cast<int>(availableMapFiles.size())) {
        // Hovering is the selection: update the active map immediately without
        // closing the list. A click can still close the dropdown afterwards.
        mapFilePath = map_directory() + availableMapFiles[idx];
        selectedMapIndex = idx;
        useCustomMap = true;
    }
}

std::string current_play_map_name() {
    if (aiGeneratedMapActive) return "AI GENERATED MAP";
    if (useCustomMap && !mapFilePath.empty()) {
        std::string name = map_basename(mapFilePath);
        if (!name.empty()) return name;
    }
    return "DEFAULT MAP";
}

void render_back_button(SDL_Renderer* renderer, const SDL_Rect& rect) {
    SDL_SetRenderDrawColor(renderer, 100, 55, 65, 255);
    SDL_RenderFillRect(renderer, &rect);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, rect);

    constexpr int scale = 2;
    const int cx = rect.x + rect.w / 2;
    const int cy = rect.y + rect.h / 2;
    const int d = 7 * scale;
    SDL_RenderDrawLine(renderer, cx - d, cy - d, cx + d, cy + d);
    SDL_RenderDrawLine(renderer, cx + d, cy - d, cx - d, cy + d);
}

// Main-menu profile card.  This deliberately uses the same chunky geometry as
// the rest of the HUD, so the profile reads like an in-game screen rather than
// a desktop settings panel.
void render_player_profile(SDL_Renderer* renderer) {
    const SDL_Rect panel{38, 285, 585, 585};
    const SDL_Rect inner{48, 295, 565, 565};
    const SDL_Rect& portrait = btnProfileImage;
    const SDL_Rect stats{68, 535, 525, 112};

    SDL_SetRenderDrawColor(renderer, 25, 39, 57, 255);
    SDL_RenderFillRect(renderer, &panel);
    SDL_SetRenderDrawColor(renderer, 92, 145, 188, 255);
    draw_rect_outline(renderer, panel, 4);
    SDL_SetRenderDrawColor(renderer, 43, 63, 84, 255);
    SDL_RenderFillRect(renderer, &inner);
    SDL_SetRenderDrawColor(renderer, 115, 164, 202, 255);
    draw_rect_outline(renderer, inner, 2);

    draw_text(renderer, "PLAYER PROFILE", centered_text_x("PLAYER PROFILE", 2, panel.x, panel.w),
              panel.y + 18, 2, {235, 240, 245, 255});
    SDL_SetRenderDrawColor(renderer, 86, 126, 161, 255);
    SDL_RenderDrawLine(renderer, panel.x + 20, panel.y + 52, panel.x + panel.w - 20, panel.y + 52);

    // Profile display-picture area: plain green until the player picks a
    // personal image, then the cropped photo fills it edge-to-edge. The
    // "select image" icon only shows up on hover (or while the picker
    // list is open) so a chosen photo isn't permanently overlaid.
    int rawMouseX = 0, rawMouseY = 0;
    SDL_GetMouseState(&rawMouseX, &rawMouseY);
    const SDL_Point mouseLogical = mouse_to_logical(renderer, rawMouseX, rawMouseY);
    const bool portraitHovered = SDL_PointInRect(&mouseLogical, &portrait) != SDL_FALSE;

    if (playerPortraitTexture) {
        SDL_RenderCopy(renderer, playerPortraitTexture, nullptr, &portrait);
    } else {
        SDL_SetRenderDrawColor(renderer, 60, 220, 100, 255);
        SDL_RenderFillRect(renderer, &portrait);
    }
    SDL_SetRenderDrawColor(renderer, 190, 255, 200, 255);
    draw_rect_outline(renderer, portrait, 3);

    if (portraitHovered || portraitPickerOpen) {
        // Dim overlay so the icon reads clearly regardless of whether the dp
        // underneath is the plain green placeholder or a loaded photo.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, playerPortraitTexture ? 140 : 55);
        SDL_RenderFillRect(renderer, &portrait);
        const SDL_Color iconColor = playerPortraitTexture ? SDL_Color{255, 255, 255, 255}
                                                            : SDL_Color{12, 55, 25, 255};
        draw_image_select_icon(renderer, portrait.x + portrait.w / 2, portrait.y + portrait.h / 2 - 4,
                                22, iconColor);
    }

    draw_text(renderer, "PLAYER: " + playerName, 230, 378, 2, {245, 245, 245, 255});
    draw_text(renderer, "TAP TO EDIT NAME", 230, 490, 1, {170, 198, 220, 255});

    SDL_SetRenderDrawColor(renderer, 25, 31, 41, 255);
    SDL_RenderFillRect(renderer, &btnProfileName);
    SDL_SetRenderDrawColor(renderer, 225, 230, 235, 255);
    draw_rect_outline(renderer, btnProfileName, 2);
    std::string editableName = playerName;
    if (editableName.size() > 17) editableName.resize(17);
    draw_text(renderer, editableName, btnProfileName.x + 14, btnProfileName.y + 18, 2,
              {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 25, 31, 41, 255);
    SDL_RenderFillRect(renderer, &stats);
    SDL_SetRenderDrawColor(renderer, 92, 130, 164, 255);
    draw_rect_outline(renderer, stats, 2);
    draw_text(renderer, "SCORE: " + std::to_string(score), stats.x + 12, stats.y + 12, 2,
              {245, 245, 245, 255});
    draw_text(renderer, "KD: " + std::to_string(playerKills) + "/" +
              std::to_string(playerDeaths), stats.x + 12, stats.y + 47, 2,
              {225, 230, 235, 255});
    draw_text(renderer, "W/L: " + std::to_string(playerWins) + "/" +
              std::to_string(playerLosses), stats.x + 180, stats.y + 47, 2,
              {225, 230, 235, 255});
    std::string profileWeapon = WEAPON_PROPS[static_cast<int>(playerWeapon)].name;
    draw_text(renderer, "TOP WEAPON: " + profileWeapon, stats.x + 12, stats.y + 82, 2,
              {225, 230, 235, 255});

    draw_text(renderer, "BADGES", panel.x + 22, 675, 2, {185, 215, 238, 255});
    const SDL_Rect badgeBoxes[] = {
        {68, 710, 155, 125}, {238, 710, 155, 125}, {408, 710, 155, 125}
    };
    const char* badgeNames[] = {"BOMB", "LAST MAN", "SHARP"};
    const char* badgeSubtitles[] = {"DEFUSER", "STANDING", "SHOOTER"};
    const SDL_Color badgeColors[] = {
        {225, 190, 75, 255}, {185, 150, 90, 255}, {155, 125, 75, 255}
    };
    for (int i = 0; i < 3; ++i) {
        const SDL_Rect& b = badgeBoxes[i];
        SDL_SetRenderDrawColor(renderer, 36, 51, 69, 255);
        SDL_RenderFillRect(renderer, &b);
        SDL_SetRenderDrawColor(renderer, 69, 92, 117, 255);
        draw_rect_outline(renderer, b, 2);
        SDL_SetRenderDrawColor(renderer, badgeColors[i].r, badgeColors[i].g, badgeColors[i].b, 255);
        SDL_Rect medal{b.x + 52, b.y + 12, 50, 50};
        SDL_RenderFillRect(renderer, &medal);
        SDL_SetRenderDrawColor(renderer, 255, 245, 185, 255);
        draw_rect_outline(renderer, medal, 3);
        draw_text(renderer, std::to_string(i + 1), medal.x + 20, medal.y + 16, 2,
                  {40, 45, 52, 255});
        draw_text(renderer, badgeNames[i], centered_text_x(badgeNames[i], 1, b.x, b.w),
                  b.y + 74, 1, {245, 245, 245, 255});
        draw_text(renderer, badgeSubtitles[i], centered_text_x(badgeSubtitles[i], 1, b.x, b.w),
                  b.y + 94, 1, {205, 215, 225, 255});
    }

    // Drawn last so it sits on top of everything above (name field, stats,
    // badges) rather than being painted over by them.
    if (portraitPickerOpen) {
        const int rowH = 38;
        SDL_SetRenderDrawColor(renderer, 15, 19, 27, 255);
        const SDL_Rect backdrop{portraitPickerBox.x, portraitPickerBox.y, portraitPickerBox.w,
                                 availablePortraitFiles.empty() ? 78 : std::min(4, static_cast<int>(availablePortraitFiles.size())) * rowH};
        SDL_RenderFillRect(renderer, &backdrop);
        SDL_SetRenderDrawColor(renderer, 92, 145, 188, 255);
        draw_rect_outline(renderer, backdrop, 3);

        if (availablePortraitFiles.empty()) {
            draw_text(renderer, "NO IMAGES FOUND", backdrop.x + 14, backdrop.y + 12, 1, {230, 230, 230, 255});
            draw_text(renderer, "COPY PHOTOS INTO:", backdrop.x + 14, backdrop.y + 34, 1, {170, 198, 220, 255});
            std::string dirLabel = portrait_directory();
            if (dirLabel.size() > 70) dirLabel = "..." + dirLabel.substr(dirLabel.size() - 67);
            draw_text(renderer, dirLabel, backdrop.x + 14, backdrop.y + 56, 1, {150, 180, 205, 255});
        } else {
            const int shown = std::min(4, static_cast<int>(availablePortraitFiles.size()));
            for (int i = 0; i < shown; ++i) {
                const SDL_Rect row{backdrop.x, backdrop.y + i * rowH, backdrop.w, rowH};
                const bool isCurrent = (playerPortraitPath == portrait_directory() + availablePortraitFiles[i]);
                SDL_SetRenderDrawColor(renderer, isCurrent ? 45 : 25, isCurrent ? 95 : 31, isCurrent ? 65 : 41, 255);
                SDL_RenderFillRect(renderer, &row);
                SDL_SetRenderDrawColor(renderer, 69, 92, 117, 255);
                draw_rect_outline(renderer, row, 1);
                std::string label = availablePortraitFiles[i];
                if (label.size() > 60) { label.resize(57); label += "..."; }
                draw_text(renderer, label, row.x + 10, row.y + 10, 1, {230, 230, 230, 255});
            }
        }
    }
}

void render_mode_selection(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 15, 18, 25, 255);
    SDL_RenderClear(renderer);

    draw_text(renderer, "TACTICAL SHOOTER", centered_text_x("TACTICAL SHOOTER", 5), 160, 5, {100, 220, 255, 255});
    draw_text(renderer, "SELECT GAME MODE", centered_text_x("SELECT GAME MODE", 3), 240, 3, {200, 200, 200, 255});

    render_player_profile(renderer);

    // Main-menu exit button: small square X in the top-right corner.
    render_back_button(renderer, btnMainMenuExit);

    SDL_SetRenderDrawColor(renderer, 40, 120, 220, 255);
    SDL_RenderFillRect(renderer, &btnTacticalMode);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnTacticalMode);
    draw_text(renderer, "TACTICAL MODE (BOMB DEFUSE)",
              centered_text_x("TACTICAL MODE (BOMB DEFUSE)", 3, btnTacticalMode.x, btnTacticalMode.w),
              btnTacticalMode.y + 30, 3, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 220, 80, 40, 255);
    SDL_RenderFillRect(renderer, &btnInfinityMode);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnInfinityMode);
    draw_text(renderer, "ENDLESS MODE (ENDLESS WAVES)",
              centered_text_x("ENDLESS MODE (ENDLESS WAVES)", 3, btnInfinityMode.x, btnInfinityMode.w),
              btnInfinityMode.y + 30, 3, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 90, 100, 120, 255);
    SDL_RenderFillRect(renderer, &btnMapEditor);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnMapEditor);
    draw_text(renderer, "MAP EDITOR (OR PRESS 'E')",
              centered_text_x("MAP EDITOR (OR PRESS 'E')", 3, btnMapEditor.x, btnMapEditor.w),
              btnMapEditor.y + 30, 3, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 45, 80, 110, 255);
    SDL_RenderFillRect(renderer, &btnMapSelect);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnMapSelect);

    std::string selectedLabel = "MAP: DEFAULT";
    if (selectedMapIndex >= 0 && selectedMapIndex < static_cast<int>(availableMapFiles.size()))
        selectedLabel = "MAP: " + availableMapFiles[selectedMapIndex];
    if (selectedLabel.size() > 38) selectedLabel.resize(38);
    draw_text(renderer, selectedLabel, centered_text_x(selectedLabel, 2, btnMapSelect.x, btnMapSelect.w),
              btnMapSelect.y + 22, 2, {255,255,255,255});

    SDL_SetRenderDrawColor(renderer, useCustomMap ? 40 : 55, useCustomMap ? 170 : 65, useCustomMap ? 90 : 80, 255);
    SDL_RenderFillRect(renderer, &btnUseCustomMap);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnUseCustomMap);
    std::string customLabel = useCustomMap ? "CUSTOM MAP ENABLED" : "USE SELECTED MAP";
    draw_text(renderer, customLabel, centered_text_x(customLabel, 2, btnUseCustomMap.x, btnUseCustomMap.w),
              btnUseCustomMap.y + 20, 2, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 45, 75, 110, 255);
    SDL_RenderFillRect(renderer, &btnAIGenerated);
    SDL_SetRenderDrawColor(renderer, 255,255,255,255);
    draw_rect_outline(renderer, btnAIGenerated);
    draw_text(renderer, "AI GENERATED MAP (2 BOMBSITES)",
              btnAIGenerated.x + 75, btnAIGenerated.y + 28, 2, {255,255,255,255});

    // Tick reflects whether the AI-generated map is the active map source.
    // Enabling "USE SELECTED MAP" clears aiGeneratedMapActive, so this
    // unticks automatically the moment a custom map is enabled instead.
    draw_rect_outline(renderer, aiGeneratedTick);
    if (aiGeneratedMapActive) {
        SDL_SetRenderDrawColor(renderer, 45, 190, 95, 255);
        SDL_RenderFillRect(renderer, &aiGeneratedTick);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawLine(renderer, aiGeneratedTick.x + 7, aiGeneratedTick.y + 18,
                           aiGeneratedTick.x + 15, aiGeneratedTick.y + 27);
        SDL_RenderDrawLine(renderer, aiGeneratedTick.x + 15, aiGeneratedTick.y + 27,
                           aiGeneratedTick.x + 29, aiGeneratedTick.y + 8);
    }

    // Explicit main-menu tick option for dodge + roll.
    SDL_SetRenderDrawColor(renderer, 45, 55, 70, 255);
    SDL_RenderFillRect(renderer, &btnDodgeRoll);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnDodgeRoll);
    draw_rect_outline(renderer, dodgeRollTick);
    if (dodgeRollEnabled) {
        SDL_SetRenderDrawColor(renderer, 45, 190, 95, 255);
        SDL_RenderFillRect(renderer, &dodgeRollTick);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDrawLine(renderer, dodgeRollTick.x + 7, dodgeRollTick.y + 18,
                           dodgeRollTick.x + 15, dodgeRollTick.y + 27);
        SDL_RenderDrawLine(renderer, dodgeRollTick.x + 15, dodgeRollTick.y + 27,
                           dodgeRollTick.x + 29, dodgeRollTick.y + 8);
    }
    draw_text(renderer, dodgeRollEnabled ? "DODGE + ROLL: ON" : "DODGE + ROLL: OFF",
              btnDodgeRoll.x + 75, btnDodgeRoll.y + 20, 2, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 55, 80, 125, 255);
    SDL_RenderFillRect(renderer, &btnWeaponMenu);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnWeaponMenu);
    draw_text(renderer, "WEAPON MENU",
              centered_text_x("WEAPON MENU", 3, btnWeaponMenu.x, btnWeaponMenu.w),
              btnWeaponMenu.y + 24, 3, {255,255,255,255});

    if (mapDropdownOpen) {
        const int rowH = 52;
        const int shownRows = std::min(7, static_cast<int>(availableMapFiles.size()));
        const int boxH = std::max(rowH, shownRows * rowH);
        SDL_Rect listBox{btnMapSelect.x, btnMapSelect.y + btnMapSelect.h + 8, btnMapSelect.w, boxH};
        SDL_SetRenderDrawColor(renderer, 12, 15, 22, 250);
        SDL_RenderFillRect(renderer, &listBox);
        SDL_SetRenderDrawColor(renderer, 220, 230, 245, 255);
        draw_rect_outline(renderer, listBox);

        if (availableMapFiles.empty()) {
            draw_text(renderer, "NO SAVED MAPS - OPEN MAP EDITOR", listBox.x + 18, listBox.y + 18, 2, {220,220,220,255});
        } else {
            for (int i = 0; i < shownRows; ++i) {
                SDL_Rect row{listBox.x + 4, listBox.y + i * rowH + 4, listBox.w - 8, rowH - 8};
                bool selected = (i == selectedMapIndex);
                bool hovered  = (i == hoveredMapIndex);
                // Hover gets its own brighter tint so the pointer position reads
                // as live feedback even before a row is committed via click.
                Uint8 rC = selected ? 65 : (hovered ? 45 : 25);
                Uint8 gC = selected ? 120 : (hovered ? 65 : 30);
                Uint8 bC = selected ? 165 : (hovered ? 95 : 40);
                SDL_SetRenderDrawColor(renderer, rC, gC, bC, 255);
                SDL_RenderFillRect(renderer, &row);
                if (hovered) {
                    SDL_SetRenderDrawColor(renderer, 190, 220, 255, 255);
                    draw_rect_outline(renderer, row);
                }
                std::string label = availableMapFiles[i];
                if (label.size() > 38) label.resize(38);
                draw_text(renderer, label, row.x + 14, row.y + 13, 2, {255,255,255,255});
            }
        }
    }

    if (profileEditActive) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
        SDL_Rect fullScreen = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
        SDL_RenderFillRect(renderer, &fullScreen);

        SDL_SetRenderDrawColor(renderer, 25, 28, 38, 250);
        SDL_RenderFillRect(renderer, &profileDialogBox);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, profileDialogBox);

        draw_text(renderer, "EDIT PLAYER NAME", profileDialogBox.x + 30, profileDialogBox.y + 20, 3, {100, 220, 255, 255});

        SDL_SetRenderDrawColor(renderer, 12, 14, 20, 255);
        SDL_RenderFillRect(renderer, &profileDialogField);
        SDL_SetRenderDrawColor(renderer, 200, 210, 230, 255);
        draw_rect_outline(renderer, profileDialogField);
        std::string shown = profileNameInput + ((SDL_GetTicks() / 500) % 2 == 0 ? "_" : "");
        draw_text(renderer, shown, profileDialogField.x + 12, profileDialogField.y + 16, 2, {255, 255, 255, 255});

        SDL_SetRenderDrawColor(renderer, 40, 170, 90, 255);
        SDL_RenderFillRect(renderer, &profileDialogSave);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, profileDialogSave);
        draw_text(renderer, "SAVE (ENTER)", profileDialogSave.x + 16, profileDialogSave.y + 15, 2, {255, 255, 255, 255});

        SDL_SetRenderDrawColor(renderer, 120, 60, 60, 255);
        SDL_RenderFillRect(renderer, &profileDialogCancel);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, profileDialogCancel);
        draw_text(renderer, "CANCEL (ESC)", profileDialogCancel.x + 16, profileDialogCancel.y + 15, 2, {255, 255, 255, 255});
    }
}

void render_weapon_menu(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 15, 18, 25, 255);
    SDL_RenderClear(renderer);
    draw_text(renderer, "WEAPON MENU", centered_text_x("WEAPON MENU", 4), 100, 4, {100,220,255,255});
    draw_text(renderer, "CHOOSE UP TO 4 WEAPONS FOR THE PIE MENU", centered_text_x("CHOOSE UP TO 4 WEAPONS FOR THE PIE MENU", 2), 175, 2, {210,215,225,255});
    draw_text(renderer, "EQUIPPED: " + std::to_string(playerWeaponState.equippedCount) + "/4", centered_text_x("EQUIPPED: " + std::to_string(playerWeaponState.equippedCount) + "/4", 3), 205, 3, {120,220,255,255});

    const WeaponType types[] = {WeaponType::PISTOL, WeaponType::RIFLE, WeaponType::ROCKET, WeaponType::LASER, WeaponType::VECTOR_REFLECT};
    const int ys[] = {250, 365, 480, 595, 710};
    for (int i=0;i<5;++i) {
        SDL_Rect b{SCREEN_WIDTH/2-350, ys[i], 700, 85};
        bool selected = is_weapon_equipped(types[i]);
        SDL_Color col = WEAPON_PROPS[static_cast<int>(types[i])].color;
        SDL_SetRenderDrawColor(renderer, selected ? col.r : 45, selected ? col.g : 55, selected ? col.b : 75, 255);
        SDL_RenderFillRect(renderer,&b);
        SDL_SetRenderDrawColor(renderer,255,255,255,255); draw_rect_outline(renderer, b);
        draw_weapon_icon(renderer, types[i], b.x+70, b.y+42, 24, selected ? SDL_Color{20,20,20,255} : col);
        draw_text(renderer, WEAPON_PROPS[static_cast<int>(types[i])].name, b.x+125, b.y+27, 3, selected ? SDL_Color{20,20,20,255} : SDL_Color{255,255,255,255});
        draw_text(renderer, selected ? "EQUIPPED" : "AVAILABLE", b.x+500, b.y+31, 2, selected ? SDL_Color{20,20,20,255} : SDL_Color{210,210,220,255});
    }
    render_back_button(renderer, btnBackToMenu);
}

// Tactical Config layout.
// Order top-to-bottom: raider count -> sentinel bot count -> raider/bot
// difficulty (difficulty sits below the sentinel bot row, and starts with
// no option highlighted until the player actually picks one).
const SDL_Rect btnIncEnemies { SCREEN_WIDTH / 2 + 100, 275, 80, 70 };
const SDL_Rect btnDecEnemies { SCREEN_WIDTH / 2 - 180, 275, 80, 70 };
const SDL_Rect tacticalEnemyCountBox { SCREEN_WIDTH / 2 - 100, 275, 200, 70 };

const SDL_Rect btnIncBots { SCREEN_WIDTH / 2 + 100, 430, 80, 70 };
const SDL_Rect btnDecBots { SCREEN_WIDTH / 2 - 180, 430, 80, 70 };
const SDL_Rect tacticalBotCountBox { SCREEN_WIDTH / 2 - 100, 430, 200, 70 };

const SDL_Rect btnDiffNormal { SCREEN_WIDTH / 2 - 330, 605, 200, 85 };
const SDL_Rect btnDiffHard   { SCREEN_WIDTH / 2 - 100, 605, 200, 85 };
const SDL_Rect btnDiffExpert { SCREEN_WIDTH / 2 + 130, 605, 200, 85 };

const SDL_Rect btnStartTac   { SCREEN_WIDTH / 2 - 200, 750, 400, 75 };

void render_tactical_config(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 15, 18, 25, 255);
    SDL_RenderClear(renderer);

    draw_text(renderer, "TACTICAL MODE CONFIG", centered_text_x("TACTICAL MODE CONFIG", 4), 160, 4, {100, 220, 255, 255});
    draw_text(renderer, "SELECT NUMBER OF RAIDERS (1 - 20)", centered_text_x("SELECT NUMBER OF RAIDERS (1 - 20)", 2), 240, 2, {200, 200, 200, 255});

    // Decrease Button (-)
    SDL_SetRenderDrawColor(renderer, 180, 50, 50, 255);
    SDL_RenderFillRect(renderer, &btnDecEnemies);
    draw_text(renderer, "-", centered_text_x("-", 5, btnDecEnemies.x, btnDecEnemies.w), btnDecEnemies.y + 10, 5, {255, 255, 255, 255});

    // Increase Button (+)
    SDL_SetRenderDrawColor(renderer, 50, 180, 80, 255);
    SDL_RenderFillRect(renderer, &btnIncEnemies);
    draw_text(renderer, "+", centered_text_x("+", 5, btnIncEnemies.x, btnIncEnemies.w), btnIncEnemies.y + 10, 5, {255, 255, 255, 255});

    // Display Current Value in a dedicated box between - and +.
    SDL_SetRenderDrawColor(renderer, 22, 28, 38, 255);
    SDL_RenderFillRect(renderer, &tacticalEnemyCountBox);
    SDL_SetRenderDrawColor(renderer, 150, 170, 190, 255);
    draw_rect_outline(renderer, tacticalEnemyCountBox);

    const std::string enemyCountText = std::to_string(customTacticalEnemies);
    draw_text(renderer, enemyCountText,
              centered_text_x(enemyCountText, 5, tacticalEnemyCountBox.x, tacticalEnemyCountBox.w),
              tacticalEnemyCountBox.y + 12, 5,
              {255, 255, 255, 255});

    // Ally Sentinel bot count row - same +/-/box pattern as the enemy count above.
    // This now sits above the difficulty row (was previously below it).
    draw_text(renderer, "SELECT NUMBER OF SENTINEL BOTS (0 - 20)",
              centered_text_x("SELECT NUMBER OF SENTINEL BOTS (0 - 20)", 2),
              385, 2, {200, 200, 200, 255});

    SDL_SetRenderDrawColor(renderer, 180, 50, 50, 255);
    SDL_RenderFillRect(renderer, &btnDecBots);
    draw_text(renderer, "-", centered_text_x("-", 5, btnDecBots.x, btnDecBots.w), btnDecBots.y + 10, 5, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 50, 180, 80, 255);
    SDL_RenderFillRect(renderer, &btnIncBots);
    draw_text(renderer, "+", centered_text_x("+", 5, btnIncBots.x, btnIncBots.w), btnIncBots.y + 10, 5, {255, 255, 255, 255});

    SDL_SetRenderDrawColor(renderer, 22, 28, 38, 255);
    SDL_RenderFillRect(renderer, &tacticalBotCountBox);
    SDL_SetRenderDrawColor(renderer, 150, 170, 190, 255);
    draw_rect_outline(renderer, tacticalBotCountBox);

    const std::string botCountText = std::to_string(customBotCount);
    draw_text(renderer, botCountText,
              centered_text_x(botCountText, 5, tacticalBotCountBox.x, tacticalBotCountBox.w),
              tacticalBotCountBox.y + 12, 5,
              {255, 255, 255, 255});

    // Difficulty heading now sits below the sentinel bot row. This tier also
    // decides ally bot difficulty - NORMAL/HARD/EXPERT bots mirror whatever's
    // picked here, so there's no separate bot-difficulty control. Nothing is
    // highlighted until the player taps one of the three options.
    draw_text(renderer, "RAIDER & BOT DIFFICULTY",
              centered_text_x("RAIDER & BOT DIFFICULTY", 2),
              570, 2, {200, 200, 200, 255});
    const SDL_Rect diffButtons[] = { btnDiffNormal, btnDiffHard, btnDiffExpert };
    const char* diffLabels[] = { "NORMAL", "HARD", "EXPERT" };
    for (int i = 0; i < 3; ++i) {
        bool selected = tacticalDifficultyChosen && (static_cast<int>(tacticalEnemyDifficulty) == i);
        SDL_SetRenderDrawColor(renderer, selected ? 40 : 45, selected ? 150 : 55, selected ? 220 : 70, 255);
        SDL_RenderFillRect(renderer, &diffButtons[i]);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, diffButtons[i]);
        draw_text(renderer, diffLabels[i], centered_text_x(diffLabels[i], 2, diffButtons[i].x, diffButtons[i].w), diffButtons[i].y + 21, 2, {255,255,255,255});
    }

    // Start Game Button
    SDL_SetRenderDrawColor(renderer, 40, 120, 220, 255);
    SDL_RenderFillRect(renderer, &btnStartTac);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    draw_rect_outline(renderer, btnStartTac);
    draw_text(renderer, "START MISSION", centered_text_x("START MISSION", 3, btnStartTac.x, btnStartTac.w), btnStartTac.y + 25, 3, {255, 255, 255, 255});

    render_back_button(renderer, btnBackToMenu);
}


void render_map_editor(SDL_Renderer* renderer, Uint32 currentTime) {
    SDL_SetRenderDrawColor(renderer, 12, 14, 20, 255);
    SDL_RenderClear(renderer);

    // Draw only the visible part of the large world. The panel now sits at
    // the top, so rows are culled from the top (behind the panel) instead
    // of the bottom, and the visible range extends all the way down.
    // editorZoom scales how much world the screen shows: cameraX/cameraY stay
    // in unscaled world-tile pixels, and every screen coordinate below is the
    // world coordinate translated by the camera, then multiplied by zoom.
    const int zTile = std::max(1, static_cast<int>(TILE_SIZE * editorZoom));
    int firstCol = std::max(0, cameraX / TILE_SIZE);
    int firstRow = std::max(0, (cameraY + static_cast<int>(EDITOR_PANEL_BOTTOM / editorZoom)) / TILE_SIZE);
    int lastCol = std::min(currentMapCols - 1, (cameraX + static_cast<int>(SCREEN_WIDTH / editorZoom)) / TILE_SIZE + 1);
    int lastRow = std::min(currentMapRows - 1, (cameraY + static_cast<int>(SCREEN_HEIGHT / editorZoom)) / TILE_SIZE + 1);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int r = firstRow; r <= lastRow; ++r) {
        for (int c = firstCol; c <= lastCol; ++c) {
            SDL_Rect tile = {
                static_cast<int>((c * TILE_SIZE - cameraX) * editorZoom),
                static_cast<int>((r * TILE_SIZE - cameraY) * editorZoom),
                zTile, zTile
            };
            int val = gameMap[r][c];
            bool isBorder = (r == 0 || r == currentMapRows - 1 || c == 0 || c == currentMapCols - 1);

            if (val == 1 || isBorder) SDL_SetRenderDrawColor(renderer, 70, 75, 90, 255);
            else if (val == 8 || val == 9) {
                SDL_Color paletteColor = editor_palette_color(val);
                SDL_SetRenderDrawColor(renderer, paletteColor.r, paletteColor.g, paletteColor.b, paletteColor.a);
            }
            else if (val == 10) SDL_SetRenderDrawColor(renderer, 130, 205, 235, 90);
            else               SDL_SetRenderDrawColor(renderer, 20, 20, 25, 255);

            SDL_RenderFillRect(renderer, &tile);

            // The permanent border wall gets a bright, thicker outline so it always
            // reads clearly as locked, uneditable geometry.
            if (isBorder) {
                SDL_SetRenderDrawColor(renderer, 140, 200, 255, 255);
                draw_rect_outline(renderer, tile);
                SDL_Rect inner = { tile.x + 1, tile.y + 1, tile.w - 2, tile.h - 2 };
                draw_rect_outline(renderer, inner);
            } else {
                SDL_SetRenderDrawColor(renderer, 40, 45, 55, 255);
                draw_rect_outline(renderer, tile);
            }

            // Enemy spawn markers (6 = red, 7 = pink) render as solid squares,
            // matching how they actually look in-game rather than a circle.
            if (val == 6 || val == 7) {
                SDL_Color markerColor = (val == 6) ? SDL_Color{220, 60, 60, 255} : SDL_Color{255, 105, 180, 255};
                int pad = std::max(2, zTile / 6);
                SDL_Rect marker = { tile.x + pad, tile.y + pad, tile.w - pad * 2, tile.h - pad * 2 };
                SDL_SetRenderDrawColor(renderer, markerColor.r, markerColor.g, markerColor.b, 255);
                SDL_RenderFillRect(renderer, &marker);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
                draw_rect_outline(renderer, marker);
            }
            // Player spawn marker (11) renders as a solid square in the player's
            // in-game color, same shape/style as red/pink enemy markers.
            if (val == 11) {
                int pad = std::max(2, zTile / 6);
                SDL_Rect marker = { tile.x + pad, tile.y + pad, tile.w - pad * 2, tile.h - pad * 2 };
                SDL_SetRenderDrawColor(renderer, 60, 220, 100, 255);
                SDL_RenderFillRect(renderer, &marker);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 220);
                draw_rect_outline(renderer, marker);
            }
            if (val == 8 || val == 9) {
                draw_text(renderer, val == 8 ? "A" : "B", tile.x + 17, tile.y + 12, 2, {255,255,255,255});
            }
            if (val == 10) {
                SDL_SetRenderDrawColor(renderer, 210, 245, 255, 180);
                SDL_RenderDrawLine(renderer, tile.x + 4, tile.y + 4, tile.x + tile.w - 4, tile.y + tile.h - 4);
                SDL_RenderDrawLine(renderer, tile.x + tile.w - 4, tile.y + 4, tile.x + 4, tile.y + tile.h - 4);
            }
        }
    }

    // Editor panel stays fixed on screen, so it is usable on a tablet.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 10, 12, 18, 235);
    SDL_Rect panel = {0, EDITOR_PANEL_TOP, SCREEN_WIDTH, EDITOR_UI_HEIGHT};
    SDL_RenderFillRect(renderer, &panel);

    auto button = [&](const SDL_Rect& r, const char* label, bool highlighted) {
        SDL_SetRenderDrawColor(renderer, highlighted ? 70 : 55, highlighted ? 170 : 70, highlighted ? 100 : 95, 255);
        SDL_RenderFillRect(renderer, &r);
        SDL_SetRenderDrawColor(renderer, 230, 240, 255, 255);
        draw_rect_outline(renderer, r);
        draw_text(renderer, label, r.x + 14, r.y + 18, 2, {255,255,255,255});
    };

    // Palette row: tap a swatch to choose what tapping the grid paints.
    draw_text(renderer, "BLOCK TYPE:", 20, EDITOR_PANEL_TOP + 10, 2, {180, 190, 210, 255});
    for (const auto& opt : editorPalette) {
        bool selected = (!editorEraseMode && editorSelectedTile == opt.value);
        SDL_SetRenderDrawColor(renderer, opt.color.r, opt.color.g, opt.color.b, 255);
        SDL_RenderFillRect(renderer, &opt.rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, selected ? 255 : 120);
        draw_rect_outline(renderer, opt.rect);
        if (selected) {
            SDL_Rect ring = { opt.rect.x - 3, opt.rect.y - 3, opt.rect.w + 6, opt.rect.h + 6 };
            draw_rect_outline(renderer, ring);
        }
        draw_text(renderer, opt.label, opt.rect.x + 8, opt.rect.y + opt.rect.h + 4, 1, {220, 220, 220, 255});
    }

    // Action row.
    button(editorSaveButton, "SAVE AS", false);
    button(editorOverwriteButton, "SAVE", false);
    button(editorLoadButton, "LOAD MAP", false);
    button(editorSizeButton, "SIZE", false);
    button(editorEraseButton, editorEraseMode ? "ERASER (ON)" : "ERASER", editorEraseMode);
    button(editorExitButton, "EXIT (E)", false);

    draw_text(renderer, "MAP EDITOR", 20, 18, 3, {100, 220, 255, 255});
    draw_text(renderer, "WORLD: " + std::to_string(editorMapCols) + " X " + std::to_string(editorMapRows), 220, 24, 2, {220,220,220,255});
    draw_text(renderer, "DRAG TO PAN, PINCH TO ZOOM", 500, 24, 2, {180, 190, 210, 255});

    // Zoom controls: on-screen +/- buttons for tablets/phones plus support
    // for the standard two-finger pinch gesture handled in the event loop.
    SDL_SetRenderDrawColor(renderer, 55, 70, 95, 255);
    SDL_RenderFillRect(renderer, &editorZoomOutButton);
    SDL_RenderFillRect(renderer, &editorZoomInButton);
    SDL_SetRenderDrawColor(renderer, 230, 240, 255, 255);
    draw_rect_outline(renderer, editorZoomOutButton);
    draw_rect_outline(renderer, editorZoomInButton);
    draw_text(renderer, "-", editorZoomOutButton.x + editorZoomOutButton.w / 2 - 6, editorZoomOutButton.y + 15, 3, {255,255,255,255});
    draw_text(renderer, "+", editorZoomInButton.x + editorZoomInButton.w / 2 - 8, editorZoomInButton.y + 15, 3, {255,255,255,255});
    std::string zoomLabel = "ZOOM " + std::to_string(static_cast<int>(editorZoom * 100 + 0.5f)) + "%";
    draw_text(renderer, zoomLabel, 985, EDITOR_ACTION_Y - 24, 2, {200, 210, 230, 255});

    // Brief status feedback after Save/Load, fades after 2 seconds.
    if (!editorStatusMsg.empty() && currentTime - editorStatusMsgTime < 2000) {
        draw_text(renderer, editorStatusMsg, 20, 60, 2, {120, 230, 160, 255});
    }

    // Drawn after the panel (which now spans the top of the screen) so it
    // stays visible on top instead of being covered by it.
    render_back_button(renderer, btnBackToMenu);

    if (editorShowSizeDialog) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0,0,0,180);
        SDL_Rect fs{0,0,SCREEN_WIDTH,SCREEN_HEIGHT};
        SDL_RenderFillRect(renderer,&fs);
        SDL_SetRenderDrawColor(renderer,25,28,38,255);
        SDL_RenderFillRect(renderer,&editorSizeDialogBox);
        SDL_SetRenderDrawColor(renderer,255,255,255,255);
        draw_rect_outline(renderer, editorSizeDialogBox);
        draw_text(renderer,"MAP SIZE",editorSizeDialogBox.x+35,editorSizeDialogBox.y+28,3,{100,220,255,255});
        draw_text(renderer,"BREADTH / WIDTH",editorSizeDialogBox.x+150,editorSizeDialogBox.y+92,2,{210,220,235,255});
        draw_text(renderer,std::to_string(editorMapCols),editorSizeDialogBox.x+290,editorSizeDialogBox.y+115,3,{255,255,255,255});
        draw_text(renderer,"HEIGHT",editorSizeDialogBox.x+150,editorSizeDialogBox.y+192,2,{210,220,235,255});
        draw_text(renderer,std::to_string(editorMapRows),editorSizeDialogBox.x+290,editorSizeDialogBox.y+215,3,{255,255,255,255});
        auto sb=[&](const SDL_Rect&r,const char*t){SDL_SetRenderDrawColor(renderer,45,65,90,255);SDL_RenderFillRect(renderer,&r);SDL_SetRenderDrawColor(renderer,220,230,245,255);draw_rect_outline(renderer, r);draw_text(renderer,t,r.x+30,r.y+17,3,{255,255,255,255});};
        sb(editorSizeWidthMinus,"-"); sb(editorSizeWidthPlus,"+"); sb(editorSizeHeightMinus,"-"); sb(editorSizeHeightPlus,"+");
        sb(editorSizeApply,"APPLY"); sb(editorSizeCancel,"CANCEL");
    }

    if (editorShowSaveDialog) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
        SDL_Rect fullScreen = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
        SDL_RenderFillRect(renderer, &fullScreen);

        SDL_SetRenderDrawColor(renderer, 25, 28, 38, 250);
        SDL_RenderFillRect(renderer, &editorDialogBox);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, editorDialogBox);

        draw_text(renderer, "SAVE MAP AS", editorDialogBox.x + 30, editorDialogBox.y + 20, 3, {100, 220, 255, 255});

        SDL_SetRenderDrawColor(renderer, 12, 14, 20, 255);
        SDL_RenderFillRect(renderer, &editorDialogField);
        SDL_SetRenderDrawColor(renderer, 200, 210, 230, 255);
        draw_rect_outline(renderer, editorDialogField);
        std::string shown = editorFilenameInput + ((SDL_GetTicks() / 500) % 2 == 0 ? "_" : "");
        draw_text(renderer, shown, editorDialogField.x + 12, editorDialogField.y + 16, 2, {255, 255, 255, 255});

        SDL_SetRenderDrawColor(renderer, 40, 170, 90, 255);
        SDL_RenderFillRect(renderer, &editorDialogSave);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, editorDialogSave);
        draw_text(renderer, "SAVE (ENTER)", editorDialogSave.x + 16, editorDialogSave.y + 15, 2, {255, 255, 255, 255});

        SDL_SetRenderDrawColor(renderer, 120, 60, 60, 255);
        SDL_RenderFillRect(renderer, &editorDialogCancel);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, editorDialogCancel);
        draw_text(renderer, "CANCEL (ESC)", editorDialogCancel.x + 16, editorDialogCancel.y + 15, 2, {255, 255, 255, 255});
    }

    if (editorShowLoadDialog) {
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
        SDL_Rect overlay{0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
        SDL_RenderFillRect(renderer, &overlay);

        SDL_Rect box{SCREEN_WIDTH / 2 - 360, 130, 720, 800};
        SDL_SetRenderDrawColor(renderer, 22, 26, 36, 255);
        SDL_RenderFillRect(renderer, &box);
        SDL_SetRenderDrawColor(renderer, 230, 240, 255, 255);
        draw_rect_outline(renderer, box);
        draw_text(renderer, "CHOOSE MAP FROM DEVICE", box.x + 30, box.y + 25, 3, {100,220,255,255});

        refresh_map_list();
        if (availableMapFiles.empty()) {
            draw_text(renderer, "NO .TXT MAPS SAVED YET", box.x + 30, box.y + 110, 2, {220,220,220,255});
        } else {
            const int rowH = 70;
            const int maxRows = std::min(8, static_cast<int>(availableMapFiles.size()));
            for (int i = 0; i < maxRows; ++i) {
                SDL_Rect row{box.x + 25, box.y + 90 + i * rowH, box.w - 50, rowH - 8};
                SDL_SetRenderDrawColor(renderer, i == selectedMapIndex ? 55 : 35,
                                       i == selectedMapIndex ? 105 : 45,
                                       i == selectedMapIndex ? 145 : 55, 255);
                SDL_RenderFillRect(renderer, &row);
                std::string label = availableMapFiles[i];
                if (label.size() > 42) label.resize(42);
                draw_text(renderer, label, row.x + 16, row.y + 18, 2, {255,255,255,255});
                SDL_Rect del{row.x + row.w - 130, row.y + 8, 115, row.h - 16};
                SDL_SetRenderDrawColor(renderer,125,55,55,255); SDL_RenderFillRect(renderer,&del);
                SDL_SetRenderDrawColor(renderer,255,255,255,255); draw_rect_outline(renderer, del);
                draw_text(renderer,"DELETE",del.x+15,del.y+10,1,{255,255,255,255});
            }
        }

        SDL_Rect cancel{box.x + 25, box.y + box.h - 65, box.w - 50, 45};
        SDL_SetRenderDrawColor(renderer, 120, 60, 60, 255);
        SDL_RenderFillRect(renderer, &cancel);
        SDL_SetRenderDrawColor(renderer, 255,255,255,255);
        draw_rect_outline(renderer, cancel);
        draw_text(renderer, "CANCEL", cancel.x + 16, cancel.y + 10, 2, {255,255,255,255});
    }
}


// Keeps the camera in range whenever the zoom level changes, so zooming out
// never leaves the view stranded past the edge of the map.
void editor_clamp_camera() {
    int maxX = std::max(0, static_cast<int>(editorMapCols * TILE_SIZE - SCREEN_WIDTH / editorZoom));
    int maxY = std::max(0, static_cast<int>(editorMapRows * TILE_SIZE - SCREEN_HEIGHT / editorZoom));
    cameraX = std::clamp(cameraX, 0, maxX);
    cameraY = std::clamp(cameraY, 0, maxY);
}

void editor_set_zoom(float newZoom) {
    editorZoom = std::clamp(newZoom, EDITOR_ZOOM_MIN, EDITOR_ZOOM_MAX);
    editor_clamp_camera();
}

void editor_apply_tile(int screenX, int screenY, bool forceErase) {
    if (screenY < EDITOR_PANEL_BOTTOM) return;
    int c = static_cast<int>(screenX / editorZoom) + cameraX;
    int r = static_cast<int>(screenY / editorZoom) + cameraY;
    c /= TILE_SIZE;
    r /= TILE_SIZE;
    if (r < 0 || r >= editorMapRows || c < 0 || c >= editorMapCols) return;

    // The outer wall border is permanent - always shown, never editable -
    // so the arena always stays fully enclosed.
    if (r == 0 || r == editorMapRows - 1 || c == 0 || c == editorMapCols - 1) return;

    int newVal = (forceErase || editorEraseMode) ? 0 : editorSelectedTile;
    if (newVal == 11) {
        // Only one player spawn makes sense - clear any earlier one when a new
        // spot is painted.
        for (int rr = 0; rr < editorMapRows; ++rr)
            for (int cc = 0; cc < editorMapCols; ++cc)
                if (gameMap[rr][cc] == 11) gameMap[rr][cc] = 0;
    }
    gameMap[r][c] = newVal;
}

void editor_set_status(const std::string& msg, Uint32 currentTime) {
    editorStatusMsg = msg;
    editorStatusMsgTime = currentTime;
}

void update_camera(float playerX, float playerY) {
    int targetX = static_cast<int>(playerX + 30) - SCREEN_WIDTH / 2;
    int targetY = static_cast<int>(playerY + 30) - SCREEN_HEIGHT / 2;
    cameraX = std::clamp(targetX, 0, std::max(0, currentMapCols * TILE_SIZE - SCREEN_WIDTH));
    cameraY = std::clamp(targetY, 0, std::max(0, currentMapRows * TILE_SIZE - SCREEN_HEIGHT));
}

int main(int /*argc*/, char* /*argv*/[]) {
    std::srand(static_cast<unsigned int>(std::time(nullptr)));

    // On touch platforms SDL by default synthesizes a mouse event for every
    // finger event (and vice versa), so a single tap could fire both
    // SDL_FINGERDOWN and SDL_MOUSEBUTTONDOWN for the same touch. Every place
    // in this file that checks "MOUSEBUTTONDOWN || FINGERDOWN" would then run
    // twice per tap - e.g. the enemy-count '+' button jumping 1 -> 3 instead
    // of 1 -> 2. Disable the cross-synthesis so each physical tap is exactly
    // one event. Must be set before SDL_Init.
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // Android-safe SDL startup: do not assume audio/video initialization succeeds.
    // A failed audio device must never prevent the game from opening.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        // Some Android devices/drivers can fail audio initialization. Retry with
        // video only because the game can run without sound.
        SDL_Quit();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            return 1;
        }
        audioDevice = 0;
    }

    const int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
    if ((IMG_Init(imgFlags) & imgFlags) != imgFlags) {
        // Images are optional at startup; continue rather than exiting.
    }

    if (char* prefPath = SDL_GetPrefPath("TacticalShooterEngine", "SaveData")) {
        mapFilePath = std::string(prefPath) + "custom_map.txt";
        SDL_free(prefPath);
    }
    ensure_portrait_directory();

    audioSpec.freq = 44100;
    audioSpec.format = AUDIO_S16SYS;
    audioSpec.channels = 1;
    audioSpec.samples = 1024;
    audioSpec.callback = audio_callback;

    // Opening audio is optional. On Android, another app/device audio policy
    // can reject the device; in that case keep audioDevice == 0 and continue.
    if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &audioSpec, nullptr, 0);
        if (audioDevice != 0) {
            SDL_PauseAudioDevice(audioDevice, 0);
        }
    }

    SDL_Window* window = SDL_CreateWindow(
        "Tactical Shooter Engine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_WIDTH, SCREEN_HEIGHT, 0);

    if (!window) {
        if (audioDevice != 0) SDL_CloseAudioDevice(audioDevice);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    // Prefer hardware rendering, but fall back to software rendering on
    // Android devices whose GPU/driver rejects the accelerated renderer.
    SDL_Renderer* renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (!renderer) {
        SDL_DestroyWindow(window);
        if (audioDevice != 0) SDL_CloseAudioDevice(audioDevice);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }
    SDL_RenderSetLogicalSize(renderer, SCREEN_WIDTH, SCREEN_HEIGHT);
    refresh_weapon_radial();
    refresh_map_list();

    float playerPosX = 5 * TILE_SIZE + 10, playerPosY = 5 * TILE_SIZE + 10;
    SDL_Rect player = { static_cast<int>(playerPosX), static_cast<int>(playerPosY), 60, 60 };

    SDL_Rect dockBase = {0, 0, 200, 200}, dockHandle = {0, 0, 60, 60};
    int dockCenterX = 0, dockCenterY = 0;
    bool isTouchingDock = false;
    float moveVecX = 0.0f, moveVecY = 0.0f;
    float joystickMagnitude = 0.0f; // 0..1, how far the stick is deflected from center

    // --- Long-press joystick dodge roll + double-tap arrow dash: shared dash state ---
    float joystickHoldTimer = 0.0f;
    bool isDashing = false;
    float dashDirX = 0.0f, dashDirY = 0.0f;
    float dashDurationSetting = DOUBLE_TAP_DASH_DURATION;
    Uint32 dashStartedAt = 0;
    float dashCooldown = 0.0f;      // seconds remaining before another dash/roll can trigger
    bool canShoot = true;           // disabled while dashing
    Uint32 lastDashTriggeredAt = 0; // drives the post-roll tint flash
    // Simple afterimage ghost trail: last few player positions while dashing.
    struct GhostFrame { float x, y; Uint32 t; };
    std::array<GhostFrame, 6> dashGhosts{};
    int dashGhostCount = 0;
    Uint32 lastGhostSpawnAt = 0;

    // Double-tap arrow key detection (UP, DOWN, LEFT, RIGHT), independent of the
    // held-key shooting logic below - a quick double press bursts a dash instead.
    Uint32 lastArrowTapTime[4] = {0, 0, 0, 0}; // 0=UP,1=DOWN,2=LEFT,3=RIGHT

    // Touch d-pad: tracks which finger pressed each button so a finger dragging off
    // the button (or lifting elsewhere) still releases it correctly instead of
    // leaving the direction "stuck" pressed - fixes the sticky arrow-key touch bug.
    // Eight-way pad finger IDs are declared with the pad state above.

    bool running = true;
    Uint32 lastShootTime = 0;
    Uint32 lastFrameTime = SDL_GetTicks();
    SDL_Event event;

    while (running) {
        Uint32 currentTime = SDL_GetTicks();
        float deltaTime = static_cast<float>(currentTime - lastFrameTime) / 1000.0f;
        if (deltaTime > 0.1f) deltaTime = 0.1f; // clamp huge frame hitches (e.g. window drag)
        lastFrameTime = currentTime;

#if defined(__ANDROID__)
        // Pick up any image the player selected via Android's system photo
        // picker since last frame (MainActivity.java hands it over via JNI).
        android_poll_picked_portrait(renderer);
#endif


        // Match-result sequence: GAME_WON first shows the Mission Accomplished
        // screen, then automatically opens the final leaderboard. GAME_OVER keeps
        // the existing immediate-results behaviour.
        static GameState prevGameStateForLB = GameState::MODE_SELECTION;
        if (prevGameStateForLB != currentGameState) {
            if (currentGameState == GameState::GAME_OVER) {
                playerLosses++;
                showMissionAccomplishedScreen = false;
                showMatchEndLeaderboard = true;
            } else if (currentGameState == GameState::GAME_WON) {
                playerWins++;
                showMissionAccomplishedScreen = true;
                missionAccomplishedAt = currentTime;
                showMatchEndLeaderboard = false;
            }
        }
        if (currentGameState == GameState::GAME_WON && showMissionAccomplishedScreen &&
            currentTime - missionAccomplishedAt >= 1800) {
            showMissionAccomplishedScreen = false;
            showMatchEndLeaderboard = true;
        }
        prevGameStateForLB = currentGameState;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;

            if (currentGameState == GameState::TACTICAL_CONFIG) {
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                    currentGameState = GameState::MODE_SELECTION;
                    continue;
                }
                if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                    int clickX = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int clickY = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                    SDL_Point pt = { clickX, clickY };

                    if (SDL_PointInRect(&pt, &btnBackToMenu)) {
                        currentGameState = GameState::MODE_SELECTION;
                    } else if (SDL_PointInRect(&pt, &btnDecEnemies)) {
                        customTacticalEnemies = std::max(1, customTacticalEnemies - 1);
                    } else if (SDL_PointInRect(&pt, &btnIncEnemies)) {
                        customTacticalEnemies = std::min(MAX_ENEMIES, customTacticalEnemies + 1);
                    } else if (SDL_PointInRect(&pt, &btnDiffNormal)) {
                        tacticalEnemyDifficulty = EnemyDifficulty::NORMAL;
                        tacticalDifficultyChosen = true;
                    } else if (SDL_PointInRect(&pt, &btnDiffHard)) {
                        tacticalEnemyDifficulty = EnemyDifficulty::HARD;
                        tacticalDifficultyChosen = true;
                    } else if (SDL_PointInRect(&pt, &btnDiffExpert)) {
                        tacticalEnemyDifficulty = EnemyDifficulty::EXPERT;
                        tacticalDifficultyChosen = true;
                    } else if (SDL_PointInRect(&pt, &btnDecBots)) {
                        customBotCount = std::max(0, customBotCount - 1);
                    } else if (SDL_PointInRect(&pt, &btnIncBots)) {
                        customBotCount = std::min(MAX_BOTS, customBotCount + 1);
                    } else if (SDL_PointInRect(&pt, &btnStartTac)) {
                        selectedMode = GameMode::TACTICAL;
                        if (aiGeneratedMapActive) generate_ai_map();
                        init_game_arena();
                        if (customPlayerSpawnValid) {
                            playerPosX = customPlayerSpawn.x * TILE_SIZE + 10;
                            playerPosY = customPlayerSpawn.y * TILE_SIZE + 10;
                        } else {
                            playerPosX = 5 * TILE_SIZE + 10;
                            playerPosY = 5 * TILE_SIZE + 10;
                        }
                        player.x = static_cast<int>(playerPosX); player.y = static_cast<int>(playerPosY);
                        currentGameState = GameState::PLAYING;
                    }
                }
                continue;
            }
            
              
                
                    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_e &&
                        !profileEditActive && !editorShowSaveDialog) {
                // Guarded against both text-entry dialogs: without this, typing the
                // letter "e" into the profile name field (or a map filename) would
                // fire this global hotkey mid-keystroke and yank the game into/out
                // of the Map Editor instead of typing an "E".
                currentGameState = (currentGameState == GameState::MAP_EDITOR) ? GameState::MODE_SELECTION : GameState::MAP_EDITOR;
            }

            if (currentGameState == GameState::MAP_EDITOR) {
                // Save-as dialog box: asks for a filename before writing to disk. While
                // open, it swallows all editor input so taps/keys can't leak through to
                // the grid or palette underneath.
                if (editorShowSizeDialog) {
                    if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                        editorShowSizeDialog = false;
                    } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                        int cx = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer,event.button.x,event.button.y).x : static_cast<int>(event.tfinger.x*SCREEN_WIDTH);
                        int cy = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer,event.button.x,event.button.y).y : static_cast<int>(event.tfinger.y*SCREEN_HEIGHT);
                        SDL_Point pt{cx,cy};
                        if (SDL_PointInRect(&pt,&editorSizeWidthMinus)) editorMapCols=std::max(20,editorMapCols-5);
                        else if (SDL_PointInRect(&pt,&editorSizeWidthPlus)) editorMapCols=std::min(COLS,editorMapCols+5);
                        else if (SDL_PointInRect(&pt,&editorSizeHeightMinus)) editorMapRows=std::max(15,editorMapRows-5);
                        else if (SDL_PointInRect(&pt,&editorSizeHeightPlus)) editorMapRows=std::min(ROWS,editorMapRows+5);
                        else if (SDL_PointInRect(&pt,&editorSizeApply)) { reset_editor_map_size(editorMapCols,editorMapRows); editor_set_status("SIZE SET",currentTime); editorShowSizeDialog=false; }
                        else if (SDL_PointInRect(&pt,&editorSizeCancel)) editorShowSizeDialog=false;
                    }
                    continue;
                }

                if (editorShowSaveDialog) {
                    if (event.type == SDL_TEXTINPUT) {
                        if (editorFilenameInput.size() < 40) editorFilenameInput += event.text.text;
                    } else if (event.type == SDL_KEYDOWN) {
                        if (event.key.keysym.sym == SDLK_BACKSPACE && !editorFilenameInput.empty()) {
                            editorFilenameInput.pop_back();
                        } else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
                            std::string name = sanitize_map_name(editorFilenameInput);
                            std::string path = map_directory() + name + ".txt";
                            if (save_map_to_file(path)) {
                                mapFilePath = path;
                                useCustomMap = true;
                                refresh_map_list();
                                editor_set_status("SAVED " + name + ".txt", currentTime);
                            } else {
                                editor_set_status("SAVE FAILED", currentTime);
                            }
                            editor_close_save_dialog();
                        } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                            editor_close_save_dialog();
                        }
                    } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                        int cx = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                        int cy = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                        SDL_Point pt{cx, cy};
                        if (SDL_PointInRect(&pt, &editorDialogSave)) {
                            std::string name = sanitize_map_name(editorFilenameInput);
                            std::string path = map_directory() + name + ".txt";
                            if (save_map_to_file(path)) {
                                mapFilePath = path;
                                useCustomMap = true;
                                refresh_map_list();
                                editor_set_status("SAVED " + name + ".txt", currentTime);
                            } else {
                                editor_set_status("SAVE FAILED", currentTime);
                            }
                            editor_close_save_dialog();
                        } else if (SDL_PointInRect(&pt, &editorDialogCancel)) {
                            editor_close_save_dialog();
                        } else if (SDL_PointInRect(&pt, &editorDialogField)) {
                            // Re-summon the keyboard if the OS dismissed it without
                            // closing the dialog - same forced-edge trick as opening.
                            SDL_StopTextInput();
                            SDL_StartTextInput();
                        }
                    }
                    continue;
                }

                if (editorShowLoadDialog) {
                    if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                        int cx = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                        int cy = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                        SDL_Point pt{cx, cy};
                        const int boxX = SCREEN_WIDTH / 2 - 360;
                        const int boxY = 130;
                        const int rowH = 70;
                        const int maxRows = std::min(8, static_cast<int>(availableMapFiles.size()));
                        for (int i = 0; i < maxRows; ++i) {
                            SDL_Rect row{boxX + 25, boxY + 90 + i * rowH, 670, rowH - 8};
                            SDL_Rect del{row.x + row.w - 125, row.y + 6, 112, row.h - 12};
                            if (SDL_PointInRect(&pt, &del)) {
                                std::string name = availableMapFiles[i];
                                if (delete_map_file_by_index(i)) editor_set_status("DELETED " + name, currentTime);
                                else editor_set_status("DELETE FAILED", currentTime);
                                editorShowLoadDialog = true;
                                break;
                            }
                            if (SDL_PointInRect(&pt, &row)) {
                                if (select_map_file_by_index(i)) {
                                    load_map_from_file(mapFilePath);
                                    editor_set_status("LOADED " + map_basename(mapFilePath), currentTime);
                                }
                                editorShowLoadDialog = false;
                                break;
                            }
                        }
                        SDL_Rect cancel{boxX + 25, boxY + 800 - 65, 670, 45};
                        if (SDL_PointInRect(&pt, &cancel)) editorShowLoadDialog = false;
                    } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                        editorShowLoadDialog = false;
                    }
                    continue;
                }

                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    SDL_Point mp = mouse_to_logical(renderer, event.button.x, event.button.y);
                    int x = mp.x, y = mp.y;
                    SDL_Point pt{x, y};
                    bool handled = false;
                    if (SDL_PointInRect(&pt, &btnBackToMenu)) {
                        currentGameState = GameState::MODE_SELECTION;
                        editorShowSaveDialog = false;
                        editorShowLoadDialog = false;
                        editorShowSizeDialog = false;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorSaveButton)) {
                        editor_open_save_dialog();
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorOverwriteButton)) {
                        if (!mapFilePath.empty() && save_map_to_file(mapFilePath)) {
                            refresh_map_list();
                            useCustomMap = true;
                            editor_set_status("SAVED " + map_basename(mapFilePath), currentTime);
                        } else {
                            editor_set_status("NO MAP TO OVERWRITE - USE SAVE AS", currentTime);
                        }
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorLoadButton)) {
                        refresh_map_list();
                        editorShowLoadDialog = true;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorSizeButton)) {
                        editorShowSizeDialog = true;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorEraseButton)) {
                        editorEraseMode = !editorEraseMode;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorExitButton)) {
                        currentGameState = GameState::MODE_SELECTION;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorZoomOutButton)) {
                        editor_set_zoom(editorZoom - EDITOR_ZOOM_STEP);
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorZoomInButton)) {
                        editor_set_zoom(editorZoom + EDITOR_ZOOM_STEP);
                        handled = true;
                    } else {
                        for (const auto& opt : editorPalette) {
                            if (SDL_PointInRect(&pt, &opt.rect)) {
                                editorSelectedTile = opt.value;
                                editorEraseMode = false;
                                handled = true;
                                break;
                            }
                        }
                    }
                    if (!handled) {
                        // Right-click is always a quick erase, regardless of the selected brush.
                        editor_apply_tile(x, y, event.button.button == SDL_BUTTON_RIGHT);
                    }
                }

                // Mouse wheel doubles as a zoom control on desktop.
                if (event.type == SDL_MOUSEWHEEL) {
                    int dir = (event.wheel.y > 0) ? 1 : (event.wheel.y < 0 ? -1 : 0);
                    if (dir != 0) editor_set_zoom(editorZoom + dir * EDITOR_ZOOM_STEP);
                }

                if (event.type == SDL_FINGERDOWN) {
                    int x = static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int y = static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                    SDL_Point pt{x,y};
                    bool handled = false;

                    if (SDL_PointInRect(&pt, &btnBackToMenu)) {
                        currentGameState = GameState::MODE_SELECTION;
                        editorShowSaveDialog = false;
                        editorShowLoadDialog = false;
                        editorShowSizeDialog = false;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorSaveButton)) {
                        editor_open_save_dialog();
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorOverwriteButton)) {
                        if (!mapFilePath.empty() && save_map_to_file(mapFilePath)) {
                            refresh_map_list();
                            useCustomMap = true;
                            editor_set_status("SAVED " + map_basename(mapFilePath), currentTime);
                        } else {
                            editor_set_status("NO MAP TO OVERWRITE - USE SAVE AS", currentTime);
                        }
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorLoadButton)) {
                        refresh_map_list();
                        editorShowLoadDialog = true;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorSizeButton)) {
                        editorShowSizeDialog = true;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorEraseButton)) {
                        editorEraseMode = !editorEraseMode;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorExitButton)) {
                        currentGameState = GameState::MODE_SELECTION;
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorZoomOutButton)) {
                        editor_set_zoom(editorZoom - EDITOR_ZOOM_STEP);
                        handled = true;
                    } else if (SDL_PointInRect(&pt, &editorZoomInButton)) {
                        editor_set_zoom(editorZoom + EDITOR_ZOOM_STEP);
                        handled = true;
                    } else {
                        for (const auto& opt : editorPalette) {
                            if (SDL_PointInRect(&pt, &opt.rect)) {
                                editorSelectedTile = opt.value;
                                editorEraseMode = false;
                                handled = true;
                                break;
                            }
                        }
                    }

                    if (!handled && y >= EDITOR_PANEL_BOTTOM) {
                        if (!editorTouchActive) {
                            // First finger on the grid: starts panning / tapping-to-paint.
                            editorTouchActive = true;
                            editorFingerId = event.tfinger.fingerId;
                            editorLastTouchX = x;
                            editorLastTouchY = y;
                            editorTouch1X = x;
                            editorTouch1Y = y;
                            editorDragged = false;
                        } else if (editorFingerId2 == -1 && event.tfinger.fingerId != editorFingerId) {
                            // Second finger lands while the first is already down on the
                            // grid: begin a pinch-to-zoom gesture instead of panning.
                            editorFingerId2 = event.tfinger.fingerId;
                            editorTouch2X = x;
                            editorTouch2Y = y;
                            float ddx = static_cast<float>(editorTouch1X - editorTouch2X);
                            float ddy = static_cast<float>(editorTouch1Y - editorTouch2Y);
                            editorPinchStartDist = std::sqrt(ddx * ddx + ddy * ddy);
                            editorPinchStartZoom = editorZoom;
                            editorPinching = editorPinchStartDist > 1.0f;
                            editorDragged = true; // suppress a tile paint when fingers lift
                        }
                    }
                }

                if (event.type == SDL_FINGERMOTION && editorTouchActive &&
                    (event.tfinger.fingerId == editorFingerId || event.tfinger.fingerId == editorFingerId2)) {
                    int x = static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int y = static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);

                    if (event.tfinger.fingerId == editorFingerId) { editorTouch1X = x; editorTouch1Y = y; }
                    else { editorTouch2X = x; editorTouch2Y = y; }

                    if (editorPinching) {
                        float ddx = static_cast<float>(editorTouch1X - editorTouch2X);
                        float ddy = static_cast<float>(editorTouch1Y - editorTouch2Y);
                        float dist = std::sqrt(ddx * ddx + ddy * ddy);
                        if (editorPinchStartDist > 1.0f) {
                            editor_set_zoom(editorPinchStartZoom * (dist / editorPinchStartDist));
                        }
                    } else if (event.tfinger.fingerId == editorFingerId) {
                        int dx = x - editorLastTouchX;
                        int dy = y - editorLastTouchY;
                        if (std::abs(dx) + std::abs(dy) > 3) {
                            editorDragged = true;
                            int maxX = std::max(0, static_cast<int>(editorMapCols * TILE_SIZE - SCREEN_WIDTH / editorZoom));
                            int maxY = std::max(0, static_cast<int>(editorMapRows * TILE_SIZE - SCREEN_HEIGHT / editorZoom));
                            cameraX = std::clamp(cameraX - static_cast<int>(dx / editorZoom), 0, maxX);
                            cameraY = std::clamp(cameraY - static_cast<int>(dy / editorZoom), 0, maxY);
                            editorLastTouchX = x;
                            editorLastTouchY = y;
                        }
                    }
                }

                if (event.type == SDL_FINGERUP && (event.tfinger.fingerId == editorFingerId || event.tfinger.fingerId == editorFingerId2)) {
                    int x = static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int y = static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);

                    if (event.tfinger.fingerId == editorFingerId2) {
                        // Second finger lifted: end the pinch but keep panning with
                        // whichever finger is still down.
                        editorFingerId2 = -1;
                        editorPinching = false;
                        editorLastTouchX = editorTouch1X;
                        editorLastTouchY = editorTouch1Y;
                    } else {
                        if (!editorDragged) editor_apply_tile(x, y, false);
                        editorTouchActive = false;
                        editorFingerId = -1;
                        // If a second finger was still down, it can't keep panning alone;
                        // clear it too so the next touch starts a clean gesture.
                        editorFingerId2 = -1;
                        editorPinching = false;
                    }
                }

                if (event.type == SDL_KEYDOWN) {
                    if (event.key.keysym.sym == SDLK_s) editor_open_save_dialog();
                    if (event.key.keysym.sym == SDLK_l) {
                        refresh_map_list();
                        editor_set_status(load_map_from_file(mapFilePath) ? ("LOADED " + map_basename(mapFilePath)) : "NO SAVED MAP FOUND", currentTime);
                    }
                }
                continue;
            }

            if (currentGameState == GameState::WEAPON_MENU) {
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
                    currentGameState = GameState::MODE_SELECTION;
                    continue;
                }
                if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                    int clickX = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int clickY = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                    const int ys[] = {250,365,480,595,710};
                    const WeaponType types[] = {WeaponType::PISTOL,WeaponType::RIFLE,WeaponType::ROCKET,WeaponType::LASER,WeaponType::VECTOR_REFLECT};
                    for (int i=0;i<5;++i) {
                        SDL_Rect b{SCREEN_WIDTH/2-350,ys[i],700,85};
                        SDL_Point pt{clickX,clickY};
                        if (SDL_PointInRect(&pt,&b)) { toggle_weapon_loadout(types[i], currentTime); break; }
                    }
                    SDL_Point pt{clickX,clickY};
                    if (SDL_PointInRect(&pt,&btnBackToMenu)) currentGameState = GameState::MODE_SELECTION;
                }
                continue;
            }

            if (currentGameState == GameState::MODE_SELECTION) {
                // Profile name edit dialog swallows all menu input while open, same
                // pattern as the map editor's save-as dialog.
                if (profileEditActive) {
                    if (event.type == SDL_TEXTINPUT) {
                        if (profileNameInput.size() < 16) profileNameInput += event.text.text;
                    } else if (event.type == SDL_KEYDOWN) {
                        if (event.key.keysym.sym == SDLK_BACKSPACE && !profileNameInput.empty()) {
                            profileNameInput.pop_back();
                        } else if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
                            if (!profileNameInput.empty()) playerName = profileNameInput;
                            profile_close_edit_dialog();
                        } else if (event.key.keysym.sym == SDLK_ESCAPE) {
                            profile_close_edit_dialog();
                        }
                    } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                        int cx = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                        int cy = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                        SDL_Point pt{cx, cy};
                        if (SDL_PointInRect(&pt, &profileDialogSave)) {
                            if (!profileNameInput.empty()) playerName = profileNameInput;
                            profile_close_edit_dialog();
                        } else if (SDL_PointInRect(&pt, &profileDialogCancel)) {
                            profile_close_edit_dialog();
                        } else if (SDL_PointInRect(&pt, &profileDialogField)) {
                            // Re-summon the keyboard if the OS dismissed it without
                            // closing our dialog - same forced-edge trick as opening.
                            SDL_StopTextInput();
                            SDL_StartTextInput();
                        }
                    }
                    continue;
                }

                // Portrait picker swallows menu input while open, same pattern
                // as the profile name dialog above.
                if (portraitPickerOpen) {
                    if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                        int cx = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                        int cy = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                        SDL_Point pt{cx, cy};
                        int rowIdx = portrait_picker_row_at(cx, cy);
                        if (rowIdx >= 0) {
                            select_portrait_file_by_index(renderer, rowIdx);
                        } else if (!SDL_PointInRect(&pt, &portraitPickerBox) && !SDL_PointInRect(&pt, &btnProfileImage)) {
                            portraitPickerOpen = false;
                        }
                    }
                    continue;
                }

                if (mapDropdownOpen && (event.type == SDL_MOUSEMOTION || event.type == SDL_FINGERMOTION)) {
                    int moveX = (event.type == SDL_MOUSEMOTION) ? mouse_to_logical(renderer, event.motion.x, event.motion.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int moveY = (event.type == SDL_MOUSEMOTION) ? mouse_to_logical(renderer, event.motion.x, event.motion.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                    hoveredMapIndex = map_index_at_menu_point(moveX, moveY);
                }
                if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN) {
                    int clickX = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).x : static_cast<int>(event.tfinger.x * SCREEN_WIDTH);
                    int clickY = (event.type == SDL_MOUSEBUTTONDOWN) ? mouse_to_logical(renderer, event.button.x, event.button.y).y : static_cast<int>(event.tfinger.y * SCREEN_HEIGHT);
                    SDL_Point pt = { clickX, clickY };

                    if (SDL_PointInRect(&pt, &btnMainMenuExit)) {
                        running = false;
                    } else if (SDL_PointInRect(&pt, &btnProfileImage)) {
#if defined(__ANDROID__)
                        android_open_image_picker();
#else
                        refresh_portrait_list();
                        portraitPickerOpen = true;
#endif
                    } else if (SDL_PointInRect(&pt, &btnProfileName)) {
                        profile_open_edit_dialog();
                    } else if (SDL_PointInRect(&pt, &btnTacticalMode)) {
                        tacticalDifficultyChosen = false; // fresh visit to config: nothing highlighted yet
                        currentGameState = GameState::TACTICAL_CONFIG;
                    } else if (SDL_PointInRect(&pt, &btnInfinityMode)) {
                        selectedMode = GameMode::ENDLESS;
                        aiGeneratedMapActive = false;
                        init_game_arena();
                        if (customPlayerSpawnValid) {
                            playerPosX = customPlayerSpawn.x * TILE_SIZE + 10;
                            playerPosY = customPlayerSpawn.y * TILE_SIZE + 10;
                        } else {
                            playerPosX = 5 * TILE_SIZE + 10;
                            playerPosY = 5 * TILE_SIZE + 10;
                        }
                        player.x = static_cast<int>(playerPosX); player.y = static_cast<int>(playerPosY);
                        currentGameState = GameState::PLAYING;
                    } else if (SDL_PointInRect(&pt, &btnMapEditor)) {
                        refresh_map_list();
                        aiGeneratedMapActive = false;
                        currentGameState = GameState::MAP_EDITOR;
                    } else if (SDL_PointInRect(&pt, &btnMapSelect)) {
                        refresh_map_list();
                        mapDropdownOpen = !mapDropdownOpen;
                        hoveredMapIndex = -1;
                    } else if (mapDropdownOpen) {
                        // The open dropdown is drawn on top of (and overlaps) the buttons
                        // below it - USE SELECTED MAP, AI GENERATED MAP, DODGE + ROLL, and
                        // WEAPON MENU all sit underneath its row area. It must claim taps
                        // in its own bounds before those buttons get a chance to, or a tap
                        // on a map row falls through and hits whatever button is behind it
                        // instead of selecting the map.
                        const int rowH = 52;
                        const int shownRows = std::min(7, static_cast<int>(availableMapFiles.size()));
                        SDL_Rect listBox{btnMapSelect.x, btnMapSelect.y + btnMapSelect.h + 8,
                                         btnMapSelect.w, std::max(rowH, shownRows * rowH)};
                        if (!availableMapFiles.empty() && SDL_PointInRect(&pt, &listBox)) {
                            const int idx = (pt.y - listBox.y) / rowH;
                            if (idx >= 0 && idx < shownRows) {
                                select_map_file_by_index(idx);
                                mapDropdownOpen = false;
                                hoveredMapIndex = -1;
                            }
                        } else {
                            mapDropdownOpen = false;
                            hoveredMapIndex = -1;
                        }
                    } else if (SDL_PointInRect(&pt, &btnAIGenerated)) {
                        // This is a tick, not a start action - it only marks AI-generated
                        // as the map source. The actual map is generated and the match
                        // begins from TACTICAL_CONFIG's Start button.
                        aiGeneratedMapActive = !aiGeneratedMapActive;
                        if (aiGeneratedMapActive) useCustomMap = false;
                    } else if (SDL_PointInRect(&pt, &btnDodgeRoll)) {
                        dodgeRollEnabled = !dodgeRollEnabled;
                        if (!dodgeRollEnabled) {
                            isDashing = false;
                            playerIsInvulnerable = false;
                            canShoot = true;
                        }
                    } else if (SDL_PointInRect(&pt, &btnWeaponMenu)) {
                        currentGameState = GameState::WEAPON_MENU;
                    } else if (SDL_PointInRect(&pt, &btnUseCustomMap)) {
                        if (selectedMapIndex >= 0) { useCustomMap = !useCustomMap; aiGeneratedMapActive = false; }
                    }
                }
                continue;
            }

            if (event.type == SDL_KEYDOWN && currentGameState == GameState::PLAYING) {
                if (event.key.keysym.sym >= SDLK_1 && event.key.keysym.sym <= SDLK_4) {
                    int slot = event.key.keysym.sym - SDLK_1;
                    if (slot < playerWeaponState.equippedCount) set_player_weapon(playerWeaponState.equipped[slot], currentTime);
                }
                // Quick-swap: hop back to whatever was equipped before the current weapon.
                if (event.key.keysym.sym == SDLK_q) set_player_weapon(playerWeaponState.previous, currentTime);
                // Shield moved off Q (now quick-swap) onto F.
                if (event.key.keysym.sym == SDLK_f) try_activate_shield(currentTime);

                // Optional double-tap arrow key dash: two presses of the SAME arrow key within
                // DOUBLE_TAP_WINDOW triggers a dash in that cardinal direction (or a
                // diagonal if an adjacent arrow is also currently held). event.key.repeat
                // guards against a held key auto-repeating into a false double-tap.
                if (dodgeRollEnabled && event.key.repeat == 0 && dashCooldown <= 0.0f && !isDashing) {
                    int arrowIdx = -1;
                    float tapDirX = 0.0f, tapDirY = 0.0f;
                    if (event.key.keysym.sym == SDLK_UP)    { arrowIdx = 0; tapDirY = -1.0f; }
                    if (event.key.keysym.sym == SDLK_DOWN)  { arrowIdx = 1; tapDirY =  1.0f; }
                    if (event.key.keysym.sym == SDLK_LEFT)  { arrowIdx = 2; tapDirX = -1.0f; }
                    if (event.key.keysym.sym == SDLK_RIGHT) { arrowIdx = 3; tapDirX =  1.0f; }

                    if (arrowIdx >= 0) {
                        float sinceLastTap = static_cast<float>(currentTime - lastArrowTapTime[arrowIdx]) / 1000.0f;
                        if (lastArrowTapTime[arrowIdx] != 0 && sinceLastTap <= DOUBLE_TAP_WINDOW) {
                            // Combine an adjacent held direction key for a diagonal dash.
                            const Uint8* dashState = SDL_GetKeyboardState(nullptr);
                            if (arrowIdx == 0 || arrowIdx == 1) { // vertical tap - check held horizontal
                                if (dashState[SDL_SCANCODE_LEFT])  tapDirX = -1.0f;
                                if (dashState[SDL_SCANCODE_RIGHT]) tapDirX =  1.0f;
                            } else { // horizontal tap - check held vertical
                                if (dashState[SDL_SCANCODE_UP])   tapDirY = -1.0f;
                                if (dashState[SDL_SCANCODE_DOWN]) tapDirY =  1.0f;
                            }
                            float len = std::sqrt(tapDirX * tapDirX + tapDirY * tapDirY);
                            if (len > 0.0001f) {
                                dashDirX = tapDirX / len; dashDirY = tapDirY / len;
                                dashDurationSetting = DOUBLE_TAP_DASH_DURATION;
                                isDashing = true; playerIsInvulnerable = true; canShoot = false;
                                dashStartedAt = currentTime; lastDashTriggeredAt = currentTime;
                                dashGhostCount = 0; lastGhostSpawnAt = 0;
                            }
                            lastArrowTapTime[arrowIdx] = 0; // consume - avoid chaining into a triple-tap
                        } else {
                            lastArrowTapTime[arrowIdx] = currentTime;
                        }
                    }
                }
            }

            if (event.type == SDL_MOUSEWHEEL && currentGameState == GameState::PLAYING) {
                int dir = (event.wheel.y > 0) ? 1 : (event.wheel.y < 0 ? -1 : 0);
                if (dir != 0) set_player_weapon(cycle_weapon(playerWeapon, dir), currentTime);
            }

            if ((currentGameState == GameState::GAME_OVER || currentGameState == GameState::GAME_WON) && 
                (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_FINGERDOWN)) {
                // A TACTICAL mission is 10 rounds regardless of whether each round
                // is won or lost - dying doesn't end the mission early, it just
                // costs that round. Same bot roster (names/kills/deaths) carries
                // over round to round. Only clearing/losing round 10 (or an
                // ENDLESS run) returns to the menu, and resets the round count.
                bool continueNextRound = selectedMode == GameMode::TACTICAL &&
                                          currentRound < MAX_TACTICAL_ROUNDS;

                showMatchEndLeaderboard = false;
                showLeaderboardPanel = false;
                playerPosX = 5 * TILE_SIZE + 10; playerPosY = 5 * TILE_SIZE + 10;
                player.x = static_cast<int>(playerPosX); player.y = static_cast<int>(playerPosY);
                isTouchingDock = false;
                moveFingerId = -1;
                joystickMagnitude = 0.0f; joystickHoldTimer = 0.0f;
                isDashing = false; dashCooldown = 0.0f; canShoot = true; playerIsInvulnerable = false;
                dpadUpPressed = dpadDownPressed = dpadLeftPressed = dpadRightPressed = false;
                dpadUpLeftPressed = dpadUpRightPressed = dpadDownLeftPressed = dpadDownRightPressed = false;
                for (auto& id : padFingerId) id = -1;
                weaponSwipeActive = false; weaponSwipeFingerId = -1;

                if (continueNextRound) {
                    currentRound++;
                    if (aiGeneratedMapActive) generate_ai_map();
                    init_game_arena(true); // keepRoster: same bots, same names/tallies
                    if (customPlayerSpawnValid) {
                        playerPosX = customPlayerSpawn.x * TILE_SIZE + 10;
                        playerPosY = customPlayerSpawn.y * TILE_SIZE + 10;
                    }
                    player.x = static_cast<int>(playerPosX); player.y = static_cast<int>(playerPosY);
                    currentGameState = GameState::PLAYING;
                } else {
                    currentGameState = GameState::MODE_SELECTION;
                }
            }

            if (currentGameState == GameState::PLAYING) {
                if (event.type == SDL_FINGERDOWN) {
                    float nx = event.tfinger.x, ny = event.tfinger.y;
                    SDL_Point backPt{static_cast<int>(nx * SCREEN_WIDTH), static_cast<int>(ny * SCREEN_HEIGHT)};
                    if (SDL_PointInRect(&backPt, &btnPlayingBack)) {
                        currentGameState = GameState::MODE_SELECTION;
                        mapDropdownOpen = false;
                        moveFingerId = -1;
                        isTouchingDock = false;
                        joystickMagnitude = 0.0f;
                        defuseHeld = false;
                        defuseStartTime = 0;
                        continue;
                    }
                    if (SDL_PointInRect(&backPt, &btnLeaderboard)) {
                        showLeaderboardPanel = !showLeaderboardPanel;
                        continue;
                    }
                    float touchX = nx * SCREEN_WIDTH, touchY = ny * SCREEN_HEIGHT;
                    SDL_Point touchPt = { static_cast<int>(touchX), static_cast<int>(touchY) };

                    if (in_weapon_zone(nx, ny)) {
                        int sector = radial_sector_at(touchPt.x - RADIAL_CX, touchPt.y - RADIAL_CY);
                        if (sector >= 0) {
                            if (weaponRadial[sector].type != WeaponType::COUNT) set_player_weapon(weaponRadial[sector].type, currentTime);
                        } else {
                            // Dead-zone tap - start tracking for a cycle-swipe instead.
                            weaponSwipeActive = true;
                            weaponSwipeFingerId = event.tfinger.fingerId;
                            weaponSwipeStartY = touchY;
                            weaponSwipeTriggered = false;
                        }
                    } else if (playerWeapon == WeaponType::PISTOL && SDL_PointInRect(&touchPt, &shieldButton)) {
                        try_activate_shield(currentTime);
                    } else if (selectedMode == GameMode::TACTICAL && tacticalBomb.planted && !tacticalBomb.defused &&
                               SDL_HasIntersection(&player, &tacticalBomb.rect) && SDL_PointInRect(&touchPt, &defuseButton)) {
                        defuseHeld = true;
                        if (defuseStartTime == 0) defuseStartTime = currentTime;
                    } else {
                        if (in_joystick_zone(nx, ny) && moveFingerId == -1) {
                            moveFingerId = event.tfinger.fingerId;
                            isTouchingDock = true;
                            joystickMagnitude = 0.0f;
                            joystickHoldTimer = 0.0f;
                            dockCenterX = touchPt.x; dockCenterY = touchPt.y;
                            dockBase.x = dockCenterX - dockBase.w / 2; dockBase.y = dockCenterY - dockBase.h / 2;
                            dockHandle.x = dockCenterX - dockHandle.w / 2; dockHandle.y = dockCenterY - dockHandle.h / 2;
                        }

                        if (touchX >= SCREEN_WIDTH / 2.0f) {
                            if (SDL_PointInRect(&touchPt, &padUpLeft)    && padFingerId[0] == -1) { dpadUpLeftPressed = true;    padFingerId[0] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padUp)        && padFingerId[1] == -1) { dpadUpPressed = true;        padFingerId[1] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padUpRight)   && padFingerId[2] == -1) { dpadUpRightPressed = true;   padFingerId[2] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padLeft)      && padFingerId[3] == -1) { dpadLeftPressed = true;      padFingerId[3] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padRight)     && padFingerId[4] == -1) { dpadRightPressed = true;     padFingerId[4] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padDownLeft) && padFingerId[5] == -1) { dpadDownLeftPressed = true; padFingerId[5] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padDown)     && padFingerId[6] == -1) { dpadDownPressed = true;     padFingerId[6] = event.tfinger.fingerId; }
                            if (SDL_PointInRect(&touchPt, &padDownRight)&& padFingerId[7] == -1) { dpadDownRightPressed = true;padFingerId[7] = event.tfinger.fingerId; }
                        }
                    }
                } else if (event.type == SDL_FINGERMOTION) {
                    if (event.tfinger.fingerId == weaponSwipeFingerId && weaponSwipeActive && !weaponSwipeTriggered) {
                        float touchY = event.tfinger.y * SCREEN_HEIGHT;
                        float dy = touchY - weaponSwipeStartY;
                        constexpr float SWIPE_THRESHOLD = 45.0f;
                        if (dy <= -SWIPE_THRESHOLD) { // swiped up -> next weapon
                            set_player_weapon(cycle_weapon(playerWeapon, 1), currentTime);
                            weaponSwipeFeedbackDir = 1;
                            weaponSwipeTriggered = true;
                        } else if (dy >= SWIPE_THRESHOLD) { // swiped down -> previous weapon
                            set_player_weapon(cycle_weapon(playerWeapon, -1), currentTime);
                            weaponSwipeFeedbackDir = -1;
                            weaponSwipeTriggered = true;
                        }
                    }
                    if (event.tfinger.fingerId == moveFingerId && isTouchingDock) {
                        float touchX = event.tfinger.x * SCREEN_WIDTH;
                        float touchY = event.tfinger.y * SCREEN_HEIGHT;
                        float dx = touchX - dockCenterX, dy = touchY - dockCenterY;
                        float dist = std::sqrt(dx * dx + dy * dy);

                        if (dist > 0) {
                            moveVecX = dx / dist; moveVecY = dy / dist;
                            float clampDist = std::min(dist, dockBase.w / 2.0f);
                            joystickMagnitude = clampDist / (dockBase.w / 2.0f);
                            dockHandle.x = dockCenterX + static_cast<int>((dx / dist) * clampDist) - dockHandle.w / 2;
                            dockHandle.y = dockCenterY + static_cast<int>((dy / dist) * clampDist) - dockHandle.h / 2;
                        }
                    }
                } else if (event.type == SDL_FINGERUP) {
                    if (event.tfinger.fingerId == weaponSwipeFingerId) {
                        weaponSwipeActive = false;
                        weaponSwipeFingerId = -1;
                    }
                    if (selectedMode == GameMode::TACTICAL) {
                        float touchX = event.tfinger.x * SCREEN_WIDTH;
                        float touchY = event.tfinger.y * SCREEN_HEIGHT;
                        SDL_Point touchPt = {static_cast<int>(touchX), static_cast<int>(touchY)};
                        if (SDL_PointInRect(&touchPt, &defuseButton)) {
                            defuseHeld = false;
                            defuseStartTime = 0;
                        }
                    }
                    if (event.tfinger.fingerId == moveFingerId) {
                        moveFingerId = -1; isTouchingDock = false; moveVecX = 0.0f; moveVecY = 0.0f;
                        joystickMagnitude = 0.0f; joystickHoldTimer = 0.0f; // release resets the long-press charge
                    }

                    // Release by fingerId, not by current touch position - a finger that
                    // drags off the button (or lifts elsewhere on screen) still releases
                    // whichever pad button it originally pressed, instead of leaving that
                    // direction stuck "pressed" forever.
                    SDL_FingerID upFinger = event.tfinger.fingerId;
                    if (padFingerId[0] == upFinger) { dpadUpLeftPressed = false; padFingerId[0] = -1; }
                    if (padFingerId[1] == upFinger) { dpadUpPressed = false; padFingerId[1] = -1; }
                    if (padFingerId[2] == upFinger) { dpadUpRightPressed = false; padFingerId[2] = -1; }
                    if (padFingerId[3] == upFinger) { dpadLeftPressed = false; padFingerId[3] = -1; }
                    if (padFingerId[4] == upFinger) { dpadRightPressed = false; padFingerId[4] = -1; }
                    if (padFingerId[5] == upFinger) { dpadDownLeftPressed = false; padFingerId[5] = -1; }
                    if (padFingerId[6] == upFinger) { dpadDownPressed = false; padFingerId[6] = -1; }
                    if (padFingerId[7] == upFinger) { dpadDownRightPressed = false; padFingerId[7] = -1; }
                }

                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    SDL_Point clickPt = mouse_to_logical(renderer, event.button.x, event.button.y);
                    if (SDL_PointInRect(&clickPt, &btnPlayingBack)) {
                        currentGameState = GameState::MODE_SELECTION;
                        mapDropdownOpen = false;
                        moveFingerId = -1;
                        isTouchingDock = false;
                        joystickMagnitude = 0.0f;
                        defuseHeld = false;
                        defuseStartTime = 0;
                    } else if (SDL_PointInRect(&clickPt, &btnLeaderboard)) {
                        showLeaderboardPanel = !showLeaderboardPanel;
                    } else if (in_weapon_zone(static_cast<float>(clickPt.x) / SCREEN_WIDTH, static_cast<float>(clickPt.y) / SCREEN_HEIGHT)) {
                        int sector = radial_sector_at(clickPt.x - RADIAL_CX, clickPt.y - RADIAL_CY);
                        if (sector >= 0) if (weaponRadial[sector].type != WeaponType::COUNT) set_player_weapon(weaponRadial[sector].type, currentTime);
                    } else if (playerWeapon == WeaponType::PISTOL && SDL_PointInRect(&clickPt, &shieldButton)) {
                        try_activate_shield(currentTime);
                    } else if (selectedMode == GameMode::TACTICAL && tacticalBomb.planted && !tacticalBomb.defused &&
                               SDL_HasIntersection(&player, &tacticalBomb.rect) && SDL_PointInRect(&clickPt, &defuseButton)) {
                        defuseHeld = true;
                        if (defuseStartTime == 0) defuseStartTime = currentTime;
                    } else if (moveFingerId == -1) {
                        if (SDL_PointInRect(&clickPt, &padUpLeft))    dpadUpLeftPressed = true;
                        if (SDL_PointInRect(&clickPt, &padUp))        dpadUpPressed = true;
                        if (SDL_PointInRect(&clickPt, &padUpRight))  dpadUpRightPressed = true;
                        if (SDL_PointInRect(&clickPt, &padLeft))     dpadLeftPressed = true;
                        if (SDL_PointInRect(&clickPt, &padRight))    dpadRightPressed = true;
                        if (SDL_PointInRect(&clickPt, &padDownLeft)) dpadDownLeftPressed = true;
                        if (SDL_PointInRect(&clickPt, &padDown))     dpadDownPressed = true;
                        if (SDL_PointInRect(&clickPt, &padDownRight))dpadDownRightPressed = true;
                    }
                }
                if (event.type == SDL_MOUSEBUTTONUP && moveFingerId == -1) {
                    dpadUpPressed = dpadDownPressed = dpadLeftPressed = dpadRightPressed = false;
                    dpadUpLeftPressed = dpadUpRightPressed = dpadDownLeftPressed = dpadDownRightPressed = false;
                    defuseHeld = false;
                    defuseStartTime = 0;
                }
            }
        }

        if (currentGameState == GameState::TACTICAL_CONFIG) {
            SDL_SetRenderDrawColor(renderer, 15, 18, 25, 255);
            SDL_RenderClear(renderer);
            render_tactical_config(renderer);
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }

        if (currentGameState == GameState::MAP_EDITOR) {
            SDL_SetRenderDrawColor(renderer, 20, 20, 25, 255);
            SDL_RenderClear(renderer);
            render_map_editor(renderer, currentTime);
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }

        if (currentGameState == GameState::WEAPON_MENU) {
            render_weapon_menu(renderer);
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }

        if (currentGameState == GameState::MODE_SELECTION) {
            render_mode_selection(renderer);
            SDL_RenderPresent(renderer);
            SDL_Delay(16);
            continue;
        }

        if (currentGameState == GameState::PLAYING) {
            const Uint8* state = SDL_GetKeyboardState(nullptr);

            if (playerShieldActive && currentTime - playerShieldActivatedAt >= SHIELD_DURATION_MS) {
                playerShieldActive = false;
                playerShieldReadyAt = currentTime + SHIELD_COOLDOWN_MS;
            }
            // Switching off pistol drops the shield immediately - it's a pistol-exclusive ability.
            if (playerShieldActive && playerWeapon != WeaponType::PISTOL) {
                playerShieldActive = false;
                playerShieldReadyAt = currentTime + SHIELD_COOLDOWN_MS;
            }


            if (selectedMode == GameMode::TACTICAL && state[SDL_SCANCODE_SPACE]) {
                defuseHeld = true;
            } else if (selectedMode == GameMode::TACTICAL && !defuseHeld) {
                defuseStartTime = 0;
            }

            // Dash/roll cooldown ticks down every frame using real delta time.
            if (dashCooldown > 0.0f) dashCooldown = std::max(0.0f, dashCooldown - deltaTime);

            // --- Optional long-press joystick dodge roll: accumulate hold time while the stick
            // is deflected past the deadzone threshold; auto-trigger once it crosses
            // LONG_PRESS_DURATION (frame-rate independent via deltaTime). ---
            bool joystickDeflected = isTouchingDock && joystickMagnitude >= JOYSTICK_DEFLECT_THRESHOLD;
            if (dodgeRollEnabled && joystickDeflected && !isDashing) {
                joystickHoldTimer += deltaTime;
                if (joystickHoldTimer >= LONG_PRESS_DURATION && dashCooldown <= 0.0f) {
                    // Lock roll direction to the exact stick vector at the instant threshold is met.
                    dashDirX = moveVecX; dashDirY = moveVecY;
                    dashDurationSetting = JOYSTICK_ROLL_DURATION;
                    isDashing = true; playerIsInvulnerable = true; canShoot = false;
                    dashStartedAt = currentTime; lastDashTriggeredAt = currentTime;
                    dashGhostCount = 0; lastGhostSpawnAt = 0;
                    joystickHoldTimer = 0.0f;
                }
            } else if (!isDashing) {
                joystickHoldTimer = 0.0f; // released or returned to deadzone - charge resets
            }

            // --- Resolve dash end / cooldown start ---
            if (isDashing) {
                float dashElapsed = static_cast<float>(currentTime - dashStartedAt) / 1000.0f;
                if (dashElapsed >= dashDurationSetting) {
                    isDashing = false;
                    playerIsInvulnerable = false;
                    canShoot = true;
                    dashCooldown = DASH_COOLDOWN;
                }
            }

            float vx, vy;
            if (isDashing) {
                // Burst movement at 2.5x walk speed along the locked roll/dash direction.
                vx = dashDirX * WALK_SPEED * DASH_SPEED_MULT;
                vy = dashDirY * WALK_SPEED * DASH_SPEED_MULT;

                // Afterimage ghost trail: sample the player's position a few times per roll.
                if (currentTime - lastGhostSpawnAt >= 30 && dashGhostCount < static_cast<int>(dashGhosts.size())) {
                    dashGhosts[dashGhostCount++] = { playerPosX, playerPosY, currentTime };
                    lastGhostSpawnAt = currentTime;
                }
            } else {
                vx = moveVecX * WALK_SPEED; vy = moveVecY * WALK_SPEED;
                if (state[SDL_SCANCODE_A]) vx = -WALK_SPEED;
                if (state[SDL_SCANCODE_D]) vx =  WALK_SPEED;
                if (state[SDL_SCANCODE_W]) vy = -WALK_SPEED;
                if (state[SDL_SCANCODE_S]) vy =  WALK_SPEED;
            }

            // Tile-map collision checks apply during the roll/dash too - the player
            // cannot phase through solid walls even at burst speed.
            SDL_Rect nPx = player; nPx.x += static_cast<int>(vx);
            if (!check_wall_collision(nPx)) { playerPosX += vx; player.x = static_cast<int>(playerPosX); }
            else if (isDashing) { isDashing = false; playerIsInvulnerable = false; canShoot = true; dashCooldown = DASH_COOLDOWN; } // roll into a wall ends it early
            SDL_Rect nPy = player; nPy.y += static_cast<int>(vy);
            if (!check_wall_collision(nPy)) { playerPosY += vy; player.y = static_cast<int>(playerPosY); }

            float pCx = playerPosX + player.w / 2.0f;
            float pCy = playerPosY + player.h / 2.0f;
            Uint32 curCooldown = WEAPON_PROPS[static_cast<int>(playerWeapon)].cooldownMs;

            // Firing is disabled entirely while dashing/rolling.
            if (canShoot && currentTime - lastShootTime >= curCooldown) {
                bool shootUp    = state[SDL_SCANCODE_UP]    || dpadUpPressed || dpadUpLeftPressed || dpadUpRightPressed;
                bool shootDown  = state[SDL_SCANCODE_DOWN]  || dpadDownPressed || dpadDownLeftPressed || dpadDownRightPressed;
                bool shootLeft  = state[SDL_SCANCODE_LEFT]  || dpadLeftPressed || dpadUpLeftPressed || dpadDownLeftPressed;
                bool shootRight = state[SDL_SCANCODE_RIGHT] || dpadRightPressed || dpadUpRightPressed || dpadDownRightPressed;

                // 8-directional aiming: combine held direction keys into a single
                // (possibly diagonal) vector, e.g. UP+RIGHT -> up-right at 45 degrees.
                float aimX = 0.0f, aimY = 0.0f;
                if (shootUp)    aimY -= 1.0f;
                if (shootDown)  aimY += 1.0f;
                if (shootLeft)  aimX -= 1.0f;
                if (shootRight) aimX += 1.0f;

                if (aimX != 0.0f || aimY != 0.0f) {
                    spawn_vector_bullet(pCx, pCy, aimX, aimY, true, playerWeapon);
                    lastShootTime = currentTime;
                }
            }

            for (auto& hp : healthPacks) {
                if (hp.active && SDL_HasIntersection(&player, &hp.rect)) {
                    hp.active = false; 
                    playerHp = playerMaxHp;
                }
            }

            if (selectedMode == GameMode::TACTICAL) {
                if (tacticalBomb.planted && !tacticalBomb.defused) {
                    bool nearBomb = SDL_HasIntersection(&player, &tacticalBomb.rect);
                    if (defuseHeld && nearBomb) {
                        if (defuseStartTime == 0) defuseStartTime = currentTime;
                        if (currentTime - defuseStartTime >= DEFUSE_TIME_MS) {
                            tacticalBomb.defused = true;
                            botDefuserIndex = -1;
                            playerBombDefuses++;
                            defuseHeld = false;
                            defuseStartTime = 0;
                            score += 100;
                            currentGameState = GameState::GAME_WON;
                        }
                    } else {
                        // Leaving the bomb or releasing the button resets the hold timer.
                        defuseStartTime = 0;
                    }

                    // Fuse tick: beep interval and pitch both scale with remaining time
                    // percentage (not a fixed second count), so a longer fuse from a
                    // bigger enemy squad still sounds calm early and urgent late.
                    Uint32 fuseElapsed = currentTime - tacticalBomb.plantTime;
                    if (fuseElapsed < tacticalBomb.fuseDuration) {
                        float remainingFrac = 1.0f - static_cast<float>(fuseElapsed) / static_cast<float>(tacticalBomb.fuseDuration);
                        Uint32 beepInterval = static_cast<Uint32>(120 + remainingFrac * remainingFrac * 680); // ~800ms -> ~120ms
                        if (currentTime - lastFuseBeepTime >= beepInterval) {
                            float beepFreq = 600.0f + (1.0f - remainingFrac) * (1.0f - remainingFrac) * 700.0f; // 600Hz -> 1300Hz
                            play_fuse_beep(beepFreq, 0.6f);
                            lastFuseBeepTime = currentTime;
                        }
                    }

                    if (!tacticalBomb.defused && currentTime - tacticalBomb.plantTime >= tacticalBomb.fuseDuration) {
                        trigger_explosion(tacticalBomb.x, tacticalBomb.y, 500.0f, 200, player, playerHp);
                        currentGameState = GameState::GAME_OVER;
                    }
                }
            }

            bool allEnemiesDefeated = true;
            for (int i = 0; i < activeEnemyCount; ++i) {
                if (enemies[i].active && enemies[i].hp > 0) {
                    allEnemiesDefeated = false;
                    break;
                }
            }

            // TACTICAL: if every enemy is eliminated before the bomb is ever planted,
            // there's no one left to plant it - mission accomplished by attrition.
            // (Once the bomb IS planted, the mission is won only by defusing it, even
            // if the last enemy standing dies to the fuse or a stray shot.)
            if (selectedMode == GameMode::TACTICAL && allEnemiesDefeated && !tacticalBomb.planted && !tacticalBomb.defused) {
                currentGameState = GameState::GAME_WON;
            }

            // Wave progression only applies to ENDLESS mode. In TACTICAL mode, if all enemies
            // are a one-time fixed squad chosen at mission config - once they're dead,
            // they stay dead; the mission is won by defusing the bomb, not by attrition.
            if (allEnemiesDefeated && selectedMode == GameMode::ENDLESS) {
                currentLevel++;
                activeEnemyCount = std::min(3 + currentLevel, MAX_ENEMIES);

                for (int i = 0; i < activeEnemyCount; ++i) {
                    spawn_single_enemy(enemies[i], i);
                }
                
                highScore = std::max(highScore, score);
            }

            for (int i = 0; i < activeEnemyCount; ++i) {
                auto& enemy = enemies[i];
                if (!enemy.active) continue;

                // Electric shield: any enemy that touches the player while it's up is instantly fried.
                if (playerShieldActive && SDL_HasIntersection(&enemy.rect, &player)) {
                    enemy.hp = 0;
                    enemy.active = false;
                    enemy.deaths = 1;
                    score += 10;
                    highScore = std::max(highScore, score);
                    continue;
                }

                // Any living enemy can plant the bomb. There is no dedicated carrier.
                if (selectedMode == GameMode::TACTICAL && !tacticalBomb.planted) {
                    Point enemyTile{static_cast<int>(enemy.x + enemy.rect.w / 2) / TILE_SIZE,
                                    static_cast<int>(enemy.y + enemy.rect.h / 2) / TILE_SIZE};
                    Point chosenSite = selectedBombSite;
                    tacticalBomb.x = chosenSite.x * TILE_SIZE + 8;
                    tacticalBomb.y = chosenSite.y * TILE_SIZE + 8;
                    tacticalBomb.rect = {static_cast<int>(tacticalBomb.x), static_cast<int>(tacticalBomb.y), 35, 35};
                    // Plant as soon as the bomber is close enough.  A single
                    // surviving RED enemy must never wait for the rest of the
                    // squad or get stuck trying to stand on the exact site tile.
                    // The 2-tile fallback also makes narrow/cornered custom maps
                    // playable when the enemy can reach the site area but not its
                    // exact 60x60 tile footprint.
                    int plantRange = (count_active_enemies() <= 1) ? 2 : 1;
                    if (std::abs(enemyTile.x-chosenSite.x) <= plantRange &&
                        std::abs(enemyTile.y-chosenSite.y) <= plantRange) {
                        trigger_bomb_plant(tacticalBomb, currentTime, i);
                    }
                }

                // While the player is actively holding the defuse, every living enemy
                // goes on high alert: they path straight for the bomb site instead of
                // the player's general position, move faster, and fire more often and
                // with relaxed aim - so they'll actually rush in and try to stop the
                // defuse rather than sit back waiting for a lucky alignment.
                bool defuseInProgress = selectedMode == GameMode::TACTICAL && tacticalBomb.planted &&
                                        !tacticalBomb.defused && defuseHeld;

                if (currentTime - enemy.lastPathCalc >= AI_PATH_INTERVAL_MS) {
                    Point aiTile{ static_cast<int>(enemy.x + enemy.rect.w / 2) / TILE_SIZE, static_cast<int>(enemy.y + enemy.rect.h / 2) / TILE_SIZE };
                    Point targetTile;
                    if (selectedMode == GameMode::TACTICAL && !tacticalBomb.planted) {
                        // Every enemy is a potential planter, so the squad converges on the selected site.
                        targetTile = selectedBombSite;
                    } else if (defuseInProgress) {
                        // Bomb is planted and being defused right now - converge on it.
                        targetTile = selectedBombSite;
                    } else {
                        targetTile = Point{ static_cast<int>(playerPosX + player.w / 2) / TILE_SIZE,
                                            static_cast<int>(playerPosY + player.h / 2) / TILE_SIZE };
                    }
                    enemy.nextTile = get_next_ai_step(aiTile, targetTile);
                    enemy.lastPathCalc = currentTime;
                }

                float aiSpeed = tactical_enemy_speed(defuseInProgress);
                // Aim for the centre of the next tile.  More importantly, move
                // one axis at a time and test the complete 60x60 hitbox before
                // committing the move.  This lets the AI slide around walls
                // instead of freezing when one component of a diagonal move is
                // blocked.
                float aiDx = (enemy.nextTile.x * TILE_SIZE + 5) - enemy.x;
                float aiDy = (enemy.nextTile.y * TILE_SIZE + 5) - enemy.y;
                float dist = std::sqrt(aiDx * aiDx + aiDy * aiDy);
                if (dist > 1.0f) {
                    float aiVx = (aiDx / dist) * aiSpeed, aiVy = (aiDy / dist) * aiSpeed;
                    SDL_Rect nAiX = enemy.rect; nAiX.x += static_cast<int>(aiVx);
                    bool blockedX = check_wall_collision(nAiX);
                    if (!blockedX) { enemy.x += aiVx; enemy.rect.x = static_cast<int>(enemy.x); }
                    else { enemy.lastPathCalc = 0; }
                    SDL_Rect nAiY = enemy.rect; nAiY.y += static_cast<int>(aiVy);
                    bool blockedY = check_wall_collision(nAiY);
                    if (!blockedY) { enemy.y += aiVy; enemy.rect.y = static_cast<int>(enemy.y); }
                    else { enemy.lastPathCalc = 0; }

                    // If the chosen step is blocked, immediately invalidate it so
                    // the next path calculation can choose a different route.
                    // Without this, the AI could keep the same bad nextTile for
                    // 250ms at a time and look frozen against a wall.
                    if (blockedX && blockedY) enemy.lastPathCalc = 0;

                    // Glass now blocks movement, but pathing doesn't route around every
                    // pane - if the blocker directly ahead is glass, shoot it open instead
                    // of stalling in place. Without this an enemy can get permanently
                    // stuck behind glass between it and the bomb site or the player.
                    if ((blockedX || blockedY) && currentTime - enemy.lastGlassBreakTime >= 260) {
                        int gc = -1, gr = -1;
                        bool blockedByGlass = (blockedX && rect_hits_glass(nAiX, &gc, &gr)) ||
                                              (blockedY && rect_hits_glass(nAiY, &gc, &gr));
                        if (blockedByGlass) {
                            float ecx = enemy.x + enemy.rect.w / 2.0f, ecy = enemy.y + enemy.rect.h / 2.0f;
                            float gdx = blockedX ? (aiVx > 0.0f ? 1.0f : -1.0f) : 0.0f;
                            float gdy = blockedY ? (aiVy > 0.0f ? 1.0f : -1.0f) : 0.0f;
                            spawn_vector_bullet(ecx, ecy, gdx, gdy, false, enemy.weaponType, &player, enemy.enemyType, i);
                            enemy.lastGlassBreakTime = currentTime;
                        }
                    }
                }

                Uint32 enemyShootCooldown = std::max(180U, WEAPON_PROPS[static_cast<int>(enemy.weaponType)].cooldownMs * tactical_enemy_shoot_multiplier(defuseInProgress));
                if (currentTime - enemy.lastShootTime >= enemyShootCooldown) {
                    float aiCx = enemy.x + enemy.rect.w / 2.0f, aiCy = enemy.y + enemy.rect.h / 2.0f;
                    float dx = pCx - aiCx, dy = pCy - aiCy;

                    bool cardinalAligned = std::abs(dx) <= CARDINAL_ALIGN_TOLERANCE || std::abs(dy) <= CARDINAL_ALIGN_TOLERANCE;
                    // Diagonal alignment: |dx| and |dy| within tolerance of each other (both nonzero).
                    float largerAxis = std::max(std::abs(dx), std::abs(dy));
                    bool diagonalAligned = largerAxis > 0.0001f &&
                        std::abs(std::abs(dx) - std::abs(dy)) <= largerAxis * DIAGONAL_ALIGN_TOLERANCE;

                    // Difficulty-aware aiming. NORMAL must obey the same basic aiming
                    // restrictions as the player: RED/cardinal enemies cannot free-aim,
                    // and PINK enemies may use the 8 cardinal/diagonal directions.
                    // Defusing raises alertness, but does NOT grant free aim on NORMAL.
                    bool normalMode = (selectedMode == GameMode::TACTICAL &&
                                       tacticalEnemyDifficulty == EnemyDifficulty::NORMAL);
                    bool rifleAim = (enemy.weaponType == WeaponType::RIFLE) && !normalMode;
                    bool defuseFreeAim = defuseInProgress && !normalMode;
                    bool canFire = cardinalAligned ||
                                   (enemy.enemyType == EnemyType::PINK && diagonalAligned) ||
                                   rifleAim || defuseFreeAim;

                    if (canFire) {
                        if (cardinalAligned) {
                            Direction sDir = (std::abs(dx) > std::abs(dy)) ? ((dx > 0) ? Direction::DIR_RIGHT : Direction::DIR_LEFT)
                                                                           : ((dy > 0) ? Direction::DIR_DOWN : Direction::DIR_UP);
                            spawn_cardinal_bullet(aiCx, aiCy, sDir, false, enemy.weaponType, &player, enemy.enemyType, i);
                        } else {
                            // Pink enemy diagonal shot, rifle track, or defuse-interrupt shot:
                            // continuous normalized 2D velocity vector straight at the player.
                            spawn_vector_bullet(aiCx, aiCy, dx, dy, false, enemy.weaponType, &player, enemy.enemyType, i);
                        }
                        enemy.lastShootTime = currentTime;
                    }
                }
            }

            // --- Ally Sentinel squad AI -------------------------------------------------
            // Sentinels behave like a team now instead of independent turret-like bots:
            //   * when there is no immediate threat, they accompany the player;
            //   * when Raiders are nearby, they spread out and engage them;
            //   * after a bomb is planted, the nearest surviving Sentinel becomes the
            //     designated defuser while the others protect the bomb/defuser;
            //   * a bot holds the bomb for the same 3 seconds as the player;
            //   * bot bomb defuses are recorded on the Sentinel leaderboard.
            {
                std::array<int, MAX_ENEMIES> targetClaims{};
                targetClaims.fill(0);

                const bool bombActive = selectedMode == GameMode::TACTICAL &&
                                        tacticalBomb.planted && !tacticalBomb.defused;

                // Only one Sentinel should actively work the bomb. If the player is
                // already defusing it, the bots become the security team instead.
                int designatedDefuser = -1;
                if (bombActive && !defuseHeld) {
                    // Keep the same bot assigned while it is alive. This prevents the
                    // defuse timer from constantly resetting because another bot walks
                    // slightly closer to the bomb. If the assigned bot dies, choose a
                    // replacement once.
                    if (botDefuserIndex >= 0 && botDefuserIndex < activeBotCount &&
                        bots[botDefuserIndex].active && bots[botDefuserIndex].hp > 0) {
                        designatedDefuser = botDefuserIndex;
                    } else {
                        float bestBombDist = 1e18f;
                        float bcx = tacticalBomb.x + tacticalBomb.rect.w / 2.0f;
                        float bcy = tacticalBomb.y + tacticalBomb.rect.h / 2.0f;
                        for (int bi = 0; bi < activeBotCount; ++bi) {
                            const Bot& bot = bots[bi];
                            if (!bot.active || bot.hp <= 0) continue;
                            float dx = bcx - (bot.x + bot.rect.w / 2.0f);
                            float dy = bcy - (bot.y + bot.rect.h / 2.0f);
                            float d2 = dx * dx + dy * dy;
                            if (d2 < bestBombDist) {
                                bestBombDist = d2;
                                designatedDefuser = bi;
                            }
                        }
                        botDefuserIndex = designatedDefuser;
                    }
                }

                auto move_bot_toward = [&](Bot& bot, int botIndex, Point destination, float stopDistance) {
                    Point botTile{ static_cast<int>(bot.x + bot.rect.w / 2) / TILE_SIZE,
                                   static_cast<int>(bot.y + bot.rect.h / 2) / TILE_SIZE };
                    if (currentTime - bot.lastPathCalc >= AI_PATH_INTERVAL_MS) {
                        bot.nextTile = get_next_ai_step(botTile, destination);
                        bot.lastPathCalc = currentTime;
                    }

                    float dx = (bot.nextTile.x * TILE_SIZE + 5) - bot.x;
                    float dy = (bot.nextTile.y * TILE_SIZE + 5) - bot.y;
                    float dist = std::sqrt(dx * dx + dy * dy);
                    if (dist <= 1.0f) return;

                    // Keep formation/fighting distances from the destination. For the
                    // actual bomb defuser this is effectively disabled by passing 0.
                    if (stopDistance > 0.0f) {
                        float destCx = destination.x * TILE_SIZE + TILE_SIZE / 2.0f;
                        float destCy = destination.y * TILE_SIZE + TILE_SIZE / 2.0f;
                        float curCx = bot.x + bot.rect.w / 2.0f;
                        float curCy = bot.y + bot.rect.h / 2.0f;
                        float realDist = std::sqrt((destCx-curCx)*(destCx-curCx) + (destCy-curCy)*(destCy-curCy));
                        if (realDist <= stopDistance) return;
                    }

                    float spd = bot_speed(bot.tier) * bot.speedJitter;
                    float bvx = (dx / dist) * spd, bvy = (dy / dist) * spd;
                    SDL_Rect nBx = bot.rect; nBx.x += static_cast<int>(bvx);
                    bool blockedX = check_wall_collision(nBx);
                    if (!blockedX) { bot.x += bvx; bot.rect.x = static_cast<int>(bot.x); }
                    else bot.lastPathCalc = 0;
                    SDL_Rect nBy = bot.rect; nBy.y += static_cast<int>(bvy);
                    bool blockedY = check_wall_collision(nBy);
                    if (!blockedY) { bot.y += bvy; bot.rect.y = static_cast<int>(bot.y); }
                    else bot.lastPathCalc = 0;

                    // Break a glass pane directly blocking a Sentinel's route rather
                    // than repeatedly walking into it.
                    if ((blockedX || blockedY) && currentTime - bot.lastGlassBreakTime >= 260) {
                        int gc = -1, gr = -1;
                        bool blockedByGlass = (blockedX && rect_hits_glass(nBx, &gc, &gr)) ||
                                              (blockedY && rect_hits_glass(nBy, &gc, &gr));
                        if (blockedByGlass) {
                            float bcx = bot.x + bot.rect.w / 2.0f, bcy = bot.y + bot.rect.h / 2.0f;
                            float gdx = blockedX ? (bvx > 0.0f ? 1.0f : -1.0f) : 0.0f;
                            float gdy = blockedY ? (bvy > 0.0f ? 1.0f : -1.0f) : 0.0f;
                            spawn_vector_bullet(bcx, bcy, gdx, gdy, true, bot.weaponType, nullptr, EnemyType::RED, botIndex);
                            bot.lastGlassBreakTime = currentTime;
                        }
                    }
                };

                // Find nearby Raiders once for target selection. Bots still de-conflict
                // their targets so the whole squad does not pile onto one Raider.
                for (int bi = 0; bi < activeBotCount; ++bi) {
                    Bot& bot = bots[bi];
                    if (!bot.active || bot.hp <= 0) continue;

                    float botCx = bot.x + bot.rect.w / 2.0f;
                    float botCy = bot.y + bot.rect.h / 2.0f;

                    int nearestEnemy = -1;
                    float nearestEnemyDist = 1e18f;
                    for (int ei = 0; ei < activeEnemyCount; ++ei) {
                        const Enemy& enemy = enemies[ei];
                        if (!enemy.active || enemy.hp <= 0) continue;
                        float ecx = enemy.x + enemy.rect.w / 2.0f;
                        float ecy = enemy.y + enemy.rect.h / 2.0f;
                        float d = std::sqrt((ecx-botCx)*(ecx-botCx) + (ecy-botCy)*(ecy-botCy));
                        float weighted = d + targetClaims[ei] * static_cast<float>(TILE_SIZE) * 3.0f;
                        if (weighted < nearestEnemyDist) {
                            nearestEnemyDist = weighted;
                            nearestEnemy = ei;
                        }
                    }
                    if (nearestEnemy >= 0) targetClaims[nearestEnemy]++;

                    const bool isDefuser = bombActive && !defuseHeld && bi == designatedDefuser;
                    Point movementTarget{ static_cast<int>(playerPosX + player.w / 2) / TILE_SIZE,
                                          static_cast<int>(playerPosY + player.h / 2) / TILE_SIZE };
                    float stopDistance = 150.0f;
                    int combatTarget = -1;

                    if (isDefuser) {
                        // The designated Sentinel has a clear, overriding objective:
                        // reach the bomb and defuse it.
                        Point bombTile{ static_cast<int>(tacticalBomb.x + tacticalBomb.rect.w / 2) / TILE_SIZE,
                                        static_cast<int>(tacticalBomb.y + tacticalBomb.rect.h / 2) / TILE_SIZE };
                        movementTarget = bombTile;
                        stopDistance = 0.0f;
                    } else if (nearestEnemy >= 0 && nearestEnemyDist < 760.0f) {
                        // Immediate combat takes priority over following formation.
                        combatTarget = nearestEnemy;
                        const Enemy& target = enemies[nearestEnemy];
                        Point targetTile{ static_cast<int>(target.x + target.rect.w / 2) / TILE_SIZE,
                                          static_cast<int>(target.y + target.rect.h / 2) / TILE_SIZE };
                        movementTarget = nearest_free_tile({
                            targetTile.x + static_cast<int>(std::cos(bot.flankAngle) * 3),
                            targetTile.y + static_cast<int>(std::sin(bot.flankAngle) * 3)
                        });
                        stopDistance = 260.0f;
                    } else if (bombActive) {
                        // No nearby Raider: remain around the bomb/player instead of
                        // wandering off to random map locations.
                        Point bombTile{ static_cast<int>(tacticalBomb.x + tacticalBomb.rect.w / 2) / TILE_SIZE,
                                        static_cast<int>(tacticalBomb.y + tacticalBomb.rect.h / 2) / TILE_SIZE };
                        movementTarget = nearest_free_tile({
                            bombTile.x + static_cast<int>(std::cos(bot.flankAngle) * 3),
                            bombTile.y + static_cast<int>(std::sin(bot.flankAngle) * 3)
                        });
                        stopDistance = 180.0f;
                    } else {
                        // No immediate threat: accompany the player in a loose formation.
                        // The golden-angle offset gives each Sentinel its own slot.
                        int radius = 2 + (bi % 3);
                        movementTarget = nearest_free_tile({
                            static_cast<int>(playerPosX + player.w / 2) / TILE_SIZE + static_cast<int>(std::cos(bot.flankAngle) * radius),
                            static_cast<int>(playerPosY + player.h / 2) / TILE_SIZE + static_cast<int>(std::sin(bot.flankAngle) * radius)
                        });
                        stopDistance = 130.0f;
                    }

                    // Once a Sentinel reaches the bomb, stop its movement completely
                    // while defusing. This prevents the old frame-to-frame collision
                    // correction from making the bot visibly jitter/vibrate in place.
                    bool botAlreadyAtBomb = false;
                    if (isDefuser && bombActive && !defuseHeld) {
                        botAlreadyAtBomb = SDL_HasIntersection(&bot.rect, &tacticalBomb.rect);
                    }
                    if (!botAlreadyAtBomb) {
                        move_bot_toward(bot, bi, movementTarget, stopDistance);
                    }

                    // --- Bot bomb defuse -------------------------------------------------
                    if (isDefuser && bombActive && !defuseHeld) {
                        bool nearBomb = SDL_HasIntersection(&bot.rect, &tacticalBomb.rect);
                        if (nearBomb) {
                            bot.defusingBomb = true;
                            if (bot.defuseStartTime == 0) bot.defuseStartTime = currentTime;
                            if (currentTime - bot.defuseStartTime >= DEFUSE_TIME_MS) {
                                tacticalBomb.defused = true;
                                botDefuserIndex = -1;
                                bot.bombDefuses++;
                                bot.defusingBomb = false;
                                bot.defuseStartTime = 0;
                                score += 100;
                                highScore = std::max(highScore, score);
                                currentGameState = GameState::GAME_WON;
                                for (int bj = 0; bj < activeBotCount; ++bj) {
                                    bots[bj].defusingBomb = false;
                                    bots[bj].defuseStartTime = 0;
                                }
                            }
                        } else {
                            bot.defusingBomb = false;
                            bot.defuseStartTime = 0;
                        }
                    } else if (!isDefuser) {
                        bot.defusingBomb = false;
                        bot.defuseStartTime = 0;
                    }

                    // --- Combat ----------------------------------------------------------
                    if (combatTarget >= 0 && enemies[combatTarget].active && enemies[combatTarget].hp > 0) {
                        const Enemy& target = enemies[combatTarget];
                        float ecx = target.x + target.rect.w / 2.0f;
                        float ecy = target.y + target.rect.h / 2.0f;
                        float engageDx = ecx - botCx;
                        float engageDy = ecy - botCy;
                        float engageDist = std::sqrt(engageDx*engageDx + engageDy*engageDy);
                        Uint32 botShootCooldown = std::max(180U,
                            WEAPON_PROPS[static_cast<int>(bot.weaponType)].cooldownMs * bot_shoot_multiplier(bot.tier));
                        if (currentTime - bot.lastShootTime >= botShootCooldown && engageDist < 700.0f) {
                            spawn_vector_bullet(botCx, botCy, engageDx, engageDy, true,
                                                bot.weaponType, nullptr, EnemyType::RED, bi);
                            bot.lastShootTime = currentTime;
                        }
                    }
                }
            }

            for (auto& b : bullets) {
                if (!b.active) continue;
                b.x += b.vx; b.y += b.vy;
                b.rect.x = static_cast<int>(b.x) - b.rect.w / 2;
                b.rect.y = static_cast<int>(b.y) - b.rect.h / 2;

                int glassCol = -1, glassRow = -1;
                bool hitGlass = rect_hits_glass(b.rect, &glassCol, &glassRow);

                // GLASS IS A BREAKABLE OBSTACLE, NOT A PERMANENT WALL.
                // Any projectile that strikes it shatters the glass immediately.
                // The glass tile is removed, so the position becomes passable.
                if (hitGlass) {
                    gameMap[glassRow][glassCol] = 0;

                    for (auto& gs : glassShatters) {
                        if (!gs.active) {
                            gs.x = glassCol * TILE_SIZE + TILE_SIZE / 2.0f;
                            gs.y = glassRow * TILE_SIZE + TILE_SIZE / 2.0f;
                            gs.spawnTime = currentTime;
                            gs.duration = 420;
                            gs.active = true;
                            break;
                        }
                    }

                    play_glass_shatter_sound();
                }

                // Glass is intentionally excluded from wall collision. Once struck,
                // it disappears and does not reflect Vector-Reflect bullets.
                bool hitWall = check_wall_collision(b.rect);
                bool hitPlayer = (!b.isPlayerBullet && SDL_HasIntersection(&b.rect, &player));
                
                bool hitEnemy = false;
                if (b.isPlayerBullet) {
                    for (int i = 0; i < activeEnemyCount; ++i) {
                        auto& enemy = enemies[i];
                        if (enemy.active && enemy.hp > 0 && SDL_HasIntersection(&b.rect, &enemy.rect)) {
                            hitEnemy = true;
                            
                            int baseDamage = WEAPON_PROPS[static_cast<int>(b.type)].damage;
                            if (b.type == WeaponType::VECTOR_REFLECT) {
                                baseDamage = std::max(10, static_cast<int>(baseDamage * std::pow(0.85f, static_cast<float>(b.bounceCount))));
                            }
                            int scaledDamage = baseDamage + (currentLevel * 3);
                            enemy.hp -= scaledDamage;

                            if (enemy.hp <= 0) {
                                enemy.active = false;
                                enemy.deaths = 1;
                                score += 10;
                                highScore = std::max(highScore, score);
                                if (b.ownerBotIndex >= 0 && b.ownerBotIndex < activeBotCount) {
                                    bots[b.ownerBotIndex].kills++;
                                } else {
                                    playerKills++;
                                }
                            }
                            break;
                        }
                    }
                }

                // Raider bullets can strike an ally Sentinel bot the same way they
                // strike the player - bots are real targets, not just decoration.
                int hitBotIndex = -1;
                if (!b.isPlayerBullet) {
                    for (int bi = 0; bi < activeBotCount; ++bi) {
                        if (bots[bi].active && bots[bi].hp > 0 && SDL_HasIntersection(&b.rect, &bots[bi].rect)) {
                            hitBotIndex = bi;
                            break;
                        }
                    }
                }
                bool hitBot = (hitBotIndex >= 0);

                if (b.type == WeaponType::ROCKET && (hitWall || hitGlass || hitPlayer || hitEnemy || hitBot)) {
                    b.active = false;
                    // Pass the rocket's shooter through so a splash kill (not just a
                    // direct hit) is credited on the leaderboard the same way.
                    trigger_explosion(b.x, b.y, 160.0f, WEAPON_PROPS[static_cast<int>(WeaponType::ROCKET)].damage, player, playerHp,
                                       b.isPlayerBullet, b.ownerBotIndex);
                    highScore = std::max(highScore, score);
                    if (playerHp <= 0) currentGameState = GameState::GAME_OVER;
                    continue;
                }

                if (b.type == WeaponType::VECTOR_REFLECT && hitWall && !hitGlass) {
                    SDL_Rect prevRect = b.rect;
                    prevRect.x = static_cast<int>(b.x - b.vx) - b.rect.w / 2;
                    prevRect.y = static_cast<int>(b.y - b.vy) - b.rect.h / 2;
                    bool hitX = check_wall_collision(SDL_Rect{prevRect.x + static_cast<int>(b.vx), prevRect.y, prevRect.w, prevRect.h});
                    bool hitY = check_wall_collision(SDL_Rect{prevRect.x, prevRect.y + static_cast<int>(b.vy), prevRect.w, prevRect.h});
                    if (hitX) b.vx = -b.vx;
                    if (hitY) b.vy = -b.vy;
                    if (!hitX && !hitY) { b.vx = -b.vx; b.vy = -b.vy; }
                    b.bounceCount++;
                    b.x = static_cast<float>(prevRect.x + prevRect.w / 2);
                    b.y = static_cast<float>(prevRect.y + prevRect.h / 2);
                    b.rect.x = static_cast<int>(b.x) - b.rect.w / 2;
                    b.rect.y = static_cast<int>(b.y) - b.rect.h / 2;
                    if (b.bounceCount >= 3) b.active = false;
                    continue;
                }

                if (b.rect.x < 0 || b.rect.x > currentMapCols * TILE_SIZE || b.rect.y < 0 || b.rect.y > currentMapRows * TILE_SIZE || hitWall) {
                    b.active = false; continue;
                }
                if (!b.isPlayerBullet && hitPlayer) {
                    b.active = false; // bullet is stopped either way - shield/I-Frames don't let it through
                    if (!playerShieldActive && !playerIsInvulnerable) {
                        playerHp -= static_cast<int>(WEAPON_PROPS[static_cast<int>(b.type)].damage * tactical_enemy_damage_multiplier());
                        highScore = std::max(highScore, score);
                        if (playerHp <= 0) {
                            currentGameState = GameState::GAME_OVER;
                            playerDeaths++;
                            // b.ownerBotIndex holds the shooting Raider's index for
                            // enemy-fired bullets (same field bots use for their own
                            // kill attribution on player-team shots).
                            if (b.ownerBotIndex >= 0 && b.ownerBotIndex < activeEnemyCount) {
                                enemies[b.ownerBotIndex].kills++;
                            }
                        }
                        // Getting hit while defusing breaks concentration - the timer
                        // resets, so an enemy that lands a shot genuinely stops the defuse
                        // rather than the player being able to tank it and finish anyway.
                        if (defuseHeld) {
                            defuseHeld = false;
                            defuseStartTime = 0;
                        }
                    }
                }
                if (!b.isPlayerBullet && hitBot) {
                    b.active = false;
                    Bot& bot = bots[hitBotIndex];
                    bot.hp -= static_cast<int>(WEAPON_PROPS[static_cast<int>(b.type)].damage * tactical_enemy_damage_multiplier());
                    if (bot.defusingBomb) {
                        bot.defusingBomb = false;
                        bot.defuseStartTime = 0;
                    }
                    if (bot.hp <= 0) {
                        bot.active = false;
                        bot.deaths++;
                        bot.defusingBomb = false;
                        bot.defuseStartTime = 0;
                        if (botDefuserIndex == hitBotIndex) botDefuserIndex = -1;
                        if (b.ownerBotIndex >= 0 && b.ownerBotIndex < activeEnemyCount) {
                            enemies[b.ownerBotIndex].kills++;
                        }
                    }
                }
                if (b.isPlayerBullet && hitEnemy) {
                    b.active = false;
                }
            }
        }

        update_camera(playerPosX, playerPosY);

        SDL_SetRenderDrawColor(renderer, 20, 20, 25, 255);
        SDL_RenderClear(renderer);

        int firstCol = std::max(0, cameraX / TILE_SIZE);
        int firstRow = std::max(0, cameraY / TILE_SIZE);
        int lastCol = std::min(currentMapCols - 1, (cameraX + SCREEN_WIDTH) / TILE_SIZE + 1);
        int lastRow = std::min(currentMapRows - 1, (cameraY + SCREEN_HEIGHT) / TILE_SIZE + 1);

SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
for (int r = firstRow; r <= lastRow; ++r) {
    for (int c = firstCol; c <= lastCol; ++c) { 
        SDL_Rect tile = {c * TILE_SIZE - cameraX, r * TILE_SIZE - cameraY, TILE_SIZE, TILE_SIZE};
        const int val = gameMap[r][c];
        if (val == 1)      SDL_SetRenderDrawColor(renderer, 50, 55, 65, 255);
        else if (val == 8 || val == 9) {
            SDL_Color paletteColor = editor_palette_color(val);
            SDL_SetRenderDrawColor(renderer, paletteColor.r, paletteColor.g, paletteColor.b, paletteColor.a);
        }
        else if (val == 10) SDL_SetRenderDrawColor(renderer, 120, 210, 240, 95);
        else               SDL_SetRenderDrawColor(renderer, 15, 18, 22, 255);
        SDL_RenderFillRect(renderer, &tile);
        if (val == 10) {
            SDL_SetRenderDrawColor(renderer, 190, 240, 255, 220);
            draw_rect_outline(renderer, tile);
            SDL_RenderDrawLine(renderer, tile.x + 5, tile.y + 5, tile.x + TILE_SIZE - 5, tile.y + TILE_SIZE - 5);
            SDL_RenderDrawLine(renderer, tile.x + TILE_SIZE - 5, tile.y + 5, tile.x + 5, tile.y + TILE_SIZE - 5);
        } else if (val != 0 && selectedMode != GameMode::TACTICAL) {
            SDL_SetRenderDrawColor(renderer, 35, 40, 50, 255);
            draw_rect_outline(renderer, tile);
        }
    }
}

        // World objects are rendered in world coordinates through the camera.
        SDL_RenderSetViewport(renderer, nullptr);
        // Keep the renderer's 2000x1200 logical coordinate system for all HUD
        // and editor controls.
        // Draw world-space objects manually offset by camera.
        if (selectedMode == GameMode::TACTICAL && !tacticalBomb.defused) {
            SDL_Rect bombScreen = tacticalBomb.rect;
            bombScreen.x -= cameraX; bombScreen.y -= cameraY;
            render_bomb_entity(renderer, tacticalBomb, bombScreen, currentTime);
        }

        if (selectedMode == GameMode::TACTICAL) {
            auto draw_site = [&](Point site, char label, SDL_Color col) {
                if (site.x < 0 || site.y < 0) return;
                SDL_Rect sr{site.x*TILE_SIZE-cameraX+5,site.y*TILE_SIZE-cameraY+5,TILE_SIZE-10,TILE_SIZE-10};
                SDL_SetRenderDrawColor(renderer,col.r,col.g,col.b,150); SDL_RenderFillRect(renderer,&sr);
                SDL_SetRenderDrawColor(renderer,255,255,255,255); draw_rect_outline(renderer, sr);
                draw_text(renderer,std::string(1,label),sr.x+18,sr.y+13,2,{255,255,255,255});
            };
            if (bombSiteAValid) draw_site(bombSiteA,'A',{255,150,70,255});
            if (bombSiteBValid) draw_site(bombSiteB,'B',{255,150,70,255});
        }
        for (const auto& hp : healthPacks) render_health_pack(renderer, hp);
        for (const auto& b : bullets) if (b.active) render_bullet(renderer, b);

        render_explosions(renderer, currentTime);
        render_glass_shatters(renderer, currentTime);

        for (auto& beam : laserBeams) {
            if (!beam.active) continue;
            if (currentTime - beam.spawnTime < beam.duration) {
                SDL_SetRenderDrawColor(renderer, beam.color.r, beam.color.g, beam.color.b, 255);
                SDL_RenderDrawLine(renderer, beam.start.x - cameraX, beam.start.y - cameraY, beam.end.x - cameraX, beam.end.y - cameraY);
            } else {
                beam.active = false;
            }
        }

        for (int i = 0; i < activeEnemyCount; ++i) {
            const auto& enemy = enemies[i];
            if (enemy.active && enemy.hp > 0) {
                SDL_Color eColor = (enemy.enemyType == EnemyType::PINK) ? SDL_Color{255, 105, 180, 255} : SDL_Color{220, 60, 60, 255};
                SDL_SetRenderDrawColor(renderer, eColor.r, eColor.g, eColor.b, 255);
                SDL_Rect enemyScreen = enemy.rect;
                enemyScreen.x -= cameraX; enemyScreen.y -= cameraY;
                SDL_RenderFillRect(renderer, &enemyScreen);
                render_health_bar(renderer, enemyScreen.x, enemyScreen.y - 12, enemyScreen.w, 8, enemy.hp, enemy.maxHp, eColor);
            }
        }

        // Ally Sentinel bots use the same green visual identity as the player.
        // This makes every friendly combatant immediately readable as Sentinel.
        for (int i = 0; i < activeBotCount; ++i) {
            const auto& bot = bots[i];
            if (bot.active && bot.hp > 0) {
                SDL_Color bColor{60, 220, 100, 255};
                SDL_SetRenderDrawColor(renderer, bColor.r, bColor.g, bColor.b, 255);
                SDL_Rect botScreen = bot.rect;
                botScreen.x -= cameraX; botScreen.y -= cameraY;
                SDL_RenderFillRect(renderer, &botScreen);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                draw_rect_outline(renderer, botScreen);
                render_health_bar(renderer, botScreen.x, botScreen.y - 12, botScreen.w, 8, bot.hp, bot.maxHp, bColor);
                draw_text(renderer, bot.name, centered_text_x(bot.name, 1, botScreen.x, botScreen.w),
                          botScreen.y - 24, 1, bColor);
                if (bot.defusingBomb) {
                    draw_text(renderer, "DEFUSING", centered_text_x("DEFUSING", 1, botScreen.x, botScreen.w),
                              botScreen.y + botScreen.h + 4, 1, {255, 220, 80, 255});
                }
            }
        }

        // Afterimage ghost trail: fading silhouettes along the roll/dash path,
        // confirming the long-press/double-tap threshold was met.
        if (dashGhostCount > 0) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            for (int i = 0; i < dashGhostCount; ++i) {
                Uint32 age = currentTime - dashGhosts[i].t;
                if (age > 220) continue;
                float fade = 1.0f - static_cast<float>(age) / 220.0f;
                SDL_Rect ghost = { static_cast<int>(dashGhosts[i].x) - cameraX, static_cast<int>(dashGhosts[i].y) - cameraY, player.w, player.h };
                SDL_SetRenderDrawColor(renderer, 140, 220, 255, static_cast<Uint8>(140 * fade));
                SDL_RenderFillRect(renderer, &ghost);
            }
        }

        // Roll/dash player tint: flashes bright white-cyan to confirm the trigger fired,
        // fading back to normal green shortly after.
        SDL_Color playerTint{60, 220, 100, 255};
        if (isDashing || (currentTime - lastDashTriggeredAt < 180)) {
            float flash = isDashing ? 1.0f : (1.0f - static_cast<float>(currentTime - lastDashTriggeredAt) / 180.0f);
            playerTint.r = static_cast<Uint8>(60 + (255 - 60) * flash);
            playerTint.g = static_cast<Uint8>(220 + (255 - 220) * flash);
            playerTint.b = static_cast<Uint8>(100 + (255 - 100) * flash);
        }
        SDL_SetRenderDrawColor(renderer, playerTint.r, playerTint.g, playerTint.b, playerTint.a);
        SDL_Rect playerScreen = player;
        playerScreen.x -= cameraX; playerScreen.y -= cameraY;
        SDL_RenderFillRect(renderer, &playerScreen);

        if (playerShieldActive) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            Uint32 shieldElapsed = currentTime - playerShieldActivatedAt;
            // Fast pulse so the shield reads as "electric" rather than a static outline.
            float pulse = 0.6f + 0.4f * std::sin(static_cast<float>(shieldElapsed) * 0.02f);
            for (int ring = 0; ring < 3; ++ring) {
                int pad = 10 + ring * 6;
                SDL_Rect glow = { playerScreen.x - pad, playerScreen.y - pad, playerScreen.w + pad * 2, playerScreen.h + pad * 2 };
                Uint8 alpha = static_cast<Uint8>(std::max(0.0f, (150 - ring * 45) * pulse));
                SDL_SetRenderDrawColor(renderer, 90, 200, 255, alpha);
                draw_rect_outline(renderer, glow);
            }
        }

        // --- Unified top title bar ------------------------------------------------
        // Clean order:
        // [Health] [Weapon] [Bomb Fuse] [Mode] [Round] [Score] [High Score] [RANKS] [X]
        SDL_Rect titleBar{15, 10, SCREEN_WIDTH - 30, 62};
        SDL_SetRenderDrawColor(renderer, 7, 15, 28, 245);
        SDL_RenderFillRect(renderer, &titleBar);
        SDL_SetRenderDrawColor(renderer, 55, 150, 205, 255);
        draw_rect_outline(renderer, titleBar, 2);

        // 1. HEALTH
        render_health_bar(renderer, 25, 29, 220, 22, playerHp, playerMaxHp,
                          {60, 220, 120, 255});

        // 2. WEAPON
        draw_text(renderer, WEAPON_PROPS[static_cast<int>(playerWeapon)].name,
                  265, 31, 2, WEAPON_PROPS[static_cast<int>(playerWeapon)].color);

        // 3. BOMB FUSE
        // Keep the fuse beside the weapon so the round-critical state is easy
        // to scan without searching the rest of the title bar.
        SDL_Rect fuseBox{475, 20, 170, 42};
        std::string fuseText = "FUSE: --.-S";
        Uint32 remainingMs = 0;
        float fuseFrac = 0.0f;
        bool fuseActive = selectedMode == GameMode::TACTICAL &&
                          tacticalBomb.planted && !tacticalBomb.defused;
        if (fuseActive) {
            Uint32 fuseElapsed = currentTime - tacticalBomb.plantTime;
            remainingMs = (fuseElapsed < tacticalBomb.fuseDuration)
                ? (tacticalBomb.fuseDuration - fuseElapsed) : 0;
            int wholeSec = static_cast<int>(remainingMs / 1000);
            int tenths = static_cast<int>((remainingMs % 1000) / 100);
            fuseText = "FUSE: " + std::to_string(wholeSec) + "." +
                       std::to_string(tenths) + "S";
            fuseFrac = static_cast<float>(remainingMs) /
                       static_cast<float>(std::max<Uint32>(1, tacticalBomb.fuseDuration));
        }

        if (selectedMode == GameMode::TACTICAL) {
            SDL_SetRenderDrawColor(renderer, fuseActive ? 65 : 25,
                                   fuseActive ? 25 : 35,
                                   fuseActive ? 30 : 45, 235);
            SDL_RenderFillRect(renderer, &fuseBox);
            SDL_SetRenderDrawColor(renderer, fuseActive ? 255 : 90,
                                   fuseActive ? 70 : 170,
                                   fuseActive ? 70 : 210, 255);
            draw_rect_outline(renderer, fuseBox, 2);

            SDL_Rect fuseBarBg{fuseBox.x + 8, fuseBox.y + 31, fuseBox.w - 16, 5};
            SDL_SetRenderDrawColor(renderer, 35, 35, 40, 255);
            SDL_RenderFillRect(renderer, &fuseBarBg);
            if (fuseActive) {
                SDL_Rect fuseBar{fuseBarBg.x + 1, fuseBarBg.y + 1,
                                 std::max(1, static_cast<int>((fuseBarBg.w - 2) * fuseFrac)), 3};
                SDL_SetRenderDrawColor(renderer, 255, 70, 70, 255);
                SDL_RenderFillRect(renderer, &fuseBar);
            }

            draw_text(renderer, fuseText,
                      centered_text_x(fuseText, 2, fuseBox.x, fuseBox.w),
                      fuseBox.y + 7, 2,
                      fuseActive ? SDL_Color{255, 110, 110, 255} : SDL_Color{130, 180, 210, 255});
        }

        // 4. MODE
        draw_text(renderer, "MODE: " +
                  std::string(selectedMode == GameMode::ENDLESS ? "ENDLESS" : "TACTICAL"),
                  675, 31, 2, {100, 220, 255, 255});

        // 5. ROUND
        draw_text(renderer, "ROUND: " + std::to_string(currentRound) + "/" +
                  std::to_string(MAX_TACTICAL_ROUNDS),
                  895, 31, 2, {100, 220, 255, 255});

        // 6. SCORE
        draw_text(renderer, "SCORE: " + std::to_string(score),
                  1085, 31, 2, {240, 200, 80, 255});

        // 7. HIGH SCORE
        draw_text(renderer, "HIGH SCORE: " + std::to_string(highScore),
                  1280, 31, 2, {250, 160, 60, 255});

        // Blueprint-style separators.
        SDL_SetRenderDrawColor(renderer, 45, 95, 125, 180);
        for (int x : {250, 465, 660, 875, 1065, 1260, 1605, 1805})
            SDL_RenderDrawLine(renderer, x, titleBar.y + 10,
                               x, titleBar.y + titleBar.h - 10);

        // 8. RANKS
        SDL_SetRenderDrawColor(renderer,
                               showLeaderboardPanel ? 40 : 55,
                               showLeaderboardPanel ? 150 : 80,
                               showLeaderboardPanel ? 210 : 100, 235);
        SDL_RenderFillRect(renderer, &btnLeaderboard);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        draw_rect_outline(renderer, btnLeaderboard);
        draw_text(renderer, "RANKS",
                  centered_text_x("RANKS", 2, btnLeaderboard.x, btnLeaderboard.w),
                  btnLeaderboard.y + 8, 2, {255, 255, 255, 255});

        // 9. CLOSE — small square X in the title bar.
        render_back_button(renderer, btnPlayingBack);

        // Fixed left-side map box.  The weapon wheel begins far below it.
        if (currentGameState == GameState::PLAYING && !showLeaderboardPanel) {
            render_live_minimap(renderer, SDL_Rect{20, 95, 320, 255}, playerPosX, playerPosY, moveVecX, moveVecY);
        }

        if (selectedMode == GameMode::TACTICAL && tacticalBomb.planted && !tacticalBomb.defused) {
            bool nearBomb = SDL_HasIntersection(&player, &tacticalBomb.rect);
            if (nearBomb) {
                SDL_SetRenderDrawColor(renderer, defuseHeld ? 40 : 55, defuseHeld ? 170 : 80, defuseHeld ? 80 : 100, 235);
                SDL_RenderFillRect(renderer, &defuseButton);
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                draw_rect_outline(renderer, defuseButton);

                int progress = defuseHeld && defuseStartTime
                    ? static_cast<int>(std::min<Uint32>(100, (currentTime - defuseStartTime) * 100 / DEFUSE_TIME_MS))
                    : 0;
                SDL_Rect bar = {defuseButton.x + 12, defuseButton.y + 40,
                                (defuseButton.w - 24) * progress / 100, 16};
                SDL_SetRenderDrawColor(renderer, 80, 230, 120, 255);
                SDL_RenderFillRect(renderer, &bar);
                draw_text(renderer, "HOLD TO DEFUSE",
                          centered_text_x("HOLD TO DEFUSE", 2, defuseButton.x, defuseButton.w),
                          defuseButton.y + 8, 2, {255,255,255,255});
            }
        }

        if (playerWeapon == WeaponType::PISTOL) {
            std::string shieldLabel;
            SDL_Color shieldFill{60, 65, 80, 220};
            if (playerShieldActive) {
                int secsLeft = static_cast<int>((SHIELD_DURATION_MS - (currentTime - playerShieldActivatedAt)) / 1000) + 1;
                shieldLabel = "SHIELD: " + std::to_string(std::max(0, secsLeft)) + "S";
                shieldFill = {40, 150, 220, 235};
            } else if (currentTime < playerShieldReadyAt) {
                int secsLeft = static_cast<int>((playerShieldReadyAt - currentTime) / 1000) + 1;
                shieldLabel = "SHIELD: " + std::to_string(std::max(0, secsLeft)) + "S";
                shieldFill = {90, 90, 60, 200};
            } else {
                shieldLabel = "SHIELD [Q]";
                shieldFill = {40, 170, 90, 235};
            }
            SDL_SetRenderDrawColor(renderer, shieldFill.r, shieldFill.g, shieldFill.b, shieldFill.a);
            SDL_RenderFillRect(renderer, &shieldButton);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            draw_rect_outline(renderer, shieldButton);
            int shieldTextX = shieldButton.x + (shieldButton.w - static_cast<int>(shieldLabel.length()) * 6 * 2) / 2;
            draw_text(renderer, shieldLabel, shieldTextX, shieldButton.y + (shieldButton.h - 14) / 2, 2, {255, 255, 255, 255});
        }

        render_arrow_pad(renderer);
if (isTouchingDock) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 60);
            SDL_RenderFillRect(renderer, &dockBase);
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 180);
            SDL_RenderFillRect(renderer, &dockHandle);

            // Long-press charge indicator: a radial ring around the joystick knob that
            // fills up as joystickHoldTimer approaches LONG_PRESS_DURATION, so the player
            // gets visual feedback before the roll auto-triggers.
            if (joystickHoldTimer > 0.0f) {
                float chargeFrac = std::min(1.0f, joystickHoldTimer / LONG_PRESS_DURATION);
                int knobCx = dockHandle.x + dockHandle.w / 2;
                int knobCy = dockHandle.y + dockHandle.h / 2;
                int ringRadius = dockHandle.w / 2 + 14;
                constexpr int ringSegments = 40;
                int filledSegments = static_cast<int>(chargeFrac * ringSegments);
                SDL_SetRenderDrawColor(renderer, 140, 230, 255, 230);
                for (int s = 0; s < filledSegments; ++s) {
                    float a0 = -static_cast<float>(M_PI) / 2.0f + (static_cast<float>(s) / ringSegments) * 2.0f * static_cast<float>(M_PI);
                    float a1 = -static_cast<float>(M_PI) / 2.0f + (static_cast<float>(s + 1) / ringSegments) * 2.0f * static_cast<float>(M_PI);
                    int x0 = knobCx + static_cast<int>(std::cos(a0) * ringRadius), y0 = knobCy + static_cast<int>(std::sin(a0) * ringRadius);
                    int x1 = knobCx + static_cast<int>(std::cos(a1) * ringRadius), y1 = knobCy + static_cast<int>(std::sin(a1) * ringRadius);
                    SDL_RenderDrawLine(renderer, x0, y0, x1, y1);
                }
            }
        }

        render_weapon_radial(renderer, currentTime);

        // Manual leaderboard toggle - only reachable mid-match via the RANKS button.
        if (currentGameState == GameState::PLAYING && showLeaderboardPanel) {
            render_leaderboard_panel(renderer, leaderboardPanelBox, "LEADERBOARD");
        }

        if (currentGameState == GameState::GAME_OVER) {
            SDL_SetRenderDrawColor(renderer, 180, 0, 0, 150);
            SDL_Rect overlay = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);
            bool hasNextRound = selectedMode == GameMode::TACTICAL && currentRound < MAX_TACTICAL_ROUNDS;
            std::string goLine1 = hasNextRound ? "SQUAD DOWN - ROUND FAILED" : "GAME OVER - TAP TO MAIN MENU";
            std::string goLine2 = hasNextRound
                ? "TAP ANYWHERE FOR ROUND " + std::to_string(currentRound + 1) + "/" + std::to_string(MAX_TACTICAL_ROUNDS)
                : "FINAL SCORE: " + std::to_string(score) + "   HIGH SCORE: " + std::to_string(highScore);
            draw_text(renderer, goLine1, centered_text_x(goLine1, 3), SCREEN_HEIGHT / 2 - 40, 3, {255, 255, 255, 255});
            draw_text(renderer, goLine2, centered_text_x(goLine2, 2), SCREEN_HEIGHT / 2 + 15, 2, {255, 220, 100, 255});
            // Auto-flash: the match-end leaderboard pops on top once, right as the
            // result lands, so the squad's final standing is the last thing seen.
            if (showMatchEndLeaderboard) {
                render_leaderboard_panel(renderer, leaderboardPanelBox, "MATCH RESULTS");
            }
        } else if (currentGameState == GameState::GAME_WON) {
            SDL_SetRenderDrawColor(renderer, 0, 180, 60, 150);
            SDL_Rect overlay = {0, 0, SCREEN_WIDTH, SCREEN_HEIGHT};
            SDL_RenderFillRect(renderer, &overlay);
            std::string wonLine1 = tacticalBomb.planted ? "BOMB DEFUSED! MISSION ACCOMPLISHED" : "ALL ENEMIES ELIMINATED! MISSION ACCOMPLISHED";
            bool hasNextRound = selectedMode == GameMode::TACTICAL && currentRound < MAX_TACTICAL_ROUNDS;
            std::string wonLine2 = hasNextRound
                ? "TAP ANYWHERE FOR ROUND " + std::to_string(currentRound + 1) + "/" + std::to_string(MAX_TACTICAL_ROUNDS)
                : "TAP ANYWHERE TO RETURN TO MENU";
            draw_text(renderer, wonLine1, centered_text_x(wonLine1, 3), SCREEN_HEIGHT / 2 - 40, 3, {255, 255, 255, 255});
            draw_text(renderer, wonLine2, centered_text_x(wonLine2, 2), SCREEN_HEIGHT / 2 + 15, 2, {255, 255, 255, 255});
            if (showMatchEndLeaderboard) {
                render_leaderboard_panel(renderer, leaderboardPanelBox, "MATCH RESULTS");
            }
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    if (playerPortraitTexture) SDL_DestroyTexture(playerPortraitTexture);
    SDL_CloseAudioDevice(audioDevice);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
