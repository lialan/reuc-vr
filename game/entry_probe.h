#pragma once
#include "ppc_rt.h"
#include "hooks.h"
#include "audio_gap_audit.h"
#include "audio_startup.h"
#include "prompt_hooks.h"
#include "state_dump.h"
#include <type_traits>
#include <utility>
#ifdef UC_FN_COUNT
#define UC_FN_COUNT_HOOK(a) uc::fn_count(a,c.lr,c.r[3]); if(c.lr==0x80077D58u) uc::vm_trace(a,c);
#else
#define UC_FN_COUNT_HOOK(a)
#endif
namespace uc {
// Initialize before guest execution; callbacks under baton; inspect stats after join.
struct EntryProbeStats {uint64_t events=0,records=0,invalid=0;};
struct PictureProbeStats {uint64_t calls=0,records=0,invalid=0;};
void entry_probe_init(bool);
EntryProbeStats entry_probe_stats();
void game_event_probe(const ppc::Ctx&);
void picture_entry_probe(const ppc::Ctx&,uint32_t entry);
void layout_entry_probe(const ppc::Ctx&);
PictureProbeStats picture_probe_stats();
}
// Only selected diagnostic entries emit probes; other entries fold away.
namespace uc {
// The entries UC_ENTER runs native code at (keep in step with the address tests in UC_ENTER below).
constexpr bool entry_has_probe(uint32_t a) {
    return a == 0x80049D64u || a == 0x80068DD8u || a == 0x802E3C10u || a == 0x802E3C94u || a == 0x800676A0u || a == 0x8038BC60u ||
           a == 0x803CFA84u || a == 0x8038AEB0u;
}
// No native code runs at the entry or exit of this translated function: a call of it may sync only the registers the function itself
// loads and stores (the translator's leaf calls); a probe, a scope or a mod hook may look at any register.
template <uint32_t a> constexpr bool plain_entry() {
    return !entry_has_probe(a) && !mods::is_hookable(a) && std::is_empty_v<decltype(prompt_scope<a>(std::declval<ppc::Ctx&>()))> &&
           std::is_empty_v<decltype(audio_start_scope<a>(std::declval<const ppc::Ctx&>()))> &&
           std::is_empty_v<decltype(mods::post_scope<a>(std::declval<ppc::Ctx&>()))>;
}
}
#undef UC_PLAIN_ENTRY
#if defined(UC_TRACE) || defined(UC_FN_COUNT)
#define UC_PLAIN_ENTRY(a) false
#else
#define UC_PLAIN_ENTRY(a) (uc::plain_entry<a>())
#endif
#undef UC_ENTER
#ifdef UC_TRACE
#define UC_ENTER(a) UC_FN_COUNT_HOOK(a) auto uc_prompt_scope=uc::prompt_scope<a>(c); auto uc_audio_scope=uc::audio_start_scope<a>(c); auto uc_post_scope=uc::mods::post_scope<a>(c); do { if constexpr((a)==0x80049D64u)uc::audio_wait_ready(c); ppc::trace_enter(a); uc::gx_audit_enter(a,c); if constexpr((a)==0x80068DD8u)uc::audio_gap_audit_request(c); if constexpr((a)==0x802E3C10u || (a)==0x802E3C94u)uc::audio_gap_audit_wpad(c,a); if constexpr((a)==0x800676A0u)uc::game_event_probe(c); if constexpr((a)==0x8038BC60u || (a)==0x803CFA84u)uc::picture_entry_probe(c,a); if constexpr((a)==0x8038AEB0u)uc::layout_entry_probe(c); if (uc::mods::hook_enter<a>(c)) return; } while(0)
#else
#define UC_ENTER(a) UC_FN_COUNT_HOOK(a) auto uc_prompt_scope=uc::prompt_scope<a>(c); auto uc_audio_scope=uc::audio_start_scope<a>(c); auto uc_post_scope=uc::mods::post_scope<a>(c); do { if constexpr((a)==0x80049D64u)uc::audio_wait_ready(c); if constexpr((a)==0x80068DD8u)uc::audio_gap_audit_request(c); if constexpr((a)==0x802E3C10u || (a)==0x802E3C94u)uc::audio_gap_audit_wpad(c,a); if constexpr((a)==0x800676A0u)uc::game_event_probe(c); if constexpr((a)==0x8038BC60u || (a)==0x803CFA84u)uc::picture_entry_probe(c,a); if constexpr((a)==0x8038AEB0u)uc::layout_entry_probe(c); if (uc::mods::hook_enter<a>(c)) return; } while(0)
#endif
