#ifndef BSNES_LIBRETRO_EXT_H
#define BSNES_LIBRETRO_EXT_H

#include <cstdint>

#if defined(_WIN32)
#define LIBRETRO_EXT_EXPORT extern "C" __declspec(dllexport)
#else
#define LIBRETRO_EXT_EXPORT extern "C" __attribute__((visibility("default")))
#endif

enum libretro_ext_watch_access : uint8_t {
  LIBRETRO_EXT_WATCH_READ = 1,
  LIBRETRO_EXT_WATCH_WRITE = 2,
};

enum libretro_ext_cpu_state_id : int32_t {
  LIBRETRO_EXT_STATE_GENPC = -1,
  LIBRETRO_EXT_STATE_GENPCBASE = -2,
  LIBRETRO_EXT_STATE_GENFLAGS = -3,
};

struct libretro_ext_watch_hit {
  bool hit;
  char cpuTag[64];
  uint64_t address;
  uint64_t pc;
  uint64_t frame;
  uint64_t totalCycles;
  uint32_t value;
  uint8_t access;
  uint8_t width;
  uint32_t historyCount;
  uint64_t pcHistory[256];
};

#define LIBRETRO_EXT_DIP_NAME_LEN 128
#define LIBRETRO_EXT_DIP_PORT_TAG_LEN 64
#define LIBRETRO_EXT_DIP_MAX_SETTINGS 64
#define LIBRETRO_EXT_STATIC_REGION_NAME_LEN 128
#define LIBRETRO_EXT_STATIC_REGION_TAG_LEN 64
#define LIBRETRO_EXT_STATIC_REGION_CPU_TAG_LEN 64
#define LIBRETRO_EXT_STATIC_REGION_SPACE_LEN 32
#define LIBRETRO_EXT_SHARE_TAG_LEN 64

struct libretro_ext_dip_setting { char name[LIBRETRO_EXT_DIP_NAME_LEN]; uint32_t value; };
struct libretro_ext_dip_info {
  char name[LIBRETRO_EXT_DIP_NAME_LEN];
  char port_tag[LIBRETRO_EXT_DIP_PORT_TAG_LEN];
  uint32_t mask, current_value, default_value, setting_count;
  libretro_ext_dip_setting settings[LIBRETRO_EXT_DIP_MAX_SETTINGS];
};
struct libretro_ext_static_region_info {
  char name[LIBRETRO_EXT_STATIC_REGION_NAME_LEN];
  char region_tag[LIBRETRO_EXT_STATIC_REGION_TAG_LEN];
  char cpu_tag[LIBRETRO_EXT_STATIC_REGION_CPU_TAG_LEN];
  char space[LIBRETRO_EXT_STATIC_REGION_SPACE_LEN];
  uint64_t offset, size;
  uint32_t flags;
};
struct libretro_ext_share_info { char share_tag[LIBRETRO_EXT_SHARE_TAG_LEN]; uint64_t size; uint32_t flags; };

// Execution traps (optional v5 tail fields).
//
// A trap fires synchronously at an instruction boundary when the CPU is about to fetch an
// opcode from a registered address (exact 24-bit bus address; register mirrors separately).
// Interrupts are dispatched before the check, so a trap never pre-empts an NMI/IRQ.
//
// The callback runs on the emulated CPU's cothread (bsnes: 32 KiB stack). Inside it the
// frontend may use the register and memory APIs, and cpu_execute_opcode() to run one
// instruction with a substitute opcode. Keep it short and non-blocking; queue heavy work.
// It is re-invoked whenever emulation re-executes that address (run-ahead, rewind,
// state loads), so handlers must be a pure function of emulated state.
// bsnes run-ahead snapshots raw cothread stacks: a frame can end while the CPU is suspended
// inside a callback (e.g. within cpu_execute_opcode), and restoring that snapshot resumes the
// callback mid-body with its saved locals. Frontend-side state mutated by a handler must
// therefore be snapshotted alongside core state, or derived purely from emulated memory.
enum libretro_ext_trap_result : uint32_t {
  LIBRETRO_EXT_TRAP_CONTINUE = 0, // execute the instruction at the (possibly updated) PC normally
  LIBRETRO_EXT_TRAP_HANDLED = 1,  // instruction step is complete; resume at the current PC
};
typedef uint32_t (*libretro_ext_exec_trap_cb)(void* user, const char* cpu_tag, uint64_t address, uint8_t opcode);

struct libretro_ext_api {
  uint32_t abi_version;
  uint32_t sizeof_struct;
  const char* (*get_driver_name)();
  int (*get_cpu_count)();
  const char* (*get_cpu_tag)(int);
  uint64_t (*get_cpu_pc)(int);
  uint8_t (*read_u8)(const char*, const char*, uint64_t);
  uint16_t (*read_u16)(const char*, const char*, uint64_t);
  uint32_t (*read_u32)(const char*, const char*, uint64_t);
  void (*write_u8)(const char*, const char*, uint64_t, uint8_t);
  void (*write_u16)(const char*, const char*, uint64_t, uint16_t);
  void (*write_u32)(const char*, const char*, uint64_t, uint32_t);
  int (*get_region_count)();
  const char* (*get_region_tag)(int);
  uint64_t (*get_region_size)(const char*);
  uint64_t (*read_region)(const char*, uint64_t, void*, uint64_t);
  uint64_t (*write_region)(const char*, uint64_t, const void*, uint64_t);
  uint64_t (*get_frame_number)();
  uint64_t (*get_time_attoseconds)();
  uint64_t (*get_cpu_total_cycles_by_tag)(const char*);
  void (*clear_watch_rules)();
  void (*add_watch_rule)(const char*, uint64_t, uint64_t, uint8_t, uint8_t);
  bool (*get_last_watch_hit)(libretro_ext_watch_hit*);
  void (*clear_last_watch_hit)();
  void (*check_watch_hit)(const char*, uint64_t, uint64_t, uint32_t, uint8_t, uint8_t, uint64_t);
  int (*get_dip_count)();
  bool (*get_dip_info)(int, libretro_ext_dip_info*);
  bool (*set_dip_value)(int, uint32_t);
  void (*set_debug_extensions_enabled)(bool);
  bool (*get_debug_extensions_enabled)();
  bool (*trigger_timing_capture)();
  uint64_t (*get_cpu_pc_by_tag)(const char*);
  bool (*read_cpu_state_u64_by_index)(int, int, uint64_t*);
  bool (*read_cpu_state_u64_by_tag)(const char*, int, uint64_t*);
  bool (*read_cpu_register_by_tag)(const char*, const char*, uint64_t*);
  bool (*write_cpu_state_u64_by_index)(int, int, uint64_t);
  bool (*write_cpu_state_u64_by_tag)(const char*, int, uint64_t);
  bool (*write_cpu_register_by_tag)(const char*, const char*, uint64_t);
  void (*add_inject_rule)(const char*, uint64_t, uint64_t, uint32_t, uint8_t, bool);
  void (*clear_inject_rules)();
  void (*add_inject_rule_ex)(const char*, uint64_t, uint64_t, uint32_t, uint8_t, bool, bool, uint32_t, uint32_t);
  int (*get_static_region_count)();
  bool (*get_static_region_info)(int, libretro_ext_static_region_info*);
  int (*get_share_count)();
  const char* (*get_share_tag)(int);
  uint64_t (*get_share_size)(const char*);
  uint32_t (*get_share_flags)(const char*);
  bool (*get_share_info)(int, libretro_ext_share_info*);
  uint64_t (*read_share)(const char*, uint64_t, void*, uint64_t);
  uint64_t (*write_share)(const char*, uint64_t, const void*, uint64_t);
  uint64_t (*get_rollback_serialize_size)();
  bool (*rollback_serialize)(void*, uint64_t);
  bool (*rollback_unserialize)(const void*, uint64_t);
  uint32_t (*get_rollback_state_interface_version)();
  uint32_t (*get_rollback_state_format_version)();
  uint64_t (*get_rollback_state_compatibility_id)();
  uint32_t (*get_rollback_state_flags)();
  uint32_t (*get_rollback_preferred_delta_block_size)();
  uint64_t (*rollback_build_delta)(const void*, const void*, uint64_t, void*, uint64_t);
  bool (*rollback_apply_delta)(void*, uint64_t, const void*, uint64_t);
  // --- execution traps (tail; gate on sizeof_struct) ---
  void (*set_exec_trap_callback)(libretro_ext_exec_trap_cb, void* user);
  bool (*add_exec_trap)(const char* cpu_tag, uint64_t address);
  bool (*remove_exec_trap)(const char* cpu_tag, uint64_t address);
  void (*clear_exec_traps)();
  // Only valid inside a trap callback: performs a real opcode fetch cycle at PC, then executes
  // `opcode` in place of the fetched byte (e.g. the instruction displaced by a WDM patch).
  bool (*cpu_execute_opcode)(const char* cpu_tag, uint8_t opcode);
  // --- BG tile overrides (tail; gate on sizeof_struct) ---
  // Video-only substitution of an 8x8 BG tile's pixels, keyed by the VRAM word address of the
  // tilemap entry that references it. Applies while that entry still equals expected_entry, so it
  // disappears by itself if the game rewrites the cell. The entry's palette, priority and flip
  // flags are used as normal; pixels are 64 row-major color indices (0 = transparent), of which
  // the layer's bit depth (2/4/8 bpp) is honoured. Uses no VRAM and does not affect emulation
  // or save states; frontends re-establish overrides after loading a state.
  // bsnes_ext: fast PPU only, 8x8 tiles, non-hires; set returns false when unsupported.
  bool (*set_bg_tile_override)(uint32_t tilemap_address, uint16_t expected_entry, const uint8_t* pixels64);
  bool (*clear_bg_tile_override)(uint32_t tilemap_address);
  void (*clear_bg_tile_overrides)();
};

LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api(void);
LIBRETRO_EXT_EXPORT const libretro_ext_api* libretro_ext_get_api_v5(void);
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_abi_version(void);
LIBRETRO_EXT_EXPORT uint32_t libretro_ext_get_api_struct_size(void);
void libretro_ext_reset(void);
void libretro_ext_frame_complete(void);

#endif
