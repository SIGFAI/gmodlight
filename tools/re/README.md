# Reverse-engineering helpers

Python 3 scripts used to work out Dying Light's internals (needs `pip install pefile capstone`).
Run them from a folder holding copies of `engine_x64_rwdi.dll` and `gamedll_x64_rwdi.dll`.
Addresses are RVAs in the Steam build 1.55.0.0 (`DyingLightGame.exe`, build id 24076292).

| Script | What it does |
|---|---|
| `disasm.py <dll> <export or rva> [n]` | Disassemble n instructions at an export or RVA |
| `func.py <dll> <rva>[,<rva>] [n]` | Disassemble the whole function (from .pdata) containing an RVA |
| `xref.py <dll> <rva>` | Find `lea reg, [rip+X]` references to an RVA (e.g. a vtable) |
| `callers.py <rva>` | Find direct calls to a gamedll function |
| `impxref.py` | Find gamedll calls to engine imports (edit the name filter) |
| `rtti.py` | Find MSVC RTTI type descriptors and vtables for `*Damage*` classes |
| `vtdump.py <dll> <vtable rva> [n]` | Print vtable entries |
| `vtof.py <type name>` | Every vtable of a class and what its slot 20 (TakeDamage) points to |
| `layout.py <type name>...` | Base classes and their offsets from RTTI (e.g. `.?AVHumanAI@@`) |
| `methods2.py` | Every script method registered with `CRTTIVoidMethod`, grouped by class |
| `enumvals.py` | `EDamageType::*` names and their values |
| `dmgtypes.py` | Damage types used where gamedll builds `SDamageInfoDi` |

`docs/re-data/` holds their output: the engine's export list and the script methods by class.
