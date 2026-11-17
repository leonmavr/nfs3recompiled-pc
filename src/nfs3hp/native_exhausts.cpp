#include <nfs3hp.h>
#include <lib/file.h>
#include <SDL3/SDL.h>
#include <cctype>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include "car_slots.h"
#include "native_exhausts.h"

/* Port: a car's exhausts, and the flames they throw (NFS_EXHAUST_FLAMES).
 *
 * The marking tool (tools/car_exhausts.py, the editor's "Mark exhausts...")
 * keeps each car's exhausts in `exhausts.json` in its own folder, in the
 * model's own space, metres.  A car with no file has no exhausts, and so no
 * flames; the file may also name the colour they burn (TECHNICAL.md, "Marking a
 * car's exhausts").
 *
 * The files are read once, the first time a race frame asks, into a table by
 * folder name -- `diab`, `traffic/0005`, `traffic/pursuit/0003` -- because a
 * race's cars know themselves by a number, not a name.  That number is the
 * car's id, which its race record keeps at [record+0x14]; the name the game
 * makes of it is what sub_4392b0 answers, and this takes the same road:
 * the loaded car's own record, from the game's car table (car_slots.h), keeps
 * its folder at +0x110; a traffic body's is in the game's table at 0x55cfb0;
 * a pursuit car's at 0x55c92c.
 *
 * The flame itself -- when it is thrown, how it grows and trembles -- is
 * native_particles.cpp's, where the port's other particles are made. */
namespace nfs3hp
{

namespace
{

/* Each car's exhausts, by the folder it was found in, lowercased with '/'. */
std::unordered_map<std::string, CarExhausts> s_byFolder;
/* What the game has already told us an id's exhausts are (null: none marked). */
std::unordered_map<x86::reg32, const CarExhausts*> s_byId;
std::string s_root;   // gamedata/carmodel/, the path the folder names are from
CarExhausts s_defaults;             // what a car with none marked gets with NFS_EXHAUST_DEFAULTS
std::set<x86::reg32> s_defaulted;   // the cars that got them, said once each
bool s_loaded = false;

/* gamedata/carmodel's own path, where the tool leaves the files. */
std::string carmodelRoot()
{
    return win32::File::dataDirectory() + "gamedata/carmodel/";
}

/* A reading of the tool's exhausts.json -- a small, known shape:
 *
 *     { "about": "...", "exhausts": [ { "x": .., "y": .., "z": .., "radius": .. }, .. ],
 *       "flame": "#f2380a" }
 *
 * Only those numbers and the colour are taken; a key this does not know, and a
 * value of a kind it does not expect, is skipped rather than refused, so the
 * file may be edited by hand and grow. */
struct Json
{
    const std::string& text;
    std::size_t at = 0;

    void space()
    {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])))
            ++at;
    }

    bool take(char c)
    {
        space();
        if (at < text.size() && text[at] == c)
        {
            ++at;
            return true;
        }
        return false;
    }

    bool peek(char c)
    {
        space();
        return at < text.size() && text[at] == c;
    }

    bool string(std::string& out)
    {
        if (!take('"'))
            return false;
        out.clear();
        while (at < text.size() && text[at] != '"')
        {
            char c = text[at++];
            if (c == '\\' && at < text.size())
                c = text[at++];
            out.push_back(c);
        }
        return take('"');
    }

    bool number(float& out)
    {
        space();
        const char* start = text.c_str() + at;
        char* end = nullptr;
        const double value = std::strtod(start, &end);
        if (end == start)
            return false;
        at += std::size_t(end - start);
        out = float(value);
        return true;
    }

    /* One value of any kind, gone over: a key's this does not want. */
    bool skip()
    {
        space();
        if (at >= text.size())
            return false;
        const char c = text[at];
        if (c == '"')
        {
            std::string ignored;
            return string(ignored);
        }
        if (c == '{' || c == '[')
        {
            const bool object = c == '{';
            const char close = object ? '}' : ']';
            ++at;
            if (take(close))
                return true;
            do
            {
                std::string key;
                if (object && (!string(key) || !take(':')))
                    return false;
                if (!skip())
                    return false;
            } while (take(','));
            return take(close);
        }
        if (c == 't' || c == 'f' || c == 'n')   // true, false, null
        {
            while (at < text.size() && std::isalpha(static_cast<unsigned char>(text[at])))
                ++at;
            return true;
        }
        float ignored = 0.0f;
        return number(ignored);
    }
};

/* "#rrggbb" (with or without the '#'), or three numbers -- 0 to 1 each, or 0 to
 * 255 -- as a flame's colour. */
bool flameColour(Json& json, float (&colour)[3])
{
    if (json.peek('"'))
    {
        std::string text;
        if (!json.string(text))
            return false;
        std::size_t at = text.size() == 7 && text[0] == '#' ? 1 : 0;
        if (text.size() - at != 6)
            return false;
        unsigned rgb = 0;
        for (; at < text.size(); ++at)
        {
            const unsigned char c = static_cast<unsigned char>(text[at]);
            if (!std::isxdigit(c))
                return false;
            rgb = rgb * 16 + (c <= '9' ? c - '0' : unsigned(std::tolower(c) - 'a' + 10));
        }
        colour[0] = float(rgb >> 16 & 0xff) / 255.0f;
        colour[1] = float(rgb >> 8 & 0xff) / 255.0f;
        colour[2] = float(rgb & 0xff) / 255.0f;
        return true;
    }
    if (!json.take('['))
        return false;
    float rgb[3] = { 0.0f, 0.0f, 0.0f };
    for (int i = 0; i < 3; ++i)
    {
        if (i && !json.take(','))
            return false;
        if (!json.number(rgb[i]))
            return false;
    }
    if (!json.take(']'))
        return false;
    const bool bytes = rgb[0] > 1.0f || rgb[1] > 1.0f || rgb[2] > 1.0f;
    for (int i = 0; i < 3; ++i)
        colour[i] = bytes ? SDL_min(rgb[i] / 255.0f, 1.0f) : SDL_clamp(rgb[i], 0.0f, 1.0f);
    return true;
}

bool parse(const std::string& text, CarExhausts& out)
{
    Json json{ text };
    if (!json.take('{'))
        return false;
    if (json.take('}'))
        return true;
    do
    {
        std::string key;
        if (!json.string(key) || !json.take(':'))
            return false;
        if (key == "exhausts" && json.peek('['))
        {
            json.take('[');
            if (json.take(']'))
                continue;
            do
            {
                ExhaustTip tip{};
                if (!json.take('{'))
                    return false;
                if (!json.take('}'))
                {
                    do
                    {
                        std::string field;
                        float value = 0.0f;
                        if (!json.string(field) || !json.take(':'))
                            return false;
                        if (!json.number(value))
                        {
                            if (!json.skip())
                                return false;
                            continue;
                        }
                        if (field == "x")
                            tip.x = value;
                        else if (field == "y")
                            tip.y = value;
                        else if (field == "z")
                            tip.z = value;
                        else if (field == "radius")
                            tip.radius = value;
                    } while (json.take(','));
                    if (!json.take('}'))
                        return false;
                }
                out.tips.push_back(tip);
            } while (json.take(','));
            if (!json.take(']'))
                return false;
        }
        else if (key == "flame")
        {
            if (!flameColour(json, out.flame) && !json.skip())
                return false;
        }
        else if (!json.skip())
        {
            return false;
        }
    } while (json.take(','));
    return json.take('}');
}

std::string lowerCase(std::string text)
{
    for (char& c : text)
        c = char(std::tolower(static_cast<unsigned char>(c == '\\' ? '/' : c)));
    return text;
}

/* The exhausts.json in this folder, if there is one, kept under the folder's
 * own name and -- if nothing else has that one -- under the folder alone (the
 * game's traffic bodies are named `0005`, `pursuit\0003`, either way). */
void readCarFolder(const std::string& path)
{
    const std::string file = path + "exhausts.json";
    if (!SDL_GetPathInfo(file.c_str(), nullptr))
        return;
    std::size_t size = 0;
    char* text = static_cast<char*>(SDL_LoadFile(file.c_str(), &size));
    if (!text)
        return;
    CarExhausts exhausts;
    const bool ok = parse(std::string(text, size), exhausts);
    SDL_free(text);
    if (!ok || exhausts.tips.empty())
        return;
    std::string folder = lowerCase(path.compare(0, s_root.size(), s_root) == 0 ? path.substr(s_root.size()) : path);
    while (!folder.empty() && folder.back() == '/')
        folder.pop_back();
    if (folder.empty())
        return;
    exhausts.folder = folder;
    s_byFolder[folder] = exhausts;
    const std::size_t slash = folder.find_last_of('/');
    if (slash != std::string::npos && !s_byFolder.count(folder.substr(slash + 1)))
        s_byFolder[folder.substr(slash + 1)] = exhausts;   // a traffic body's own name
}

/* Every folder under gamedata/carmodel -- a car's, and traffic's bodies below
 * it -- for the exhausts.json the marking tool leaves in it. */
void scan(const std::string& path, int depth)
{
    readCarFolder(path);
    if (depth >= 3)
        return;
    struct Walk
    {
        const std::string* path;
        int depth;
    } walk{ &path, depth };
    SDL_EnumerateDirectory(path.c_str(), [](void* data, const char*, const char* entry) {
        const Walk& walk = *static_cast<const Walk*>(data);
        if (entry[0] == '.')
            return SDL_ENUM_CONTINUE;
        const std::string child = *walk.path + entry + "/";
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(child.c_str(), &info) || info.type != SDL_PATHTYPE_DIRECTORY)
            return SDL_ENUM_CONTINUE;
        scan(child, walk.depth + 1);   // a car's folder, or traffic's below it
        return SDL_ENUM_CONTINUE;
    }, &walk);
}

/* Every car with no exhausts marked keeps this pair instead when
 * NFS_EXHAUST_DEFAULTS is on: two pipes at the rear corners, sized and placed by
 * the game's own marked cars (x 0.26-0.69 m either side of the middle, y about
 * -0.27, z about -2.05, pipes 0.035-0.06 m across).  A guess, and only for the
 * cars nothing was marked on -- a marked car keeps its own, four where it names
 * four. */
CarExhausts defaults()
{
    CarExhausts exhausts;
    exhausts.folder = "defaults (NFS_EXHAUST_DEFAULTS)";
    exhausts.tips = { ExhaustTip{ -0.42f, -0.27f, -2.05f, 0.05f }, ExhaustTip{ 0.42f, -0.27f, -2.05f, 0.05f } };
    return exhausts;
}

void load()
{
    if (s_loaded)
        return;
    s_loaded = true;
    if (!exhaustFlamesOn())
        return;
    s_root = carmodelRoot();
    scan(s_root, 0);
    s_defaults = defaults();
    if (SDL_getenv("NFS_EXHAUSTS_TRACE") && exhaustDefaultsOn())
        for (const ExhaustTip& tip : s_defaults.tips)
            SDL_Log("[EXHAUST] default: x %.3f  y %.3f  z %.3f  radius %.3f", double(tip.x), double(tip.y),
                    double(tip.z), double(tip.radius));
    if (SDL_getenv("NFS_EXHAUSTS_TRACE"))
        for (const auto& found : s_byFolder)
        {
            if (found.first.compare(lowerCase(found.second.folder)) != 0)
                continue;   // the same car again, under the name traffic finds it by
            SDL_Log("[EXHAUST] %s: %zu tips, flame %.2f %.2f %.2f", found.first.c_str(), found.second.tips.size(),
                    double(found.second.flame[0]), double(found.second.flame[1]), double(found.second.flame[2]));
            for (const ExhaustTip& tip : found.second.tips)
                SDL_Log("[EXHAUST]     x %.3f  y %.3f  z %.3f  radius %.3f", double(tip.x), double(tip.y),
                        double(tip.z), double(tip.radius));
        }
}

/* The folder the game has a car under, by the id its race record carries:
 * sub_4392b0's way (sub_4392b0, nfs3hp.9.cpp), which is what the game builds
 * the car's own file names from. */
std::string carFolder(win32::WinApplication* app, x86::reg32 carId)
{
    constexpr x86::reg32 kCarNameInRecord = 0x110;   // the loaded car's own record keeps its folder here
    constexpr x86::reg32 kTrafficNames = 0x55cfb0;   // a traffic body's, by its id, 0x32 to 0x1f3
    constexpr x86::reg32 kPursuitNames = 0x55c92c;   // a pursuit car's, 0x1f4 to 0x257
    x86::reg32 name = 0;
    if (const x86::reg32 record = carSlotRecord(carId))
        name = record + kCarNameInRecord;
    else if (carId <= 0x1f3)
        name = app->getMemory<x86::reg32>(kTrafficNames + carId * 4);
    else if (carId <= 0x257)
        name = app->getMemory<x86::reg32>(kPursuitNames + carId * 4);
    std::string text;
    for (x86::reg32 at = 0; name && at < 64; ++at)
    {
        const char c = app->getMemory<char>(name + at);
        if (!c)
            break;
        text.push_back(c);
    }
    return lowerCase(text);
}

}  // namespace

bool exhaustFlamesOn()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_EXHAUST_FLAMES");
        return !(text && (SDL_strcmp(text, "0") == 0 || SDL_strcasecmp(text, "off") == 0));
    }();
    return on;
}

bool exhaustDefaultsOn()
{
    static const bool on = [] {
        const char* text = SDL_getenv("NFS_EXHAUST_DEFAULTS");
        return text && (SDL_strcmp(text, "1") == 0 || SDL_strcasecmp(text, "on") == 0);
    }();
    return on;
}

void loadCarExhausts()
{
    load();
}

/* The body name the race gave a car, at +0x1e4: a whole C string, strcpy'd
 * there once when the car was set up out of sub_4392b0's answer for its id
 * (nfs3hp.6.cpp, 0x42876d: `lea edi, [edx+0x1e4]`, `call 0x4392b0`, then a byte
 * at a time until the 0) -- so it is the very name the game made the car's file
 * names from, empty before the race has named it.  For a racer that is its
 * folder (`diab`); for a traffic or pursuit car with no record of its own it is
 * the body code out of the game's tables (`0005`, `vett`, the same bodies the
 * racers use).  Lowercased, because a folder's name on disk is.  Every car of
 * the race carries it, so an opponent's exhausts are found as the player's are. */
std::string carBodyName(win32::WinApplication* app, x86::reg32 car)
{
    std::string name;
    for (x86::reg32 at = 0; at < 32; ++at)
    {
        const char c = app->getMemory<char>(car + 0x1e4 + at);
        if (!c)
            break;
        name.push_back(c);
    }
    return lowerCase(name);
}

const CarExhausts* carExhausts(win32::WinApplication* app, x86::reg32 car)
{
    load();
    const std::string name = carBodyName(app, car);
    if (!name.empty())
    {
        const auto named = s_byFolder.find(name);
        if (named != s_byFolder.end())
            return &named->second;
    }
    const x86::reg32 carId = app->getMemory<x86::reg32>(car + 0x220 + 0x14);
    const auto cached = s_byId.find(carId);
    if (cached != s_byId.end())
        return cached->second;
    const std::string folder = carFolder(app, carId);
    const CarExhausts* found = nullptr;
    auto exact = s_byFolder.find(folder);
    if (exact != s_byFolder.end())
        found = &exact->second;
    else
    {
        const std::size_t slash = folder.find_last_of('/');
        auto named = s_byFolder.find(slash == std::string::npos ? folder : folder.substr(slash + 1));
        if (named != s_byFolder.end())
            found = &named->second;
    }
    if (!found && exhaustDefaultsOn())
    {
        found = &s_defaults;
        if (s_defaulted.insert(carId).second)
            SDL_Log("[EXHAUST] car id %u \"%s\": none marked, the defaults -- %zu tips", unsigned(carId),
                    name.c_str(), s_defaults.tips.size());
    }
    if (found && found != &s_defaults)
        s_byId[carId] = found;   // (one not found yet is asked again: the game may not have loaded it)
    return found;
}

}
