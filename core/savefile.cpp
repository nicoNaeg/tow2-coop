// Save-file container and a small zlib inflater (RFC 1950/1951, the puff.c decoding scheme) for the game's saves.
#include "savefile.hpp"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace savefile {
namespace {
struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    int bit() {
        if (!cnt) {
            if (pos >= n) throw std::runtime_error("zlib stream truncated");
            buf = p[pos++], cnt = 8;
        }
        int b = buf & 1;
        buf >>= 1, cnt--;
        return b;
    }
    uint32_t bits(int k) {
        uint32_t v = 0;
        for (int i = 0; i < k; i++) v |= static_cast<uint32_t>(bit()) << i;
        return v;
    }
};

struct Huff {
    uint16_t count[16];
    uint16_t sym[320];
};

void build(Huff& h, const uint8_t* len, int n) {
    memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; i++) h.count[len[i]]++;
    h.count[0] = 0;
    uint16_t offs[16] = {};
    for (int i = 1; i < 15; i++) offs[i + 1] = static_cast<uint16_t>(offs[i] + h.count[i]);
    for (int i = 0; i < n; i++)
        if (len[i]) h.sym[offs[len[i]]++] = static_cast<uint16_t>(i);
}

int decode(Bits& b, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= b.bit();
        int count = h.count[len];
        if (code - count < first) return h.sym[index + (code - first)];
        index += count, first += count;
        first <<= 1, code <<= 1;
    }
    throw std::runtime_error("bad Huffman code");
}

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
                                6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void codes(Bits& b, std::vector<uint8_t>& out, const Huff& lit, const Huff& dist) {
    for (;;) {
        int s = decode(b, lit);
        if (s < 256) { out.push_back(static_cast<uint8_t>(s)); continue; }
        if (s == 256) return;
        s -= 257;
        if (s >= 29) throw std::runtime_error("bad length code");
        size_t len = kLenBase[s] + b.bits(kLenExtra[s]);
        int d = decode(b, dist);
        if (d >= 30) throw std::runtime_error("bad distance code");
        size_t back = kDistBase[d] + b.bits(kDistExtra[d]);
        if (back > out.size()) throw std::runtime_error("distance too far back");
        size_t from = out.size() - back;
        for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);  // may overlap: byte by byte
    }
}

uint32_t rd32(const std::vector<uint8_t>& d, size_t at) {
    uint32_t v;
    memcpy(&v, d.data() + at, 4);
    return v;
}

struct Entry {
    std::string name;
    size_t size_at, off, size;  // where its size field is, its data
};

bool parse(const std::vector<uint8_t>& d, std::vector<Entry>& out, std::string& err) {
    if (d.size() < 23 || memcmp(d.data() + 4, "SGDF", 4) != 0) { err = "not a save container (no SGDF header)"; return false; }
    for (size_t pos = 23; pos < d.size();) {
        if (pos + 4 > d.size()) { err = "container entry cut"; return false; }
        uint32_t n = rd32(d, pos);
        if (!n || n > 1024 || pos + 8 + n > d.size()) { err = "bad entry name"; return false; }
        uint32_t size = rd32(d, pos + 4 + n);
        if (pos + 8 + n + size > d.size()) { err = "entry runs past the end"; return false; }
        std::string name(reinterpret_cast<const char*>(d.data() + pos + 4), n);
        while (!name.empty() && name.back() == '\0') name.pop_back();
        out.push_back({name, pos + 4 + n, pos + 8 + n, size});
        pos += 8 + n + size;
    }
    return true;
}

bool load_container(const std::wstring& dir, std::vector<uint8_t>& d, std::vector<Entry>& ents, const Entry*& player, std::string& err) {
    std::vector<uint8_t> raw;
    if (!read_file(dir + L"\\SaveGame.dat", raw)) { err = "cannot read SaveGame.dat"; return false; }
    if (!inflate(raw, d, err) || !parse(d, ents, err)) return false;
    player = nullptr;
    for (const Entry& e : ents)
        if (e.name == "Player.dat") player = &e;
    if (!player) { err = "the save holds no Player.dat"; return false; }
    return true;
}

// Player.dat starts: 01, version 5, "PSHF", the version block, two section sizes, 01 05 01 00 00 00, then the position
// (3 doubles at 46) and rotation (3 doubles at 70) of the player (probe 61, matched against the pawn's location).
bool player_ok(const uint8_t* p, size_t n) {
    if (n < 200 || p[0] != 1 || memcmp(p + 5, "PSHF", 4) != 0) return false;
    for (int i = 0; i < 6; i++) {
        double v;
        memcpy(&v, p + 46 + 8 * i, 8);
        if (!std::isfinite(v) || std::fabs(v) > 1e8) return false;
    }
    return true;
}
}  // namespace

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) a += *p++, b += a;
        a %= 65521, b %= 65521;
    }
    return (b << 16) | a;
}

bool inflate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out, std::string& err) {
    try {
        if (in.size() < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20))
            throw std::runtime_error("not a zlib stream");
        Bits b{in.data(), in.size() - 4, 2};
        out.clear();
        out.reserve(in.size() * 8);
        int last;
        do {
            last = b.bit();
            int type = static_cast<int>(b.bits(2));
            if (type == 0) {
                b.cnt = 0;  // to the next byte
                if (b.pos + 4 > b.n) throw std::runtime_error("stored block cut");
                uint32_t len = b.p[b.pos] | (b.p[b.pos + 1] << 8), nlen = b.p[b.pos + 2] | (b.p[b.pos + 3] << 8);
                if ((len ^ 0xFFFF) != nlen || b.pos + 4 + len > b.n) throw std::runtime_error("bad stored block");
                out.insert(out.end(), b.p + b.pos + 4, b.p + b.pos + 4 + len);
                b.pos += 4 + len;
            } else if (type == 1) {
                static Huff lit, dist;
                static bool built = false;
                if (!built) {
                    uint8_t l[288], d[30];
                    for (int i = 0; i < 144; i++) l[i] = 8;
                    for (int i = 144; i < 256; i++) l[i] = 9;
                    for (int i = 256; i < 280; i++) l[i] = 7;
                    for (int i = 280; i < 288; i++) l[i] = 8;
                    for (int i = 0; i < 30; i++) d[i] = 5;
                    build(lit, l, 288), build(dist, d, 30);
                    built = true;
                }
                codes(b, out, lit, dist);
            } else if (type == 2) {
                int nlen = static_cast<int>(b.bits(5)) + 257, ndist = static_cast<int>(b.bits(5)) + 1, ncode = static_cast<int>(b.bits(4)) + 4;
                if (nlen > 286 || ndist > 30) throw std::runtime_error("bad dynamic block counts");
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t lengths[320] = {};
                for (int i = 0; i < ncode; i++) lengths[order[i]] = static_cast<uint8_t>(b.bits(3));
                Huff lencode, lit, dist;
                build(lencode, lengths, 19);
                memset(lengths, 0, sizeof lengths);
                for (int i = 0; i < nlen + ndist;) {
                    int s = decode(b, lencode);
                    if (s < 16) { lengths[i++] = static_cast<uint8_t>(s); continue; }
                    uint8_t v = 0;
                    int rep;
                    if (s == 16) {
                        if (!i) throw std::runtime_error("repeat with no length");
                        v = lengths[i - 1], rep = 3 + static_cast<int>(b.bits(2));
                    } else if (s == 17) rep = 3 + static_cast<int>(b.bits(3));
                    else rep = 11 + static_cast<int>(b.bits(7));
                    if (i + rep > nlen + ndist) throw std::runtime_error("too many lengths");
                    while (rep--) lengths[i++] = v;
                }
                build(lit, lengths, nlen), build(dist, lengths + nlen, ndist);
                codes(b, out, lit, dist);
            } else throw std::runtime_error("bad block type");
        } while (!last);
        const uint8_t* t = in.data() + in.size() - 4;
        uint32_t want = (static_cast<uint32_t>(t[0]) << 24) | (t[1] << 16) | (t[2] << 8) | t[3];
        if (adler32(out.data(), out.size()) != want) throw std::runtime_error("checksum mismatch");
        return true;
    } catch (const std::exception& e) {
        err = std::string("inflate: ") + e.what();
        return false;
    }
}

std::vector<uint8_t> deflate_stored(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() + in.size() / 65535 * 5 + 16);
    out.push_back(0x78), out.push_back(0x01);
    size_t pos = 0;
    do {
        size_t k = std::min<size_t>(65535, in.size() - pos);
        out.push_back(pos + k == in.size() ? 1 : 0);
        out.push_back(static_cast<uint8_t>(k)), out.push_back(static_cast<uint8_t>(k >> 8));
        out.push_back(static_cast<uint8_t>(~k)), out.push_back(static_cast<uint8_t>(~k >> 8));
        out.insert(out.end(), in.begin() + pos, in.begin() + pos + k);
        pos += k;
    } while (pos < in.size());
    uint32_t a = adler32(in.data(), in.size());
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<uint8_t>(a >> s));
    return out;
}

bool read_file(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(h, &sz) && sz.QuadPart < (1LL << 31);
    if (ok) {
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring& path, const std::vector<uint8_t>& data) {
    std::wstring tmp = path + L".tow2coop-tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = data.empty() || (WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) && put == data.size());
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool read_player(const std::wstring& dir, std::vector<uint8_t>& player, std::string& err) {
    std::vector<uint8_t> d;
    std::vector<Entry> ents;
    const Entry* e;
    if (!load_container(dir, d, ents, e, err)) return false;
    if (!player_ok(d.data() + e->off, e->size)) { err = "Player.dat has an unknown layout"; return false; }
    player.assign(d.begin() + e->off, d.begin() + e->off + e->size);
    return true;
}

bool splice_player(const std::wstring& dir, std::vector<uint8_t> player, std::string& err) {
    std::vector<uint8_t> d;
    std::vector<Entry> ents;
    const Entry* e;
    if (!load_container(dir, d, ents, e, err)) return false;
    const uint8_t* host = d.data() + e->off;
    if (!player_ok(host, e->size) || !player_ok(player.data(), player.size()) || memcmp(host + 40, player.data() + 40, 6) != 0) {
        err = "Player.dat has an unknown layout";
        return false;
    }
    memcpy(player.data() + 46, host + 46, 48);  // the save's position and rotation: appear where the host stood
    std::vector<uint8_t> out(d.begin(), d.begin() + e->size_at);
    uint32_t n = static_cast<uint32_t>(player.size());
    out.insert(out.end(), reinterpret_cast<uint8_t*>(&n), reinterpret_cast<uint8_t*>(&n) + 4);
    out.insert(out.end(), player.begin(), player.end());
    out.insert(out.end(), d.begin() + e->off + e->size, d.end());
    // Metadata.dat holds the unpacked size right after the build strings ("Release\0" + 7 bytes; probe 61).
    std::vector<uint8_t> meta;
    if (!read_file(dir + L"\\Metadata.dat", meta)) { err = "cannot read Metadata.dat"; return false; }
    size_t at = std::string::npos;
    static const char tag[] = "Release";
    for (size_t i = 0; i + sizeof tag + 7 + 4 <= meta.size(); i++)
        if (!memcmp(meta.data() + i, tag, sizeof tag) && rd32(meta, i + sizeof tag + 7) == d.size()) { at = i + sizeof tag + 7; break; }
    if (at == std::string::npos) { err = "Metadata.dat: no unpacked size found"; return false; }
    uint32_t size = static_cast<uint32_t>(out.size());
    memcpy(meta.data() + at, &size, 4);
    if (!write_file(dir + L"\\SaveGame.dat", deflate_stored(out)) || !write_file(dir + L"\\Metadata.dat", meta)) {
        err = "cannot write the spliced save";
        return false;
    }
    return true;
}
}  // namespace savefile
