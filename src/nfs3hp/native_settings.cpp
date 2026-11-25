#include <nfs3hp.h>
#include "native_settings.h"
#include <SDL3/SDL.h>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

/* Port: the switches as a file (native_settings.h).
 *
 * The file is read once, before the game starts, and each switch it names is
 * put into the environment before anything looks one up -- every reader of a
 * switch being a lazy `SDL_getenv` on its first use, nothing else has to know
 * this file exists.  A name already in the environment is left alone, so a
 * launch script or `adb shell am start --ez ...` still wins.
 *
 * The file is kept as the lines it was read as, remarks and groups and all, so
 * that a menu changing one switch changes one line and leaves the rest of what
 * a player has written alone.  A switch the file has commented out -- the
 * default it was written with -- is found by its name wherever it is and
 * uncommented there. */
namespace nfs3hp
{
namespace
{

constexpr const char kFileName[] = "nfs3hp.ini";

std::string s_folder;               // where the file is, folder and trailing '/'
std::string s_path;                 // the file itself
std::vector<std::string> s_lines;   // its lines, as read

/* Every switch the port reads, a group of them a `[Group]` line, each at the
 * value the port gives it when the line stays a comment. */
const char kDefaults[] = R"INI(NFS III: Hot Pursuit -- the port's switches.

Every switch the port reads, at the value it has while its line is a comment.
Remove the leading '#' from a line to change it, and put it back for the
default.  The port's own menus write this file.  A switch already set in the
environment wins over this one.

A switch is 'on' or 'off' (also 1 and 0), or a number where it takes one:
higher is more, and 0 is off.  TECHNICAL.md's "Environment variables" says what
each one does.

[The picture]
# NFS_WINDOW = 1280x720
# NFS_FULLSCREEN = on
# NFS_FPS_CAP = 30
# NFS_WIDESCREEN = on
# NFS_TEXTURE_FILTER = on
# NFS_ANISOTROPY = 8
# NFS_TEXTURE_SHARP = on
# NFS_GPU_FILTER = on
# NFS_ROAD_DETAIL = 0.35
# NFS_OBJECT_DETAIL = 0.3
# NFS_HAZE = 0.8
# NFS_BLOOM = 0.9
# NFS_GRADE = on
# NFS_EXPOSURE = 0.7
# NFS_NIGHT_LIGHT = 0.6
# NFS_AA = on
# NFS_GAMMA = 1.0
# NFS_BRIGHTNESS = 1.0
# NFS_CONTRAST = 1.0
# NFS_DEPTH16 = auto
# NFS_CAR_DETAIL_FULL = on
# NFS_MIRROR_FULL = on
# NFS_MOTION_BLUR = 2
# NFS_BLUR_FRINGE = 0.6

[The cars, and the light round them]
# NFS_CAR_SHINE = 1
# NFS_HEADLIGHT_OBJECTS = 1.6
# NFS_HEADLIGHT_REACH = 2
# NFS_HEADLIGHT_WIDTH = 1.4
# NFS_SOFT_EFFECTS = on
# NFS_OBJECT_SHADOWS = 0.45
# NFS_CAR_SHADOW = 1
# NFS_SHADOW_STRETCH = 0.7
# NFS_SHADOW_RAIN = 0.8
# NFS_SHADOW_NIGHT = 0.4
# NFS_WET_ROADS = 1
# NFS_SUN_GLARE = 1
# NFS_LIGHT_GLOW = 1
# NFS_LIGHT_TRAILS = 1
# NFS_TAILLIGHT_TRAILS = 1
# NFS_COP_LIGHTS = 1
# NFS_SPARKS = 2

[The port's own particles]
# NFS_PARTICLES = 1
# NFS_GAME_SPRITES = off
# NFS_WHEEL_SPRAY = on
# NFS_WEATHER_PARTICLES = on
# NFS_WEATHER_MAX = 2.5
# NFS_LENS_RAIN = 1
# NFS_TYRE_SMOKE = 1
# NFS_TYRE_MARKS = on
# NFS_TYRE_MARKS_LIFE = 60
# NFS_EXHAUST_FLAMES = 1
# NFS_FLAME_LIGHT = 1
# NFS_EXHAUST_DEFAULTS = 0
# NFS_SLIPSTREAM = 1
# NFS_SLIPSTREAM_TRAILS = 1
# NFS_SLIP_ICON = on
# NFS_OPPONENT_LIST = on
# NFS_NITRO = 4
# NFS_NITRO_PUSH = 5
# NFS_NITRO_SECONDS = 3
# NFS_NITRO_KEY = 0x00002a04
# NFS_NITRO_UI = on
# NFS_NITRO_FILL = 1
# NFS_NITRO_SOUND = 2
# NFS_NITRO_SHAKE = 1.5
# NFS_NITRO_SHAKE_RATE = 9
# NFS_NITRO_AI = on
# NFS_NITRO_AI_FILL = 30

[What you feel of the race]
# NFS_CRASH_SHAKE = 2
# NFS_CRASH_SOUND = on
# NFS_KNOCK_TREES = 1
# NFS_TREE_MASS = 1
# NFS_LAMP_MASS = 1
# NFS_WALL_SLIDE = 0.8
# NFS_KNOCK_RESPAWN = 10
# NFS_AIR_ROLL_DAMP = 1
# NFS_SELF_RIGHT = 1
# NFS_AIR_SPEED = on
# NFS_AIR_STEER = 0.2
# NFS_AIR_PITCH = 0.2
# NFS_BODY_MOTION = 1
# NFS_FAST_LOAD = 1
# NFS_CAMERA_PULL = 1.2
# NFS_SPEED_FOV = 1
# NFS_SPEED_SHAKE = 2.5
# NFS_SPEED_SHAKE_RATE = 3.5

[The driving]
# NFS_SURFACE_GRIP = 1
# NFS_GRIP_ON_OPPONENTS = 1
# NFS_VOLUME = 1.8
# NFS_WIND = 2
# NFS_NOW_PLAYING = on
# NFS_NEXT_SONG_KEY = 0x5d001b04
# NFS_RADIO = on
# NFS_MINIMAP = on
# NFS_MINIMAP_SCALE = 1

[The opponents, the cops and the traffic]
# NFS_AI_TUNE = on
# NFS_AI_PACE = 1.06
# NFS_AI_BEGINNER = 1.23
# NFS_GRID_TRAFFIC = on
# NFS_PURSUIT_GRID = on
# NFS_TRAFFIC_WEIGHT = 0.5
# NFS_COP_WEIGHT = 1.25
# NFS_COP_PACE = 1
# NFS_SPIKE_STRIPS = off

[Traces, for working on the port]
# NFS_AI_TRACE = 1
# NFS_EXHAUSTS_TRACE = 1
# NFS_SURFACE_TRACE = 1
# NFS_SLIP_TRACE = 1
# NFS_CRASH_TRACE = 1
# NFS_KNOCK_TRACE = 1
# NFS_INPUT_TRACE = 1
# NFS_TICK_TRACE = 1
# NFS_PERF_HINT = 1
# NFS_NATIVE_CHECK = 1
# NFS_CHROMA_TRACE = 1
# NFS_SPRITE_TRACE = 1
)INI";

std::string upper(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
        out += char(SDL_toupper(std::uint8_t(c)));
    return out;
}

/* A line's switch, where that is what it is: `NFS_FOO = 1`, and not
 * `# NFS_FOO = 1` -- commented out, and so not set -- nor a group's
 * `[The picture]`, nor a remark. */
bool switchOf(const std::string& line, std::string& key, std::string& value)
{
    const std::size_t equals = line.find('=');
    if (equals == std::string::npos)
        return false;
    key.clear();
    for (std::size_t k = 0; k < equals; ++k)
    {
        const char c = line[k];
        if (c != ' ' && c != '\t')
            key += char(SDL_toupper(std::uint8_t(c)));
    }
    if (key.size() < 5 || key.compare(0, 4, "NFS_") != 0)
        return false;
    value.clear();
    for (std::size_t k = equals + 1; k < line.size(); ++k)
    {
        const char c = line[k];
        if (value.empty() && (c == ' ' || c == '\t'))
            continue;
        if (c == '#' && !value.empty())   // a remark after the value
            break;
        if (c != '\r')
            value += c;
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    return true;
}

/* Whether a line names this switch at all, commented out or set: `NFS_FOO` as a
 * whole name, with a value after it. */
bool namesSwitch(const std::string& line, const std::string& name)
{
    for (std::size_t at = line.find(name); at != std::string::npos; at = line.find(name, at + 1))
    {
        const std::size_t end = at + name.size();
        if (at > 0 && (SDL_isalnum(line[at - 1]) || line[at - 1] == '_'))
            continue;
        if (end < line.size() && (SDL_isalnum(line[end]) || line[end] == '_'))
            continue;
        if (line.find('=', end) != std::string::npos)
            return true;
    }
    return false;
}

bool readLines(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        return false;
    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        s_lines.push_back(line);
    }
    return true;
}

bool writeLines()
{
    std::ofstream out(s_folder + kFileName, std::ios::trunc);
    if (!out)
        return false;
    for (const std::string& line : s_lines)
        out << line << '\n';
    return bool(out);
}

}  // namespace

void settingsLoad(const std::string& folder)
{
    s_folder = folder;
    if (!s_folder.empty() && s_folder.back() != '/')
        s_folder += '/';
    s_path = s_folder + kFileName;
    s_lines.clear();
    if (!readLines(settingsFile()))
    {
        // The first run: every switch written out, none of them set.
        const std::string text(kDefaults);
        std::size_t at = 0;
        while (at < text.size())
        {
            const std::size_t end = text.find('\n', at);
            s_lines.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        SDL_Log("[SETTINGS] %s written, every switch in it at its default", settingsFile().c_str());
        writeLines();
    }
    int set = 0, kept = 0;
    for (const std::string& line : s_lines)
    {
        std::string key, value;
        if (!switchOf(line, key, value))
            continue;
        const char* was = SDL_getenv(key.c_str());
        if (was && *was)
        {
            ++kept;   // the environment's own: it wins
            continue;
        }
        SDL_SetEnvironmentVariable(SDL_GetEnvironment(), key.c_str(), value.c_str(), true);
        ++set;
    }
    SDL_Log("[SETTINGS] %s: %d switches set, %d already in the environment", settingsFile().c_str(), set, kept);
}

std::string settingsGet(const std::string& key, const std::string& otherwise)
{
    const std::string name = upper(key);
    const char* value = SDL_getenv(name.c_str());
    return value && *value ? std::string(value) : otherwise;
}

void settingsSet(const std::string& key, const std::string& value)
{
    const std::string name = upper(key);
    SDL_SetEnvironmentVariable(SDL_GetEnvironment(), name.c_str(), value.c_str(), true);
    const std::string line = name + " = " + value;
    for (std::string& it : s_lines)
    {
        if (namesSwitch(it, name))
        {
            it = line;
            return;
        }
    }
    s_lines.push_back(line);   // one the file has never had: at its end
}

const std::string& settingsFile()
{
    return s_path;
}

bool settingsSave()
{
    if (s_folder.empty())
        return false;
    return writeLines();
}

}  // namespace nfs3hp
