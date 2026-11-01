#include "core/Zip.h"

#include <cstring>

namespace {

// ---- DEFLATE ----------------------------------------------------------------
// A small canonical-Huffman decoder in the style of zlib's "puff".

constexpr int kMaxBits = 15;

struct Huffman {
    std::uint16_t count[kMaxBits + 1];
    std::uint16_t symbol[288];
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool Bits(int need, int& value) {
        long long v = bitBuffer_;
        while (bitCount_ < need) {
            if (pos_ >= size_) return false;
            v |= static_cast<long long>(data_[pos_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuffer_ = static_cast<std::uint32_t>(v >> need);
        bitCount_ -= need;
        value = static_cast<int>(v & ((1LL << need) - 1));
        return true;
    }

    void AlignToByte() {
        bitBuffer_ = 0;
        bitCount_ = 0;
    }

    bool Bytes(std::size_t n, const std::uint8_t*& out) {
        if (size_ - pos_ < n) return false;
        out = data_ + pos_;
        pos_ += n;
        return true;
    }

    // Decodes one symbol bit by bit; codes are at most 15 bits.
    bool Decode(const Huffman& h, int& sym) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            int bit;
            if (!Bits(1, bit)) return false;
            code |= bit;
            const int count = h.count[len];
            if (code - count < first) {
                sym = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
};

// Returns false for an over-subscribed code; incomplete codes are allowed
// (a single distance code is legal).
bool Build(Huffman& h, const std::uint16_t* lengths, int n) {
    std::memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < n; ++i) h.count[lengths[i]]++;
    if (h.count[0] == n) return true;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return false;
    }
    std::uint16_t offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<std::uint16_t>(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
    }
    return true;
}

constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint16_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint16_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                          6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(BitReader& in, const Huffman& lencode, const Huffman& distcode, std::size_t maxOutput,
           std::vector<std::uint8_t>& out) {
    for (;;) {
        int symbol;
        if (!in.Decode(lencode, symbol)) return false;
        if (symbol < 256) {
            if (out.size() >= maxOutput) return false;
            out.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
            return true;
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            int extra;
            if (!in.Bits(kLengthExtra[symbol], extra)) return false;
            const std::size_t length = kLengthBase[symbol] + static_cast<std::size_t>(extra);
            int dsym;
            if (!in.Decode(distcode, dsym) || dsym >= 30) return false;
            if (!in.Bits(kDistExtra[dsym], extra)) return false;
            const std::size_t dist = kDistBase[dsym] + static_cast<std::size_t>(extra);
            if (dist > out.size() || maxOutput - out.size() < length) return false;
            const std::size_t from = out.size() - dist;
            for (std::size_t i = 0; i < length; ++i) out.push_back(out[from + i]);
        }
    }
}

bool Fixed(BitReader& in, std::size_t maxOutput, std::vector<std::uint8_t>& out) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        std::uint16_t lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        Build(lencode, lengths, 288);
        for (i = 0; i < 30; ++i) lengths[i] = 5;
        Build(distcode, lengths, 30);
        built = true;
    }
    return Codes(in, lencode, distcode, maxOutput, out);
}

bool Dynamic(BitReader& in, std::size_t maxOutput, std::vector<std::uint8_t>& out) {
    static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int nlen, ndist, ncode;
    if (!in.Bits(5, nlen) || !in.Bits(5, ndist) || !in.Bits(4, ncode)) return false;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) return false;
    std::uint16_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) {
        int v;
        if (!in.Bits(3, v)) return false;
        lengths[kOrder[i]] = static_cast<std::uint16_t>(v);
    }
    Huffman lencode, distcode;
    if (!Build(lencode, lengths, 19)) return false;
    int index = 0;
    while (index < nlen + ndist) {
        int symbol;
        if (!in.Decode(lencode, symbol)) return false;
        if (symbol < 16) {
            lengths[index++] = static_cast<std::uint16_t>(symbol);
            continue;
        }
        std::uint16_t value = 0;
        int repeat;
        if (symbol == 16) {
            if (index == 0) return false;
            value = lengths[index - 1];
            if (!in.Bits(2, repeat)) return false;
            repeat += 3;
        } else if (symbol == 17) {
            if (!in.Bits(3, repeat)) return false;
            repeat += 3;
        } else {
            if (!in.Bits(7, repeat)) return false;
            repeat += 11;
        }
        if (index + repeat > nlen + ndist) return false;
        while (repeat--) lengths[index++] = value;
    }
    if (lengths[256] == 0) return false;
    if (!Build(lencode, lengths, nlen)) return false;
    if (!Build(distcode, lengths + nlen, ndist)) return false;
    return Codes(in, lencode, distcode, maxOutput, out);
}

std::uint16_t U16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t U32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

bool Inflate(const std::uint8_t* data, std::size_t size, std::size_t maxOutput, std::vector<std::uint8_t>& out) {
    out.clear();
    BitReader in(data, size);
    int last;
    do {
        int type;
        if (!in.Bits(1, last) || !in.Bits(2, type)) return false;
        if (type == 0) {
            in.AlignToByte();
            const std::uint8_t* header;
            if (!in.Bytes(4, header)) return false;
            const std::uint16_t len = U16(header);
            if (static_cast<std::uint16_t>(~U16(header + 2)) != len) return false;
            const std::uint8_t* block;
            if (!in.Bytes(len, block) || maxOutput - out.size() < len) return false;
            out.insert(out.end(), block, block + len);
        } else if (type == 1) {
            if (!Fixed(in, maxOutput, out)) return false;
        } else if (type == 2) {
            if (!Dynamic(in, maxOutput, out)) return false;
        } else {
            return false;
        }
    } while (!last);
    return true;
}

bool ListZip(const std::vector<std::uint8_t>& zip, std::vector<ZipEntry>& entries) {
    entries.clear();
    const std::size_t n = zip.size();
    if (n < 22) return false;
    // The end record sits in the last 22 + 65535 (comment) bytes.
    std::size_t end = n - 22;
    const std::size_t stop = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (;;) {
        if (U32(&zip[end]) == 0x06054b50) break;
        if (end == stop) return false;
        --end;
    }
    const std::uint16_t count = U16(&zip[end + 10]);
    const std::uint32_t dirSize = U32(&zip[end + 12]);
    std::size_t pos = U32(&zip[end + 16]);
    if (pos > n || dirSize > n - pos) return false;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > n || U32(&zip[pos]) != 0x02014b50) return false;
        ZipEntry entry;
        if (U16(&zip[pos + 8]) & 1) return false;  // encrypted
        entry.method = U16(&zip[pos + 10]);
        entry.compressedSize = U32(&zip[pos + 20]);
        entry.size = U32(&zip[pos + 24]);
        const std::uint16_t nameLength = U16(&zip[pos + 28]);
        const std::uint16_t extraLength = U16(&zip[pos + 30]);
        const std::uint16_t commentLength = U16(&zip[pos + 32]);
        entry.localOffset = U32(&zip[pos + 42]);
        if (pos + 46 + nameLength > n) return false;
        entry.name.assign(reinterpret_cast<const char*>(&zip[pos + 46]), nameLength);
        entries.push_back(std::move(entry));
        pos += 46u + nameLength + extraLength + commentLength;
    }
    return true;
}

bool ReadZipEntry(const std::vector<std::uint8_t>& zip, const ZipEntry& entry, std::size_t maxOutput,
                  std::vector<std::uint8_t>& out) {
    out.clear();
    const std::size_t n = zip.size();
    const std::size_t local = entry.localOffset;
    if (local > n || n - local < 30 || U32(&zip[local]) != 0x04034b50) return false;
    const std::size_t data = local + 30 + U16(&zip[local + 26]) + U16(&zip[local + 28]);
    if (data > n || n - data < entry.compressedSize) return false;
    const std::uint8_t* begin = zip.data() + data;
    if (entry.method == 0) {
        if (entry.compressedSize > maxOutput) return false;
        out.assign(begin, begin + entry.compressedSize);
        return true;
    }
    if (entry.method != 8) return false;
    if (entry.size <= maxOutput) out.reserve(entry.size);
    return Inflate(begin, entry.compressedSize, maxOutput, out);
}
