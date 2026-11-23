#include "dll_extract.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace mfg::lsfg
{
namespace
{
struct Reader
{
    const uint8_t* data;
    size_t size;
};

[[noreturn]] void fail(const std::string& what)
{
    throw std::runtime_error("PE extract: " + what);
}

uint16_t rd16(const Reader& r, size_t off)
{
    if (off + 2 > r.size)
        fail("read16 out of bounds");
    uint16_t v;
    std::memcpy(&v, r.data + off, 2);
    return v;
}

uint32_t rd32(const Reader& r, size_t off)
{
    if (off + 4 > r.size)
        fail("read32 out of bounds");
    uint32_t v;
    std::memcpy(&v, r.data + off, 4);
    return v;
}

uint32_t resDirEntryCount(const Reader& r, size_t dir)
{
    // named entries (12 bytes each) + id entries (14 bytes each) follow
    // the 16-byte directory header
    return static_cast<uint32_t>(rd16(r, dir + 12)) + rd16(r, dir + 14);
}
} // namespace

std::map<uint32_t, std::vector<uint8_t>> ExtractRcData(const std::filesystem::path& peFile)
{
    FILE* f = std::fopen(peFile.string().c_str(), "rb");
    if (!f)
        fail("cannot open " + peFile.string());

    std::vector<uint8_t> bytes;
    {
        std::fseek(f, 0, SEEK_END);
        const long len = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (len <= 0)
        {
            std::fclose(f);
            fail("empty file");
        }
        bytes.resize(static_cast<size_t>(len));
        if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size())
        {
            std::fclose(f);
            fail("short read");
        }
        std::fclose(f);
    }

    const Reader r{bytes.data(), bytes.size()};

    if (rd16(r, 0) != 0x5A4D)
        fail("not an Mi image");
    const size_t peOff = rd32(r, 0x3C);
    if (rd32(r, peOff) != 0x00004550)
        fail("not a PE image");
    const uint32_t sectCount = rd16(r, peOff + 6);
    const uint32_t optSize = rd16(r, peOff + 20);
    const size_t opt = peOff + 24;
    if (rd16(r, opt) != 0x20B)
        fail("not PE32+");

    // data directory #2 (resources): RVA at optional header offset 128
    const uint32_t resRva = rd32(r, opt + 128);

    // find the section holding the resource directory
    const size_t sects = opt + optSize;
    size_t resRaw = 0;
    bool foundSect = false;
    for (uint32_t i = 0; i < sectCount; ++i)
    {
        const size_t s = sects + 40 * i;
        const uint32_t vsize = rd32(r, s + 8);
        const uint32_t vaddr = rd32(r, s + 12);
        const uint32_t foff = rd32(r, s + 20);
        if (resRva >= vaddr && resRva < vaddr + vsize)
        {
            resRaw = resRva - vaddr + foff;
            foundSect = true;
            break;
        }
    }
    if (!foundSect)
        fail("resource section not found");

    // top level: find the RT_RCDATA (id 10) directory
    size_t rcdataDir = 0;
    bool foundRc = false;
    const uint32_t top = resDirEntryCount(r, resRaw);
    for (uint32_t i = 0; i < top; ++i)
    {
        const size_t e = resRaw + 16 + 8 * i;
        const uint32_t id = rd32(r, e);
        const uint32_t off = rd32(r, e + 4);
        if (id != 10)
            continue;
        if ((off & 0x80000000) == 0)
            fail("RT_RCDATA entry is not a directory");
        rcdataDir = resRaw + (off & 0x7FFFFFFF);
        foundRc = true;
    }
    if (!foundRc)
        fail("no RT_RCDATA directory");

    std::map<uint32_t, std::vector<uint8_t>> out;
    const uint32_t rc = resDirEntryCount(r, rcdataDir);
    for (uint32_t i = 0; i < rc; ++i)
    {
        const size_t e = rcdataDir + 16 + 8 * i;
        const uint32_t id = rd32(r, e);
        const uint32_t off = rd32(r, e + 4);
        if ((off & 0x80000000) == 0)
            fail("RCDATA entry is not a directory");
        // language directory -> actual data entry
        const size_t langDir = resRaw + (off & 0x7FFFFFFF);
        const uint32_t langCount = resDirEntryCount(r, langDir);
        if (langCount < 1)
            fail("empty language directory");
        const size_t langEntry = langDir + 16 + 4;
        const uint32_t dataOff = rd32(r, langEntry);
        if ((dataOff & 0x80000000) != 0)
            fail("language entry is not a data entry");
        const size_t dataEntry = resRaw + (dataOff & 0x7FFFFFFF);
        const uint32_t rva = rd32(r, dataEntry + 0);
        const uint32_t size = rd32(r, dataEntry + 4);
        const size_t fo = rva - resRva + resRaw;
        if (fo + size > r.size)
            fail("resource data outside file");
        out.emplace(id, std::vector<uint8_t>(bytes.begin() + fo, bytes.begin() + fo + size));
    }
    return out;
}
} // namespace mfg::lsfg
