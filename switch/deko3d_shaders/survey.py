#!/usr/bin/env python3
"""Pasa todos los shaders de RT64 y recompui por la cadena del backend deko3d
(HLSL -> dxc -> SPIR-V -> spirv_cross_dksh -> GLSL -> uam -> DKSH) y reporta en que
etapa falla cada uno. No se detiene en el primer error: el objetivo es el censo."""

import argparse
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

# Mismas opciones que el CMakeLists de RT64 (DXC_SPV_OPTS y DXC_{PS,VS,CS}_OPTS)
DXC_SPV_OPTS = ["-spirv", "-fspv-target-env=vulkan1.0", "-fvk-use-dx-layout"]
STAGES = {
    "pixel": (["-E", "PSMain", "-T", "ps_6_3"], "frag"),
    "vertex": (["-E", "VSMain", "-T", "vs_6_3", "-fvk-invert-y"], "vert"),
    "compute": (["-E", "CSMain", "-T", "cs_6_3"], "comp"),
}
# build_ray_shader y build_library_shader son de raytracing/DXIL: no aplican en Switch
CALL_RE = re.compile(r"^\s*build_(pixel|vertex|compute)_shader(_spec_constants)?\s*\(\s*(\w+)\s+(.*)\)\s*$")
# Shaders que no se usaran en Switch; el backend les dara un shader de reemplazo
EXCLUDED = {
    "DebugPS": "vista de depuracion del raytracing: usa 16 imagenes y deko3d admite 8",
}


def parse_calls(cmake_path, project_root):
    """Lee las llamadas build_*_shader de un CMakeLists y devuelve una entrada por variante."""
    shaders = []
    for line in Path(cmake_path).read_text().splitlines():
        m = CALL_RE.match(line)
        if not m:
            continue
        stage, spec, _target, args = m.groups()
        args = shlex.split(args)
        src = args[0]
        out = args[1] if len(args) > 1 else src
        defines = []
        for extra in args[2:]:
            defines += extra.split()
        shaders.append({
            "name": Path(out).stem,
            "stage": stage,
            "spec_constants": spec is not None,
            "src": project_root / src,
            "include": project_root / "src",
            "defines": defines,
        })
    return shaders


def run(cmd, env=None):
    p = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return p.returncode == 0, (p.stdout + p.stderr).strip()


def first_error(text, max_lines=6):
    lines = [l for l in text.splitlines() if l.strip()]
    errors = [l for l in lines if "error" in l.lower()] or lines
    return "\n".join(errors[:max_lines])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rt64", required=True, type=Path)
    ap.add_argument("--recompui", required=True, type=Path)
    ap.add_argument("--dxc", required=True)
    ap.add_argument("--dksh-tool", required=True, help="ruta a spirv_cross_dksh")
    ap.add_argument("--uam", required=True)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    shaders = parse_calls(args.rt64 / "CMakeLists.txt", args.rt64)
    shaders += parse_calls(args.recompui / "CMakeLists.txt", args.recompui)
    if not shaders:
        sys.exit("No se encontro ninguna llamada build_*_shader")

    args.out.mkdir(parents=True, exist_ok=True)
    results = []
    for s in shaders:
        base = args.out / s["name"]
        dxc_opts, uam_stage = STAGES[s["stage"]]
        status, detail = "ok", ""
        if s["name"] in EXCLUDED:
            results.append((s, "excluido", EXCLUDED[s["name"]]))
            print(f"[   excluido] {s['name']}: {EXCLUDED[s['name']]}", flush=True)
            continue

        ok, log = run([args.dxc] + DXC_SPV_OPTS + dxc_opts + ["-I", str(s["include"])] + s["defines"]
                      + [str(s["src"]), "-Fo", f"{base}.spv"])
        if not ok:
            status, detail = "dxc", first_error(log)
        else:
            ok, log = run([args.dksh_tool, f"{base}.spv", f"{base}.glsl", f"{base}.map"])
            if not ok:
                status, detail = "spirv_cross_dksh", first_error(log)
            else:
                ok, log = run([args.uam, "-s", uam_stage, "-o", f"{base}.dksh", f"{base}.glsl"])
                if not ok:
                    status, detail = "uam", first_error(log)

        (args.out / f"{s['name']}.log").write_text(log + "\n")
        results.append((s, status, detail))
        print(f"[{status:>11}] {s['name']} ({s['stage']}{', spec' if s['spec_constants'] else ''})", flush=True)
        if detail:
            print("    " + detail.replace("\n", "\n    "), flush=True)

    excluded = sum(1 for _, st, _ in results if st == "excluido")
    total = len(results) - excluded
    passed = sum(1 for _, st, _ in results if st == "ok")
    lines = [f"## Censo de shaders deko3d: {passed}/{total} compilan ({excluded} excluidos)", "",
             "| Shader | Etapa | Falla en | Primer error |", "|---|---|---|---|"]
    for s, st, detail in results:
        err = detail.splitlines()[0].replace("|", "\\|")[:160] if detail else ""
        lines.append(f"| {s['name']} | {s['stage']}{' (spec)' if s['spec_constants'] else ''} | {'' if st == 'ok' else st} | {err} |")
    summary = "\n".join(lines) + "\n"
    (args.out / "SUMMARY.md").write_text(summary)
    if "GITHUB_STEP_SUMMARY" in os.environ:
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write(summary)
    print(f"\n{passed}/{total} shaders compilan ({excluded} excluidos)")


if __name__ == "__main__":
    main()
