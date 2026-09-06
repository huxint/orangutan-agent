# Compile Cost

Keep heavy dependencies in implementation files and use narrow public headers.
Prefer value types and ordinary functions over template machinery. Split a
translation unit by responsibility when measurement identifies a hotspot.

`xmake/targets.lua` owns library boundaries; each library builds separately with
the stable PCH. GUI, messaging and scheduling dependencies are absent from the
core build. The existing vector backend remains optional.

Run affected builds while iterating. For a compile-cost change, measure before
and after with `scripts/measure-tu.sh` and compare the same compiler, hardware,
mode and job count. `scripts/check-compile-budget.sh` drives the broader budget
check. Thresholds and reference hardware live in
[compile-budget](rules/compile-budget.md).

Hosted functional C++ jobs are separate from reference-hardware performance
provisioning. Report unmeasured cost as unverified.
