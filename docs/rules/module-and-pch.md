# Modules And PCH

Public headers expose narrow value and ownership contracts. Heavy third-party
headers stay private. Consumers may include a narrow header or library umbrella.
`src/oran-<lib>/_impl/` holds implementation details shared by that library's TUs.

The stable PCH in `include/oran/_pch.hpp` contains low-cost standard headers and
core value types. Frequently edited runtime headers and generated dependencies
do not belong in it. A per-target exception needs measured compile-cost evidence.

The supported build uses ordinary headers and static libraries. The modules
option remains experimental; migrate only with a complete build/test and measured
cost comparison. C++26 and the [compile budget](compile-budget.md) remain binding.
