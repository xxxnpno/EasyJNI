// Standalone (no-JVM) unit test for hotspot::decode_oop_pointer /
// encode_oop_pointer null-safety and hotspot::is_valid_pointer boundary logic.
//
// This executable runs with NO HotSpot JVM in-process.  gHotSpotVMStructs is
// therefore never resolvable, so the only OOP-codec behaviour that is
// statically determinable here is the *null-input* contract (the early-return
// guards that fire before any VMStruct lookup) plus the no-JVM fall-through
// (VMStruct lookup fails -> codec returns its null/zero sentinel without
// crashing).  Anything that needs a live oop, a real heap base, or a running
// JVM (e.g. a non-trivial decode->encode round-trip across a non-zero
// narrow_oop_base) is OUT OF SCOPE here and is covered by JVM integration in
// example.cpp instead.
//
// is_valid_pointer is pure address arithmetic (range + alignment + poison
// switch), so its full boundary behaviour IS checkable without a JVM.  Source
// of truth: vmhook/ext/vmhook/vmhook.hpp:1768-1805 (is_valid_pointer),
// :4226-4290 (decode_oop_pointer), :4298-4361 (encode_oop_pointer),
// :505/:510 (os::user_address_ceiling / user_address_floor).
#include <vmhook/vmhook.hpp>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

static int failures{ 0 };
static auto check(const char* name, bool ok) -> void
{
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name);
    if (!ok) { ++failures; }
}

int main()
{
    using vmhook::hotspot::decode_oop_pointer;
    using vmhook::hotspot::encode_oop_pointer;
    using vmhook::hotspot::is_valid_pointer;

    // ===================================================================
    // A. decode_oop_pointer / encode_oop_pointer — null-input contract.
    //    These guards (vmhook.hpp:4229 and :4301) run BEFORE any
    //    gHotSpotVMStructs lookup, so they are JVM-independent and fully
    //    deterministic in this no-JVM build.
    // ===================================================================

    // decode_oop_pointer(0) -> nullptr.  A null compressed oop is the
    // canonical encoding of a Java null reference; it must map to a null
    // native pointer regardless of heap base/shift.  (Documented:
    // "@return ... or nullptr if compressed is 0".)
    check("decode_oop_pointer_zero_is_null",
          decode_oop_pointer(0u) == nullptr);

    // The same call expressed via a typed variable, to make sure the
    // public signature really is (std::uint32_t) -> void*.
    {
        const std::uint32_t null_oop{ 0u };
        void* const decoded{ decode_oop_pointer(null_oop) };
        check("decode_oop_pointer_zero_is_null_typed", decoded == nullptr);
    }

    // encode_oop_pointer(nullptr) -> 0.  Inverse guard (vmhook.hpp:4301):
    // a null native pointer encodes back to the null compressed oop.
    check("encode_oop_pointer_null_is_zero",
          encode_oop_pointer(nullptr) == 0u);

    // Round-trip through null in both directions.  These compose the two
    // guards above and are the ONLY decode<->encode identity that holds
    // without a live heap base (a non-null round-trip needs the JVM's
    // narrow_oop_base/shift, covered in example.cpp).
    check("roundtrip_decode_then_encode_null",
          encode_oop_pointer(decode_oop_pointer(0u)) == 0u);
    check("roundtrip_encode_then_decode_null",
          decode_oop_pointer(encode_oop_pointer(nullptr)) == nullptr);

    // No-JVM fall-through: a *non-zero* compressed oop cannot be decoded
    // because gHotSpotVMStructs is absent, so base_entry/shift_entry stay
    // null and decode_oop_pointer returns nullptr (vmhook.hpp:4280-4283)
    // WITHOUT crashing.  This documents the no-JVM behaviour; under a live
    // JVM this same input would decode to a real heap address instead.
    check("decode_oop_pointer_nonzero_no_jvm_is_null",
          decode_oop_pointer(0x0000'0001u) == nullptr);
    check("decode_oop_pointer_max_no_jvm_is_null",
          decode_oop_pointer(0xFFFF'FFFFu) == nullptr);

    // No-JVM fall-through for the encoder: a non-null pointer with no
    // resolvable VMStructs returns 0 (vmhook.hpp:4348-4351) and does not
    // crash.  Use the address of a stack local as a plausible "decoded"
    // pointer.  Under a live JVM the result would be a real narrow oop.
    {
        int stack_anchor{ 0 };
        check("encode_oop_pointer_nonnull_no_jvm_is_zero",
              encode_oop_pointer(&stack_anchor) == 0u);
    }

    // Signature / return-type pinning: decode yields a void*, encode yields
    // a std::uint32_t.  A compile-time mismatch here would fail the build,
    // which is itself the assertion; the runtime check just exercises it.
    {
        const bool decode_returns_voidptr{
            std::is_same_v<decltype(decode_oop_pointer(0u)), void*> };
        const bool encode_returns_u32{
            std::is_same_v<decltype(encode_oop_pointer(nullptr)),
                           std::uint32_t> };
        check("decode_oop_pointer_returns_void_ptr", decode_returns_voidptr);
        check("encode_oop_pointer_returns_uint32", encode_returns_u32);
    }

    // Both codec entry points are declared noexcept (vmhook.hpp:4226/:4298);
    // pin that so a future change that can throw is caught at compile time.
    {
        int stack_anchor{ 0 };
        check("decode_oop_pointer_is_noexcept",
              noexcept(decode_oop_pointer(0u)));
        check("encode_oop_pointer_is_noexcept",
              noexcept(encode_oop_pointer(&stack_anchor)));
    }

    // ===================================================================
    // B. is_valid_pointer — floor / ceiling boundaries.
    //    is_valid_pointer rejects addr <= user_address_floor and
    //    addr >= user_address_ceiling, rejects odd (bit-0 set) addresses,
    //    and rejects a fixed set of debug-poison low-32 patterns.
    //    All of this is pure arithmetic and fully checkable with no JVM.
    //    (vmhook.hpp:1771-1804.)
    // ===================================================================

    constexpr std::uintptr_t floor{ vmhook::os::user_address_floor };    // 0xFFFF
    constexpr std::uintptr_t ceiling{ vmhook::os::user_address_ceiling };// 0x7FFF'FFFF'FFFF

    // nullptr and the very lowest integers are below the floor -> rejected.
    check("is_valid_pointer_null_rejected",
          !is_valid_pointer(nullptr));
    check("is_valid_pointer_zero_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(std::uintptr_t{ 0 })));
    // 1 is both below the floor AND odd: doubly rejected.
    check("is_valid_pointer_one_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(std::uintptr_t{ 1 })));

    // Exactly AT the floor must be rejected — the comparison is `<=`.
    check("is_valid_pointer_at_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(floor)));
    // One below the floor: rejected.
    check("is_valid_pointer_below_floor_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(floor - 1)));
    // floor itself (0xFFFF) is odd, so floor+1 = 0x10000 is the first
    // address that clears BOTH the range check and the 2-byte-alignment
    // check: it must be accepted.  This is the low canonical boundary.
    check("is_valid_pointer_just_above_floor_accepted",
          is_valid_pointer(reinterpret_cast<void*>(floor + 1)));

    // Exactly AT the ceiling must be rejected — the comparison is `>=`.
    check("is_valid_pointer_at_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(ceiling)));
    // One above the ceiling: rejected (non-canonical / kernel side).
    check("is_valid_pointer_above_ceiling_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(ceiling + 1)));
    // One below the ceiling is even (ceiling 0x7FFF'FFFF'FFFF is odd, so
    // ceiling-1 is even) and in range: the highest canonical address that
    // is still accepted.
    check("is_valid_pointer_just_below_ceiling_accepted",
          is_valid_pointer(reinterpret_cast<void*>(ceiling - 1)));

    // A clearly non-canonical high address (top half of the 64-bit space,
    // well above the user ceiling) is rejected.
    check("is_valid_pointer_high_noncanonical_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0xFFFF'8000'0000'0000ull })));
    // The lowest kernel-half address (just past the canonical user range)
    // is also rejected.
    check("is_valid_pointer_kernel_base_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0x0000'8000'0000'0000ull })));

    // ===================================================================
    // C. is_valid_pointer — alignment (bit-0) rejection.
    //    Only bit 0 is checked (2-byte alignment); the function deliberately
    //    does NOT require 8-byte alignment, so an in-range odd address is
    //    rejected but the +1/+2/+4 even neighbours are accepted.
    // ===================================================================

    // An in-range but odd (bit-0 set) address is rejected even though it is
    // comfortably inside [floor, ceiling].
    check("is_valid_pointer_odd_low_canonical_rejected",
          !is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0x0000'0000'0040'0001ull })));
    // The even neighbour of that same address is accepted.
    check("is_valid_pointer_even_low_canonical_accepted",
          is_valid_pointer(reinterpret_cast<void*>(
              std::uintptr_t{ 0x0000'0000'0040'0000ull })));

    {
        // A naturally-aligned stack array gives us real 2/4/8-byte aligned
        // addresses without hardcoding any constant.
        std::int64_t aligned_block[4]{};
        void* const eight_aligned{ &aligned_block[0] };              // 8-byte
        void* const four_aligned{
            reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(&aligned_block[0]) + 4) };
        void* const two_aligned{
            reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(&aligned_block[0]) + 2) };
        void* const odd_aligned{
            reinterpret_cast<void*>(
                reinterpret_cast<std::uintptr_t>(&aligned_block[0]) + 1) };

        check("is_valid_pointer_8byte_aligned_accepted",
              is_valid_pointer(eight_aligned));
        check("is_valid_pointer_4byte_aligned_accepted",
              is_valid_pointer(four_aligned));
        check("is_valid_pointer_2byte_aligned_accepted",
              is_valid_pointer(two_aligned));
        // odd interior pointer (bit 0 set) into a live stack object: rejected
        // purely on the alignment rule even though the memory is real.
        check("is_valid_pointer_odd_interior_rejected",
              !is_valid_pointer(odd_aligned));
    }

    // ===================================================================
    // D. is_valid_pointer — debug-poison low-32 patterns.
    //    The switch (vmhook.hpp:1789-1803) rejects any pointer whose low 32
    //    bits match a known uninitialised/freed fill, even though the
    //    address sits inside the canonical user range.  IMPORTANT: the
    //    alignment check (bit 0) runs BEFORE the poison switch, so for the
    //    ODD-valued sentinels (0xDEADBEEF, 0xCDCDCDCD, 0xBAADF00D,
    //    0xABABABAB, 0xFDFDFDFD, 0xDDDDDDDD) the rejection is actually
    //    produced by alignment, not by the switch.  Only the EVEN-valued
    //    sentinels (0xCAFEBABE, 0xCCCCCCCC, 0xFEEEFEEE) reach and exercise
    //    the poison switch.  We test the two groups separately so each
    //    assertion pins the rule that genuinely fires.
    // ===================================================================

    {
        // high_prefix is in range (0x1234 < 0x7FFF) and even, so it never
        // trips the range or alignment checks on its own.
        constexpr std::uintptr_t high_prefix{ 0x0000'1234'0000'0000ull };

        // -- Even sentinels: in-range, bit-0 clear, so ONLY the poison
        //    switch can reject them.  This is the assertion that actually
        //    proves the switch does its job.
        const std::uint32_t even_poison[]{
            0xCAFEBABEu, 0xCCCCCCCCu, 0xFEEEFEEEu,
        };
        bool even_poison_rejected{ true };
        for (const std::uint32_t low : even_poison)
        {
            const std::uintptr_t addr{ high_prefix | low };
            // Guard the premise: these must be even and in range.
            const bool premise_even_in_range{
                (addr & 0x1u) == 0u
                && addr > floor && addr < ceiling };
            if (!premise_even_in_range
                || is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                even_poison_rejected = false;
            }
        }
        check("is_valid_pointer_even_debug_poison_rejected_by_switch",
              even_poison_rejected);

        // -- Odd sentinels: still rejected, but by the alignment rule that
        //    precedes the switch.  Asserting they are rejected documents the
        //    end-to-end contract (these low-32 patterns never pass), while
        //    the premise check records WHY (bit 0 set).
        const std::uint32_t odd_poison[]{
            0xDEADBEEFu, 0xCDCDCDCDu, 0xBAADF00Du,
            0xABABABABu, 0xFDFDFDFDu, 0xDDDDDDDDu,
        };
        bool odd_poison_rejected{ true };
        bool odd_poison_all_odd{ true };
        for (const std::uint32_t low : odd_poison)
        {
            if ((low & 0x1u) == 0u) { odd_poison_all_odd = false; }
            const std::uintptr_t addr{ high_prefix | low };
            if (is_valid_pointer(reinterpret_cast<void*>(addr)))
            {
                odd_poison_rejected = false;
            }
        }
        check("is_valid_pointer_odd_debug_poison_are_all_odd",
              odd_poison_all_odd);
        check("is_valid_pointer_odd_debug_poison_rejected",
              odd_poison_rejected);

        // Control: the SAME high prefix with a benign even low half is
        // accepted, proving the rejections above came from the value of the
        // low half (poison / alignment) and not from the range check.
        const std::uintptr_t benign{ high_prefix | 0x0000'1000ull };
        check("is_valid_pointer_benign_low_half_accepted",
              is_valid_pointer(reinterpret_cast<void*>(benign)));

        // Sanity: the poison match is EXACT on the full low 32 bits.  A value
        // one bit away from 0xDEADBEEF that is also made even (0xDEADBEEE) is
        // NOT a sentinel, so with an in-range high prefix it is accepted.
        // This proves the switch is an exact low-32 compare, not a byte/sub-
        // pattern scan.  (low32 0xDEADBEEE: bit 0 clear, not in the switch.)
        const std::uintptr_t near_poison{ high_prefix | 0xDEADBEEEu };
        check("is_valid_pointer_near_poison_low_half_accepted",
              is_valid_pointer(reinterpret_cast<void*>(near_poison)));
    }

    // ===================================================================
    // E. is_valid_pointer — real live addresses are accepted.
    //    The whole point of the helper is to wave through genuine, mapped,
    //    aligned pointers.  A stack address, a heap allocation, and an
    //    allocate_rwx page are all real in this process with no JVM.
    // ===================================================================

    {
        int on_stack{ 7 };
        check("is_valid_pointer_real_stack_address_accepted",
              is_valid_pointer(&on_stack));
    }

    {
        // A heap allocation via std::vector backing store: real, mapped,
        // and at least 2-byte aligned (operator new is over-aligned in
        // practice).  This is the high-confidence "real pointer is valid"
        // case the boundary helper is designed to pass.
        std::vector<std::uint64_t> heap_block(8, 0);
        void* const heap_ptr{ heap_block.data() };
        check("is_valid_pointer_real_heap_address_accepted",
              is_valid_pointer(heap_ptr));
    }

    {
        // allocate_rwx returns a page in this process' user address space.
        // It must satisfy is_valid_pointer; release it afterwards.  If the
        // platform refuses RWX (returns nullptr) the assertion is skipped
        // so the test stays portable.
        const std::size_t size{ vmhook::os::page_size() };
        void* const page{ vmhook::os::allocate_rwx(nullptr, size) };
        if (page != nullptr)
        {
            check("is_valid_pointer_allocate_rwx_page_accepted",
                  is_valid_pointer(page));
            vmhook::os::release(page, size);
        }
        else
        {
            std::printf("[INFO] is_valid_pointer_allocate_rwx_page_accepted:"
                        " skipped (allocate_rwx returned nullptr)\n");
        }
    }

    // is_valid_pointer is declared noexcept (vmhook.hpp:1768); pin it.
    check("is_valid_pointer_is_noexcept",
          noexcept(is_valid_pointer(nullptr)));

    // ===================================================================
    // F. decode_oop_pointer null result is consistent with is_valid_pointer.
    //    A decoded null oop is, by definition, NOT a valid dereferenceable
    //    pointer — so decode_oop_pointer(0) feeding is_valid_pointer must
    //    report false.  Ties the two clusters together with a pure-logic
    //    invariant that holds with no JVM.
    // ===================================================================
    check("decoded_null_oop_is_not_valid_pointer",
          !is_valid_pointer(decode_oop_pointer(0u)));

    return failures == 0 ? 0 : 1;
}
