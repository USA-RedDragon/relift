//! The dispatch part of the mock CPU layers the emitter tests compile
//! lifted C against: the lookups and miss entries lifted code calls
//! (`rl_indirect`, `rl_jump_find`, `rl_dispatch_miss`,
//! `rl_dispatch_jump_miss`), so every harness follows the ABI in one place.
//! The mock `struct rl_ctx` must have `uint64_t pc` and `rl_pending pend`.

/// Declarations for the mock ABI header, after `struct rl_ctx`. Every
/// lookup misses: an indirect transfer reaches the miss entry.
pub const DISPATCH_H: &str = r#"
RL_RUNTIME_API uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem);
RL_RUNTIME_API uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem);
static inline rl_fn rl_indirect(rl_ctx *c, uint64_t target)
{
    c->pc = target;
    return rl_dispatch_miss;
}
static inline rl_fn rl_jump_find(rl_ctx *c, uint64_t target)
{
    c->pc = target;
    return 0;
}
"#;

/// Definitions for the harness, which defines `rl_dispatch_miss` itself:
/// a jump miss is a miss.
pub const DISPATCH_C: &str = "uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem) { return rl_dispatch_miss(c, mem); }\n";
