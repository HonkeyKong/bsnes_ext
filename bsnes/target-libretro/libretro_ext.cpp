#include "libretro_ext.h"
#include <sfc/sfc.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* CpuTag = ":maincpu";

struct WatchRule { uint64_t start, end; uint8_t access, width; };
struct InjectRule {
  uint64_t start, end;
  uint32_t value, matchValue, matchMask;
  uint8_t width;
  bool oneShot, hasMatchValue;
};

bool debugEnabled = false;
uint64_t frameNumber = 0;
std::vector<WatchRule> watchRules;
std::vector<InjectRule> injectRules;
libretro_ext_watch_hit lastWatchHit{};
uint64_t pcHistory[256]{};
uint32_t pcHistoryHead = 0;
uint32_t pcHistoryCount = 0;

bool loaded() { return SuperFamicom::system.loaded(); }
bool validCpu(const char* tag) { return loaded() && tag && !std::strcmp(tag, CpuTag); }
bool validSpace(const char* space) { return space && !std::strcmp(space, "program"); }
bool validWidth(uint8_t width) { return width == 0 || width == 1 || width == 2 || width == 4; }

const char* getDriverName() { return loaded() ? "bsnes" : nullptr; }
int getCpuCount() { return loaded() ? 1 : 0; }
const char* getCpuTag(int index) { return loaded() && index == 0 ? CpuTag : nullptr; }
uint64_t getCpuPc(int index) { return loaded() && index == 0 ? (uint32_t)SuperFamicom::cpu.r.pc.d : 0; }
uint64_t getCpuPcByTag(const char* tag) { return validCpu(tag) ? (uint32_t)SuperFamicom::cpu.r.pc.d : 0; }

uint8_t read8(const char* tag, const char* space, uint64_t address) {
  if(!validCpu(tag) || !validSpace(space) || address > 0xffffff) return 0;
  return SuperFamicom::bus.read((uint32_t)address);
}
uint16_t read16(const char* tag, const char* space, uint64_t address) {
  if(!validCpu(tag) || !validSpace(space) || address > 0xfffffe) return 0;
  return read8(tag, space, address) | read8(tag, space, address + 1) << 8;
}
uint32_t read32(const char* tag, const char* space, uint64_t address) {
  if(!validCpu(tag) || !validSpace(space) || address > 0xfffffc) return 0;
  return read16(tag, space, address) | (uint32_t)read16(tag, space, address + 2) << 16;
}
void write8(const char* tag, const char* space, uint64_t address, uint8_t value) {
  if(validCpu(tag) && validSpace(space) && address <= 0xffffff) SuperFamicom::bus.write((uint32_t)address, value);
}
void write16(const char* tag, const char* space, uint64_t address, uint16_t value) {
  if(!validCpu(tag) || !validSpace(space) || address > 0xfffffe) return;
  write8(tag, space, address, value); write8(tag, space, address + 1, value >> 8);
}
void write32(const char* tag, const char* space, uint64_t address, uint32_t value) {
  if(!validCpu(tag) || !validSpace(space) || address > 0xfffffc) return;
  write16(tag, space, address, value); write16(tag, space, address + 2, value >> 16);
}

struct Region { const char* tag; uint8_t* data; uint64_t size; bool writable; };
bool regionAt(int index, Region& out) {
  if(!loaded()) return false;
  int current = 0;
  auto select = [&](const char* tag, uint8_t* data, uint64_t size, bool writable) {
    if(!size || current++ != index) return false;
    out = {tag, data, size, writable}; return true;
  };
  if(select("sfc.rom", SuperFamicom::cartridge.rom.data(), SuperFamicom::cartridge.rom.size(), true)) return true;
  if(select("sfc.ram", SuperFamicom::cartridge.ram.data(), SuperFamicom::cartridge.ram.size(), true)) return true;
  if(select("sfc.wram", SuperFamicom::cpu.wram, sizeof SuperFamicom::cpu.wram, true)) return true;
  //PPU memories have no flat byte backing (and differ between the fast and accurate PPU), so
  //they are served by ppuRead/ppuWrite below. data == nullptr marks such a region.
  if(select("sfc.vram", nullptr, 64 * 1024, true)) return true;
  if(select("sfc.cgram", nullptr, 512, true)) return true;
  return false;
}
bool findRegion(const char* tag, Region& out) {
  if(!tag) return false;
  for(int index = 0; regionAt(index, out); index++) if(!std::strcmp(tag, out.tag)) return true;
  return false;
}
int getRegionCount() { Region region{}; int count = 0; while(regionAt(count, region)) count++; return count; }
const char* getRegionTag(int index) { Region region{}; return regionAt(index, region) ? region.tag : nullptr; }
uint64_t getRegionSize(const char* tag) { Region region{}; return findRegion(tag, region) ? region.size : 0; }
//VRAM is 32K little-endian words; CGRAM is 256 15-bit colors stored as little-endian words.
uint16_t ppuWord(bool vram, uint32_t index) {
  namespace S = SuperFamicom;
  if(S::system.fastPPU()) return vram ? S::ppufast.vram[index & 0x7fff] : S::ppufast.cgram[index & 0xff];
  return vram ? (uint16_t)S::ppu.extVRAM(index) : (uint16_t)S::ppu.extCGRAM(index);
}
void ppuSetWord(bool vram, uint32_t index, uint16_t value) {
  namespace S = SuperFamicom;
  if(S::system.fastPPU()) {
    if(vram) S::ppufast.vram[index & 0x7fff] = value; else S::ppufast.cgram[index & 0xff] = value & 0x7fff;
  } else {
    if(vram) S::ppu.extVRAM(index) = value; else S::ppu.extCGRAM(index) = value & 0x7fff;
  }
}
uint64_t ppuRead(bool vram, uint64_t offset, uint8_t* out, uint64_t bytes) {
  for(uint64_t n = 0; n < bytes; n++) out[n] = ppuWord(vram, (offset + n) >> 1) >> ((offset + n) & 1) * 8;
  return bytes;
}
uint64_t ppuWrite(bool vram, uint64_t offset, const uint8_t* in, uint64_t bytes) {
  //the fast PPU renders scanlines lazily from live VRAM/CGRAM; render any already-latched lines
  //with the old contents first, exactly as its own $2118/$2122 write handlers do.
  if(SuperFamicom::system.fastPPU()) SuperFamicom::PPUfast::Line::flush();
  for(uint64_t n = 0; n < bytes; n++) {
    const uint32_t index = (offset + n) >> 1; const uint8_t shift = ((offset + n) & 1) * 8;
    ppuSetWord(vram, index, (ppuWord(vram, index) & ~(0xff << shift)) | in[n] << shift);
  }
  return bytes;
}
uint64_t readRegion(const char* tag, uint64_t offset, void* destination, uint64_t bytes) {
  Region region{}; if(!destination || !findRegion(tag, region) || offset >= region.size) return 0;
  bytes = std::min(bytes, region.size - offset);
  if(!region.data) return ppuRead(!std::strcmp(region.tag, "sfc.vram"), offset, (uint8_t*)destination, bytes);
  std::memcpy(destination, region.data + offset, bytes); return bytes;
}
uint64_t writeRegion(const char* tag, uint64_t offset, const void* source, uint64_t bytes) {
  Region region{}; if(!source || !findRegion(tag, region) || !region.writable || offset >= region.size) return 0;
  bytes = std::min(bytes, region.size - offset);
  if(!region.data) return ppuWrite(!std::strcmp(region.tag, "sfc.vram"), offset, (const uint8_t*)source, bytes);
  std::memcpy(region.data + offset, source, bytes); return bytes;
}

uint64_t getFrameNumber() { return loaded() ? frameNumber : 0; }
uint64_t getTimeAttoseconds() {
  if(!loaded()) return 0;
  const long double fps = SuperFamicom::system.region() == SuperFamicom::System::Region::PAL ? 50.0069789082L : 60.0988138974L;
  const long double value = frameNumber * 1000000000000000000.0L / fps;
  return value <= UINT64_MAX ? (uint64_t)value : 0;
}
uint64_t getCpuCycles(const char*) { return 0; }

void clearLastWatchHit() { std::memset(&lastWatchHit, 0, sizeof lastWatchHit); }
void clearWatchRules() { watchRules.clear(); clearLastWatchHit(); }
void addWatchRule(const char* tag, uint64_t start, uint64_t end, uint8_t access, uint8_t width) {
  if(!debugEnabled || !validCpu(tag) || start > end || end > 0xffffff || !(access & 3) || !validWidth(width)) return;
  watchRules.push_back({start, end, (uint8_t)(access & 3), width});
}
bool getLastWatchHit(libretro_ext_watch_hit* out) {
  if(!loaded() || !out || !lastWatchHit.hit) return false;
  *out = lastWatchHit; return true;
}
void checkWatchHit(const char* tag, uint64_t pc, uint64_t address, uint32_t value, uint8_t access, uint8_t width, uint64_t cycles) {
  if(!debugEnabled || !validCpu(tag)) return;
  for(const auto& rule : watchRules) {
    if(address < rule.start || address > rule.end || !(rule.access & access) || (rule.width && rule.width != width)) continue;
    clearLastWatchHit(); lastWatchHit.hit = true;
    std::strncpy(lastWatchHit.cpuTag, tag, sizeof lastWatchHit.cpuTag - 1);
    lastWatchHit.address = address; lastWatchHit.pc = pc; lastWatchHit.frame = frameNumber;
    lastWatchHit.totalCycles = cycles; lastWatchHit.value = value; lastWatchHit.access = access; lastWatchHit.width = width;
    lastWatchHit.historyCount = pcHistoryCount;
    const uint32_t oldest = pcHistoryCount == 256 ? pcHistoryHead : 0;
    for(uint32_t n = 0; n < pcHistoryCount; n++) lastWatchHit.pcHistory[n] = pcHistory[(oldest + n) & 255];
    return;
  }
}

void clearInjectRules() { injectRules.clear(); }
void addInjectRuleEx(const char* tag, uint64_t start, uint64_t end, uint32_t value, uint8_t width, bool oneShot,
                     bool hasMatchValue, uint32_t matchValue, uint32_t matchMask) {
  if(!debugEnabled || !validCpu(tag) || start > end || end > 0xffffff || !validWidth(width)) return;
  injectRules.push_back({start, end, value, matchValue, matchMask, width, oneShot, hasMatchValue});
}
void addInjectRule(const char* tag, uint64_t start, uint64_t end, uint32_t value, uint8_t width, bool oneShot) {
  addInjectRuleEx(tag, start, end, value, width, oneShot, false, 0, 0);
}
void setDebugEnabled(bool enabled) {
  debugEnabled = enabled; clearWatchRules(); clearInjectRules();
  pcHistoryHead = pcHistoryCount = 0; std::memset(pcHistory, 0, sizeof pcHistory);
}
bool getDebugEnabled() { return debugEnabled; }

bool readState(int index, int state, uint64_t* out) {
  if(!out || !loaded() || index != 0) return false;
  if(state == LIBRETRO_EXT_STATE_GENPC || state == LIBRETRO_EXT_STATE_GENPCBASE) *out = (uint32_t)SuperFamicom::cpu.r.pc.d;
  else if(state == LIBRETRO_EXT_STATE_GENFLAGS) *out = (uint32_t)SuperFamicom::cpu.r.p;
  else return false;
  return true;
}
bool readStateTag(const char* tag, int state, uint64_t* out) { return validCpu(tag) && readState(0, state, out); }
bool writeState(int index, int state, uint64_t value) {
  if(!loaded() || index != 0) return false;
  if(state == LIBRETRO_EXT_STATE_GENPC || state == LIBRETRO_EXT_STATE_GENPCBASE) SuperFamicom::cpu.r.pc = (uint32_t)value & 0xffffff;
  else if(state == LIBRETRO_EXT_STATE_GENFLAGS) SuperFamicom::cpu.r.p = (uint8_t)value;
  else return false;
  return true;
}
bool writeStateTag(const char* tag, int state, uint64_t value) { return validCpu(tag) && writeState(0, state, value); }

bool readRegister(const char* tag, const char* name, uint64_t* out) {
  if(!validCpu(tag) || !name || !out) return false;
  if(!std::strcmp(name, "PC") || !std::strcmp(name, "GENPC") || !std::strcmp(name, "GENPCBASE")) *out = (uint32_t)SuperFamicom::cpu.r.pc.d;
  else if(!std::strcmp(name, "A")) *out = SuperFamicom::cpu.r.a.w;
  else if(!std::strcmp(name, "X")) *out = SuperFamicom::cpu.r.x.w;
  else if(!std::strcmp(name, "Y")) *out = SuperFamicom::cpu.r.y.w;
  else if(!std::strcmp(name, "S") || !std::strcmp(name, "SP")) *out = SuperFamicom::cpu.r.s.w;
  else if(!std::strcmp(name, "D")) *out = SuperFamicom::cpu.r.d.w;
  else if(!std::strcmp(name, "B")) *out = SuperFamicom::cpu.r.b;
  else if(!std::strcmp(name, "P")) *out = (uint32_t)SuperFamicom::cpu.r.p;
  else return false;
  return true;
}
bool writeRegister(const char* tag, const char* name, uint64_t value) {
  if(!validCpu(tag) || !name) return false;
  if(!std::strcmp(name, "PC") || !std::strcmp(name, "GENPC") || !std::strcmp(name, "GENPCBASE")) SuperFamicom::cpu.r.pc = (uint32_t)value & 0xffffff;
  else if(!std::strcmp(name, "A")) SuperFamicom::cpu.r.a = value;
  else if(!std::strcmp(name, "X")) SuperFamicom::cpu.r.x = value;
  else if(!std::strcmp(name, "Y")) SuperFamicom::cpu.r.y = value;
  else if(!std::strcmp(name, "S") || !std::strcmp(name, "SP")) SuperFamicom::cpu.r.s = value;
  else if(!std::strcmp(name, "D")) SuperFamicom::cpu.r.d = value;
  else if(!std::strcmp(name, "B")) SuperFamicom::cpu.r.b = value;
  else if(!std::strcmp(name, "P")) SuperFamicom::cpu.r.p = value;
  else return false;
  return true;
}

// --- execution traps ---------------------------------------------------------------
// Sparse 24-bit address bitset (2 MiB, allocated on first use) so the per-instruction cost
// with traps registered is one bit test, and zero (a bool test) with none registered.
std::vector<uint64_t> trapBits;
uint32_t trapCount = 0;
libretro_ext_exec_trap_cb trapCallback = nullptr;
void* trapUser = nullptr;

void updateTrapsActive() { SuperFamicom::libretroExtExecTrapsActive = trapCount && trapCallback; }
void setExecTrapCallback(libretro_ext_exec_trap_cb cb, void* user) { trapCallback = cb; trapUser = user; updateTrapsActive(); }
bool addExecTrap(const char* tag, uint64_t address) {
  if(!validCpu(tag) || address > 0xffffff) return false;
  if(trapBits.empty()) trapBits.assign((1u << 24) / 64, 0);
  uint64_t& word = trapBits[address >> 6]; const uint64_t bit = 1ull << (address & 63);
  if(!(word & bit)) { word |= bit; trapCount++; }
  updateTrapsActive(); return true;
}
bool removeExecTrap(const char* tag, uint64_t address) {
  if(!validCpu(tag) || address > 0xffffff || trapBits.empty()) return false;
  uint64_t& word = trapBits[address >> 6]; const uint64_t bit = 1ull << (address & 63);
  if(!(word & bit)) return false;
  word &= ~bit; trapCount--; updateTrapsActive(); return true;
}
void clearExecTraps() { trapBits.clear(); trapBits.shrink_to_fit(); trapCount = 0; updateTrapsActive(); }
bool cpuExecuteOpcode(const char* tag, uint8_t opcode) {
  //only legal on the CPU cothread, i.e. from inside a trap callback. deliberately not a flag:
  //bsnes state loads recreate (synchronized) or rewind (stack-serialized) the cothread, which
  //would leave a frontend-side flag stale.
  if(!validCpu(tag) || !SuperFamicom::cpu.active()) return false;
  SuperFamicom::cpu.executeOpcode(opcode); return true;
}

// --- BG tile overrides ----------------------------------------------------------------
constexpr uint32_t MaxTileOverrides = 4096;
std::vector<uint16_t> freeOverrideSlots;
uint32_t overrideCount = 0;

void beforeOverrideChange() {
  //render any scanlines the fast PPU has latched but not drawn yet with the current state
  if(SuperFamicom::system.fastPPU()) SuperFamicom::PPUfast::Line::flush();
}
void resetTileOverrides() {
  std::memset(SuperFamicom::libretroExtTileOverrideIndex, 0, sizeof SuperFamicom::libretroExtTileOverrideIndex);
  freeOverrideSlots.clear();
  for(uint32_t n = MaxTileOverrides; n > 0; n--) freeOverrideSlots.push_back((uint16_t)n);  //slot ids are 1-based
  overrideCount = 0; SuperFamicom::libretroExtTileOverridesActive = false;
}
bool setBgTileOverride(uint32_t address, uint16_t expect, const uint8_t* pixels) {
  if(!loaded() || !pixels || address > 0x7fff || !SuperFamicom::system.fastPPU()) return false;
  if(freeOverrideSlots.empty() && !overrideCount) resetTileOverrides();
  uint16_t& index = SuperFamicom::libretroExtTileOverrideIndex[address];
  if(!index) { if(freeOverrideSlots.empty()) return false; index = freeOverrideSlots.back(); freeOverrideSlots.pop_back(); overrideCount++; }
  beforeOverrideChange();
  auto& o = SuperFamicom::libretroExtTileOverrideSlots[index - 1];
  o.expect = expect;
  //planarize each row into the fetch layout: word k holds bitplanes 2k (low byte) and 2k+1 (high byte)
  for(uint32_t row = 0; row < 8; row++) {
    uint64_t data = 0;
    for(uint32_t col = 0; col < 8; col++) {
      const uint8_t color = pixels[row * 8 + col], bit = 7 - col;
      for(uint32_t plane = 0; plane < 8; plane++)
        if(color >> plane & 1) data |= 1ull << ((plane >> 1) * 16 + (plane & 1) * 8 + bit);
    }
    o.rows[row] = data;
  }
  SuperFamicom::libretroExtTileOverridesActive = true;
  return true;
}
bool clearBgTileOverride(uint32_t address) {
  if(address > 0x7fff) return false;
  uint16_t& index = SuperFamicom::libretroExtTileOverrideIndex[address];
  if(!index) return false;
  beforeOverrideChange();
  freeOverrideSlots.push_back(index); index = 0;
  if(!--overrideCount) SuperFamicom::libretroExtTileOverridesActive = false;
  return true;
}
void clearBgTileOverrides() { if(loaded()) beforeOverrideChange(); resetTileOverrides(); }

const libretro_ext_api extApi = [] {
  libretro_ext_api table{};
  table.abi_version = 5; table.sizeof_struct = sizeof table;
  table.get_driver_name = getDriverName; table.get_cpu_count = getCpuCount; table.get_cpu_tag = getCpuTag; table.get_cpu_pc = getCpuPc;
  table.read_u8 = read8; table.read_u16 = read16; table.read_u32 = read32;
  table.write_u8 = write8; table.write_u16 = write16; table.write_u32 = write32;
  table.get_region_count = getRegionCount; table.get_region_tag = getRegionTag; table.get_region_size = getRegionSize;
  table.read_region = readRegion; table.write_region = writeRegion;
  table.get_frame_number = getFrameNumber; table.get_time_attoseconds = getTimeAttoseconds; table.get_cpu_total_cycles_by_tag = getCpuCycles;
  table.clear_watch_rules = clearWatchRules; table.add_watch_rule = addWatchRule; table.get_last_watch_hit = getLastWatchHit;
  table.clear_last_watch_hit = clearLastWatchHit; table.check_watch_hit = checkWatchHit;
  table.set_debug_extensions_enabled = setDebugEnabled; table.get_debug_extensions_enabled = getDebugEnabled;
  table.get_cpu_pc_by_tag = getCpuPcByTag; table.read_cpu_state_u64_by_index = readState;
  table.read_cpu_state_u64_by_tag = readStateTag; table.read_cpu_register_by_tag = readRegister;
  table.write_cpu_state_u64_by_index = writeState; table.write_cpu_state_u64_by_tag = writeStateTag;
  table.write_cpu_register_by_tag = writeRegister; table.add_inject_rule = addInjectRule;
  table.clear_inject_rules = clearInjectRules; table.add_inject_rule_ex = addInjectRuleEx;
  table.set_exec_trap_callback = setExecTrapCallback; table.add_exec_trap = addExecTrap;
  table.remove_exec_trap = removeExecTrap; table.clear_exec_traps = clearExecTraps;
  table.cpu_execute_opcode = cpuExecuteOpcode;
  table.set_bg_tile_override = setBgTileOverride; table.clear_bg_tile_override = clearBgTileOverride;
  table.clear_bg_tile_overrides = clearBgTileOverrides;
  return table;
}();

}

void libretro_ext_reset() {
  frameNumber = 0; clearWatchRules(); clearInjectRules(); clearLastWatchHit(); clearExecTraps(); resetTileOverrides();
  pcHistoryHead = pcHistoryCount = 0; std::memset(pcHistory, 0, sizeof pcHistory);
}
void libretro_ext_frame_complete() { if(loaded()) frameNumber++; }

namespace SuperFamicom {
bool libretroExtExecTrapsActive = false;
bool libretroExtTileOverridesActive = false;
uint16_t libretroExtTileOverrideIndex[32768] = {};
LibretroExtTileOverride libretroExtTileOverrideSlots[MaxTileOverrides] = {};
auto libretroExtExecTrap(uint32_t pc) -> bool {
  pc &= 0xffffff;
  //no re-entrancy guard needed: cpu_execute_opcode() calls dispatch() directly, never CPU::main().
  if(!(trapBits[pc >> 6] >> (pc & 63) & 1) || !loaded()) return false;
  const uint8_t opcode = bus.read(pc, cpu.r.mdr);  //ROM/WRAM peek; traps are not meant for MMIO
  const uint32_t result = trapCallback(trapUser, CpuTag, pc, opcode);
  return result == LIBRETRO_EXT_TRAP_HANDLED;
}
auto libretroExtCpuExecute(uint32_t pc) -> void {
  if(!debugEnabled || !loaded()) return;
  pcHistory[pcHistoryHead] = pc; pcHistoryHead = (pcHistoryHead + 1) & 255;
  if(pcHistoryCount < 256) pcHistoryCount++;
}
auto libretroExtCpuRead(uint32_t pc, uint32_t address, uint8_t value) -> uint8_t {
  if(!debugEnabled || !loaded()) return value;
  checkWatchHit(CpuTag, pc, address, value, LIBRETRO_EXT_WATCH_READ, 1, 0);
  for(size_t index = 0; index < injectRules.size(); index++) {
    auto& rule = injectRules[index];
    if(address < rule.start || address > rule.end || (rule.width && rule.width != 1)) continue;
    if(rule.hasMatchValue && (value & rule.matchMask) != (rule.matchValue & rule.matchMask)) continue;
    value = (uint8_t)rule.value;
    if(rule.oneShot) injectRules.erase(injectRules.begin() + index);
    break;
  }
  return value;
}
auto libretroExtCpuWrite(uint32_t pc, uint32_t address, uint8_t value) -> void {
  checkWatchHit(CpuTag, pc, address, value, LIBRETRO_EXT_WATCH_WRITE, 1, 0);
}
}

LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api(void) { return &extApi; }
LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api_v5(void) { return &extApi; }
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_abi_version(void) { return 5; }
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_struct_size(void) { return sizeof(extApi); }
