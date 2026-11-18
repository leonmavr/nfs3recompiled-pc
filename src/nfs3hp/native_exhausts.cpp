#include <nfs3hp.h>
#include <lib/file.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

/* The car's body, for the flames it stands in front of (exhaustBodyLetsThrough).
 *
 * The particle pass cuts a flame by the depth buffer, but a car's own body did
 * not reliably cut the flames burning on it: they showed through the car from in
 * front of it, and a side pipe's through the rear bumper (TODOs.md, 2026-11-18),
 * whatever the car wrote or did not write there.  The flame and the body are
 * both known exactly in the car's own space, so the body is asked directly:
 * its high-detail triangles, out of the same car.fce the exhausts were marked
 * on (tools/car_model.py documents the format). */
namespace body
{
constexpr std::size_t kHeader = 0x1f04;          // the FCE's fixed header; its tables' offsets count from its end
constexpr x86::reg32 kTriangleCount = 0x04, kVertexCount = 0x08;
constexpr x86::reg32 kVertexTable = 0x10, kTriangleTable = 0x18;
constexpr x86::reg32 kPartCount = 0xf8, kPartOrigins = 0xfc;
constexpr x86::reg32 kPartFirstVertex = 0x3fc, kPartFirstTriangle = 0x5fc, kPartTriangles = 0x6fc;
constexpr x86::reg32 kPartNames = 0xe04;
constexpr std::size_t kNameSize = 64, kMostParts = 64;
constexpr std::size_t kTriangleSize = 0x38;      // page, three indices (the part's own), flags, (u, v) three times
constexpr std::uint32_t kLeafTriangles = 4;      // a box tree's leaf holds this many or fewer
constexpr float kPipeSlack = 0.12f;              // m: a hit this near the flame is its own pipe's mouth
constexpr float kInsideMargin = 0.05f;           // m: the camera this far inside the body's box is in the car
constexpr float kAround = 0.6f;                  // the four rays round the middle, at this much of the radius

std::uint32_t u32(const std::string& data, std::size_t at)
{
    std::uint32_t value = 0;
    if (at + 4 <= data.size())
        std::memcpy(&value, data.data() + at, 4);
    return value;
}

float f32(const std::string& data, std::size_t at)
{
    float value = 0.0f;
    if (at + 4 <= data.size())
        std::memcpy(&value, data.data() + at, 4);
    return value;
}

std::uint32_t bigEndian(const std::string& data, std::size_t at)
{
    if (at + 4 > data.size())
        return 0;
    const auto* b = reinterpret_cast<const unsigned char*>(data.data() + at);
    return std::uint32_t(b[0]) << 24 | std::uint32_t(b[1]) << 16 | std::uint32_t(b[2]) << 8 | std::uint32_t(b[3]);
}

std::string loadFile(const std::string& path)
{
    std::size_t size = 0;
    void* bytes = SDL_LoadFile(path.c_str(), &size);
    if (!bytes)
        return std::string();
    std::string data(static_cast<const char*>(bytes), size);
    SDL_free(bytes);
    return data;
}

/* The car's car.fce: out of its car.viv first, the copy the game reads (a BIGF
 * archive: a count at 8, big-endian, then from 16 an offset, a size and a name
 * each), else the loose one beside it. */
std::string modelBytes(const std::string& folder)
{
    const std::string archive = loadFile(folder + "car.viv");
    if (archive.size() >= 16 && archive.compare(0, 4, "BIGF") == 0)
    {
        const std::uint32_t count = bigEndian(archive, 8);
        std::size_t at = 16;
        for (std::uint32_t i = 0; i < count && at + 8 < archive.size(); ++i)
        {
            const std::uint32_t offset = bigEndian(archive, at), size = bigEndian(archive, at + 4);
            at += 8;
            const std::size_t end = archive.find('\0', at);
            if (end == std::string::npos)
                break;
            const std::string name = lowerCase(archive.substr(at, end - at));
            at = end + 1;
            if (name == "car.fce" && std::size_t(offset) + size <= archive.size())
                return archive.substr(offset, size);
        }
    }
    std::string loose = loadFile(folder + "car.fce");
    return loose.empty() ? loadFile(folder + "CAR.FCE") : loose;
}

bool startsWith(const std::string& text, const char* prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

/* Not a wheel, which turns and steers: "left front wheel", or in the short
 * names a converted model carries, ":HLFW" and the like.  (Which level of
 * detail a part is, is read()'s business: the parts come high first, then
 * medium, small, tiny, and only some say which -- tools/car_model.py's
 * assign_lods.) */
bool notWheel(const std::string& name)
{
    if (name.find("wheel") != std::string::npos)
        return false;
    const bool shortName = startsWith(name, ":") && name.size() == 5;
    return !(shortName && (name.compare(3, 2, "fw") == 0 || name.compare(3, 2, "rw") == 0));
}

void boxOf(const CarBody& body, std::uint32_t first, std::uint32_t count, CarBody::Node& node)
{
    for (int a = 0; a < 3; ++a)
    {
        node.lo[a] = 1e30f;
        node.hi[a] = -1e30f;
    }
    for (std::uint32_t k = first; k < first + count; ++k)
    {
        const float* c = &body.corners[std::size_t(body.order[k]) * 9];
        for (int v = 0; v < 3; ++v)
            for (int a = 0; a < 3; ++a)
            {
                node.lo[a] = std::min(node.lo[a], c[v * 3 + a]);
                node.hi[a] = std::max(node.hi[a], c[v * 3 + a]);
            }
    }
}

std::uint32_t build(CarBody& body, std::uint32_t first, std::uint32_t count)
{
    const std::uint32_t index = std::uint32_t(body.nodes.size());
    body.nodes.push_back(CarBody::Node{});
    CarBody::Node node{};
    boxOf(body, first, count, node);
    node.first = first;
    node.count = count;
    if (count > kLeafTriangles)
    {
        int axis = 0;
        for (int a = 1; a < 3; ++a)
            if (node.hi[a] - node.lo[a] > node.hi[axis] - node.lo[axis])
                axis = a;
        auto middle = [&](std::uint32_t t) {
            const float* c = &body.corners[std::size_t(t) * 9];
            return c[axis] + c[3 + axis] + c[6 + axis];
        };
        const auto begin = body.order.begin() + first;
        std::nth_element(begin, begin + count / 2, begin + count,
                         [&](std::uint32_t a, std::uint32_t b) { return middle(a) < middle(b); });
        node.count = 0;
        node.left = build(body, first, count / 2);
        node.right = build(body, first + count / 2, count - count / 2);
    }
    body.nodes[index] = node;
    return index;
}

std::shared_ptr<const CarBody> read(const std::string& folder)
{
    const std::string data = modelBytes(folder);
    if (data.size() < kHeader)
        return nullptr;
    const std::uint32_t vertices = u32(data, kVertexCount), triangles = u32(data, kTriangleCount);
    const std::size_t vertexAt = kHeader + u32(data, kVertexTable), triangleAt = kHeader + u32(data, kTriangleTable);
    if (vertexAt + std::size_t(vertices) * 12 > data.size() || triangleAt + std::size_t(triangles) * kTriangleSize > data.size())
        return nullptr;
    auto body = std::make_shared<CarBody>();
    const std::uint32_t parts = std::min<std::uint32_t>(u32(data, kPartCount), kMostParts);
    bool lower = false;   // past the high level of detail
    for (std::uint32_t p = 0; p < parts; ++p)
    {
        std::string name(data.data() + kPartNames + p * kNameSize, kNameSize);
        name = lowerCase(name.substr(0, name.find('\0')));
        if (startsWith(name, "medium") || startsWith(name, "small") || startsWith(name, "tiny")
            || startsWith(name, ":m") || startsWith(name, ":l") || startsWith(name, ":t"))
            lower = true;
        const bool high = startsWith(name, "hi ") || startsWith(name, "high") || startsWith(name, ":h") || !lower;
        if (!high || !notWheel(name))
            continue;
        const float origin[3] = { f32(data, kPartOrigins + p * 12), f32(data, kPartOrigins + p * 12 + 4),
                                  f32(data, kPartOrigins + p * 12 + 8) };
        const std::uint32_t firstVertex = u32(data, kPartFirstVertex + p * 4);
        const std::uint32_t firstTriangle = u32(data, kPartFirstTriangle + p * 4), count = u32(data, kPartTriangles + p * 4);
        for (std::uint32_t t = firstTriangle; t < firstTriangle + count && t < triangles; ++t)
        {
            const std::size_t at = triangleAt + std::size_t(t) * kTriangleSize;
            float corner[9];
            bool ok = true;
            for (int v = 0; v < 3; ++v)
            {
                const std::uint32_t i = firstVertex + u32(data, at + 4 + std::size_t(v) * 4);
                if (i >= vertices)
                {
                    ok = false;
                    break;
                }
                for (int a = 0; a < 3; ++a)
                    corner[v * 3 + a] = f32(data, vertexAt + std::size_t(i) * 12 + std::size_t(a) * 4) + origin[a];
            }
            if (ok)
                body->corners.insert(body->corners.end(), corner, corner + 9);
        }
    }
    const std::uint32_t count = std::uint32_t(body->corners.size() / 9);
    if (count == 0)
        return nullptr;
    body->order.resize(count);
    for (std::uint32_t i = 0; i < count; ++i)
        body->order[i] = i;
    body->nodes.reserve(std::size_t(count) / kLeafTriangles * 2 + 1);
    build(*body, 0, count);
    return body;
}

/* Where along from + t*d, 0 < t < most, the segment first meets the box; false
 * where it never does. */
bool meetsBox(const CarBody::Node& node, const float from[3], const float inverse[3], float most)
{
    float lo = 0.0f, hi = most;
    for (int a = 0; a < 3; ++a)
    {
        float t0 = (node.lo[a] - from[a]) * inverse[a], t1 = (node.hi[a] - from[a]) * inverse[a];
        if (t0 > t1)
            std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
        if (lo > hi)
            return false;
    }
    return true;
}

/* Whether from + t*d meets the triangle for some 0 < t < most (both its faces:
 * the ray may come from inside a part). */
bool meetsTriangle(const float* c, const float from[3], const float d[3], float most)
{
    const float e1[3] = { c[3] - c[0], c[4] - c[1], c[5] - c[2] };
    const float e2[3] = { c[6] - c[0], c[7] - c[1], c[8] - c[2] };
    const float p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-12f)
        return false;
    const float inv = 1.0f / det;
    const float s[3] = { from[0] - c[0], from[1] - c[1], from[2] - c[2] };
    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    const float q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
    return t > 0.0f && t < most;
}

/* Whether the body stands anywhere on the way from `from` to within the pipe's
 * slack of `to`. */
bool blocked(const CarBody& body, const float from[3], const float to[3])
{
    float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length <= kPipeSlack)
        return false;
    for (float& x : d)
        x /= length;
    const float most = length - kPipeSlack;
    float inverse[3];
    for (int a = 0; a < 3; ++a)
        inverse[a] = std::fabs(d[a]) > 1e-12f ? 1.0f / d[a] : (d[a] < 0.0f ? -1e30f : 1e30f);
    std::uint32_t stack[64];
    int top = 0;
    stack[top++] = 0;
    while (top > 0)
    {
        const CarBody::Node& node = body.nodes[stack[--top]];
        if (!meetsBox(node, from, inverse, most))
            continue;
        if (node.count == 0)
        {
            if (top + 2 > 64)
                return false;   // deeper than any car's tree; give the flame the benefit
            stack[top++] = node.left;
            stack[top++] = node.right;
            continue;
        }
        for (std::uint32_t k = node.first; k < node.first + node.count; ++k)
            if (meetsTriangle(&body.corners[std::size_t(body.order[k]) * 9], from, d, most))
                return true;
    }
    return false;
}
}  // namespace body

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
    exhausts.body = body::read(path);
    if (SDL_getenv("NFS_EXHAUSTS_TRACE"))
        SDL_Log("[EXHAUST] %s: body of %zu triangles, for cutting its flames%s", folder.c_str(),
                exhausts.body ? exhausts.body->corners.size() / 9 : std::size_t(0),
                exhausts.body ? "" : " (none read: the depth buffer alone cuts them)");
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

float exhaustBodyLetsThrough(const CarExhausts& exhausts, const float eye[3], const float at[3], float radius)
{
    if (!exhausts.body)
        return 1.0f;
    const CarBody& car = *exhausts.body;
    const CarBody::Node& whole = car.nodes[0];
    bool inside = true;
    for (int a = 0; a < 3; ++a)
        inside = inside && eye[a] > whole.lo[a] + body::kInsideMargin && eye[a] < whole.hi[a] - body::kInsideMargin;
    if (inside)
        return 1.0f;
    // Two ways square to the view, for the four rays round the middle.
    float d[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length < 1e-4f)
        return 1.0f;
    for (float& x : d)
        x /= length;
    float across[3] = { d[2], 0.0f, -d[0] };   // d x up
    float span = std::sqrt(across[0] * across[0] + across[2] * across[2]);
    if (span < 1e-4f)
    {
        across[0] = 1.0f;
        across[2] = 0.0f;
        span = 1.0f;
    }
    for (float& x : across)
        x /= span;
    const float up[3] = { across[1] * d[2] - across[2] * d[1], across[2] * d[0] - across[0] * d[2],
                          across[0] * d[1] - across[1] * d[0] };
    const float r = radius * body::kAround;
    const float offsets[5][2] = { { 0, 0 }, { r, 0 }, { -r, 0 }, { 0, r }, { 0, -r } };
    int through = 0;
    for (const auto& o : offsets)
    {
        const float to[3] = { at[0] + across[0] * o[0] + up[0] * o[1], at[1] + across[1] * o[0] + up[1] * o[1],
                              at[2] + across[2] * o[0] + up[2] * o[1] };
        through += body::blocked(car, eye, to) ? 0 : 1;
    }
    return float(through) / 5.0f;
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
